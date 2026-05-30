"""`nl-router init` — first-run / post-install setup wizard.

Idempotent — designed to run after the .deb/.rpm has been installed and
the operator has edited /etc/nl-router/config.toml with their DSN.
Performs six steps in order, skipping any that are already done:

  1. Generate /etc/nl-router/kek.key (32 random bytes, base64url-encoded)
     if missing; chown to nl-router:nl-router; chmod 0400.
  2. Generate /etc/nl-router/env from config.toml (M22) so systemd units
     pick up NL_ROUTER_SERVER_ID + NL_ROUTER_DATABASE_URL on start. Pre-
     M22, operators had to write this file by hand or every daemon
     crashed with "required env var not set."
  3. Generate /etc/nl-router/module-<kind>.env for every shipped module
     kind, assigning a unique NL_ROUTER_METRICS_PORT (M26). Lights up
     the per-module-kind Prometheus scrape targets without any operator
     port-allocation work.
  4. Run `nl-router migrate up` if the schema isn't initialized.
  5. Mint an initial admin API token (one only — re-runs are no-ops
     unless --force).
  6. Print next-steps banner.

Operator can opt out of any step with --skip-kek-gen / --skip-env /
--skip-module-env / --skip-migrate / --skip-token. The KEK and
migrations are typically the slowest path; the token mint is fast.

Future expansion (deferred): create a local admin user with password,
seed initial OIDC config from a TOML, install Grafana dashboards.
"""

from __future__ import annotations

import base64
import json
import os
import pwd
import secrets
import shutil
import subprocess
from pathlib import Path
from typing import Annotated

import typer

from nl_router.cli._common import die, out
from nl_router.config import DEFAULT_KEK_FILE, load
from nl_router.db import connect, schema_version


# Canonical metrics-port assignments per shipped module kind.
#
# 9190+ is the design-plan range for module workers (receiver=9180,
# router=9181, dispatcher=9182, cleaner=9183, api=9184). When a new
# module ships, add it here AND uncomment the matching entry in
# monitoring/prometheus.yml — the two must stay in sync.
#
# Operators don't edit this dict; if they're running their own custom
# module they hand-write /etc/nl-router/module-<kind>.env with their
# chosen port and add their own scrape config.
MODULE_METRICS_PORTS: dict[str, int] = {
    "anonymize_basic":               9190,
    "standardize_institution_group": 9191,
}


