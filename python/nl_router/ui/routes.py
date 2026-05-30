"""Top-level UI routes: login, logout, dashboard.

The other UI surfaces (rules, destinations, studies, etc.) live in
their own route modules under ui/routes_*.py to keep file sizes
sensible; everything is mounted under /ui by ui/__init__.py via the
mount() helper at the bottom of this file.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Annotated, Any

from fastapi import APIRouter, Depends, Form, Request
from fastapi.responses import RedirectResponse, Response, StreamingResponse

from nl_router.api.auth import AuthContext, InvalidToken, validate_raw_token
from nl_router.db import pool
from nl_router.ui.auth import (
    SESSION_COOKIE,
    SESSION_MAX_AGE,
    ui_auth_required,
)
from nl_router.ui.common import pill, render, set_flash, templates

log = logging.getLogger("nl_router.ui.routes")


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

    Renders the dashboard with one initial snapshot of work_queue
    status counts + recent rows + node metadata. The page then
    subscribes to /ui/dashboard/_stream which pushes refreshed
    fragments every ~2 seconds via SSE (see dashboard_stream below).
    """
    data = _load_dashboard_data()
    return render(
        request, "dashboard.html",
        auth=auth,
        **data,
    )


# ---- Dashboard SSE -----------------------------------------------------


# How often we push a fresh snapshot down to subscribed dashboards. Sized
# to feel "live" without crushing the DB — the underlying query is
# ~10ms on a populated work_queue, so 2 s leaves plenty of headroom.
_DASHBOARD_STREAM_INTERVAL_S = 2.0


@router.get("/dashboard/_stream")
async def dashboard_stream(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    """Server-Sent Events endpoint for live dashboard updates.

    Streams one `dashboard-update` event every ~2 s carrying the
    rendered _dashboard_live.html fragment as the data payload. The
    HTMX-SSE extension on the dashboard page swaps the fragment into
    place when each event arrives.

    Lifecycle:
      * Disconnects (browser close, navigation away) raise
        asyncio.CancelledError in the generator — we catch and exit
        cleanly so the DB pool slot is released.
      * Each iteration opens + releases its own pool connection. We
        don't hold a connection open for the lifetime of the stream;
        with N concurrent dashboards open that would burn through the
        pool. The trade-off is N more queries per scrape interval —
        cheap.
      * Each event is prefixed by `event:` + `data:` lines per the
        SSE spec; the closing blank line is what tells the browser
        the event is complete.
    """
    async def event_stream():
        try:
            while True:
                if await request.is_disconnected():
                    return

                # Compose the next event payload. The template rendering
                # itself is sync, so wrap in to_thread to avoid blocking
                # the event loop on a slow DB query under heavy load.
                fragment = await asyncio.to_thread(_render_dashboard_live, request)

                # SSE message framing: `event:` is the named event the
                # client subscribes to; multi-line `data:` payloads are
                # supported but the HTML fragment is rendered on one
                # line by `_sse_encode` so we don't have to worry.
                yield _sse_encode("dashboard-update", fragment)

                await asyncio.sleep(_DASHBOARD_STREAM_INTERVAL_S)
        except asyncio.CancelledError:
            # Client disconnected (browser closed the EventSource).
            # Re-raise so Starlette can finish the response cleanly.
            raise
        except Exception:
            log.exception("dashboard_stream.failed")
            # End the stream gracefully on unexpected errors — the
            # browser will auto-reconnect after EventSource's default
            # retry interval.
            return

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={
            # nginx / similar proxies buffer responses by default; that
            # breaks SSE's "push as you go" semantics. This header is
            # the well-known opt-out.
            "X-Accel-Buffering": "no",
            # Helpful for testing through clients that cache aggressively.
            "Cache-Control": "no-cache",
        },
    )


# ---- Helpers -----------------------------------------------------------


def _load_dashboard_data() -> dict[str, Any]:
    """Pull the live data shown on the dashboard.

    Shared between the initial GET render and the SSE stream so the
    fragment shape stays consistent. Returns the kwargs the dashboard
    template expects (`status_counts`, `recent_rows`, `schema_version`).
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

    return {
        "status_counts":  status_counts,
        "recent_rows":    recent_rows,
        "schema_version": schema_version,
    }


def _render_dashboard_live(request: Request) -> str:
    """Render the `_dashboard_live.html` partial with fresh data.

    The partial only contains the bits the stream updates — the
    stat-card grid + recent-associations table. Static node metadata
    stays in the parent template and isn't re-rendered.
    """
    data = _load_dashboard_data()
    tpl = templates.get_template("_dashboard_live.html")
    return tpl.render({"request": request, **data})


def _sse_encode(event: str, html: str) -> str:
    """Format an SSE message as a single multi-line string.

    The spec says `data:` lines are joined by newlines on the client,
    so we strip newlines from the HTML before encoding to keep the
    framing simple. Compact HTML output is fine — browsers parse it
    identically. Trailing blank line is the message terminator.
    """
    flat = html.replace("\r", "").replace("\n", " ")
    return f"event: {event}\ndata: {flat}\n\n"
