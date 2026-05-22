"""FastAPI app factory + lifespan.

The DB pool is opened at startup and closed at shutdown via the lifespan
context manager. Routers are mounted under /api/v1; /healthz and /readyz
live at the root.
"""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from typing import AsyncIterator

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pathlib import Path

from nl_router import __version__
from nl_router.api.metrics import (
    DB_POOL_IN_USE,
    DB_POOL_SIZE,
    PrometheusMiddleware,
    serve_metrics,
)
from nl_router.api.routes import (
    assignments,
    audit,
    credentials,
    destinations,
    health,
    processing_modules,
    rules,
    tokens,
    workqueue,
)
from nl_router.config import load as load_bootstrap
from nl_router.db import pool
from nl_router.ui import routes as ui_routes
from nl_router.ui import routes_destinations as ui_routes_destinations
from nl_router.ui import routes_misc as ui_routes_misc
from nl_router.ui import routes_processing as ui_routes_processing
from nl_router.ui import routes_rules as ui_routes_rules
from nl_router.ui import routes_studies as ui_routes_studies
from nl_router.ui.auth import UIAuthRequired, login_redirect

log = logging.getLogger("nl_router.api")


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    """Open the DB pool at startup; close at shutdown."""
    log.info("api.startup")
    p = pool()                 # eager init so a misconfigured DSN fails fast
    log.info("api.db_pool_open", extra={"pool_size": p.get_stats().get("pool_size")})

    # M17: start the Prometheus exposer on its own port. Wrapped so a
    # misconfigured port doesn't gate the API process — see serve_metrics.
    try:
        cfg = load_bootstrap()
        serve_metrics(port=cfg.metrics_port, addr="0.0.0.0")
        _refresh_pool_gauges(p)
    except Exception as e:                # pragma: no cover — startup-only
        log.error("api.metrics_setup_failed", extra={"error": str(e)})

    try:
        yield
    finally:
        log.info("api.shutdown")
        p.close()


def _refresh_pool_gauges(p) -> None:                          # type: ignore[no-untyped-def]
    """Seed the pool gauges with a one-shot read at startup.

    We avoid a poller thread for now — operators can scrape on a tight
    interval (e.g. 15s) and the gauges get refreshed by the next-request
    side-effect in the middleware. If pool pressure becomes a real
    concern this turns into a daemon thread.
    """
    try:
        stats = p.get_stats()
        DB_POOL_SIZE.set(stats.get("pool_size", 0))
        DB_POOL_IN_USE.set(stats.get("pool_size", 0) - stats.get("pool_available", 0))
    except Exception:
        # psycopg pool stats schema differs across versions; don't bring
        # the API down because the gauge labels drifted.
        pass


def create_app() -> FastAPI:
    """Build the FastAPI application. Factory so test fixtures can
    spin up a fresh app per session."""
    app = FastAPI(
        title="nl-router management API",
        version=__version__,
        description=(
            "Operator-facing HTTP surface for nl-router. The DICOM hot path "
            "runs as separate native processes (nl-receiver / nl-route / "
            "nl-dispatch / nl-clean) and is not controlled directly via this "
            "API — they read the same Postgres database."
        ),
        lifespan=lifespan,
        # OpenAPI on /api/v1/openapi.json so it's tucked under the same prefix
        # as the routes themselves.
        openapi_url="/api/v1/openapi.json",
        docs_url="/api/v1/docs",
        redoc_url="/api/v1/redoc",
    )

    # M17: time + count every HTTP request. The /metrics endpoint itself
    # runs on a separate port (see lifespan → serve_metrics) so scrapes
    # don't show up in this counter.
    app.add_middleware(PrometheusMiddleware)

    # API routes
    app.include_router(health.router)                              # /healthz, /readyz
    app.include_router(rules.router,        prefix="/api/v1")      # /api/v1/rules (+ /destinations bindings)
    app.include_router(destinations.router, prefix="/api/v1")      # /api/v1/destinations
    app.include_router(workqueue.router,    prefix="/api/v1")      # /api/v1/workqueue
    app.include_router(assignments.router,  prefix="/api/v1")      # /api/v1/assignments
    app.include_router(audit.router,        prefix="/api/v1")      # /api/v1/audit
    app.include_router(credentials.router,  prefix="/api/v1")      # /api/v1/credentials
    app.include_router(tokens.router,       prefix="/api/v1")      # /api/v1/tokens
    app.include_router(processing_modules.router,       prefix="/api/v1")  # /api/v1/processing-modules
    app.include_router(processing_modules.chain_router, prefix="/api/v1")  # /api/v1/rules/{id}/processing-chain

    # UI routes — server-rendered Jinja2 + HTMX. Excluded from OpenAPI
    # via include_in_schema=False on the router itself.
    app.include_router(ui_routes.router)                           # /ui, /ui/login, /ui/logout
    app.include_router(ui_routes_rules.router)                     # /ui/rules
    app.include_router(ui_routes_destinations.router)              # /ui/destinations
    app.include_router(ui_routes_studies.router)                   # /ui/studies
    app.include_router(ui_routes_misc.router)                      # /ui/credentials, /holds, /audit, /config
    app.include_router(ui_routes_processing.router)                # /ui/processing-modules
    app.include_router(ui_routes_processing.chain_router)          # /ui/rules/{id}/processing-chain
    _static_dir = Path(__file__).parent.parent / "ui" / "static"
    if _static_dir.exists():
        app.mount("/ui/static", StaticFiles(directory=str(_static_dir)), name="ui-static")

    # When a UI route's auth dep raises UIAuthRequired (cookie missing
    # or invalid), redirect to /ui/login instead of returning the
    # JSON 401 the API surface uses. The handler preserves ?next= so
    # the user lands on their original page after sign-in.
    @app.exception_handler(UIAuthRequired)
    async def _on_ui_auth_required(request: Request, exc: UIAuthRequired):  # noqa: ARG001
        return login_redirect(exc.requested_path)

    # Convenience root redirect: hitting bare / takes you to the UI.
    @app.get("/", include_in_schema=False)
    async def _root() -> RedirectResponse:
        return RedirectResponse(url="/ui", status_code=303)

    return app


# Module-level instance for `uvicorn nl_router.api.app:app`.
app = create_app()
