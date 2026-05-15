"""Read-only views of the route_assignments table."""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, status

from nl_router.api.auth import AuthContext, require
from nl_router.api.models import AssignmentOut, AssignmentPage, PageInfo
from nl_router.db import pool

router = APIRouter(prefix="/assignments", tags=["assignments"])


_ASSIGNMENT_COLS = (
    "id, work_queue_id, rule_id, destination_id, dispatch_kind, server_id, "
    "status, attempts, last_error, next_retry_at, dispatched_at, "
    "claimed_by, claimed_at, claim_expires_at, response_detail, created_at"
)


def _row_to_assignment(row: dict[str, Any]) -> AssignmentOut:
    return AssignmentOut(**row)


@router.get("", response_model=AssignmentPage)
def list_assignments(
    status_filter: str | None = Query(default=None, alias="status",
        description="pending | dispatching | dispatched | failed"),
    work_queue_id: int | None = None,
    rule_id: int | None = None,
    destination_id: int | None = None,
    dispatch_kind: str | None = None,
    server_id: str | None = None,
    limit: int = Query(default=50, ge=1, le=500),
    offset: int = Query(default=0, ge=0),
    _: AuthContext = Depends(require("workqueue.read")),
) -> AssignmentPage:
    """List route_assignments rows.

    The natural drill-in pattern is from a work_queue row, so callers
    typically filter on `work_queue_id`. Status + destination filters
    serve "what's still pending?" or "what's failing on Archive_A?".
    """
    clauses: list[str] = []
    params: list[Any] = []
    if status_filter is not None:
        clauses.append("status = %s")
        params.append(status_filter)
    if work_queue_id is not None:
        clauses.append("work_queue_id = %s")
        params.append(work_queue_id)
    if rule_id is not None:
        clauses.append("rule_id = %s")
        params.append(rule_id)
    if destination_id is not None:
        clauses.append("destination_id = %s")
        params.append(destination_id)
    if dispatch_kind is not None:
        clauses.append("dispatch_kind = %s")
        params.append(dispatch_kind)
    if server_id is not None:
        clauses.append("server_id = %s")
        params.append(server_id)

    where_sql = ("WHERE " + " AND ".join(clauses)) if clauses else ""

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(f"SELECT count(*) AS n FROM route_assignments {where_sql}", params)
        total = cur.fetchone()["n"]

        cur.execute(
            f"""
            SELECT {_ASSIGNMENT_COLS}
              FROM route_assignments
              {where_sql}
             ORDER BY created_at DESC, id DESC
             LIMIT %s OFFSET %s
            """,
            (*params, limit, offset),
        )
        items = [_row_to_assignment(r) for r in cur.fetchall()]

    return AssignmentPage(
        items=items,
        page=PageInfo(total=total, limit=limit, offset=offset),
    )


@router.get("/{aid}", response_model=AssignmentOut)
def get_assignment(
    aid: int,
    _: AuthContext = Depends(require("workqueue.read")),
) -> AssignmentOut:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            f"SELECT {_ASSIGNMENT_COLS} FROM route_assignments WHERE id = %s",
            (aid,),
        )
        row = cur.fetchone()
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                            detail="assignment not found")
    return _row_to_assignment(row)
