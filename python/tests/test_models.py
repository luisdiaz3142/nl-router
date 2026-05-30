"""Tests for nl_router.api.models — predicate validation in particular.

Covers both the structural pre-flight (length, paren balance, quote
balance, depth) and the M22 DSL-parse shellout. The DSL shellout
tests skip when nl-dsl-validate isn't built locally.
"""

from __future__ import annotations

import os

import pytest

from nl_router.api import models
from nl_router.api.models import RuleBase, _validate_predicate_text


# ---- Structural checks --------------------------------------------------


def test_accepts_valid_predicate(monkeypatch) -> None:
    # Force "no helper present" so we exercise only the structural
    # path here — DSL-parse cases are below in their own test.
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    out = _validate_predicate_text('tags.Modality == "CT"')
    assert out == 'tags.Modality == "CT"'


def test_rejects_oversize(monkeypatch) -> None:
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    too_big = "x" * (models._MAX_PREDICATE_LEN + 1)
    with pytest.raises(ValueError, match="exceeds maximum length"):
        _validate_predicate_text(too_big)


def test_rejects_unbalanced_open_paren(monkeypatch) -> None:
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    with pytest.raises(ValueError, match="unbalanced parentheses"):
        _validate_predicate_text("(tags.X == 1")


def test_rejects_unbalanced_close_paren(monkeypatch) -> None:
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    with pytest.raises(ValueError, match="unbalanced closing parenthesis"):
        _validate_predicate_text("tags.X == 1)")


def test_rejects_unterminated_string(monkeypatch) -> None:
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    with pytest.raises(ValueError, match="unterminated"):
        _validate_predicate_text('tags.X == "missing-close')


def test_rejects_excessive_depth(monkeypatch) -> None:
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    deep = "(" * (models._MAX_PREDICATE_DEPTH + 1) + "1" + ")" * (
        models._MAX_PREDICATE_DEPTH + 1
    )
    with pytest.raises(ValueError, match="nesting depth"):
        _validate_predicate_text(deep)


def test_paren_inside_string_doesnt_count(monkeypatch) -> None:
    """A `(` inside a string literal should not bump the paren depth."""
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    # Two literal parens inside a string; outer expression is just a
    # string literal. Should pass structural checks.
    out = _validate_predicate_text('"contains ( two ) parens"')
    assert out == '"contains ( two ) parens"'


# ---- DSL parse shellout -------------------------------------------------


def test_dsl_parse_accepts_lowercase_true(dsl_validate_bin, monkeypatch) -> None:
    """M22 regression guard: `predicate: true` parses now."""
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)
    out = _validate_predicate_text("true")
    assert out == "true"


def test_dsl_parse_accepts_capitalized_true(dsl_validate_bin, monkeypatch) -> None:
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)
    out = _validate_predicate_text("True")
    assert out == "True"


def test_dsl_parse_rejects_syntax_error(dsl_validate_bin, monkeypatch) -> None:
    """An incomplete predicate fails with the parser's own message."""
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)
    with pytest.raises(ValueError, match="syntax error"):
        _validate_predicate_text("tags.Modality ==")


def test_dsl_helper_missing_falls_through(monkeypatch, caplog) -> None:
    """When the binary isn't installed (dev mode), the validator logs
    and proceeds — the router still catches it on cache refresh."""
    monkeypatch.setattr(models, "_dsl_validate_binary", lambda: None)
    # An obviously invalid predicate STRUCTURALLY passes (paren-balanced,
    # short enough) — without the helper we accept it. Router log is
    # the second line of defense in this mode.
    out = _validate_predicate_text("nonsense gibberish syntax")
    assert out == "nonsense gibberish syntax"


# ---- RuleBase model integration ----------------------------------------


def test_rule_base_round_trip(dsl_validate_bin, monkeypatch) -> None:
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)

    r = RuleBase(
        name="route-all",
        description="test",
        scope="study",
        predicate="true",
        priority=10,
        status="enabled",
        dispatch_order="parallel",
    )
    assert r.predicate == "true"
    assert r.status == "enabled"
    assert r.priority == 10


def test_rule_base_rejects_invalid_predicate(dsl_validate_bin, monkeypatch) -> None:
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)
    import pydantic
    with pytest.raises(pydantic.ValidationError):
        RuleBase(
            name="bad",
            scope="study",
            predicate="tags.Modality ==",
        )
