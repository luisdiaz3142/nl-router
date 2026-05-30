"""Tests for nl_router.crypto.

Round-trip correctness, nonce uniqueness, and tamper detection. The
KEK is set via the test_kek_env fixture so we don't touch any real
key file.
"""

from __future__ import annotations

import pytest

from nl_router import crypto


pytestmark = pytest.mark.usefixtures("test_kek_env")


def test_encrypt_decrypt_roundtrip() -> None:
    plaintext = b'{"username": "alice", "password": "hunter2"}'
    envelope = crypto.encrypt(plaintext)
    assert envelope.enc_version == crypto.CURRENT_ENC_VERSION
    assert len(envelope.nonce) == 12         # AES-GCM standard
    assert envelope.ciphertext != plaintext  # actually encrypted
    assert crypto.decrypt(envelope) == plaintext


def test_encrypt_two_calls_produce_distinct_nonces() -> None:
    """AES-GCM nonce reuse is catastrophic — make sure encrypt() draws
    a fresh nonce every call. We don't try to prove cryptographic
    randomness (that's the OS's job) but a basic distinctness check
    catches a regression where someone made the nonce a constant."""
    a = crypto.encrypt(b"same plaintext")
    b = crypto.encrypt(b"same plaintext")
    assert a.nonce != b.nonce
    assert a.ciphertext != b.ciphertext


def test_decrypt_tampered_ciphertext_raises() -> None:
    """The AEAD tag should catch any single-byte flip."""
    env = crypto.encrypt(b"sensitive")
    tampered = crypto.Envelope(
        enc_version=env.enc_version,
        nonce=env.nonce,
        # Flip the last byte (which is part of the GCM tag, always last 16).
        ciphertext=env.ciphertext[:-1] + bytes([env.ciphertext[-1] ^ 0x01]),
    )
    with pytest.raises(crypto.DecryptError):
        crypto.decrypt(tampered)


def test_decrypt_wrong_nonce_raises() -> None:
    """Even with a matching ciphertext, swapping the nonce fails."""
    env = crypto.encrypt(b"sensitive")
    swapped = crypto.Envelope(
        enc_version=env.enc_version,
        nonce=bytes(12),                     # all-zero nonce
        ciphertext=env.ciphertext,
    )
    with pytest.raises(crypto.DecryptError):
        crypto.decrypt(swapped)


def test_decrypt_rejects_unknown_enc_version() -> None:
    env = crypto.encrypt(b"sensitive")
    # Construct a forged envelope claiming v99 — the decrypt path
    # should refuse before even reaching AEAD.
    forged = crypto.Envelope(
        enc_version=99,
        nonce=env.nonce,
        ciphertext=env.ciphertext,
    )
    with pytest.raises(crypto.DecryptError):
        crypto.decrypt(forged)