def init(
    token_name: Annotated[
        str,
        typer.Option(
            "--token-name",
            help="Name attached to the bootstrap admin token (for audit).",
        ),
    ] = "bootstrap-admin",
    force: Annotated[
        bool,
        typer.Option(
            "--force",
            help="Create a fresh bootstrap token even if api_tokens rows already exist.",
        ),
    ] = False,
    skip_kek_gen: Annotated[
        bool,
        typer.Option(
            "--skip-kek-gen",
            help="Don't generate a KEK even if /etc/nl-router/kek.key is missing.",
        ),
    ] = False,
    skip_env: Annotated[
        bool,
        typer.Option(
            "--skip-env",
            help="Don't generate /etc/nl-router/env from config.toml.",
        ),
    ] = False,
    skip_module_env: Annotated[
        bool,
        typer.Option(
            "--skip-module-env",
            help="Don't generate /etc/nl-router/module-<kind>.env files "
                 "with metrics-port assignments.",
        ),
    ] = False,
    skip_migrate: Annotated[
        bool,
        typer.Option(
            "--skip-migrate",
            help="Don't auto-run migrations even if the schema is uninitialized.",
        ),
    ] = False,
    skip_token: Annotated[
        bool,
        typer.Option(
            "--skip-token",
            help="Don't mint a bootstrap token (KEK + migrations only).",
        ),
    ] = False,
) -> None:
    """First-run setup. Generates KEK, applies migrations, mints admin token.

    Re-runnable: every step checks current state and only acts if needed.
    Use --force to mint an additional token when active ones already exist.
    """
    cfg = load()
    out.rule("[bold]nl-router init[/bold]")
    out.print(f"  config: [cyan]{cfg.config_path or '(none — using defaults)'}[/cyan]")
    out.print(f"  dsn:    [cyan]{_redact_dsn(cfg.database_url)}[/cyan]")
    out.print("")

    # ---- 1. KEK -----------------------------------------------------------
    if not skip_kek_gen:
        _ensure_kek(cfg.kek_file or DEFAULT_KEK_FILE)

    # ---- 2. Shared systemd env file ---------------------------------------
    if not skip_env:
        _ensure_env_file(cfg)

    # ---- 3. Per-module-kind env files (metrics ports) ---------------------
    if not skip_module_env:
        _ensure_module_env_files()

    # ---- 4. Migrations ----------------------------------------------------
    if not skip_migrate:
        version = schema_version()
        if version is None:
            out.print("  [yellow]→ schema uninitialized, running migrations[/yellow]")
            _run_migrate_up()
            version = schema_version()
            if version is None:
                die("migrations ran but schema_migrations is still empty?", code=2)
        out.print(f"  schema version: [green]{version}[/green]")
    else:
        version = schema_version()
        if version is None:
            die(
                "Schema is not initialized and --skip-migrate was passed. "
                "Run `nl-router migrate` first or rerun without --skip-migrate.",
                code=2,
            )
        out.print(f"  schema version: [green]{version}[/green]")

    # ---- 3. Token ---------------------------------------------------------
    raw_token: str | None = None
    if not skip_token:
        raw_token = _mint_bootstrap_token(token_name=token_name, force=force)

    # ---- 4. Banner --------------------------------------------------------
    out.print("")
    out.rule("[green]nl-router ready[/green]")
    out.print("[dim]Next steps:[/dim]")
    out.print("[dim]  sudo systemctl enable --now nl-router-api \\\\[/dim]")
    out.print("[dim]                              nl-router-receiver \\\\[/dim]")
    out.print("[dim]                              nl-router-route \\\\[/dim]")
    out.print("[dim]                              nl-router-dispatcher \\\\[/dim]")
    out.print("[dim]                              nl-router-cleaner[/dim]")

    # Token is printed exactly once, last. We deliberately do NOT include
    # it inside the `Next steps` block — operators sometimes copy/paste
    # that block into shell history and we don't want the literal token
    # to land there. Operators set their own env var separately.
    if raw_token is not None:
        out.print("")
        out.print("[bold yellow]Bootstrap admin token — save now, not shown again:[/bold yellow]")
        out.print(f"  [bold green]{raw_token}[/bold green]")


# ---- Helpers --------------------------------------------------------------


