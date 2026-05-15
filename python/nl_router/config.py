"""Bootstrap configuration loader for the management CLI and API.

Loads the small set of local config keys needed before we can talk to the
central database: DSN, server_id, listen ports, log level, KEK source.

Precedence (per the design plan):
    1. Explicit constructor arg.
    2. Environment variable.
    3. TOML file at $NL_ROUTER_CONFIG (default /etc/nl-router/config.toml).
    4. Hard-coded default.

The KEK has its own precedence rule (file beats env) handled in
`load_kek()`.
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path
from typing import Any

DEFAULT_CONFIG_PATH = Path("/etc/nl-router/config.toml")
DEFAULT_DSN = "postgres://nl_router:nl_router@localhost:5432/nl_router?sslmode=disable"
DEFAULT_LISTEN_PORT = 11112
DEFAULT_TLS_LISTEN_PORT = 2762
DEFAULT_API_PORT = 8080
DEFAULT_METRICS_PORT = 9184  # api process; receiver/router/etc. have their own
DEFAULT_KEK_FILE = Path("/etc/nl-router/kek.key")


@dataclass(frozen=True)
class BootstrapConfig:
    """Minimal config loaded from local file/env before DB connectivity.

    DB-resident config (rules, destinations, system_config knobs) is read
    separately via the DB once this is loaded.
    """

    # --- core ---
    server_id: str
    database_url: str
    log_level: str = "info"
    config_path: Path | None = None       # the file we loaded from, if any

    # --- DICOM listeners ---
    listen_port: int = DEFAULT_LISTEN_PORT
    tls_listen_port: int = DEFAULT_TLS_LISTEN_PORT
    tls_enabled: bool = False

    # --- HTTP API ---
    api_port: int = DEFAULT_API_PORT
    metrics_port: int = DEFAULT_METRICS_PORT
    api_bind: str = "127.0.0.1"

    # --- filesystem ---
    landing_zone: Path = Path("/var/lib/nl-router/incoming")
    processing_zone: Path = Path("/var/lib/nl-router/processing")

    # --- KEK source (file beats env at load time) ---
    kek_file: Path | None = None
    kek_env_set: bool = False             # whether NL_ROUTER_KEK is present

    # --- raw TOML for app-specific keys we haven't promoted to fields ---
    extras: dict[str, Any] = field(default_factory=dict)


def _toml_load(path: Path) -> dict[str, Any]:
    """Read and parse a TOML file. Returns {} if the file does not exist."""
    if not path.exists():
        return {}
    with path.open("rb") as f:
        return tomllib.load(f)


def _env_or(toml_section: dict[str, Any], key: str, env_name: str, default: Any) -> Any:
    """Resolve a config value with env > toml > default precedence."""
    if env_value := os.environ.get(env_name):
        return env_value
    if key in toml_section:
        return toml_section[key]
    return default


@cache
def load() -> BootstrapConfig:
    """Load and cache the bootstrap config.

    Subsequent calls return the same instance. Tests should call
    `load.cache_clear()` between cases.
    """
    config_path = Path(os.environ.get("NL_ROUTER_CONFIG", str(DEFAULT_CONFIG_PATH)))
    raw = _toml_load(config_path)

    core = raw.get("core", {})
    dicom = raw.get("dicom", {})
    tls = raw.get("tls", {})
    api = raw.get("api", {})
    fs = raw.get("filesystem", {})
    sec = raw.get("security", {})

    server_id = _env_or(core, "server_id", "NL_ROUTER_SERVER_ID", os.uname().nodename)
    database_url = _env_or(
        core,
        "database_url",
        "NL_ROUTER_DATABASE_URL",
        # Also accept the conventional DATABASE_URL env (matches Makefile).
        os.environ.get("DATABASE_URL", DEFAULT_DSN),
    )

    return BootstrapConfig(
        server_id=str(server_id),
        database_url=str(database_url),
        log_level=str(_env_or(core, "log_level", "NL_ROUTER_LOG_LEVEL", "info")),
        config_path=config_path if config_path.exists() else None,
        listen_port=int(_env_or(dicom, "listen_port", "NL_ROUTER_LISTEN_PORT", DEFAULT_LISTEN_PORT)),
        tls_listen_port=int(
            _env_or(tls, "listen_port", "NL_ROUTER_TLS_LISTEN_PORT", DEFAULT_TLS_LISTEN_PORT)
        ),
        tls_enabled=bool(_env_or(tls, "enabled", "NL_ROUTER_TLS_ENABLED", False)),
        api_port=int(_env_or(api, "port", "NL_ROUTER_API_PORT", DEFAULT_API_PORT)),
        metrics_port=int(_env_or(api, "metrics_port", "NL_ROUTER_METRICS_PORT", DEFAULT_METRICS_PORT)),
        api_bind=str(_env_or(api, "bind", "NL_ROUTER_API_BIND", "127.0.0.1")),
        landing_zone=Path(_env_or(fs, "landing_zone", "NL_ROUTER_LANDING_ZONE", "/var/lib/nl-router/incoming")),
        processing_zone=Path(
            _env_or(fs, "processing_zone", "NL_ROUTER_PROCESSING_ZONE", "/var/lib/nl-router/processing")
        ),
        kek_file=Path(sec["kek_file"]) if "kek_file" in sec else None,
        kek_env_set="NL_ROUTER_KEK" in os.environ,
        extras=raw,
    )


def load_kek() -> bytes:
    """Resolve the KEK (master encryption key) from configured sources.

    Precedence (per the design plan): file beats env when both are set.
    The KEK is 32 bytes; we expect either a raw 32-byte file or a
    base64url-encoded value (auto-detected by length).

    Raises:
        RuntimeError: if no KEK source is configured or the value is invalid.
    """
    import base64

    cfg = load()

    # File first.
    kek_path = cfg.kek_file or DEFAULT_KEK_FILE
    if kek_path.exists():
        data = kek_path.read_bytes()
        # Strip a trailing newline if someone `echo "..." > kek.key`'d.
        data = data.rstrip(b"\r\n")
        return _decode_kek(data, source=f"file:{kek_path}")

    # Then env.
    if env := os.environ.get("NL_ROUTER_KEK"):
        return _decode_kek(env.encode(), source="env:NL_ROUTER_KEK")

    raise RuntimeError(
        f"No KEK configured. Provide either {kek_path} (32 raw bytes or "
        f"base64url) or set NL_ROUTER_KEK environment variable."
    )


def _decode_kek(raw: bytes, *, source: str) -> bytes:
    """Return a 32-byte key, accepting raw bytes or base64url-encoded input."""
    import base64

    if len(raw) == 32:
        return bytes(raw)
    try:
        decoded = base64.urlsafe_b64decode(raw + b"=" * (-len(raw) % 4))
    except Exception as exc:
        raise RuntimeError(f"KEK from {source}: not 32 raw bytes and not valid base64url") from exc
    if len(decoded) != 32:
        raise RuntimeError(f"KEK from {source}: decoded length {len(decoded)} != 32 bytes")
    return decoded
