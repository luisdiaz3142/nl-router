"""UI auth dependency.

HTML routes read the nlr_session cookie instead of the Authorization
header, but they validate against the same api_tokens table via
api.auth.validate_raw_token. A missing or invalid cookie redirects to
/ui/login; the JSON 401 response that the API surface returns is wrong
for an HTML form workflow.
"""

from __future__ import annotations

from fastapi import Cookie, Request
from fastapi.responses import RedirectResponse
from typing import Annotated

from nl_router.api.auth import AuthContext, InvalidToken, validate_raw_token

# Cookie name + lifetime. SameSite=Strict means the cookie isn't sent on
# cross-origin requests, which kills CSRF for state-changing POSTs from
# other sites. HttpOnly keeps it out of JS reach. Secure should be set
# by a reverse proxy on real deployments — we leave it off in dev so
# http://localhost works without TLS.
SESSION_COOKIE   = "nlr_session"
SESSION_MAX_AGE  = 60 * 60 * 12     # 12h — operators don't login often


class UIAuthRequired(Exception):
    """Raised by ui_auth_required when no valid session cookie is
    present. The app-level exception handler converts this into a 302
    to /ui/login (preserving the requested path in ?next=)."""

    def __init__(self, requested_path: str):
        self.requested_path = requested_path


async def ui_auth_required(
    request: Request,
    nlr_session: Annotated[str | None, Cookie()] = None,
) -> AuthContext:
    """FastAPI dependency for HTML routes that require login.

    Reads the nlr_session cookie, validates the token it carries, and
    returns the AuthContext. Failure raises UIAuthRequired so the
    handler can redirect to /ui/login.
    """
    if not nlr_session:
        raise UIAuthRequired(request.url.path)
    try:
        return validate_raw_token(nlr_session)
    except InvalidToken as e:
        raise UIAuthRequired(request.url.path) from e


def login_redirect(requested_path: str | None = None) -> RedirectResponse:
    """Build a 302 to /ui/login with ?next= preserved for after-login
    bounce-back. Returns 303 See Other to force GET on the redirect
    (the original request might have been a POST). Used by the
    exception handler for UIAuthRequired and explicitly by the logout
    handler."""
    next_qs = f"?next={requested_path}" if requested_path else ""
    return RedirectResponse(url=f"/ui/login{next_qs}", status_code=303)