def _ensure_kek(kek_path: Path) -> None:
    """Generate a fresh 32-byte KEK at kek_path if it doesn't already exist.

    The file is base64url-encoded so it's safe to copy/paste between
    environments. We chown to the nl-router user when running as root
    (the typical install path); on dev machines the chown is a no-op
    if the user doesn't exist.
    """
    if kek_path.exists():
        out.print(f"  KEK already present: [green]{kek_path}[/green]")
        return

    # Make sure the parent dir exists. The package's postinstall creates
    # /etc/nl-router, but if the operator pointed kek_file elsewhere they
    # may not have created it.
    kek_path.parent.mkdir(parents=True, exist_ok=True)

    # 32 raw bytes, base64url-encoded for transport-safety. The crypto
    # loader (crypto.py:load_kek) accepts either form.
    raw = secrets.token_bytes(32)
    encoded = base64.urlsafe_b64encode(raw).decode("ascii")

    # Atomic write: create a sibling temp file, fsync it, then rename
    # over the target. A crash mid-write would otherwise leave a
    # truncated file that *might* base64-decode to a valid-looking but
    # low-entropy KEK. The rename is atomic on POSIX filesystems.
    tmp_path = kek_path.with_suffix(kek_path.suffix + ".tmp")
    fd = os.open(str(tmp_path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
    try:
        os.write(fd, encoded.encode("ascii") + b"\n")
        os.fsync(fd)
    finally:
        os.close(fd)

    # chown to nl-router before the rename so the target file shows up
    # with the right ownership in one step. When running as root, refuse
    # to proceed if the nl-router user doesn't exist — the service
    # account is created by the .deb's preinstall, so its absence means
    # we're on a misconfigured production host where the daemons won't
    # be able to read the KEK we just wrote. On a non-root dev install
    # the current user keeps ownership.
    if os.geteuid() == 0:
        try:
            entry = pwd.getpwnam("nl-router")
        except KeyError:
            # Clean up the temp file before failing.
            tmp_path.unlink(missing_ok=True)
            die(
                "Running as root, but the 'nl-router' system user does not "
                "exist. The package's postinstall creates this user; if you "
                "got here without installing via .deb/.rpm/tarball, create "
                "the user manually:\n"
                "  groupadd --system nl-router\n"
                "  useradd --system --gid nl-router --no-create-home "
                "--shell /usr/sbin/nologin nl-router",
                code=2,
            )
        os.chown(tmp_path, entry.pw_uid, entry.pw_gid)

    os.rename(tmp_path, kek_path)

    # Fsync the parent directory so the rename is durable.
    dir_fd = os.open(str(kek_path.parent), os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)

    out.print(f"  [green]→ KEK generated:[/green] {kek_path} [dim](0400)[/dim]")


def _ensure_env_file(cfg) -> None:                  # type: ignore[no-untyped-def]
    """Write /etc/nl-router/env from config.toml values.

    Every nl-router daemon's systemd unit sources this file. Pre-M22 the
    file didn't exist and the daemons crashed with "required env var not
    set: NL_ROUTER_SERVER_ID" until operators hand-wrote it. Now `init`
    produces it from the canonical bootstrap config.

    The file is **rewritten on every init** — running `init` after
    editing config.toml is the documented way to sync env to the new
    values. We deliberately don't try to merge with operator hand-edits
    of this file; per-service `*.env` files are the override path.
    """
    env_path = Path("/etc/nl-router/env")
    env_path.parent.mkdir(parents=True, exist_ok=True)

    # Values mirror what the C++ binaries actually read. Keep the list
    # narrow — anything not required (TLS knobs, batch sizes, etc.) has
    # a sensible default baked into each daemon's config loader and
    # operators tune them via per-service .env overrides.
    lines = [
        "# Generated by `nl-router init` from /etc/nl-router/config.toml.",
        "# Hand edits are overwritten on the next `nl-router init` run.",
        "# For per-service tuning use /etc/nl-router/<service>.env which",
        "# sources second and overrides values from this file.",
        f"NL_ROUTER_SERVER_ID={cfg.server_id}",
        f"NL_ROUTER_DATABASE_URL={cfg.database_url}",
        f"NL_ROUTER_LOG_LEVEL={cfg.log_level}",
    ]
    content = "\n".join(lines) + "\n"

    # Atomic write: temp file → fsync → rename. Same pattern as KEK.
    tmp_path = env_path.with_suffix(env_path.suffix + ".tmp")
    fd = os.open(str(tmp_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o640)
    try:
        os.write(fd, content.encode("utf-8"))
        os.fsync(fd)
    finally:
        os.close(fd)

    # Group ownership to nl-router so the daemons can read it. World-
    # readable is intentionally avoided — the DSN here includes the
    # Postgres password.
    if os.geteuid() == 0:
        try:
            entry = pwd.getpwnam("nl-router")
            os.chown(tmp_path, entry.pw_uid, entry.pw_gid)
        except KeyError:
            # Same failure mode as KEK gen — don't silently produce a
            # file the daemons can't read.
            tmp_path.unlink(missing_ok=True)
            die(
                "Running as root, but the 'nl-router' system user does not "
                "exist. Cannot write env file with safe ownership.",
                code=2,
            )

    os.rename(tmp_path, env_path)
    out.print(f"  [green]→ env file written:[/green] {env_path} [dim](0640)[/dim]")


def _ensure_module_env_files() -> None:
    """Write /etc/nl-router/module-<kind>.env for each shipped module.

    Each file sets NL_ROUTER_METRICS_PORT to the kind's canonical
    assignment from MODULE_METRICS_PORTS. The systemd template unit
    (`nl-router-module@.service`) sources these on each worker start.

    The receiver of this auto-generation is Prometheus, not the worker
    itself — the worker runs fine without a metrics port (it just won't
    expose `/metrics`). But the Prometheus scrape jobs in
    `monitoring/prometheus.yml` assume the canonical ports, so this is
    what keeps the per-module-kind Grafana panels populated.

    Operators with custom module kinds skip this step (via
    --skip-module-env) and hand-write their own port assignments.
    """
    if not MODULE_METRICS_PORTS:
        return
    out.print(f"  → module env files for [cyan]{len(MODULE_METRICS_PORTS)}[/cyan] kind(s):")

    try:
        nl_router_uid_gid = pwd.getpwnam("nl-router")
    except KeyError:
        nl_router_uid_gid = None

    for kind, port in MODULE_METRICS_PORTS.items():
        env_path = Path(f"/etc/nl-router/module-{kind}.env")
        env_path.parent.mkdir(parents=True, exist_ok=True)

        lines = [
            f"# Generated by `nl-router init` (M26).",
            f"# Metrics port assignment for the {kind!r} module-worker.",
            f"# Prometheus's scrape config expects this exact port; if you",
            f"# change it, update monitoring/prometheus.yml to match.",
            f"NL_ROUTER_METRICS_PORT={port}",
        ]
        content = "\n".join(lines) + "\n"

        # Atomic write — same temp+rename pattern as the other init files.
        tmp_path = env_path.with_suffix(env_path.suffix + ".tmp")
        fd = os.open(str(tmp_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o640)
        try:
            os.write(fd, content.encode("utf-8"))
            os.fsync(fd)
        finally:
            os.close(fd)

        if os.geteuid() == 0 and nl_router_uid_gid is not None:
            os.chown(tmp_path, nl_router_uid_gid.pw_uid, nl_router_uid_gid.pw_gid)

        os.rename(tmp_path, env_path)
        out.print(f"    [green]✓[/green] {env_path.name} → port {port}")


def _run_migrate_up() -> None:
    """Run `nl-router migrate up` as a subprocess.

    We shell out rather than importing the migrate module so the
    operator's stdout sees the same output they'd see running
    `nl-router migrate` directly — same binary, same logs, same
    error surface.
    """
    nl_router_cli = shutil.which("nl-router") or "nl-router"
    rc = subprocess.run(
        [nl_router_cli, "migrate", "up"],
        check=False,
    ).returncode
    if rc != 0:
        die(f"`nl-router migrate up` exited {rc}; fix the underlying error and rerun init.",
            code=rc or 2)


def _mint_bootstrap_token(*, token_name: str, force: bool) -> str | None:
    """Mint an admin token. Returns the raw token, or None if skipped.

    Skips (returns None) if an active token already exists and --force
    wasn't passed.
    """
    from nl_router.api.auth import ROLE_PERMISSIONS, mint_token

    with connect() as conn, conn.cursor() as cur:
        cur.execute("SELECT count(*) AS n FROM api_tokens WHERE revoked = FALSE")
        existing = cur.fetchone()["n"]
        if existing > 0 and not force:
            out.print(
                f"  [yellow]→ {existing} active token(s) already exist; "
                f"skipping mint (use --force to add another).[/yellow]"
            )
            return None

        raw_token, token_hash = mint_token()
        admin_perms = ROLE_PERMISSIONS["admin"]
        cur.execute(
            """
            INSERT INTO api_tokens (name, token_hash, permissions)
            VALUES (%s, %s, %s::jsonb)
            RETURNING id
            """,
            (token_name, token_hash, json.dumps(admin_perms)),
        )
        token_id = cur.fetchone()["id"]
        conn.commit()

    out.print(f"  [green]→ admin token minted:[/green] id={token_id} name={token_name}")
    return raw_token


def _redact_dsn(dsn: str) -> str:
    """Hide the password in a libpq DSN before logging.

    libpq accepts both URI form (postgres://user:pass@host/db) and
    key=value form (user=foo password=bar host=baz). We handle the URI
    form here; key=value DSNs are uncommon in this codebase.
    """
    import re

    return re.sub(r"(postgres(?:ql)?://[^:]+:)([^@]+)(@)", r"\1***\3", dsn)
