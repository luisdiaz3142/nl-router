"""Tests for nl_router.dsl — the validate_predicate() helper.

This module is the canonical shellout to nl-dsl-validate; both
api/models.py and the CLI validate-predicate subcommand use it. The
tests cover binary discovery, parse success, parse failure (with
verbatim error pass-through), and the no-binary soft-pass case.
"""

from __future__ import annotations

import pytest

from nl_router import dsl


# ---- Binary discovery ---------------------------------------------------


def test_find_binary_via_env_override(tmp_path, monkeypatch) -> None:
    """NL_ROUTER_DSL_VALIDATE_BIN should win over any canonical path."""
    fake = tmp_path / "nl-dsl-validate-fake"
    fake.write_bytes(b"#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    monkeypatch.setenv(dsl.ENV_OVERRIDE, str(fake))
    assert dsl.find_binary() == str(fake)


def test_find_binary_env_override_missing_file_returns_none(monkeypatch) -> None:
    """If the operator misconfigures the override to a path that doesn't
    exist, we don't silently fall back to the canonical path — we
    return None so they notice."""
    monkeypatch.setenv(dsl.ENV_OVERRIDE, "/no/such/file/anywhere")
    assert dsl.find_binary() is None


def test_find_binary_none_when_nothing_installed(monkeypatch) -> None:
    """No env override, no canonical path, no PATH hit → None."""
    monkeypatch.delenv(dsl.ENV_OVERRIDE, raising=False)
    monkeypatch.setattr(dsl, "CANONICAL_BINARY_PATH", "/no/such/file/anywhere")
    monkeypatch.setattr(dsl.shutil, "which", lambda _: None)
    assert dsl.find_binary() is None


# ---- validate_predicate behavior ----------------------------------------


def test_no_binary_returns_soft_pass(monkeypatch) -> None:
    """Dev mode: no binary installed → ok=True, binary_available=False.
    Callers that want a hard fail (the CLI) inspect binary_available."""
    monkeypatch.setattr(dsl, "find_binary", lambda: None)
    r = dsl.validate_predicate("anything goes")
    assert r.ok is True
    assert r.binary_available is False
    assert r.detail == ""


def test_valid_predicate(dsl_validate_bin, monkeypatch) -> None:
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built")
    monkeypatch.setenv(dsl.ENV_OVERRIDE, dsl_validate_bin)
    r = dsl.validate_predicate('tags.Modality == "CT"')
    assert r.ok is True
    assert r.binary_available is True
    assert r.detail == ""


def test_lowercase_true_valid(dsl_validate_bin, monkeypatch) -> None:
    """M22 regression guard — lowercase booleans should parse."""
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built")
    monkeypatch.setenv(dsl.ENV_OVERRIDE, dsl_validate_bin)
    assert dsl.validate_predicate("true").ok is True
    assert dsl.validate_predicate("false").ok is True


def test_syntax_error_surfaces_parser_message(dsl_validate_bin, monkeypatch) -> None:
    """The parser's line/column message should come through verbatim."""
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built")
    monkeypatch.setenv(dsl.ENV_OVERRIDE, dsl_validate_bin)
    r = dsl.validate_predicate("tags.Modality ==")
    assert r.ok is False
    assert r.binary_available is True
    assert "syntax error" in r.detail
    assert "line" in r.detail and "column" in r.detail


def test_empty_predicate_rejected(dsl_validate_bin, monkeypatch) -> None:
    """The helper writes 'empty predicate' on stderr and exits 1 for an
    empty input — that's the contract callers depend on."""
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built")
    monkeypatch.setenv(dsl.ENV_OVERRIDE, dsl_validate_bin)
    r = dsl.validate_predicate("")
    assert r.ok is False
    assert r.detail.strip() != ""
