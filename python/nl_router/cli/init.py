"""`nl-router init` — first-run setup.

Verifies the schema is at the expected version (operator should have run
`nl-router migrate` first; we don't auto-migrate by default — that's an
explicit decision per the design plan) and mints an initial admin API
token. The raw token is printed to stdout exactly once; the DB stores
only its SHA-256 hash.

Future expansion (deferred): create a local admin user with password,
seed initial OIDC config.
"""

from __future__ import annotations

import json
from typing import Annotated

import typer

from nl_router.cli._common import die, out
from nl_router.db import connect, schema_version


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
) -> None:
    """One-shot setup. Prints a fresh admin API token to stdout."""
    # Lazy import so the regular CLI doesn't drag in the auth module.
    from nl_router.api.auth import ROLE_PERMISSIONS, mint_token

    version = schema_version()
    if version is None:
        die(
            "Schema is not initialized. Run `nl-router migrate` first.",
            code=2,
        )
    out.print(f"  schema version: [green]{version}[/green]")

    with connect() as conn, conn.cursor() as cur:
        cur.execute("SELECT count(*) AS n FROM api_tokens WHERE revoked = FALSE")
        existing = cur.fetchone()["n"]
        if existing > 0 and not force:
            die(
                f"{existing} active token(s) already exist. Use --force to mint another, "
                f"or revoke existing tokens first via the API.",
                code=2,
            )

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

    out.rule("[green]bootstrap admin token created[/green]")
    out.print(f"  token id    : [cyan]{token_id}[/cyan]")
    out.print(f"  token name  : [cyan]{token_name}[/cyan]")
    out.print(f"  permissions : [dim]admin role ({len(admin_perms)} perms)[/dim]")
    out.print("")
    out.print("[bold yellow]Save this token NOW — it will not be shown again:[/bold yellow]")
    out.print(f"  [bold green]{raw_token}[/bold green]")
    out.print("")
    out.print("[dim]Next steps:[/dim]")
    out.print("  [dim]export NL_ROUTER_TOKEN=" + raw_token + "[/dim]")
    out.print("  [dim]nl-router serve   # then curl with Authorization: Bearer $NL_ROUTER_TOKEN[/dim]")
