"""Envelope encryption for credential payloads.

Per the design plan, credential payloads (e.g. GCP service-account JSON,
OAuth bearer tokens, S3 keys) are encrypted at rest using AES-256-GCM
with a master key (KEK) loaded from outside the database. The KEK never
touches Postgres; only ciphertext + per-credential nonce do.

Storage layout
--------------
The `credentials` table has:

    enc_version SMALLINT   -- algorithm version (1 = AES-256-GCM as below)
    nonce       BYTEA      -- 12 bytes per credential, random
    ciphertext  BYTEA      -- AES-256-GCM ciphertext concatenated with the
                           --   16-byte authentication tag.

Format for enc_version=1
------------------------
    nonce       = 12 bytes (NIST-recommended for GCM)
    ciphertext  = encrypt(plaintext) || tag        (16-byte tag appended)
    associated_data = b"nl-router/credentials/v1"  (binds the version into
                                                    AEAD to prevent
                                                    cross-version replay)

Plaintext shape per credential `kind` is enforced by Pydantic in the
routes layer (`kinds.py`); this module deals with bytes-to-bytes only.

KEK sources (file beats env when both set; matches the design plan):
    1. /etc/nl-router/kek.key   (raw 32 bytes or base64url string)
    2. NL_ROUTER_KEK env var    (base64url string)

See `nl_router.config.load_kek()` for the source resolver.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Final

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from nl_router.config import load_kek

# Current algorithm version. Bump when introducing a new format and add
# decryption branches below; encryption always uses CURRENT_ENC_VERSION.
CURRENT_ENC_VERSION: Final[int] = 1

# Associated data bound into AEAD. Domain-separates against other AES-GCM
# uses with the same KEK (none today, but cheap insurance).
_AAD_V1: Final[bytes] = b"nl-router/credentials/v1"

# Standard NIST-recommended GCM nonce size.
_NONCE_BYTES: Final[int] = 12


class CryptoError(Exception):
    """Base for credential crypto errors. Subclasses indicate cause so
    callers can return a useful HTTP status (e.g. 503 when KEK is missing,
    500 when decrypt fails due to corruption)."""


class KEKUnavailableError(CryptoError):
    """No KEK source configured. Credential ops cannot proceed."""


class DecryptError(CryptoError):
    """Ciphertext could not be authenticated/decrypted. Either the KEK has
    rotated without re-encrypting, the row was tampered with, or the
    `enc_version` is from a newer release than this binary knows about."""


@dataclass(frozen=True)
class Envelope:
    """The on-disk shape: (enc_version, nonce, ciphertext+tag).

    Returned by `encrypt()` and consumed by `decrypt()`. The fields map
    1:1 to the `credentials` columns of the same name.
    """

    enc_version: int
    nonce: bytes
    ciphertext: bytes        # includes the AEAD tag suffix


def encrypt(plaintext: bytes) -> Envelope:
    """Encrypt a payload with AES-256-GCM using the configured KEK.

    Generates a fresh random nonce each call. Returns the wire-format
    Envelope ready for INSERT into `credentials`.

    Raises:
        KEKUnavailableError: if no KEK source is configured.
        CryptoError: on unexpected internal failure.
    """
    kek = _load_kek_safe()
    aead = AESGCM(kek)
    nonce = os.urandom(_NONCE_BYTES)
    ct_with_tag = aead.encrypt(nonce, plaintext, _AAD_V1)
    return Envelope(
        enc_version=CURRENT_ENC_VERSION,
        nonce=nonce,
        ciphertext=ct_with_tag,
    )


def decrypt(envelope: Envelope) -> bytes:
    """Decrypt an Envelope using the configured KEK.

    Validates the AEAD tag; any tampering or KEK mismatch raises
    DecryptError. Callers should NOT swallow this — log it and surface
    a 500 / structured error to the operator.

    Raises:
        KEKUnavailableError: if no KEK source is configured.
        DecryptError: on authentication failure or unsupported enc_version.
    """
    if envelope.enc_version != 1:
        raise DecryptError(
            f"unsupported enc_version={envelope.enc_version}; this binary supports 1"
        )
    if len(envelope.nonce) != _NONCE_BYTES:
        raise DecryptError(
            f"nonce length {len(envelope.nonce)} != {_NONCE_BYTES}; row is corrupt"
        )

    kek = _load_kek_safe()
    aead = AESGCM(kek)
    try:
        return aead.decrypt(envelope.nonce, envelope.ciphertext, _AAD_V1)
    except InvalidTag as e:
        raise DecryptError(
            "ciphertext failed authentication — KEK mismatch or row tampered"
        ) from e


# ---- helpers -------------------------------------------------------------


def _load_kek_safe() -> bytes:
    """Wrap `nl_router.config.load_kek()` to translate the runtime error
    into our typed exception. Callers shouldn't have to know about the
    underlying config-layer error type."""
    try:
        return load_kek()
    except RuntimeError as e:
        raise KEKUnavailableError(str(e)) from e
