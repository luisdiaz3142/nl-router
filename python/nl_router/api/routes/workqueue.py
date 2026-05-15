"""Read-only views of the work_queue table.

The work_queue row carries everything the router/dispatcher need to
process a study; this surface lets operators see what's in flight and
drill into any one row's full tag set.

No write paths land in this slice — `cleanup_hold` setting, force-retry,
etc. arrive when the operator UI gets a "studies" view in a follow-up.
"""

from __future__ import annotations

from datetime import datetime
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, status

from nl_router.api.auth import AuthContext, require
from nl_router.api.models import (
    PageInfo,
    WorkQueueDetail,
    WorkQueuePage,
    WorkQueueSummary,
)
from nl_router.db import pool

router = APIRouter(prefix="/workqueue", tags=["workqueue"])


# Columns the list endpoint returns. Keeping this list explicit (rather
# than SELECT *) protects the API surface from leaking columns we add to
# work_queue later for internal bookkeeping.
_SUMMARY_COLS = (
    "id, server_id, status::text, study_instance_uid, series_instance_uid, "
    "accession_number, patient_id, patient_name, modality, station_name, "
    "study_description, calling_aet, called_aet, "
    "instance_count, byte_count, received_at, closed_at, "
    "routed_at, dispatched_at, cleaned_at, "
    "close_trigger::text, priority, retry_count, failed_phase, last_error, "
    "cleanup_hold"
)


def _row_to_summary(row: dict[str, Any]) -> WorkQueueSummary:
    return WorkQueueSummary(**row)


@router.get("", response_model=WorkQueuePage)
def list_workqueue(
    status_filter: str | None = Query(default=None, alias="status",
        description="Filter by lifecycle status (received|routing|routed|...|cleaned)."),
    server_id: str | None = None,
    calling_aet: str | None = None,
    modality: str | None = None,
    study_instance_uid: str | None = None,
    received_after: datetime | None = None,
    received_before: datetime | None = None,
    limit: int = Query(default=50, ge=1, le=500),
    offset: int = Query(default=0, ge=0),
    _: AuthContext = Depends(require("workqueue.read")),
) -> WorkQueuePage:
    """List work_queue rows.

    Filters compose with AND. The default sort is most-recent-first by
    received_at so operators see fresh activity at the top.
    """
    clauses: list[str] = []
    params: list[Any] = []
    if status_filter is not None:
        clauses.append("status = %s::work_status")
        params.append(status_filter)
    if server_id is not None:
        clauses.append("server_id = %s")
        params.append(server_id)
    if calling_aet is not None:
        clauses.append("calling_aet = %s")
        params.append(calling_aet)
    if modality is not None:
        clauses.append("modality = %s")
        params.append(modality)
    if study_instance_uid is not None:
        clauses.append("study_instance_uid = %s")
        params.append(study_instance_uid)
    if received_after is not None:
        clauses.append("received_at >= %s")
        params.append(received_after)
    if received_before is not None:
        clauses.append("received_at < %s")
        params.append(received_before)

    where_sql = ("WHERE " + " AND ".join(clauses)) if clauses else ""

    with pool().connection() as conn, conn.cursor() as cur:
        # Total count for the page envelope. We accept the second query
        # cost here because operators expect a "X of N" indicator.
        cur.execute(f"SELECT count(*) AS n FROM work_queue {where_sql}", params)
        total = cur.fetchone()["n"]

        cur.execute(
            f"""
            SELECT {_SUMMARY_COLS}
              FROM work_queue
              {where_sql}
             ORDER BY received_at DESC, id DESC
             LIMIT %s OFFSET %s
            """,
            (*params, limit, offset),
        )
        items = [_row_to_summary(r) for r in cur.fetchall()]

    return WorkQueuePage(
        items=items,
        page=PageInfo(total=total, limit=limit, offset=offset),
    )


@router.get("/{wq_id}", response_model=WorkQueueDetail)
def get_workqueue_row(
    wq_id: int,
    _: AuthContext = Depends(require("workqueue.read")),
) -> WorkQueueDetail:
    """Return one work_queue row with full tag set and on-disk path."""
    with pool().connection() as conn, conn.cursor() as cur:
        # SELECT * is OK here because the response model controls what's
        # serialized; new columns are silently dropped from the output.
        cur.execute(
            """
            SELECT *,
                   status::text       AS status_text,
                   close_trigger::text AS close_trigger_text
              FROM work_queue
             WHERE id = %s
            """,
            (wq_id,),
        )
        row = cur.fetchone()
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                            detail="work_queue row not found")

    # Override the enum columns with their text rendering so Pydantic accepts
    # them as plain strings.
    row["status"] = row.pop("status_text")
    row["close_trigger"] = row.pop("close_trigger_text")
    # tags is already a Python dict via psycopg's jsonb decoder.
    return WorkQueueDetail(**row)
