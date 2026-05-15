"""`nl-router destination` — manage outbound destinations (read-only in this slice).

Create/update/delete + connection-test land alongside the FastAPI service
layer in M4, where credential decryption and per-kind validation live.
"""

from __future__ import annotations

import json
from typing import Annotated

import typer

from nl_router.cli._common import die, out, render_table
from nl_router.db import connect

app = typer.Typer(no_args_is_help=True, help="Manage outbound destinations.")


@app.command("list")
def list_(
    kind: Annotated[
        str | None,
        typer.Option("--kind", help="Filter by kind (dicom|dicomweb_stow|gcp_dicomweb|http|file|object_storage|...)."),
    ] = None,
    enabled_only: Annotated[
        bool,
        typer.Option("--enabled-only", help="Hide disabled destinations."),
    ] = False,
) -> None:
    """List destinations."""
    sql = (
        "SELECT id, name, kind, enabled, dispatch_concurrency, "
        "       credential_id IS NOT NULL AS has_credential "
        "FROM destinations"
    )
    where: list[str] = []
    params: list[object] = []
    if kind:
        where.append("kind = %s")
        params.append(kind)
    if enabled_only:
        where.append("enabled = TRUE")
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY name"

    with connect() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        rows = cur.fetchall()

    render_table(
        "Destinations",
        ["id", "name", "kind", "enabled", "concurrency", "has_cred"],
        [
            (r["id"], r["name"], r["kind"], r["enabled"], r["dispatch_concurrency"],
             r["has_credential"])
            for r in rows
        ],
        empty_message="(no destinations defined)",
    )


@app.command("show")
def show(name: Annotated[str, typer.Argument(help="Destination name.")]) -> None:
    """Show one destination with full config (credential payload remains opaque)."""
    with connect() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT d.*, c.kind AS credential_kind, c.name AS credential_name
              FROM destinations d
              LEFT JOIN credentials c ON c.id = d.credential_id
             WHERE d.name = %s
            """,
            (name,),
        )
        row = cur.fetchone()
        if row is None:
            die(f"No destination named {name!r}.")

    out.rule(f"Destination: {row['name']}")
    out.print(f"  id:                   {row['id']}")
    out.print(f"  kind:                 [cyan]{row['kind']}[/cyan]")
    out.print(f"  enabled:              {row['enabled']}")
    out.print(f"  dispatch_concurrency: {row['dispatch_concurrency']}")
    if row["description"]:
        out.print(f"  description:          {row['description']}")
    if row["credential_id"] is not None:
        out.print(
            f"  credential:           [green]{row['credential_name']}[/green] "
            f"(kind={row['credential_kind']})  [dim]<payload encrypted at rest>[/dim]"
        )
    else:
        out.print("  credential:           [dim]none[/dim]")

    out.print("\n  config:")
    out.print(_pretty_json(row["config"]))

    out.print("\n  retry_policy:")
    out.print(_pretty_json(row["retry_policy"]))


def _pretty_json(value: object) -> str:
    """Render a JSONB value (already deserialized by psycopg) as indented JSON."""
    return json.dumps(value, indent=2, sort_keys=True)
