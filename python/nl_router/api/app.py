"""FastAPI app factory + lifespan.

The DB pool is opened at startup and closed at shutdown via the lifespan
context manager. Routers are mounted under /api/v1; /healthz and /readyz
live at the root.
"""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from typing import AsyncIterator

from fastapi import FastAPI
from fastapi.responses import JSONResponse

from nl_router import __version__
from nl_router.api.routes import (
    assignments,
    audit,
    credentials,
    destinations,
    health,
    rules,
    tokens,
    workqueue,
)
from nl_router.db import pool

log = logging.getLogger("nl_router.api")


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    """Open the DB pool at startup; close at shutdown."""
    log.info("api.startup")
    p = pool()                 # eager init so a misconfigured DSN fails fast
    log.info("api.db_pool_open", extra={"pool_size": p.get_stats().get("pool_size")})
    try:
        yield
    finally:
        log.info("api.shutdown")
        p.close()


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

    # Routes
    app.include_router(health.router)                              # /healthz, /readyz
    app.include_router(rules.router,        prefix="/api/v1")      # /api/v1/rules (+ /destinations bindings)
    app.include_router(destinations.router, prefix="/api/v1")      # /api/v1/destinations
    app.include_router(workqueue.router,    prefix="/api/v1")      # /api/v1/workqueue
    app.include_router(assignments.router,  prefix="/api/v1")      # /api/v1/assignments
    app.include_router(audit.router,        prefix="/api/v1")      # /api/v1/audit
    app.include_router(credentials.router,  prefix="/api/v1")      # /api/v1/credentials
    app.include_router(tokens.router,       prefix="/api/v1")      # /api/v1/tokens

    return app


# Module-level instance for `uvicorn nl_router.api.app:app`.
app = create_app()
