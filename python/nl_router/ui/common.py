"""Shared UI helpers — template setup, flash messages, status-pill
mapping, common context vars.

Kept separate from the route files so each view module can stay
narrow."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from fastapi import Request
from fastapi.responses import Response
from fastapi.templating import Jinja2Templates

from nl_router import __version__
from nl_router.api.auth import AuthContext
from nl_router.config import load


# ---- Template setup ----------------------------------------------------

_TEMPLATES_DIR = Path(__file__).parent / "templates"
templates = Jinja2Templates(directory=str(_TEMPLATES_DIR))


# ---- Flash messages ----------------------------------------------------
#
# A one-shot cookie carries a brief status message across a 303 redirect
# (e.g. "Rule created" after a POST). We store it as a cookie rather
# than a server-side session because FastAPI doesn't ship a session
# middleware by default and the message is throw-away (1 KB max).

FLASH_COOKIE = "nlr_flash"


def set_flash(response: Response, message: str, kind: str = "ok") -> None:
    """Attach a flash cookie to an outgoing redirect response.

    Cleared by `read_flash` on the next request.
    """
    # kind|message — kind is one char ('o' for ok, 'e' for error).
    short = ("e" if kind == "err" else "o") + "|" + message
    response.set_cookie(
        FLASH_COOKIE, short,
        max_age=30, httponly=True, samesite="strict",
    )


def read_flash(request: Request, response: Response) -> tuple[str | None, str | None]:
    """Return (message, kind) from the flash cookie and clear it.

    Returns (None, None) if no flash is set.
    """
    raw = request.cookies.get(FLASH_COOKIE)
    if not raw:
        return None, None
    response.delete_cookie(FLASH_COOKIE)
    if "|" not in raw:
        return raw, "ok"
    kind_char, msg = raw.split("|", 1)
    return msg, "err" if kind_char == "e" else "ok"


# ---- Status-pill mapping ----------------------------------------------
#
# Maps work_queue.status / route_assignments.status values to CSS class
# suffixes (base.html defines .pill-ok / .pill-warn / .pill-err / .pill-info
# / .pill-muted).

_STATUS_TO_PILL: dict[str, str] = {
    # work_queue.status
    "received":           "info",
    "routing":            "info",
    "routed":             "ok",
    "processing":         "info",
    "processed":          "ok",
    "dispatching":        "info",
    "dispatched":         "ok",
    "dispatched_partial": "warn",
    "failed":             "err",
    "cleaned":            "muted",
    # route_assignments.status
    "pending":     "info",
    # rules.status
    "draft":       "muted",
    "disabled":    "muted",
    "enabled":     "ok",
    # processing_jobs.status
    "done":        "ok",
}


def pill(status: str | None) -> str:
    """Return the CSS class suffix for a status string. Unknown → 'muted'."""
    if status is None:
        return "muted"
    return _STATUS_TO_PILL.get(status, "muted")


# ---- Template rendering helpers ----------------------------------------

def render(
    request: Request,
    template_name: str,
    *,
    auth: AuthContext | None = None,
    **ctx: Any,
):
    """Wrap TemplateResponse with the common context every page expects.

    Always passes `auth`, `server_id`, and `nlrouter_version`. Reads
    the flash cookie + clears it. Caller supplies the rest of the
    context as kwargs.
    """
    response = templates.TemplateResponse(request, template_name, ctx)
    flash_msg, flash_kind = read_flash(request, response)
    # Stuff the resolved values into the response's context — Jinja
    # already rendered, so we re-render with the flash baked in. Simpler
    # to just include them in ctx up front.
    cfg = load()
    full_ctx = {
        "auth":             auth,
        "server_id":        cfg.server_id,
        "nlrouter_version": __version__,
        "flash_msg":        flash_msg,
        "flash_kind":       flash_kind,
        **ctx,
    }
    return templates.TemplateResponse(request, template_name, full_ctx)
