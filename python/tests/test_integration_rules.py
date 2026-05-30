"""Integration tests: rule CRUD against a real Postgres.

These exercise the full path FastAPI handler → Pydantic model →
predicate validation shellout → SQL insert → audit emission. The
unit tests in test_models.py cover the Pydantic validator in
isolation; this file proves the same code composes correctly with
the auth dependency, the DB pool, and the audit hook.

Every test depends on the `db` fixture (which depends on `test_db`
which skips when Postgres isn't reachable), so the whole file
auto-skips on a fresh checkout without Postgres — keeping the
unit-test suite green for developers who haven't run `make db-up`.
"""

from __future__ import annotations

import pytest


# Force the DSL parse helper for every test in this file — the route
# handler will pass through to the helper which would auto-skip the
# validation otherwise. Setting NL_ROUTER_DSL_VALIDATE_BIN here means
# "true" gets recognized as a valid predicate.
@pytest.fixture(autouse=True)
def _dsl_bin(dsl_validate_bin, monkeypatch):
    if dsl_validate_bin is None:
        pytest.skip("nl-dsl-validate not built; run cmake --build first")
    monkeypatch.setenv("NL_ROUTER_DSL_VALIDATE_BIN", dsl_validate_bin)


def _auth(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


# ---- create -------------------------------------------------------------


def test_create_rule_round_trip(api_client, admin_token) -> None:
    """POST /api/v1/rules with valid body → 201 with id, then GET
    returns the same record."""
    r = api_client.post(
        "/api/v1/rules",
        json={
            "name":            "route-all",
            "description":     "smoke test",
            "scope":           "study",
            "predicate":       "true",
            "priority":        0,
            "status":          "draft",
            "dispatch_order":  "parallel",
        },
        headers=_auth(admin_token),
    )
    assert r.status_code == 201, r.text
    body = r.json()
    assert body["name"] == "route-all"
    assert body["predicate"] == "true"
    assert isinstance(body["id"], int) and body["id"] > 0
    rule_id = body["id"]

    # Fetch it back through GET.
    r2 = api_client.get(f"/api/v1/rules/{rule_id}", headers=_auth(admin_token))
    assert r2.status_code == 200
    fetched = r2.json()
    assert fetched["name"] == "route-all"
    assert fetched["status"] == "draft"
    assert fetched["scope"] == "study"


def test_create_rule_rejects_invalid_predicate(api_client, admin_token) -> None:
    """The M22 parse-time validation kicks in via the Pydantic model;
    the route handler should return 422 (or 400) with the parser's
    line/column message in the response body."""
    r = api_client.post(
        "/api/v1/rules",
        json={
            "name":      "broken",
            "scope":     "study",
            "predicate": "tags.Modality ==",
            "status":    "draft",
        },
        headers=_auth(admin_token),
    )
    assert r.status_code in (400, 422), r.text
    body = r.text.lower()
    assert "syntax error" in body or "predicate" in body


def test_create_rule_duplicate_name_fails(api_client, admin_token) -> None:
    """rules.name has a UNIQUE constraint; a second POST with the same
    name should fail (likely 400 or 409)."""
    payload = {
        "name":      "dup-name",
        "scope":     "study",
        "predicate": "true",
        "status":    "draft",
    }
    r1 = api_client.post("/api/v1/rules", json=payload, headers=_auth(admin_token))
    assert r1.status_code == 201
    r2 = api_client.post("/api/v1/rules", json=payload, headers=_auth(admin_token))
    # The route handler can map UNIQUE violation to either 400 or 409;
    # both are reasonable. Just assert we didn't get a 201 or 5xx.
    assert 400 <= r2.status_code < 500 and r2.status_code != 401


# ---- list / get ---------------------------------------------------------


def test_list_rules_empty(api_client, admin_token) -> None:
    r = api_client.get("/api/v1/rules", headers=_auth(admin_token))
    assert r.status_code == 200
    assert r.json() == []


def test_list_rules_after_create(api_client, admin_token) -> None:
    api_client.post(
        "/api/v1/rules",
        json={"name": "a", "scope": "study", "predicate": "true", "status": "draft"},
        headers=_auth(admin_token),
    )
    api_client.post(
        "/api/v1/rules",
        json={"name": "b", "scope": "series", "predicate": "true", "status": "enabled"},
        headers=_auth(admin_token),
    )
    r = api_client.get("/api/v1/rules", headers=_auth(admin_token))
    assert r.status_code == 200
    names = sorted(row["name"] for row in r.json())
    assert names == ["a", "b"]


def test_get_rule_404_for_unknown_id(api_client, admin_token) -> None:
    r = api_client.get("/api/v1/rules/99999", headers=_auth(admin_token))
    assert r.status_code == 404


# ---- update / delete ----------------------------------------------------


def test_update_rule_changes_status(api_client, admin_token) -> None:
    created = api_client.post(
        "/api/v1/rules",
        json={"name": "to-flip", "scope": "study", "predicate": "true", "status": "draft"},
        headers=_auth(admin_token),
    )
    rid = created.json()["id"]

    r = api_client.patch(
        f"/api/v1/rules/{rid}",
        json={"status": "enabled"},
        headers=_auth(admin_token),
    )
    assert r.status_code == 200, r.text
    assert r.json()["status"] == "enabled"

    # Verify the change persisted.
    fetched = api_client.get(f"/api/v1/rules/{rid}", headers=_auth(admin_token))
    assert fetched.json()["status"] == "enabled"


def test_delete_rule_removes_it(api_client, admin_token) -> None:
    created = api_client.post(
        "/api/v1/rules",
        json={"name": "to-delete", "scope": "study", "predicate": "true", "status": "draft"},
        headers=_auth(admin_token),
    )
    rid = created.json()["id"]

    r = api_client.delete(f"/api/v1/rules/{rid}", headers=_auth(admin_token))
    assert r.status_code == 204

    fetched = api_client.get(f"/api/v1/rules/{rid}", headers=_auth(admin_token))
    assert fetched.status_code == 404


# ---- auth ---------------------------------------------------------------


def test_create_rule_without_token_is_401(api_client) -> None:
    """No Authorization header → 401 from the auth dep."""
    r = api_client.post(
        "/api/v1/rules",
        json={"name": "x", "scope": "study", "predicate": "true", "status": "draft"},
    )
    assert r.status_code == 401


def test_create_rule_with_bogus_token_is_401(api_client) -> None:
    r = api_client.post(
        "/api/v1/rules",
        json={"name": "x", "scope": "study", "predicate": "true", "status": "draft"},
        headers=_auth("nlr_totally-fake-token"),
    )
    assert r.status_code == 401


# ---- audit emission ------------------------------------------------------


def test_create_rule_emits_audit_row(api_client, admin_token, db) -> None:
    """Every state-changing call should land a row in admin_audit. The
    diff captures structural facts (name, predicate) so the audit page
    is meaningful without re-running anything."""
    api_client.post(
        "/api/v1/rules",
        json={"name": "audited", "scope": "study", "predicate": "true", "status": "draft"},
        headers=_auth(admin_token),
    )

    import psycopg
    with psycopg.connect(db, autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT action, resource_kind, resource_id "
                "FROM admin_audit ORDER BY id DESC LIMIT 1"
            )
            row = cur.fetchone()
    assert row is not None
    action, resource_kind, resource_id = row
    assert action == "rule.create"
    assert resource_kind == "rule"
    assert int(resource_id) > 0
