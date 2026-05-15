"""`nl-router status` — local node health snapshot.

Shows what the CLI can see at a glance:
    * Schema migration version
    * Queue depth per phase (overall, plus this node's share)
    * Top recent errors
    * Active cleanup holds
    * Worker liveness (claimed rows whose lease has not expired)

This is intentionally cheap — a handful of indexed COUNT(*)s. Operators run
it ad-hoc and from CI smoke tests.
"""

from __future__ import annotations

from typing import Annotated

import typer

from nl_router.cli._common import die, out, render_table
from nl_router.config import load
from nl_router.db import connect, schema_version


def status(
    server_id: Annotated[
        str | None,
        typer.Option(
            "--server-id",
            help="Show per-server slices for this server_id (defaults to bootstrap config).",
        ),
    ] = None,
    all_servers: Annotated[
        bool,
        typer.Option("--all", help="Show queue depth across all servers, not just one."),
    ] = False,
) -> None:
    """Show a snapshot of database and queue state."""
    cfg = load()
    sid = server_id or cfg.server_id

    out.rule(f"nl-router status ([cyan]{sid}[/cyan])")

    try:
        version = schema_version()
    except RuntimeError as exc:
        die(str(exc))

    if version is None:
        die(
            "Schema is not initialized. Run `nl-router migrate` first.",
            code=2,
        )
    out.print(f"  Schema version:   [green]{version}[/green]")
    out.print(f"  DSN:              [dim]{_redact_dsn(cfg.database_url)}[/dim]")
    out.print(f"  Server ID:        [cyan]{sid}[/cyan]")
    out.print("")

    with connect() as conn, conn.cursor() as cur:
        # Queue depth per phase. Filter by server_id unless --all.
        where = "" if all_servers else "WHERE server_id = %s"
        params: tuple = () if all_servers else (sid,)
        cur.execute(
            f"""
            SELECT status, count(*) AS n
              FROM work_queue
              {where}
             GROUP BY status
             ORDER BY status
            """,
            params,
        )
        rows = cur.fetchall()
        render_table(
            "Work queue depth" + (" (all servers)" if all_servers else f" ({sid})"),
            ["status", "count"],
            [(r["status"], r["n"]) for r in rows],
            empty_message="(no rows in work_queue)",
        )

        # Cleanup holds.
        cur.execute(
            """
            SELECT count(*) AS n FROM work_queue WHERE cleanup_hold = TRUE
            """
        )
        hold_count = cur.fetchone()["n"]
        out.print(f"\n  Active cleanup holds: [yellow]{hold_count}[/yellow]")

        # Worker leases that haven't expired (proxy for "workers are alive").
        cur.execute(
            """
            SELECT count(*) AS n FROM work_queue
             WHERE claimed_by IS NOT NULL AND claim_expires_at > now()
            """
        )
        active_claims = cur.fetchone()["n"]
        out.print(f"  Active worker leases: [green]{active_claims}[/green]")

        # Top recent errors (last 24h).
        cur.execute(
            """
            SELECT failed_phase, last_error, count(*) AS n
              FROM work_queue
             WHERE status = 'failed'
               AND received_at > now() - interval '24 hours'
             GROUP BY failed_phase, last_error
             ORDER BY n DESC
             LIMIT 5
            """
        )
        recent_errors = cur.fetchall()
        if recent_errors:
            out.print("")
            render_table(
                "Recent failures (24h)",
                ["phase", "error", "count"],
                [(r["failed_phase"], r["last_error"], r["n"]) for r in recent_errors],
            )


def _redact_dsn(dsn: str) -> str:
    """Hide the password in DSN strings for display."""
    if "@" not in dsn or "://" not in dsn:
        return dsn
    scheme, rest = dsn.split("://", 1)
    creds, host = rest.split("@", 1)
    if ":" in creds:
        user, _ = creds.split(":", 1)
        return f"{scheme}://{user}:****@{host}"
    return dsn
