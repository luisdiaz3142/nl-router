"""Shared pytest fixtures for the nl-router Python test suite.

What's here:
  * `dsl_validate_bin`     — path to the cpp/build artifact when present;
                              tests that need it skip otherwise so the
                              suite runs without a C++ build.
  * `test_kek_env`         — sets `NL_ROUTER_KEK` to a deterministic
                              32-byte test key and clears the
                              `nl_router.config.load_kek` cache. Lets
                              crypto tests round-trip without poking
                              /etc/nl-router/kek.key.
  * `unset_db_env`         — drops every NL_ROUTER_* / DATABASE_URL env
                              var that could otherwise leak into a test
                              and trigger a real Postgres connect.

DB-touching fixtures (M32):
  * `test_dsn`             — session-scoped DSN string. Honors
                              NL_ROUTER_TEST_DSN env override; otherwise
                              falls back to the docker-compose dev DSN.
  * `test_db`              — session-scoped Postgres sanity check.
                              Skips every DB-touching test if Postgres
                              isn't reachable or migrations haven't run.
  * `db`                   — per-test fixture that truncates mutable
                              tables before each test. Built-in seed
                              tables (processing_modules, system_config)
                              are preserved.
  * `api_client`           — FastAPI TestClient pointed at the test DB.
                              Overrides env vars + clears pool/config
                              caches so create_app() resolves to the
                              test DSN.
  * `admin_token`          — mints a real api_tokens row, yields the
                              raw Bearer token tests can send.
"""

from __future__ import annotations

import base64
import os
from pathlib import Path
from typing import Iterator

import pytest


# ---- nl-dsl-validate path discovery -------------------------------------

_HELPER_REL_PATH = "cpp/build/common/dsl/nl-dsl-validate"


def _repo_root() -> Path:
    """Walk up from this file looking for pyproject.toml."""
    here = Path(__file__).resolve()
    for parent in [here, *here.parents]:
        if (parent / "pyproject.toml").exists():
            return parent
    raise RuntimeError("pyproject.toml not found from " + str(here))


@pytest.fixture(scope="session")
def dsl_validate_bin() -> str | None:
    """Return the path to the locally-built nl-dsl-validate, or None.

    Tests that need a real DSL parse (test_models's syntax-error case)
    skip themselves when this is None — keeps the suite green for
    developers who haven't built the C++ side yet.
    """
    # Honor the override env var that the API code itself uses, so
    # `make test-py NL_ROUTER_DSL_VALIDATE_BIN=...` works.
    override = os.environ.get("NL_ROUTER_DSL_VALIDATE_BIN")
    if override and Path(override).is_file():
        return override

    canonical = _repo_root() / _HELPER_REL_PATH
    if canonical.is_file():
        return str(canonical)

    return None


# ---- KEK fixture --------------------------------------------------------

# 32 bytes of `\x01\x02\x03\x04...` repeated — deterministic so test
# ciphertexts are reproducible, distinct from a production KEK so a
# leak is obvious.
_TEST_KEK_BYTES = bytes(range(1, 33))
_TEST_KEK_BASE64URL = base64.urlsafe_b64encode(_TEST_KEK_BYTES).decode("ascii")


@pytest.fixture
def test_kek_env(monkeypatch) -> Iterator[bytes]:
    """Set NL_ROUTER_KEK to a deterministic test key for one test.

    Yields the 32 raw bytes so tests can compute expected ciphertexts
    if they need to. Crypto tests should just call encrypt()/decrypt()
    and roundtrip-check the plaintext.

    Why monkeypatch instead of patching `crypto._load_kek_safe()`
    directly: load_kek is `@cache`d, so a patch after the first call
    has no effect. Setting the env var before any crypto code runs is
    the robust path.
    """
    # Drop any KEK file path that might exist so the env var wins.
    # config.load_kek prefers file over env when both are set — that's
    # the right production precedence but wrong for tests, so we make
    # sure no file is configured.
    monkeypatch.setenv("NL_ROUTER_KEK", _TEST_KEK_BASE64URL)
    # Clear the @cache so the new env value is picked up.
    from nl_router import config as cfg_mod
    cfg_mod.load.cache_clear()
    cfg_mod.load_kek.cache_clear()
    yield _TEST_KEK_BYTES
    # Cache clear on teardown so the next test doesn't reuse this KEK.
    cfg_mod.load.cache_clear()
    cfg_mod.load_kek.cache_clear()


