"""UI: credentials list, holds list, audit log, system config.

Four narrow read-mostly views packaged together to keep the slice
small. Credential creation needs per-kind forms (basic_http,
bearer_token, api_key, aws_keys, gcp_service_account, mtls_cert,
tls_cert — seven different shapes); for now operators mint credentials
via `nl-router credential create` or the API, and the UI is read-only
+ delete. Same for system_config — we expose an edit-by-key form
because the catalog is small and homogeneous JSON.
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


router = APIRouter(prefix="/ui", tags=["ui"], include_in_schema=False)


# =========================================================================
# Credentials
# =========================================================================


@router.get("/credentials", response_class=Response)
async def credentials_list(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, kind, enc_version, metadata,
                   created_at, created_by, updated_at, updated_by
              FROM credentials
             ORDER BY name
        """)
        rows = []
        for c in cur.fetchall():
            md = c.get("metadata") or {}
            rows.append({
                **c,
                "last_used_at": md.get("last_used_at"),
                "expires_at":   md.get("expires_at"),
            })

    return render(request, "credentials_list.html",
                  auth=auth, credentials=rows)


@router.post("/credentials/{cid}/delete", response_class=Response)
async def credentials_delete(
    cid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT name FROM credentials WHERE id = %s", (cid,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "credential not found")
        try:
            cur.execute("DELETE FROM credentials WHERE id = %s", (cid,))
        except Exception as e:
            conn.rollback()
            resp = RedirectResponse(url="/ui/credentials", status_code=303)
            set_flash(resp,
                f"Cannot delete: {e}. Unlink from destinations first.", "err")
            return resp
        cur.execute("""
            INSERT INTO admin_audit (actor, actor_kind, action,
                                     resource_kind, resource_id, diff)
            VALUES (%s, %s, %s, %s, %s, %s::jsonb)
        """, (str(auth), "token", "credential.delete", "credential", str(cid),
              json.dumps({"before": {"name": prior["name"]}})))
        conn.commit()

    resp = RedirectResponse(url="/ui/credentials", status_code=303)
    set_flash(resp, f"Credential {prior['name']!r} deleted.")
    return resp


# =========================================================================
# Holds — filtered view of work_queue.cleanup_hold = TRUE
# =========================================================================


@router.get("/holds", response_class=Response)
async def holds_list(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, study_instance_uid, calling_aet, modality,
                   patient_id, status::text AS status,
                   cleanup_hold_by, cleanup_hold_at, cleanup_hold_reason,
                   received_at
              FROM work_queue
             WHERE cleanup_hold = TRUE
             ORDER BY cleanup_hold_at DESC
        """)
        rows = [{**r, "pill": pill(r["status"]),
                 "short_uid": (r["study_instance_uid"] or "")[-20:]}
                for r in cur.fetchall()]

    return render(request, "holds_list.html", auth=auth, rows=rows)


# =========================================================================
# Audit log
# =========================================================================


@router.get("/audit", response_class=Response)
async def audit_list(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    actor: str | None = None,
    action: str | None = None,
    resource_kind: str | None = None,
    limit: int = 100,
):
    where: list[str] = []
    params: list = []
    if actor:
        where.append("actor = %s")
        params.append(actor)
    if action:
        where.append("action = %s")
        params.append(action)
    if resource_kind:
        where.append("resource_kind = %s")
        params.append(resource_kind)

    sql = """
        SELECT id, occurred_at, actor, actor_kind, action,
               resource_kind, resource_id, host(client_ip) AS client_ip,
               user_agent, diff
          FROM admin_audit
    """
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY occurred_at DESC LIMIT %s"
    params.append(min(max(limit, 1), 500))

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        rows = []
        for r in cur.fetchall():
            r = dict(r)
            r["diff_pretty"] = (
                json.dumps(r["diff"], indent=2, sort_keys=True, default=str)
                if r.get("diff") else ""
            )
            rows.append(r)

    return render(
        request, "audit_list.html",
        auth=auth, rows=rows,
        active_filters={"actor": actor, "action": action,
                        "resource_kind": resource_kind, "limit": limit},
    )


# =========================================================================
# System config
# =========================================================================


@router.get("/config", response_class=Response)
async def config_list(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT key, value, description, updated_at, updated_by
              FROM system_config
             ORDER BY key
        """)
        rows = []
        for c in cur.fetchall():
            c = dict(c)
            c["value_pretty"] = json.dumps(c["value"], indent=2, sort_keys=True, default=str)
            rows.append(c)

    return render(request, "config_list.html", auth=auth, config=rows)


@router.post("/config/{key}", response_class=Response)
async def config_update(
    key: str,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    value: Annotated[str, Form()],
):
    try:
        value_obj = json.loads(value)
    except json.JSONDecodeError as e:
        resp = RedirectResponse(url="/ui/config", status_code=303)
        set_flash(resp, f"{key}: invalid JSON ({e.msg} at line {e.lineno}, col {e.colno})",
                  "err")
        return resp

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT value FROM system_config WHERE key = %s", (key,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, f"unknown config key: {key}")
        cur.execute("""
            UPDATE system_config
               SET value = %s::jsonb, updated_at = NOW(), updated_by = %s
             WHERE key = %s
        """, (json.dumps(value_obj), str(auth), key))
        cur.execute("""
            INSERT INTO admin_audit (actor, actor_kind, action,
                                     resource_kind, resource_id, diff)
            VALUES (%s, %s, %s, %s, %s, %s::jsonb)
        """, (str(auth), "token", "system_config.update", "system_config",
              key, json.dumps({"before": prior["value"], "after": value_obj},
                               default=str)))
        conn.commit()

    resp = RedirectResponse(url="/ui/config", status_code=303)
    set_flash(resp, f"{key} updated.")
    return resp
