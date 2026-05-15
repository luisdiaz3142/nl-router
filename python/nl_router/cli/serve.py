"""`nl-router serve` — run the FastAPI management API under Uvicorn."""

from __future__ import annotations

from typing import Annotated

import typer

from nl_router.cli._common import out
from nl_router.config import load


def serve(
    host: Annotated[
        str | None,
        typer.Option(
            help="Bind address. Defaults to config.api_bind / NL_ROUTER_API_BIND "
                 "(falling back to 127.0.0.1). Use 0.0.0.0 to listen externally."
        ),
    ] = None,
    port: Annotated[
        int | None,
        typer.Option(
            help="HTTP port. Defaults to config.api_port / NL_ROUTER_API_PORT.",
        ),
    ] = None,
    reload: Annotated[bool, typer.Option(help="Auto-reload on source changes (dev only).")] = False,
    workers: Annotated[int, typer.Option(help="Uvicorn worker count. >1 disables --reload.")] = 1,
    log_level: Annotated[
        str, typer.Option(help="Uvicorn log level: trace|debug|info|warning|error|critical.")
    ] = "info",
) -> None:
    """Run the FastAPI app under Uvicorn.

    Behind the scenes this just hands off to `uvicorn.run` — the app is
    `nl_router.api.app:app`. For production, run via systemd or a
    container; this command is intended for dev and small single-node
    deployments.
    """
    # Import lazily so importing the CLI doesn't drag in FastAPI for users
    # who just want migrate / status / rule list.
    try:
        import uvicorn  # noqa: F401
    except ImportError as e:
        raise typer.BadParameter(
            "FastAPI/Uvicorn not installed. Install with: pip install 'nl-router[api]'"
        ) from e

    cfg = load()
    # Explicit flags win; otherwise pull from the env-loaded config.
    # Final fallback is the function's old hardcoded defaults so callers
    # who never set anything still get sane behavior.
    effective_host = host if host is not None else cfg.api_bind
    effective_port = port if port is not None else cfg.api_port
    out.print(
        f"[green]starting[/green] nl-router API on [cyan]{effective_host}:{effective_port}[/cyan]\n"
        f"  server_id : [cyan]{cfg.server_id}[/cyan]\n"
        f"  database  : [dim]{_redact_dsn(cfg.database_url)}[/dim]"
    )

    import uvicorn
    uvicorn.run(
        "nl_router.api.app:app",
        host=effective_host,
        port=effective_port,
        reload=reload,
        workers=workers if not reload else 1,
        log_level=log_level,
        access_log=True,
    )


def _redact_dsn(dsn: str) -> str:
    if "@" not in dsn or "://" not in dsn:
        return dsn
    scheme, rest = dsn.split("://", 1)
    creds, host = rest.split("@", 1)
    if ":" in creds:
        user, _ = creds.split(":", 1)
        return f"{scheme}://{user}:****@{host}"
    return dsn
