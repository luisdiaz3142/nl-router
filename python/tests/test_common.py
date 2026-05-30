"""Tests for nl_router.ui.common — pill mapping + flash cookie roundtrip.

Pure unit tests; no DB or HTTP server involved.
"""

from __future__ import annotations

from starlette.responses import Response
from starlette.requests import Request

from nl_router.ui.common import FLASH_COOKIE, pill, read_flash, set_flash


# ---- pill mapping -------------------------------------------------------


def test_pill_known_status_returns_expected_class() -> None:
    # Cover one of each color so a future palette refactor surfaces in tests.
    assert pill("received") == "info"
    assert pill("routed") == "ok"
    assert pill("dispatched") == "ok"
    assert pill("dispatched_partial") == "warn"
    assert pill("failed") == "err"
    assert pill("cleaned") == "muted"
    assert pill("enabled") == "ok"
    assert pill("draft") == "muted"
    assert pill("disabled") == "muted"


def test_pill_none_returns_muted() -> None:
    assert pill(None) == "muted"


def test_pill_unknown_status_returns_muted() -> None:
    # An unmapped status string falls back to muted rather than
    # raising — keeps the UI from crashing if the DB ever returns a
    # value we haven't seen.
    assert pill("just_added_in_the_db_yesterday") == "muted"


# ---- flash cookie roundtrip ---------------------------------------------


def _build_request(cookies: dict[str, str]) -> Request:
    """Synthesize a minimal Starlette Request that read_flash can use.

    Starlette's Request reads cookies from the ASGI scope's
    `headers` list — we encode the cookie there.
    """
    cookie_hdr = "; ".join(f"{k}={v}" for k, v in cookies.items())
    scope = {
        "type": "http",
        "method": "GET",
        "path": "/",
        "headers": [(b"cookie", cookie_hdr.encode())] if cookies else [],
        "query_string": b"",
    }
    return Request(scope)


def test_flash_set_round_trip_ok() -> None:
    resp = Response()
    set_flash(resp, "all good", kind="ok")
    # set_flash adds a Set-Cookie header to the outgoing response.
    cookie_header = resp.headers["set-cookie"]
    assert FLASH_COOKIE in cookie_header

    # Simulate the next request carrying that cookie.
    next_req = _build_request({FLASH_COOKIE: "o|all good"})
    next_resp = Response()
    msg, kind = read_flash(next_req, next_resp)
    assert msg == "all good"
    assert kind == "ok"


def test_flash_set_round_trip_err() -> None:
    resp = Response()
    set_flash(resp, "boom", kind="err")
    next_req = _build_request({FLASH_COOKIE: "e|boom"})
    next_resp = Response()
    msg, kind = read_flash(next_req, next_resp)
    assert msg == "boom"
    assert kind == "err"


def test_flash_no_cookie_returns_none() -> None:
    next_req = _build_request({})
    next_resp = Response()
    msg, kind = read_flash(next_req, next_resp)
    assert msg is None and kind is None


def test_flash_clears_on_read() -> None:
    """One-shot semantics: reading the flash clears it from the
    response so it doesn't fire again next request."""
    next_req = _build_request({FLASH_COOKIE: "o|first read"})
    next_resp = Response()
    msg, _ = read_flash(next_req, next_resp)
    assert msg == "first read"
    # The outgoing response should have a Set-Cookie that clears it.
    set_cookie = next_resp.headers.get("set-cookie", "")
    assert FLASH_COOKIE in set_cookie
    # Either Max-Age=0 or an expired Expires — Starlette uses Max-Age.
    assert "max-age=0" in set_cookie.lower() or 'expires=thu, 01 jan 1970' in set_cookie.lower()
