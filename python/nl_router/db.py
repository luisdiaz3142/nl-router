"""Database connection helpers.

Two flavors:
    * `connect()` - one-shot sync connection for CLI commands.
    * `pool()` - shared sync connection pool for the API process.

The API process also opens a separate, un-pooled connection for LISTEN/NOTIFY
(see `notify.py` in M4). Notifications are connection-scoped and don't survive
PgBouncer's transaction-mode pooling.
"""

from __future__ import annotations

from contextlib import contextmanager
from functools import lru_cache
from typing import Iterator

import psycopg
from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool

from nl_router.config import load


def _conninfo() -> str:
    """Return the DSN string from bootstrap config.

    Always use this rather than reading the env directly so the same precedence
    rules apply in CLI, API, and tests.
    """
    return load().database_url


@contextmanager
def connect() -> Iterator[psycopg.Connection]:
    """Open a single sync connection, yield it, and close on exit.

    CLI commands use this rather than the pool because they're short-lived;
    pool overhead is wasted for one-shot operations.
    """
    conn = psycopg.connect(_conninfo(), autocommit=False, row_factory=dict_row)
    try:
        yield conn
    finally:
        conn.close()


@lru_cache(maxsize=1)
def pool() -> ConnectionPool:
    """Return the process-wide connection pool.

    Sized for typical management API workloads; tune via system_config or
    config.toml in production. Lazy-initialized on first call.
    """
    return ConnectionPool(
        conninfo=_conninfo(),
        min_size=2,
        max_size=16,
        kwargs={"row_factory": dict_row, "autocommit": False},
        open=True,
    )


def schema_version() -> int | None:
    """Return the current migrations version, or None if no migrations applied.

    The `schema_migrations` table is owned and managed by golang-migrate; we
    only read from it. Returns `None` if the table doesn't exist yet (fresh DB).
    """
    with connect() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT to_regclass('public.schema_migrations') IS NOT NULL AS exists"
        )
        row = cur.fetchone()
        if not row or not row["exists"]:
            return None
        cur.execute("SELECT version, dirty FROM schema_migrations LIMIT 1")
        row = cur.fetchone()
        if row is None:
            return None
        if row["dirty"]:
            raise RuntimeError(
                f"Database migrations are in a 'dirty' state at version "
                f"{row['version']}. Manual recovery required: see "
                f"https://github.com/golang-migrate/migrate/blob/master/MIGRATIONS.md"
            )
        return int(row["version"])
