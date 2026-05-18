"""UI: studies (work_queue) live view + detail page.

The list view supports filters (status, calling_aet, modality,
search) and HTMX-polls every 3 seconds so operators see new traffic
arrive without refreshing. The detail page shows the full tag set,
route_assignments, and processing_jobs timeline for one row, plus
operator actions (cleanup_hold set/release).
"""

from __future__ import annotations

import json
from typing import Annotated

from fastapi import APIRouter, Depends, Form, HTTPException, Request
from fastapi.responses import RedirectResponse, Response

from nl_router.api.auth import AuthContext
from nl_router.db import pool
from nl_router.ui.auth import ui_auth_required
from nl_router.ui.common import pill, render, set_flash


router = APIRouter(prefix="/ui/studies", tags=["ui"], include_in_schema=False)


VALID_STATUSES = (
    "received", "routing", "routed", "processing", "processed",
    "dispatching", "dispatched", "dispatched_partial", "failed", "cleaned",
)


# ---- List + filtered table -------------------------------------------------


def _query_rows(
    *, status: str | None, calling_aet: str | None, modality: str | None,
    search: str | None, limit: int = 100,
) -> list[dict]:
    """Run the filtered work_queue query. Used by both the full-page
    list view and the HTMX-polled _table fragment."""
    where: list[str] = []
    params: list = []

    if status:
        where.append("status::text = %s")
        params.append(status)
    if calling_aet:
        where.append("calling_aet = %s")
        params.append(calling_aet)
    if modality:
        where.append("modality = %s")
        params.append(modality)
    if search:
        # Match against study UID, accession, or patient_id.
        where.append("(study_instance_uid = %s "
                     "OR accession_number = %s "
                     "OR patient_id = %s)")
        params.extend([search, search, search])

    sql = """
        SELECT id, received_at, calling_aet, modality, accession_number,
               study_instance_uid, patient_id, status::text AS status,
               cleanup_hold, instance_count, byte_count
          FROM work_queue
    """
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY received_at DESC LIMIT %s"
    params.append(limit)

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        rows = list(cur.fetchall())

    for r in rows:
        r["pill"]      = pill(r["status"])
        r["short_uid"] = (r["study_instance_uid"] or "")[-20:]
    return rows


@router.get("", response_class=Response)
async def list_studies(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    status: str | None = None,
    calling_aet: str | None = None,
    modality: str | None = None,
    search: str | None = None,
):
    rows = _query_rows(status=status, calling_aet=calling_aet,
                       modality=modality, search=search)
    return render(
        request, "studies_list.html",
        auth=auth, rows=rows,
        active_filters={"status": status, "calling_aet": calling_aet,
                        "modality": modality, "search": search},
        valid_statuses=VALID_STATUSES,
    )


@router.get("/_table", response_class=Response)
async def list_studies_fragment(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    status: str | None = None,
    calling_aet: str | None = None,
    modality: str | None = None,
    search: str | None = None,
):
    """HTMX-polled fragment — just the <table>. Returns the same data
    as the full page but without the layout."""
    rows = _query_rows(status=status, calling_aet=calling_aet,
                       modality=modality, search=search)
    return render(request, "_studies_table.html", auth=auth, rows=rows)


# ---- Detail page ---------------------------------------------------------


