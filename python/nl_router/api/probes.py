"""Per-kind destination probes for the "Test connection" button.

Each probe takes a destination's config + optional decrypted credential
payload and runs a minimum-viable connectivity check against the target.
On success it returns the elapsed time; on failure it returns a short
operator-readable detail message. No state is mutated; probes never
write real DICOM, never POST real instances, never PUT real objects.

Probes are deliberately tight on timeout (default 8 s) — the UI button
shouldn't hang the operator on a dead host. Network errors,
authentication failures, and permission denials all collapse to
ProbeResult(ok=False, detail=...) — callers don't need to know about
the underlying exception type.

The `dicom` kind needs a DCMTK SCU for C-ECHO, which only the dispatcher
process has linked in today. v1 returns 'not supported via API' for
that kind and points operators at storescu/echoscu. A follow-up either
ships a small nl-dcm-probe helper binary or exposes the dispatcher's
own handler.probe() over HTTP.

Threading: probes are sync (httpx.Client, boto3, blocking I/O). FastAPI
runs sync route handlers in a thread pool, so calling these from a
route is fine.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlparse

import httpx

from nl_router import crypto


# ---- result type ---------------------------------------------------------

@dataclass(frozen=True)
class ProbeResult:
    """Outcome of one probe.

    `ok`         : True on a clean success.
    `detail`     : short message — error reason, HTTP status, etc.
    `elapsed_ms` : wall-clock duration of the probe call.
    `kind`       : destination kind that ran (for the UI / audit).
    """

    ok: bool
    detail: str
    elapsed_ms: int
    kind: str


# ---- entry point ---------------------------------------------------------

DEFAULT_TIMEOUT_S = 8.0


def probe_destination(
    *,
    kind: str,
    config: dict[str, Any],
    credential: dict[str, Any] | None,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> ProbeResult:
    """Dispatch on `kind` to the right per-kind probe.

    `credential` is the *decrypted* payload, already JSON-parsed. The
    caller (the API route) is responsible for the decrypt step — that
    keeps the probe layer free of DB and KEK access.
    """
    fn = _DISPATCH.get(kind)
    if fn is None:
        return ProbeResult(
            ok=False,
            detail=f"unknown destination kind {kind!r}; no probe registered",
            elapsed_ms=0,
            kind=kind,
        )
    start = time.perf_counter()
    try:
        return fn(config=config, credential=credential, timeout_s=timeout_s)
    except Exception as e:                              # noqa: BLE001 — caller wants a clean message
        elapsed = int((time.perf_counter() - start) * 1000)
        return ProbeResult(
            ok=False,
            detail=f"{type(e).__name__}: {e}",
            elapsed_ms=elapsed,
            kind=kind,
        )


# ---- helpers -------------------------------------------------------------


def decrypt_credential_payload(
    *, enc_version: int, nonce: bytes, ciphertext: bytes
) -> dict[str, Any]:
    """Decrypt a credentials row's payload to a dict.

    Lives here (not in probes per se) because every probe that needs a
    credential calls this with the same column tuple. Raises whatever
    nl_router.crypto raises — the API route catches and turns it into a
    ProbeResult(ok=False).
    """
    env = crypto.Envelope(enc_version=enc_version, nonce=nonce, ciphertext=ciphertext)
    plaintext = crypto.decrypt(env)
    obj = json.loads(plaintext.decode("utf-8"))
    if not isinstance(obj, dict):
        raise ValueError("credential payload is not a JSON object")
    return obj


# ---- per-kind probes -----------------------------------------------------


_DCM_PROBE_BIN_CANONICAL = "/usr/libexec/nl-router/nl-dcm-probe"
_DCM_PROBE_BIN_ENV = "NL_ROUTER_DCM_PROBE_BIN"


def _find_dcm_probe_binary() -> str | None:
    """Locate nl-dcm-probe (M31 helper). Same discovery as nl-dsl-validate.

    Env override → canonical .deb path → PATH lookup. Returns None when
    no copy is installed; in that mode `_probe_dicom` falls back to the
    pre-M31 TCP-only behavior so dev setups without a C++ build still
    get a meaningful answer.
    """
    override = os.environ.get(_DCM_PROBE_BIN_ENV)
    if override:
        return override if os.path.isfile(override) else None
    if os.path.isfile(_DCM_PROBE_BIN_CANONICAL):
        return _DCM_PROBE_BIN_CANONICAL
    return shutil.which("nl-dcm-probe")


def _probe_dicom(*, config, credential, timeout_s) -> ProbeResult:
    """Real DIMSE C-ECHO probe via the nl-dcm-probe helper (M31).

    Replaces the M19 TCP-only check. Falls back to TCP-only when the
    helper isn't installed (dev mode without a C++ build) or when
    TLS is configured (M31 helper doesn't speak TLS yet — deferred
    with the rest of DICOM TLS).
    """
    host = config.get("host")
    port = int(config.get("port", 0))
    if not host or not port:
        return ProbeResult(
            ok=False,
            detail="config must include host + port",
            elapsed_ms=0,
            kind="dicom",
        )
    called_aet  = config.get("called_aet")
    calling_aet = config.get("calling_aet")
    if not called_aet or not calling_aet:
        return ProbeResult(
            ok=False,
            detail="config must include called_aet + calling_aet",
            elapsed_ms=0,
            kind="dicom",
        )

    binary = _find_dcm_probe_binary()
    tls_enabled = bool(config.get("tls", False))
    if binary is None or tls_enabled:
        # Fall back to TCP-only — same as pre-M31. Reasons documented
        # in detail message so operators know why this isn't a real
        # C-ECHO.
        return _probe_dicom_tcp_only(
            host=host, port=port, timeout_s=timeout_s,
            why=("TLS configured (helper doesn't speak TLS yet)" if tls_enabled
                 else "nl-dcm-probe helper not installed"),
        )

    max_pdu = int(config.get("max_pdu_size", 131072))
    cmd = [
        binary,
        host, str(port),
        called_aet, calling_aet,
        str(int(timeout_s)),
        str(max_pdu),
    ]

    start = time.perf_counter()
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout_s + 2,        # extra grace for process teardown
        )
    except subprocess.TimeoutExpired:
        elapsed = int((time.perf_counter() - start) * 1000)
        return ProbeResult(
            ok=False,
            detail=f"C-ECHO to {host}:{port} timed out after {timeout_s}s",
            elapsed_ms=elapsed,
            kind="dicom",
        )

    elapsed = int((time.perf_counter() - start) * 1000)
    if result.returncode == 0:
        return ProbeResult(
            ok=True,
            detail=f"C-ECHO to {host}:{port} succeeded ({called_aet} ← {calling_aet})",
            elapsed_ms=elapsed,
            kind="dicom",
        )
    # nl-dcm-probe writes a single-line stderr message on failure; pass
    # verbatim so the operator sees the DCMTK-level reason ("Connection
    # refused", "Association Rejected", etc.).
    err = result.stderr.decode("utf-8", errors="replace").strip()
    return ProbeResult(
        ok=False,
        detail=f"C-ECHO to {host}:{port} failed: {err or 'unknown (exit ' + str(result.returncode) + ')'}",
        elapsed_ms=elapsed,
        kind="dicom",
    )


def _probe_dicom_tcp_only(*, host: str, port: int, timeout_s: float, why: str) -> ProbeResult:
    """Pre-M31 fallback: TCP socket connect, no DIMSE handshake.

    Kept as a fallback for (a) dev setups without a C++ build and
    (b) TLS-enabled destinations until nl-dcm-probe learns TLS.
    """
    start = time.perf_counter()
    try:
        with socket.create_connection((host, port), timeout=timeout_s):
            elapsed = int((time.perf_counter() - start) * 1000)
            return ProbeResult(
                ok=True,
                detail=(
                    f"TCP connect to {host}:{port} succeeded — "
                    f"DIMSE check skipped ({why})."
                ),
                elapsed_ms=elapsed,
                kind="dicom",
            )
    except (socket.timeout, OSError) as e:
        elapsed = int((time.perf_counter() - start) * 1000)
        return ProbeResult(
            ok=False,
            detail=f"TCP connect to {host}:{port} failed: {e}",
            elapsed_ms=elapsed,
            kind="dicom",
        )


def _probe_http(*, config, credential, timeout_s) -> ProbeResult:
    """Generic HTTP webhook: OPTIONS against the literal URL.

    The `url_template` may contain ${TagName} placeholders. We use the
    template verbatim — substituting placeholders with empty strings
    would point at a different URL and defeat the test. The probe
    asserts "the target server answers HTTP," not "the templated URL
    for some study works."
    """
    template = config.get("url_template") or config.get("url")
    if not template:
        return ProbeResult(
            ok=False,
            detail="config must include url_template (or url)",
            elapsed_ms=0,
            kind="http",
        )
    headers = dict(config.get("headers") or {})
    auth = _http_auth(credential)
    start = time.perf_counter()
    with httpx.Client(timeout=timeout_s, follow_redirects=False) as client:
        r = client.request("OPTIONS", template, headers=headers, auth=auth)
    elapsed = int((time.perf_counter() - start) * 1000)
    # 2xx / 3xx / 405 (method not allowed for OPTIONS) are all "server
    # is reachable + responsive." Most webhooks won't implement OPTIONS,
    # so 405 is the common-and-fine case.
    if r.status_code < 400 or r.status_code in (401, 403, 405):
        return ProbeResult(
            ok=(r.status_code < 400 or r.status_code == 405),
            detail=f"OPTIONS {template} → {r.status_code}",
            elapsed_ms=elapsed,
            kind="http",
        )
    return ProbeResult(
        ok=False,
        detail=f"OPTIONS {template} → {r.status_code}",
        elapsed_ms=elapsed,
        kind="http",
    )


def _probe_dicomweb_stow(*, config, credential, timeout_s) -> ProbeResult:
    """DICOMweb STOW-RS: probe via a minimal QIDO query against the
    same base URL. We trim the trailing `/studies` (if any) and append
    `/studies?limit=0`. Any 2xx / 3xx counts as "endpoint is alive."
    """
    url = config.get("url")
    if not url:
        return ProbeResult(
            ok=False,
            detail="config must include url",
            elapsed_ms=0,
            kind="dicomweb_stow",
        )
    base = url.rstrip("/")
    if base.endswith("/studies"):
        base = base[: -len("/studies")]
    probe_url = f"{base}/studies?limit=0"
    headers = {"Accept": "application/dicom+json"}
    auth = _http_auth(credential)
    start = time.perf_counter()
    with httpx.Client(timeout=timeout_s, follow_redirects=False) as client:
        r = client.get(probe_url, headers=headers, auth=auth)
    elapsed = int((time.perf_counter() - start) * 1000)
    if r.status_code < 400:
        return ProbeResult(
            ok=True,
            detail=f"GET {probe_url} → {r.status_code}",
            elapsed_ms=elapsed,
            kind="dicomweb_stow",
        )
    return ProbeResult(
        ok=False,
        detail=f"GET {probe_url} → {r.status_code} {r.reason_phrase}",
        elapsed_ms=elapsed,
        kind="dicomweb_stow",
    )


def _probe_gcp_dicomweb(*, config, credential, timeout_s) -> ProbeResult:
    """GCP Healthcare DICOMweb: mint an OAuth2 access token from the
    service-account credential, then QIDO-probe the configured store.
    A real failure path here is "wrong project / store name / role" —
    catching that at config time saves a real study from failing.
    """
    if credential is None:
        return ProbeResult(
            ok=False,
            detail="gcp_dicomweb destinations require a gcp_service_account credential",
            elapsed_ms=0,
            kind="gcp_dicomweb",
        )

    project_id = config.get("project_id")
    location = config.get("location")
    dataset = config.get("dataset")
    dicom_store = config.get("dicom_store")
    if not all((project_id, location, dataset, dicom_store)):
        return ProbeResult(
            ok=False,
            detail="config must include project_id, location, dataset, dicom_store",
            elapsed_ms=0,
            kind="gcp_dicomweb",
        )

    scope = config.get("scope", "https://www.googleapis.com/auth/cloud-platform")
    start = time.perf_counter()
    token = _gcp_access_token(credential, scope, timeout_s)
    qido = (
        f"https://healthcare.googleapis.com/v1/projects/{project_id}/locations/{location}"
        f"/datasets/{dataset}/dicomStores/{dicom_store}/dicomWeb/studies?limit=0"
    )
    with httpx.Client(timeout=timeout_s, follow_redirects=False) as client:
        r = client.get(
            qido,
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/dicom+json",
            },
        )
    elapsed = int((time.perf_counter() - start) * 1000)
    if r.status_code < 400:
        return ProbeResult(
            ok=True,
            detail=f"QIDO → {r.status_code} (access token minted)",
            elapsed_ms=elapsed,
            kind="gcp_dicomweb",
        )
    return ProbeResult(
        ok=False,
        detail=f"QIDO → {r.status_code} {r.text[:200]}",
        elapsed_ms=elapsed,
        kind="gcp_dicomweb",
    )


def _probe_file(*, config, credential, timeout_s) -> ProbeResult:
    """Local FS write probe: create a sentinel file in the templated
    path's parent, then delete it. Templating is bypassed (placeholders
    left as literal); we just check `dirname(path_template)` is writable
    by the API process.

    Caveat: 'file' destinations are written by the dispatcher, which may
    or may not be the same process as the API. In a single-node
    deployment (what we have today) they share a FS; in a split
    deployment this probe is misleading — see INSTALL.md.
    """
    template = config.get("path_template") or config.get("path")
    if not template:
        return ProbeResult(
            ok=False,
            detail="config must include path_template (or path)",
            elapsed_ms=0,
            kind="file",
        )
    # Strip the template variables to get a real parent directory.
    # ${X} → '' for the probe; the goal is "can the FS be written to,"
    # not "does the templated final path exist."
    base = template.split("${", 1)[0] or os.path.dirname(template) or "/"
    parent = base if os.path.isdir(base) else os.path.dirname(base.rstrip("/")) or "/"
    start = time.perf_counter()
    try:
        os.makedirs(parent, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            prefix=".nl-router-probe-", dir=parent, delete=True
        ):
            pass
    except OSError as e:
        elapsed = int((time.perf_counter() - start) * 1000)
        return ProbeResult(
            ok=False,
            detail=f"write probe in {parent!r} failed: {e}",
            elapsed_ms=elapsed,
            kind="file",
        )
    elapsed = int((time.perf_counter() - start) * 1000)
    return ProbeResult(
        ok=True,
        detail=(
            f"wrote+removed probe file in {parent!r}. "
            f"(Note: dispatcher runs as the nl-router user — check it has write perms too.)"
        ),
        elapsed_ms=elapsed,
        kind="file",
    )


def _probe_object_storage(*, config, credential, timeout_s) -> ProbeResult:
    """S3-compatible HeadBucket via boto3. AWS, GCS-XML, MinIO — all
    accept the same call shape, so one probe covers every endpoint
    the dispatcher's handler does.
    """
    if credential is None:
        return ProbeResult(
            ok=False,
            detail="object_storage destinations require an aws_keys credential",
            elapsed_ms=0,
            kind="object_storage",
        )
    bucket = config.get("bucket")
    endpoint = config.get("endpoint")
    region = config.get("region", "us-east-1")
    if not bucket:
        return ProbeResult(
            ok=False,
            detail="config must include bucket",
            elapsed_ms=0,
            kind="object_storage",
        )

    # Imported lazily so installs that don't use object_storage don't
    # pay the boto3 import cost on every API process startup.
    import boto3                                       # noqa: PLC0415
    from botocore.config import Config as BotoConfig  # noqa: PLC0415
    from botocore.exceptions import ClientError       # noqa: PLC0415

    start = time.perf_counter()
    s3 = boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name=region,
        aws_access_key_id=credential.get("access_key_id"),
        aws_secret_access_key=credential.get("secret_access_key"),
        aws_session_token=credential.get("session_token"),
        config=BotoConfig(
            connect_timeout=timeout_s,
            read_timeout=timeout_s,
            retries={"max_attempts": 1},
        ),
    )
    try:
        s3.head_bucket(Bucket=bucket)
    except ClientError as e:
        elapsed = int((time.perf_counter() - start) * 1000)
        return ProbeResult(
            ok=False,
            detail=f"HeadBucket({bucket}) → {e}",
            elapsed_ms=elapsed,
            kind="object_storage",
        )
    elapsed = int((time.perf_counter() - start) * 1000)
    return ProbeResult(
        ok=True,
        detail=f"HeadBucket({bucket}) succeeded",
        elapsed_ms=elapsed,
        kind="object_storage",
    )


# ---- small auth + token helpers -----------------------------------------


def _http_auth(credential: dict[str, Any] | None) -> Any:
    """Translate a decrypted credential payload into the right
    httpx.Auth shape — or None if no auth is configured.

    Recognized kinds (matches dispatcher/credential.cpp):
      - basic_http   → BasicAuth(user, pw)
      - bearer_token → Bearer header via httpx's `auth=(... )` would be
                        wrong; instead we synthesize a header. Returned as
                        a small functor.
      - api_key      → header-mode only here; query-mode is rare and
                        the value would go in the URL — out of scope for
                        a probe.
    """
    if credential is None:
        return None
    # The credentials table doesn't ship `kind` to the probe layer
    # explicitly; we infer from the payload shape.
    if "username" in credential and "password" in credential:
        return httpx.BasicAuth(credential["username"], credential["password"])
    if "token" in credential:
        # httpx supports a callable `auth` that mutates each request.
        def _bearer(request):
            request.headers["Authorization"] = f"Bearer {credential['token']}"
            return request
        return _bearer
    if "header" in credential and "value" in credential:
        def _hdr(request):
            request.headers[credential["header"]] = credential["value"]
            return request
        return _hdr
    return None


def _gcp_access_token(
    sa: dict[str, Any], scope: str, timeout_s: float
) -> str:
    """Mint a Google OAuth2 access token from a service-account JSON.

    Standard JWT-Bearer flow:
      1. Build a signed JWT claiming `iss=client_email`, `scope=...`,
         `aud=https://oauth2.googleapis.com/token`, 1-hour expiry.
      2. POST to the token endpoint with grant_type=jwt-bearer.
      3. Read back `access_token` from the JSON response.

    Lifted from the dispatcher's gcp_oauth.cpp logic; same algorithm,
    different language. The token isn't cached here — each probe call
    mints fresh. That's wasteful at real dispatch time but fine for a
    button-press.
    """
    import base64                                              # noqa: PLC0415
    from cryptography.hazmat.primitives import hashes, serialization  # noqa: PLC0415
    from cryptography.hazmat.primitives.asymmetric import padding as rsa_padding  # noqa: PLC0415

    private_key_pem = sa.get("private_key", "").encode("utf-8")
    client_email = sa["client_email"]
    token_uri = sa.get("token_uri", "https://oauth2.googleapis.com/token")

    now = int(time.time())
    header = {"alg": "RS256", "typ": "JWT", "kid": sa.get("private_key_id")}
    claim = {
        "iss":   client_email,
        "scope": scope,
        "aud":   token_uri,
        "iat":   now,
        "exp":   now + 3600,
    }
    def b64(d: dict) -> bytes:
        return base64.urlsafe_b64encode(
            json.dumps(d, separators=(",", ":")).encode("utf-8")
        ).rstrip(b"=")
    signing_input = b64(header) + b"." + b64(claim)
    key = serialization.load_pem_private_key(private_key_pem, password=None)
    signature = key.sign(signing_input, rsa_padding.PKCS1v15(), hashes.SHA256())
    jwt = (signing_input + b"." + base64.urlsafe_b64encode(signature).rstrip(b"=")).decode("ascii")

    with httpx.Client(timeout=timeout_s) as client:
        r = client.post(
            token_uri,
            data={
                "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
                "assertion":  jwt,
            },
        )
    if r.status_code >= 400:
        raise RuntimeError(f"token exchange → {r.status_code} {r.text[:200]}")
    tok = r.json().get("access_token")
    if not tok:
        raise RuntimeError("token endpoint returned no access_token")
    return tok


# ---- dispatch table ------------------------------------------------------

_DISPATCH = {
    "dicom":          _probe_dicom,
    "http":           _probe_http,
    "dicomweb_stow":  _probe_dicomweb_stow,
    "gcp_dicomweb":   _probe_gcp_dicomweb,
    "file":           _probe_file,
    "object_storage": _probe_object_storage,
}
