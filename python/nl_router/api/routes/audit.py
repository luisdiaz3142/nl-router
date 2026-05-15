"""Read-only view of the admin_audit table.

Every state-changing API call writes one row here. Reads are guarded by
the `audit.read` permission, which is granted to admin/operator/viewer
by default.
"""

from __future__ import annotations

from datetime import datetime
from typing import Any

from fastapi import APIRouter, Depends, Query

from nl_router.api.auth import AuthContext, require
from nl_router.api.models import AuditOut, AuditPage, PageInfo
from nl_router.db import pool

router = APIRouter(prefix="/audit", tags=["audit"])


_AUDIT_COLS = (
    "id, actor, actor_kind, action, resource_kind, resource_id, "
    "diff, host(client_ip) AS client_ip, user_agent, occurred_at"
)


def _row_to_audit(row: dict[str, Any]) -> AuditOut:
    return AuditOut(**row)


@router.get("", response_model=AuditPage)
def list_audit(
    actor: str | None = Query(default=None, description="Filter by exact actor string."),
    action: str | None = Query(default=None,
        description="Filter by action (e.g. 'rule.create', 'destination.update')."),
    resource_kind: str | None = None,
    resource_id: str | None = None,
    occurred_after: datetime | None = None,
    occurred_before: datetime | None = None,
    limit: int = Query(default=100, ge=1, le=1000),
    offset: int = Query(default=0, ge=0),
    _: AuthContext = Depends(require("audit.read")),
) -> AuditPage:
    """List admin_audit rows, most recent first.

    Filters compose with AND. The `action` filter supports an exact match;
    pattern matching (LIKE 'rule.%') is a follow-up if operators ask.
    """
    clauses: list[str] = []
    params: list[Any] = []
    if actor is not None:
        clauses.append("actor = %s")
        params.append(actor)
    if action is not None:
        clauses.append("action = %s")
        params.append(action)
    if resource_kind is not None:
        clauses.append("resource_kind = %s")
        params.append(resource_kind)
    if resource_id is not None:
        clauses.append("resource_id = %s")
        params.append(resource_id)
    if occurred_after is not None:
        clauses.append("occurred_at >= %s")
        params.append(occurred_after)
    if occurred_before is not None:
        clauses.append("occurred_at < %s")
        params.append(occurred_before)

    where_sql = ("WHERE " + " AND ".join(clauses)) if clauses else ""

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(f"SELECT count(*) AS n FROM admin_audit {where_sql}", params)
        total = cur.fetchone()["n"]

        cur.execute(
            f"""
            SELECT {_AUDIT_COLS}
              FROM admin_audit
              {where_sql}
             ORDER BY occurred_at DESC, id DESC
             LIMIT %s OFFSET %s
            """,
            (*params, limit, offset),
        )
        items = [_row_to_audit(r) for r in cur.fetchall()]

    return AuditPage(items=items, page=PageInfo(total=total, limit=limit, offset=offset))