@router.get("/{wq_id}", response_class=Response)
async def study_detail(
    wq_id: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, server_id, received_at, closed_at, close_trigger::text AS close_trigger,
                   calling_aet, called_aet, host(peer_ip) AS peer_ip,
                   study_instance_uid, series_instance_uid, accession_number,
                   patient_id, patient_name, patient_birth_date, patient_sex,
                   modality, station_name, station_aet, retrieve_aet,
                   institution_name, manufacturer, manufacturer_model_name,
                   study_description, series_description, protocol_name,
                   body_part_examined, study_date, study_time,
                   instance_count, byte_count,
                   status::text AS status, routed_at, processed_at, dispatched_at,
                   cleaned_at, cleanup_hold, cleanup_hold_reason,
                   cleanup_hold_by, cleanup_hold_at,
                   last_error, failed_phase, retry_count,
                   file_root_path, tags
              FROM work_queue
             WHERE id = %s
        """, (wq_id,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(404, "work_queue row not found")

        cur.execute("""
            SELECT ra.id, ra.destination_id, d.name AS destination_name,
                   d.kind, ra.dispatch_kind, ra.status, ra.attempts,
                   ra.last_error, ra.next_retry_at, ra.dispatched_at,
                   ra.response_detail
              FROM route_assignments ra
              JOIN destinations d ON d.id = ra.destination_id
             WHERE ra.work_queue_id = %s
             ORDER BY ra.id
        """, (wq_id,))
        assignments = [
            {**a, "pill": pill(a["status"])}
            for a in cur.fetchall()
        ]

        cur.execute("""
            SELECT id, module_kind, ordinal, status::text AS status,
                   attempts, last_error, started_at, completed_at,
                   input_path, output_path
              FROM processing_jobs
             WHERE work_queue_id = %s
             ORDER BY ordinal, id
        """, (wq_id,))
        jobs = [
            {**j, "pill": pill(j["status"])}
            for j in cur.fetchall()
        ]

    # Render tags JSONB as a pretty-printed string for the <pre> block.
    row = dict(row)
    if row.get("tags") is not None:
        row["tags_pretty"] = json.dumps(row["tags"], indent=2, sort_keys=True)
    row["pill"] = pill(row["status"])

    return render(
        request, "study_detail.html",
        auth=auth, row=row, assignments=assignments, jobs=jobs,
    )


# ---- Hold / release ------------------------------------------------------


@router.post("/{wq_id}/hold", response_class=Response)
async def set_hold(
    wq_id: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    reason: Annotated[str, Form()] = "",
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            UPDATE work_queue
               SET cleanup_hold = TRUE,
                   cleanup_hold_reason = %s,
                   cleanup_hold_by = %s,
                   cleanup_hold_at = NOW()
             WHERE id = %s
            RETURNING id
        """, (reason or None, str(auth), wq_id))
        if cur.fetchone() is None:
            raise HTTPException(404, "work_queue row not found")
        cur.execute("""
            INSERT INTO admin_audit (actor, actor_kind, action,
                                     resource_kind, resource_id, diff)
            VALUES (%s, %s, %s, %s, %s, %s::jsonb)
        """, (str(auth), "token", "workqueue.hold_set", "work_queue", str(wq_id),
              json.dumps({"after": {"cleanup_hold": True, "reason": reason}})))
        conn.commit()

    resp = RedirectResponse(url=f"/ui/studies/{wq_id}", status_code=303)
    set_flash(resp, "Cleanup hold set.")
    return resp


@router.post("/{wq_id}/release", response_class=Response)
async def release_hold(
    wq_id: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            UPDATE work_queue
               SET cleanup_hold = FALSE,
                   cleanup_hold_reason = NULL,
                   cleanup_hold_by = NULL,
                   cleanup_hold_at = NULL
             WHERE id = %s
            RETURNING id
        """, (wq_id,))
        if cur.fetchone() is None:
            raise HTTPException(404, "work_queue row not found")
        cur.execute("""
            INSERT INTO admin_audit (actor, actor_kind, action,
                                     resource_kind, resource_id, diff)
            VALUES (%s, %s, %s, %s, %s, %s::jsonb)
        """, (str(auth), "token", "workqueue.hold_release", "work_queue", str(wq_id),
              json.dumps({"after": {"cleanup_hold": False}})))
        conn.commit()

    resp = RedirectResponse(url=f"/ui/studies/{wq_id}", status_code=303)
    set_flash(resp, "Cleanup hold released.")
    return resp
