"""`nl-router migrate` — shells out to the bundled golang-migrate binary.

The design plan calls for one migration runner (golang-migrate) used everywhere
— production deploys, CI, and dev. This command is a thin wrapper that:

    * Finds the bundled `nl-router-migrate` binary (or system `migrate`).
    * Reads the DSN from bootstrap config (or --database overrides).
    * Locates the migrations directory (--migrations-dir or package data).
    * Forwards the requested action (up / down / version / force) and exits
      with the same code as the binary.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Annotated

import typer

from nl_router.cli._common import die, out
from nl_router.config import load


def migrate(
    action: Annotated[
        str,
        typer.Argument(
            help="Action: up | down | version | force <V> | drop. "
                 "`up` applies all pending migrations; `down N` rolls back N."
        ),
    ] = "up",
    n: Annotated[
        int | None,
        typer.Option("--n", "-n", help="Number of migrations for `down` (default: 1)."),
    ] = None,
    database: Annotated[
        str | None,
        typer.Option("--database", help="Override DSN; defaults to bootstrap config."),
    ] = None,
    migrations_dir: Annotated[
        Path | None,
        typer.Option("--migrations-dir", help="Override migrations directory."),
    ] = None,
    version: Annotated[
        int | None,
        typer.Option("--version", help="Version for `force` action."),
    ] = None,
) -> None:
    """Run database migrations via the bundled golang-migrate tool.

    Examples:
        nl-router migrate                      # apply all up migrations
        nl-router migrate down -n 1            # roll back the most recent migration
        nl-router migrate version              # show current schema version
        nl-router migrate force --version 7    # mark version 7 as clean (recovery)
    """
    binary = _find_migrate_binary()
    if binary is None:
        die(
            "Could not find a migration binary. Looked for `nl-router-migrate` "
            "and `migrate` on PATH. Install golang-migrate or set "
            "NL_ROUTER_MIGRATE_BIN to the full path."
        )

    dsn = database or load().database_url
    migrations_path = migrations_dir or _find_migrations_dir()
    if migrations_path is None or not migrations_path.exists():
        die(
            "Could not locate migrations directory. Pass --migrations-dir or "
            "install nl-router so /usr/share/nl-router/migrations is populated."
        )

    cmd: list[str] = [str(binary), "-path", str(migrations_path), "-database", dsn]

    if action == "up":
        cmd.append("up")
    elif action == "down":
        cmd.extend(["down", str(n if n is not None else 1)])
    elif action == "version":
        cmd.append("version")
    elif action == "drop":
        cmd.extend(["drop", "-f"])
    elif action == "force":
        if version is None:
            die("`migrate force` requires --version <N>.")
        cmd.extend(["force", str(version)])
    else:
        die(f"Unknown action: {action!r}. Use one of: up, down, version, force, drop.")

    out.print(f"[dim]$ {' '.join(cmd)}[/dim]")
    proc = subprocess.run(cmd, check=False)
    raise typer.Exit(code=proc.returncode)


def _find_migrate_binary() -> Path | None:
    """Locate the migration binary.

    Order:
        1. NL_ROUTER_MIGRATE_BIN env var (full path override).
        2. /usr/libexec/nl-router/nl-router-migrate (package install location).
        3. /usr/bin/nl-router-migrate (alt package location).
        4. `migrate` on PATH (dev environments with `brew install golang-migrate`).
    """
    if env := os.environ.get("NL_ROUTER_MIGRATE_BIN"):
        path = Path(env)
        if path.exists():
            return path

    for candidate in (
        Path("/usr/libexec/nl-router/nl-router-migrate"),
        Path("/usr/bin/nl-router-migrate"),
    ):
        if candidate.exists():
            return candidate

    if which := shutil.which("migrate"):
        return Path(which)

    return None


def _find_migrations_dir() -> Path | None:
    """Locate the migrations directory.

    Order:
        1. NL_ROUTER_MIGRATIONS_DIR env var.
        2. /usr/share/nl-router/migrations (package install location).
        3. ../../migrations relative to this file (dev checkout).
    """
    if env := os.environ.get("NL_ROUTER_MIGRATIONS_DIR"):
        return Path(env)

    package_share = Path("/usr/share/nl-router/migrations")
    if package_share.exists():
        return package_share

    # Dev checkout: python/nl_router/cli/migrate.py → repo root → migrations/
    dev_path = Path(__file__).resolve().parents[3] / "migrations"
    if dev_path.exists():
        return dev_path

    return None
