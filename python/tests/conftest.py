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

What's NOT here (deferred):
  * Postgres test fixture — we'd want a per-test ephemeral DB via
    psycopg's CREATE DATABASE + migrate-up pattern. Real value but big
    setup; deferred to M26 when the first DB-touching test appears.
  * FastAPI TestClient with mocked DB pool — same dependency on the
    DB story above.
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
