"""`nl-router rule` — manage routing rules.

Read-only commands (list, show) land in this scaffold; create/update/delete
land alongside the FastAPI service-layer in M4 since they need DSL validation,
admin_audit emission, and RBAC enforcement. Operators can still seed rules
via SQL or the API in the meantime.
"""

from __future__ import annotations

from typing import Annotated

import typer

from nl_router.cli._common import die, out, render_table
from nl_router.db import connect

app = typer.Typer(no_args_is_help=True, help="Manage routing rules.")


@app.command("list")
def list_(
    status_filter: Annotated[
        str | None,
        typer.Option("--status", help="Filter by lifecycle status (draft|disabled|enabled)."),
    ] = None,
    scope: Annotated[
        str | None,
        typer.Option("--scope", help="Filter by scope (study|series)."),
    ] = None,
) -> None:
    """List routing rules."""
    sql = (
        "SELECT id, name, status, scope, priority, "
        "       (SELECT count(*) FROM rule_destinations rd WHERE rd.rule_id = r.id) AS destinations, "
        "       (SELECT count(*) FROM rule_processing_chain rpc WHERE rpc.rule_id = r.id) AS chain_len "
        "FROM rules r"
    )
    where: list[str] = []
    params: list[object] = []
    if status_filter:
        where.append("status = %s")
        params.append(status_filter)
    if scope:
        where.append("scope = %s")
        params.append(scope)
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY priority DESC, name"

    with connect() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        rows = cur.fetchall()

    render_table(
        "Rules",
        ["id", "name", "status", "scope", "priority", "destinations", "chain_len"],
        [
            (r["id"], r["name"], r["status"], r["scope"], r["priority"],
             r["destinations"], r["chain_len"])
            for r in rows
        ],
        empty_message="(no rules defined)",
    )


@app.command("show")
def show(name: Annotated[str, typer.Argument(help="Rule name.")]) -> None:
    """Show one rule including its predicate, processing chain, and destinations."""
    with connect() as conn, conn.cursor() as cur:
        cur.execute("SELECT * FROM rules WHERE name = %s", (name,))
        rule = cur.fetchone()
        if rule is None:
            die(f"No rule named {name!r}.")

        cur.execute(
            """
            SELECT pm.name AS module_name, pm.kind, rpc.ordinal, rpc.config_override
              FROM rule_processing_chain rpc
              JOIN processing_modules pm ON pm.id = rpc.module_id
             WHERE rpc.rule_id = %s
             ORDER BY rpc.ordinal
            """,
            (rule["id"],),
        )
        chain = cur.fetchall()

        cur.execute(
            """
            SELECT d.name, d.kind, d.enabled, rd.ordinal, rd.retry_policy_override
              FROM rule_destinations rd
              JOIN destinations d ON d.id = rd.destination_id
             WHERE rd.rule_id = %s
             ORDER BY rd.ordinal, d.name
            """,
            (rule["id"],),
        )
        destinations = cur.fetchall()

    out.rule(f"Rule: {rule['name']}")
    out.print(f"  id:        {rule['id']}")
    out.print(f"  status:    [cyan]{rule['status']}[/cyan]")
    out.print(f"  scope:     {rule['scope']}")
    out.print(f"  priority:  {rule['priority']}")
    out.print(f"  dispatch_order: {rule['dispatch_order']}")
    out.print(f"  predicate:\n    [yellow]{rule['predicate']}[/yellow]")
    if rule["description"]:
        out.print(f"  description: {rule['description']}")
    out.print("")

    render_table(
        "Processing chain",
        ["ordinal", "module", "kind", "config_override"],
        [(c["ordinal"], c["module_name"], c["kind"], c["config_override"]) for c in chain],
        empty_message="(no processing modules — routes straight to dispatch)",
    )

    out.print("")
    render_table(
        "Destinations",
        ["ordinal", "name", "kind", "enabled", "retry_override"],
        [
            (d["ordinal"], d["name"], d["kind"], d["enabled"], d["retry_policy_override"])
            for d in destinations
        ],
        empty_message="(no destinations bound — rule will match but route nowhere)",
    )
