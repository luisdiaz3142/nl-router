"""Health probes. Unauthenticated.

/healthz   — fast liveness; just confirms the process is responsive.
/readyz    — readiness; verifies the database is reachable + at expected schema.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, status

from nl_router.api.models import HealthOut
from nl_router.config import load
from nl_router.db import pool, schema_version

router = APIRouter(tags=["health"])


@router.get("/healthz", response_model=HealthOut, include_in_schema=True)
def healthz() -> HealthOut:
    """Liveness probe.

    Doesn't touch the DB so it's always cheap. /readyz is the right hook
    for service-mesh / load-balancer health checks since it verifies the
    DB is reachable too.
    """
    cfg = load()
    return HealthOut(status="ok", schema_version=None, server_id=cfg.server_id)


@router.get("/readyz", response_model=HealthOut, include_in_schema=True)
def readyz() -> HealthOut:
    """Readiness probe: confirms DB is reachable and at a known schema version."""
    cfg = load()
    try:
        version = schema_version()
    except Exception as e:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=f"database not reachable: {e}",
        )
    if version is None:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="schema not initialized; run `nl-router migrate`",
        )
    # Test the pool with a no-op query so we know it's healthy beyond just
    # the version-table read.
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1")
        cur.fetchone()
    return HealthOut(status="ok", schema_version=version, server_id=cfg.server_id)