# ---- DB env scrubbing ---------------------------------------------------


@pytest.fixture
def unset_db_env(monkeypatch) -> None:
    """Strip every NL_ROUTER_* / DATABASE_URL env var.

    Some tests import code that, at first call, would otherwise try
    to open a real DB connection from the developer's shell env.
    Tests that need a DB should be in M26 with a real fixture; tests
    in M25 explicitly *don't* want a DB and should fail loudly if
    they accidentally try to talk to one.
    """
    for k in list(os.environ):
        if k.startswith("NL_ROUTER_") or k == "DATABASE_URL":
            monkeypatch.delenv(k, raising=False)


# =============================================================================
# DB-touching fixtures (M32)
# =============================================================================
#
# Strategy:
#   * Session-scoped `test_dsn` resolves where to talk. Honors
#     NL_ROUTER_TEST_DSN env override (CI sets this against a service
#     container); otherwise tries the docker-compose dev DSN.
#   * Session-scoped `test_db` does a fast reachability + schema check;
#     skips all DB-touching tests if Postgres is missing or migrations
#     haven't run. That keeps the unit-test suite (~50 cases) running
#     green on a fresh checkout without Postgres.
#   * Per-test `db` TRUNCATEs every mutable table before yielding, so
#     each test starts from a known empty state. Seed tables
#     (processing_modules, system_config) are intentionally preserved
#     since migrations populate them and route handlers expect them.
#   * `api_client` builds a fresh TestClient with the bootstrap config
#     pointed at the test DSN.

# The docker-compose dev DB exposed on host port 5432.
_DEV_DSN = "postgres://nl_router:nl_router@localhost:5432/nl_router?sslmode=disable"

# Tables that hold operator-supplied or run-time data — TRUNCATE these
# between tests. Order doesn't matter when CASCADE is used.
_MUTABLE_TABLES = [
    "admin_audit",
    "route_assignments",
    "processing_jobs",
    "work_queue",
    "rule_processing_chain",
    "rule_destinations",
    "rules",
    "destinations",
    "credentials",
    "api_tokens",
    "users",
    "login_attempts",
]

# Tables that migrations seed — leave them alone or built-in test
# scenarios that reference these seeds break.
_SEED_TABLES_TO_PRESERVE = {
    "processing_modules",   # 0009 seeds the built-in module catalog
    "system_config",        # 0014 (or similar) seeds default knobs
    "schema_migrations",    # owned by golang-migrate
}


@pytest.fixture(scope="session")
def test_dsn() -> str:
    """Pick a DSN for the test Postgres.

    Order:
      1. $NL_ROUTER_TEST_DSN (CI sets this to the service container)
      2. $DATABASE_URL  (some dev shells already have this exported)
      3. Docker-compose default
    """
    return (
        os.environ.get("NL_ROUTER_TEST_DSN")
        or os.environ.get("DATABASE_URL")
        or _DEV_DSN
    )


@pytest.fixture(scope="session")
def test_db(test_dsn: str) -> str:
    """Verify the test Postgres is reachable + schema is current.

    Skips the whole DB-touching test set on a fresh checkout that
    hasn't run `make db-up && make migrate`. The skip message names
    the fix.
    """
    try:
        import psycopg  # type: ignore[import-untyped]
    except ImportError:
        pytest.skip("psycopg not installed — pip install -e '.[api]'")

    try:
        with psycopg.connect(test_dsn, connect_timeout=2) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT to_regclass('public.schema_migrations') IS NOT NULL AS exists"
                )
                exists = cur.fetchone()
                if not exists or not exists[0]:
                    pytest.skip(
                        f"test DB at {test_dsn} has no schema_migrations table; "
                        f"run `make migrate` to apply schema"
                    )
                # And the work_queue table must exist (smoke check that
                # migrations actually ran, not just that the migrations
                # table is there).
                cur.execute("SELECT to_regclass('public.work_queue') IS NOT NULL")
                wq = cur.fetchone()
                if not wq or not wq[0]:
                    pytest.skip(
                        f"test DB at {test_dsn} missing work_queue table; "
                        f"run `make migrate`"
                    )
    except Exception as e:
        pytest.skip(
            f"test Postgres at {test_dsn} not reachable ({e.__class__.__name__}: {e}); "
            f"run `make db-up && make migrate` to enable DB-touching tests"
        )

    return test_dsn


