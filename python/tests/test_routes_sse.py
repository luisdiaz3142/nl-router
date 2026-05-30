"""Tests for the SSE encoder used by the live dashboard (M24).

Just the framing helper — we don't try to exercise the full
streaming endpoint here because that needs a TestClient + a
working DB pool, which lives in the deferred DB-fixture work.
"""

from __future__ import annotations

from nl_router.ui.routes import _sse_encode


def test_basic_framing() -> None:
    msg = _sse_encode("dashboard-update", "<div>hi</div>")
    assert msg.startswith("event: dashboard-update\n")
    assert "data: <div>hi</div>" in msg
    # SSE messages end in a blank line — this is the message terminator
    # the browser looks for.
    assert msg.endswith("\n\n")


def test_newlines_in_payload_are_flattened() -> None:
    """SSE supports multi-line `data:` payloads, but each newline in
    the source becomes a separate `data:` line on the client. We
    flatten newlines so one HTML fragment lands as one logical
    payload."""
    multi_line_html = "<div>\n  <p>line 1</p>\n  <p>line 2</p>\n</div>"
    msg = _sse_encode("update", multi_line_html)
    # Exactly one `data:` line (plus framing).
    data_lines = [ln for ln in msg.split("\n") if ln.startswith("data:")]
    assert len(data_lines) == 1
    # Spaces preserved, newlines replaced.
    assert "\n" not in data_lines[0]
    assert "<p>line 1</p>" in data_lines[0]
    assert "<p>line 2</p>" in data_lines[0]


def test_carriage_returns_stripped() -> None:
    """Some templating engines emit \\r\\n line endings on Windows. The
    encoder should normalize CR away so the SSE framing isn't broken
    by a stray \\r mid-payload."""
    msg = _sse_encode("update", "<div>a</div>\r\n<div>b</div>")
    assert "\r" not in msg
    # Both halves present in the single data: line.
    assert "<div>a</div>" in msg
    assert "<div>b</div>" in msg


def test_event_name_distinguishes_streams() -> None:
    """A page can subscribe to a specific named event. Verify the
    event name lands on its own line."""
    a = _sse_encode("dashboard-update", "<p/>")
    b = _sse_encode("studies-update", "<p/>")
    assert "event: dashboard-update" in a
    assert "event: studies-update" in b
    assert a != b
