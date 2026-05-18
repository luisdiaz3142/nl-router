"""Top-level UI routes: login, logout, dashboard.

The other UI surfaces (rules, destinations, studies, etc.) live in
their own route modules under ui/routes_*.py to keep file sizes
sensible; everything is mounted under /ui by ui/__init__.py via the
mount() helper at the bottom of this file.
"""

from __future__ import annotations

from typing import Annotated

from fastapi import APIRouter, Depends, Form, Request
from fastapi.responses import RedirectResponse, Response

from nl_router.api.auth import AuthContext, InvalidToken, validate_raw_token
from nl_router.db import pool
from nl_router.ui.auth import (
    SESSION_COOKIE,
    SESSION_MAX_AGE,
    ui_auth_required,
)
from nl_router.ui.common import pill, render, set_flash


router = APIRouter(prefix="/ui", tags=["ui"], include_in_schema=False)


# ---- Login / logout ----------------------------------------------------


@router.get("/login", response_class=Response)
async def login_page(request: Request, next: str | None = None, error: str | None = None):
    """GET /ui/login — render the login form.

    Optional `next` query param preserves the original URL the user
    was trying to reach when their session expired; on successful
    login we 303 back to it.
    """
    return render(request, "login.html",
                  auth=None, next=next, error=error)


@router.post("/login", response_class=Response)
async def login_submit(
    request: Request,
    token: Annotated[str, Form()],
    next: str | None = None,
):
    """POST /ui/login — validate the pasted token, set the session
    cookie, 303 to ?next= or /ui."""
    try:
        validate_raw_token(token)
    except InvalidToken:
        # Re-render the form with an inline error. We don't leak token
        # contents (the input is `type=password` and we never echo it
        # back); just say "invalid or revoked token."
        return render(
            request, "login.html",
            auth=None, next=next,
            error="Invalid or revoked token. Mint a fresh one via "
                  "`nl-router init` or the /api/v1/tokens endpoint.",
        )

    target = next or "/ui"
    # Defensive: only allow same-origin relative paths. An open-redirect
    # on /ui/login could be abused for credential phishing if we
    # blindly trusted ?next.
    if not target.startswith("/") or target.startswith("//"):
        target = "/ui"

    resp = RedirectResponse(url=target, status_code=303)
    resp.set_cookie(
        SESSION_COOKIE,
        token,
        max_age=SESSION_MAX_AGE,
        httponly=True,
        samesite="strict",
        # secure=True should be set by a reverse proxy on real deploys;
        # we leave it off so dev http://localhost works.
        secure=False,
        path="/",
    )
    return resp


@router.get("/logout")
async def logout(request: Request):
    """GET /ui/logout — clear the session cookie and bounce to login."""
    resp = RedirectResponse(url="/ui/login", status_code=303)
    resp.delete_cookie(SESSION_COOKIE, path="/")
    return resp


# ---- Dashboard ---------------------------------------------------------


@router.get("/", response_class=Response)
async def dashboard(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    """GET /ui — landing page after login.

    Two simple aggregates for now: status counts across work_queue,
    and the 20 most recent rows. Real-time updates (SSE) land with
    slice 4's studies view.
    """
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT status::text AS status, COUNT(*) AS count
              FROM work_queue
             GROUP BY status
             ORDER BY count DESC
        """)
        status_counts = [
            {"status": r["status"], "count": r["count"], "pill": pill(r["status"])}
            for r in cur.fetchall()
        ]

        cur.execute("""
            SELECT id, received_at, calling_aet, modality,
                   study_instance_uid, status::text AS status
              FROM work_queue
             ORDER BY received_at DESC
             LIMIT 20
        """)
        recent_rows = [
            {**r, "pill": pill(r["status"])} for r in cur.fetchall()
        ]

        # Schema version is pulled from schema_migrations (golang-migrate's
        # own bookkeeping table). It's a single row keyed by `version`.
        try:
            cur.execute("SELECT version FROM schema_migrations LIMIT 1")
            sv_row = cur.fetchone()
            schema_version = sv_row["version"] if sv_row else None
        except Exception:
            schema_version = None

    return render(
        request, "dashboard.html",
        auth=auth,
        status_counts=status_counts,
        recent_rows=recent_rows,
        schema_version=schema_version,
    )
