"""Tests for nl_router.api.probes — destination "test connection" helpers.

These are pure unit tests against the helpers; live probes against
real DICOM/HTTP servers are out of scope here (covered by the
end-to-end smoke test).
"""

from __future__ import annotations

import httpx

from nl_router.api import probes
from nl_router.api.probes import (
    ProbeResult,
    _http_auth,
    probe_destination,
)


# ---- _http_auth shape ---------------------------------------------------


def test_http_auth_none_for_no_credential() -> None:
    assert _http_auth(None) is None


def test_http_auth_basic_for_username_password() -> None:
    auth = _http_auth({"username": "alice", "password": "hunter2"})
    assert isinstance(auth, httpx.BasicAuth)


def test_http_auth_bearer_callable_for_token() -> None:
    """Bearer auth is built as an httpx-compatible callable that
    mutates the outgoing Authorization header. Verify the header is
    set when the callable runs against a stub request."""
    auth = _http_auth({"token": "abc123"})
    assert callable(auth)
    # Synthesize a fake request with a mutable headers dict.
    req = httpx.Request("GET", "https://example.test/")
    auth(req)
    assert req.headers["Authorization"] == "Bearer abc123"


def test_http_auth_header_credential() -> None:
    """API-key-as-custom-header: {"header": "X-Foo", "value": "v"}."""
    auth = _http_auth({"header": "X-API-Key", "value": "topsecret"})
    assert callable(auth)
    req = httpx.Request("GET", "https://example.test/")
    auth(req)
    assert req.headers["X-API-Key"] == "topsecret"


def test_http_auth_unknown_shape_returns_none() -> None:
    """A payload that doesn't match any recognized shape should not
    raise — we just emit no auth and let the probe see whatever the
    server returns."""
    assert _http_auth({"random_key": "x"}) is None


# ---- probe_destination dispatch -----------------------------------------


def test_probe_unknown_kind_fails_cleanly() -> None:
    """Unknown kind should never raise — it should produce a clean
    `ok=False` ProbeResult so the UI can render a normal error card."""
    result = probe_destination(kind="totally_fake_kind", config={}, credential=None)
    assert isinstance(result, ProbeResult)
    assert result.ok is False
    assert "unknown destination kind" in result.detail
    assert result.kind == "totally_fake_kind"


def test_probe_dicom_requires_host_port() -> None:
    """The DICOM kind probe rejects empty config without trying to
    open a socket."""
    result = probe_destination(kind="dicom", config={}, credential=None)
    assert result.ok is False
    assert "host" in result.detail and "port" in result.detail


def test_probe_dicom_unreachable_fails_fast() -> None:
    """An invalid TEST-NET-1 address times out in well under 8 s and
    returns ok=False with a descriptive detail. We use a 0.5s timeout
    so the test itself stays fast — there's no listener at 192.0.2.1
    by RFC 5737."""
    result = probe_destination(
        kind="dicom",
        config={"host": "192.0.2.1", "port": 1},
        credential=None,
        timeout_s=0.5,
    )
    assert result.ok is False
    assert result.kind == "dicom"
    assert "failed" in result.detail or "timed out" in result.detail


def test_probe_file_requires_path() -> None:
    result = probe_destination(kind="file", config={}, credential=None)
    assert result.ok is False
    assert "path_template" in result.detail


def test_probe_object_storage_requires_credential() -> None:
    """No credential → fail cleanly before doing any boto3 work."""
    result = probe_destination(
        kind="object_storage",
        config={"bucket": "irrelevant"},
        credential=None,
    )
    assert result.ok is False
    assert "credential" in result.detail.lower()


def test_dispatch_table_covers_every_shipped_kind() -> None:
    """The KIND_HELP map in ui/routes_destinations.py is the source of
    truth for the destination kinds we ship. probes._DISPATCH must
    have an entry for every one, or the UI lets operators create
    destinations the test-connection button can't probe."""
    from nl_router.ui.routes_destinations import KIND_HELP

    ui_kinds = set(KIND_HELP.keys())
    probe_kinds = set(probes._DISPATCH.keys())
    missing = ui_kinds - probe_kinds
    assert not missing, f"probes missing entries for: {missing}"
