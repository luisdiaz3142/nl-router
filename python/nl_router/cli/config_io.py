"""`nl-router config-export` / `config-import` — portable config snapshots.

Exports the **configuration** tables (rules, destinations, processing_modules,
rule_destinations, rule_processing_chain, credentials, system_config) as a
self-contained JSON document operators can move between environments.

Credential payloads stay encrypted — we export `enc_version`, `nonce`, and
`ciphertext` verbatim. Imports into a different deployment require the same
KEK to decrypt them. This is by design.

Work-queue / route_assignments / processing_jobs / admin_audit are NOT
exported — they're runtime state, not configuration.
"""

from __future__ import annotations

import base64
import json
import sys
from datetime import datetime, date, time
from decimal import Decimal
from pathlib import Path
from typing import Annotated, Any

import typer

from nl_router import __version__
from nl_router.cli._common import die, out
from nl_router.db import connect, schema_version

# Tables exported, in dependency order (parents first; importers reapply this order).
# Maps table name → ORDER BY column. Most tables have an `id` BIGSERIAL PK;
# system_config is keyed on `key`.
EXPORT_TABLES: dict[str, str] = {
    "credentials": "id",
    "destinations": "id",
    "rules": "id",
    "rule_destinations": "id",
    "processing_modules": "id",
    "rule_processing_chain": "id",
    "system_config": "key",
}


def export_(
    output: Annotated[
        Path | None,
        typer.Option("--output", "-o", help="Output file path (default: stdout)."),
    ] = None,
) -> None:
    """Export configuration tables to a JSON document.

    The output is a single JSON object:
        {
          "nl_router_export_version": 1,
          "nl_router_version": "0.0.1",
          "schema_version": 8,
          "exported_at": "2024-...",
          "tables": {
            "credentials": [ {...row...}, ... ],
            ...
          }
        }
    """
    version = schema_version()
    if version is None:
        die("Schema not initialized; run `nl-router migrate` first.", code=2)

    payload: dict[str, Any] = {
        "nl_router_export_version": 1,
        "nl_router_version": __version__,
        "schema_version": version,
        "exported_at": datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "tables": {},
    }

    with connect() as conn, conn.cursor() as cur:
        for table, order_col in EXPORT_TABLES.items():
            # Table and column names come from a static map controlled by us,
            # not from user input, so f-string interpolation here is safe.
            cur.execute(f"SELECT * FROM {table} ORDER BY {order_col}")
            payload["tables"][table] = [_serialize_row(r) for r in cur.fetchall()]

    text = json.dumps(payload, indent=2)
    if output is None:
        sys.stdout.write(text + "\n")
    else:
        output.write_text(text + "\n")
        out.print(f"[green]wrote[/green] {output}")


def import_(
    source: Annotated[
        Path,
        typer.Argument(help="Path to a config-export JSON file."),
    ],
    apply: Annotated[
        bool,
        typer.Option(
            "--apply",
            help="Actually apply the import. Without --apply, runs in dry-run mode "
                 "and prints what would change without writing.",
        ),
    ] = False,
) -> None:
    """Import configuration tables from a config-export JSON document.

    Dry-run by default. Pass `--apply` to write to the database.

    Conflict handling: rows are upserted by name where the table has a unique
    name column (rules, destinations, processing_modules, credentials), and
    by primary key otherwise. system_config keys are upserted by key.
    """
    if not source.exists():
        die(f"No such file: {source}")
    payload = json.loads(source.read_text())

    if payload.get("nl_router_export_version") != 1:
        die(
            f"Unsupported export version: {payload.get('nl_router_export_version')}. "
            "This version of nl-router understands export_version=1."
        )

    mode = "[yellow]DRY RUN[/yellow]" if not apply else "[green]APPLYING[/green]"
    out.print(f"{mode}  import from {source}")
    out.print(
        f"  source schema_version={payload.get('schema_version')!r}  "
        f"nl_router_version={payload.get('nl_router_version')!r}"
    )

    target_version = schema_version()
    if target_version is None:
        die("Target schema not initialized; run `nl-router migrate` first.", code=2)
    if target_version != payload.get("schema_version"):
        out.print(
            f"  [yellow]warning[/yellow]: target schema_version={target_version} "
            f"differs from source schema_version={payload.get('schema_version')}. "
            "Import may fail or produce stale data."
        )

    tables = payload.get("tables", {})
    for table in EXPORT_TABLES:
        rows = tables.get(table, [])
        out.print(f"  {table}: [cyan]{len(rows)}[/cyan] row(s)")

    if not apply:
        out.print("\n[dim]Dry run complete; re-run with --apply to write.[/dim]")
        return

    die("import --apply is not yet implemented in this scaffold. "
        "Will land alongside the FastAPI service-layer where validation and "
        "admin_audit emission happen.")


# ---- serialization helpers -----------------------------------------------

def _serialize_row(row: dict[str, Any]) -> dict[str, Any]:
    """Convert a psycopg dict row into JSON-safe values."""
    return {k: _serialize_value(v) for k, v in row.items()}


def _serialize_value(v: Any) -> Any:
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    if isinstance(v, (datetime, date, time)):
        return v.isoformat()
    if isinstance(v, Decimal):
        # Preserve precision as string; importer can parse.
        return str(v)
    if isinstance(v, (bytes, bytearray, memoryview)):
        # Credential ciphertext / nonce. Base64url with no padding for compact JSON.
        b = bytes(v)
        return {"__bytes_b64__": base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")}
    if isinstance(v, dict):
        return {k: _serialize_value(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_serialize_value(x) for x in v]
    # Fallback: stringify (e.g. uuid, network types).
    return str(v)