@pytest.fixture
def db(test_db: str) -> str:
    """Per-test fixture: TRUNCATE mutable tables, yield the DSN.

    Truncate runs in a single autocommit statement; CASCADE handles
    foreign-key chains automatically. RESTART IDENTITY resets bigserial
    sequences so id=1 is reliable across tests.
    """
    import psycopg
    table_list = ", ".join(_MUTABLE_TABLES)
    with psycopg.connect(test_db, autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.execute(f"TRUNCATE TABLE {table_list} RESTART IDENTITY CASCADE")
    return test_db


@pytest.fixture
def api_client(db: str, monkeypatch):
    """FastAPI TestClient pointed at the test DB.

    Overrides bootstrap env so create_app() opens a pool against the
    test DSN. Clears the @lru_cache on `pool()` and `load()` so the
    next call rebuilds them with the new env. Sets metrics_port=0 so
    the Prometheus exposer doesn't try to bind a real port across
    parallel tests.
    """
    try:
        from fastapi.testclient import TestClient  # type: ignore[import-not-found]
    except ImportError:
        pytest.skip("fastapi not installed — pip install -e '.[api]'")

    monkeypatch.setenv("NL_ROUTER_SERVER_ID",    "test-server")
    monkeypatch.setenv("NL_ROUTER_DATABASE_URL", db)
    monkeypatch.setenv("NL_ROUTER_API_PORT",     "0")
    monkeypatch.setenv("NL_ROUTER_METRICS_PORT", "0")
    monkeypatch.setenv("NL_ROUTER_LOG_LEVEL",    "info")
    # The crypto layer reads a KEK on credential ops. Set a
    # deterministic one even if the current test doesn't touch crypto;
    # cheap to do and avoids "where's the KEK" surprises.
    import base64
    monkeypatch.setenv(
        "NL_ROUTER_KEK",
        base64.urlsafe_b64encode(bytes(range(1, 33))).decode("ascii"),
    )

    # Flush the caches that bake in env values.
    from nl_router import config as cfg_mod
    from nl_router import db as db_mod
    cfg_mod.load.cache_clear()
    cfg_mod.load_kek.cache_clear()
    db_mod.pool.cache_clear()

    from nl_router.api.app import create_app
    app = create_app()
    # TestClient context manager triggers the lifespan, which opens
    # the pool against the test DSN. Pool closes on exit.
    with TestClient(app) as client:
        yield client

    # Re-flush caches so the next test (or the next pytest run) builds
    # fresh against whatever env it's given.
    cfg_mod.load.cache_clear()
    cfg_mod.load_kek.cache_clear()
    db_mod.pool.cache_clear()


@pytest.fixture
def admin_token(db: str) -> str:
    """Mint a real api_tokens row, return the raw Bearer token.

    Uses the same mint_token + ROLE_PERMISSIONS the CLI's
    `nl-router init` does, so the resulting token has the exact
    permission set of a real bootstrap admin.
    """
    import json
    import psycopg
    from nl_router.api.auth import ROLE_PERMISSIONS, mint_token

    raw, hashed = mint_token()
    with psycopg.connect(db, autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                INSERT INTO api_tokens (name, token_hash, permissions)
                VALUES (%s, %s, %s::jsonb)
                """,
                ("test-admin", hashed, json.dumps(ROLE_PERMISSIONS["admin"])),
            )
    return raw
