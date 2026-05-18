"""UI: rules list + create/edit + delete.

Predicate preflight validation runs inline on form submit — the user
sees the same errors the API would return, before the page bounces.
"""

from __future__ import annotations

import json  # noqa: F401  (used in _audit; kept here to make intent obvious)
from typing import Annotated

from fastapi import APIRouter, Depends, Form, HTTPException, Request, status
from fastapi.responses import RedirectResponse, Response

from nl_router.api.auth import AuthContext
from nl_router.api.models import _validate_predicate_text
from nl_router.db import pool
from nl_router.ui.auth import ui_auth_required
from nl_router.ui.common import pill, render, set_flash


router = APIRouter(prefix="/ui/rules", tags=["ui"], include_in_schema=False)


# ---- List --------------------------------------------------------------


@router.get("", response_class=Response)
async def list_rules(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT r.id, r.name, r.description, r.scope::text AS scope,
                   r.status::text AS status, r.priority, r.predicate,
                   r.dispatch_order, r.updated_at,
                   (SELECT COUNT(*) FROM rule_destinations rd WHERE rd.rule_id = r.id) AS dest_count
              FROM rules r
             ORDER BY r.priority DESC, r.name
        """)
        rows = [{**r, "pill": pill(r["status"])} for r in cur.fetchall()]

    return render(request, "rules_list.html", auth=auth, rules=rows)


# ---- Create ------------------------------------------------------------


@router.get("/new", response_class=Response)
async def new_rule_page(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    return render(
        request, "rules_form.html",
        auth=auth, mode="create", rule=_empty_rule(), errors={},
    )


@router.post("", response_class=Response)
async def create_rule(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name: Annotated[str, Form()],
    predicate: Annotated[str, Form()],
    scope: Annotated[str, Form()] = "study",
    description: Annotated[str, Form()] = "",
    priority: Annotated[int, Form()] = 0,
    status_: Annotated[str, Form(alias="status")] = "draft",
    dispatch_order: Annotated[str, Form()] = "parallel",
):
    errors = _validate_rule_form(name=name, predicate=predicate, scope=scope,
                                  priority=priority, status=status_,
                                  dispatch_order=dispatch_order)
    if errors:
        return render(
            request, "rules_form.html",
            auth=auth, mode="create",
            rule={"name": name, "description": description, "scope": scope,
                  "predicate": predicate, "priority": priority,
                  "status": status_, "dispatch_order": dispatch_order},
            errors=errors,
        )

    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO rules (
                    name, description, scope, predicate, priority, status,
                    dispatch_order, created_by, updated_by
                ) VALUES (%s, %s, %s::rule_scope, %s, %s, %s::rule_status, %s, %s, %s)
                RETURNING id
            """, (name, description or None, scope, predicate, priority, status_,
                  dispatch_order, str(auth), str(auth)))
            new_id = cur.fetchone()["id"]
            _audit(cur, auth, "rule.create", "rule", new_id,
                   {"after": {"name": name, "scope": scope, "status": status_,
                              "priority": priority}})
            conn.commit()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                errors["name"] = f"A rule named {name!r} already exists."
            else:
                errors["__form__"] = f"Database error: {e}"
            return render(
                request, "rules_form.html",
                auth=auth, mode="create",
                rule={"name": name, "description": description, "scope": scope,
                      "predicate": predicate, "priority": priority,
                      "status": status_, "dispatch_order": dispatch_order},
                errors=errors,
            )

    resp = RedirectResponse(url=f"/ui/rules/{new_id}", status_code=303)
    set_flash(resp, f"Rule {name!r} created.")
    return resp


# ---- Edit --------------------------------------------------------------


@router.get("/{rid}", response_class=Response)
async def edit_rule_page(
    rid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, scope::text AS scope, predicate,
                   priority, status::text AS status, dispatch_order,
                   created_at, updated_at
              FROM rules
             WHERE id = %s
        """, (rid,))
        rule = cur.fetchone()
        if not rule:
            raise HTTPException(404, "rule not found")

        cur.execute("""
            SELECT rd.destination_id, rd.ordinal, d.name, d.kind,
                   d.enabled
              FROM rule_destinations rd
              JOIN destinations d ON d.id = rd.destination_id
             WHERE rd.rule_id = %s
             ORDER BY rd.ordinal, d.name
        """, (rid,))
        bindings = list(cur.fetchall())

        cur.execute("""
            SELECT id, name, kind, enabled
              FROM destinations
             WHERE id NOT IN (
                SELECT destination_id FROM rule_destinations WHERE rule_id = %s
             )
             ORDER BY name
        """, (rid,))
        available_destinations = list(cur.fetchall())

    return render(
        request, "rules_form.html",
        auth=auth, mode="edit", rule=rule, errors={},
        bindings=bindings, available_destinations=available_destinations,
    )


@router.post("/{rid}", response_class=Response)
async def update_rule(
    rid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name: Annotated[str, Form()],
    predicate: Annotated[str, Form()],
    scope: Annotated[str, Form()] = "study",
    description: Annotated[str, Form()] = "",
    priority: Annotated[int, Form()] = 0,
    status_: Annotated[str, Form(alias="status")] = "draft",
    dispatch_order: Annotated[str, Form()] = "parallel",
):
    errors = _validate_rule_form(name=name, predicate=predicate, scope=scope,
                                  priority=priority, status=status_,
                                  dispatch_order=dispatch_order)
    if errors:
        # Re-fetch bindings so the form stays whole.
        with pool().connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT id FROM rules WHERE id = %s", (rid,))
            if not cur.fetchone():
                raise HTTPException(404, "rule not found")
        return render(
            request, "rules_form.html",
            auth=auth, mode="edit",
            rule={"id": rid, "name": name, "description": description,
                  "scope": scope, "predicate": predicate, "priority": priority,
                  "status": status_, "dispatch_order": dispatch_order},
            errors=errors,
            bindings=[], available_destinations=[],
        )

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT * FROM rules WHERE id = %s", (rid,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "rule not found")

        cur.execute("""
            UPDATE rules SET
                name = %s, description = %s,
                scope = %s::rule_scope, predicate = %s,
                priority = %s, status = %s::rule_status,
                dispatch_order = %s,
                updated_by = %s, updated_at = NOW()
             WHERE id = %s
        """, (name, description or None, scope, predicate, priority,
              status_, dispatch_order, str(auth), rid))
        _audit(cur, auth, "rule.update", "rule", rid, {
            "before": {"name": prior["name"], "predicate": prior["predicate"],
                       "status": str(prior["status"])},
            "after":  {"name": name, "predicate": predicate, "status": status_},
        })
        conn.commit()

    resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
    set_flash(resp, f"Rule {name!r} saved.")
    return resp


# ---- Delete ------------------------------------------------------------


@router.post("/{rid}/delete", response_class=Response)
async def delete_rule(
    rid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT name FROM rules WHERE id = %s", (rid,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "rule not found")
        cur.execute("DELETE FROM rules WHERE id = %s", (rid,))
        _audit(cur, auth, "rule.delete", "rule", rid,
               {"before": {"name": prior["name"]}})
        conn.commit()

    resp = RedirectResponse(url="/ui/rules", status_code=303)
    set_flash(resp, f"Rule {prior['name']!r} deleted.")
    return resp


# ---- Bindings ----------------------------------------------------------


@router.post("/{rid}/destinations", response_class=Response)
async def add_binding(
    rid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    destination_id: Annotated[int, Form()],
    ordinal: Annotated[int, Form()] = 0,
):
    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO rule_destinations (rule_id, destination_id, ordinal)
                VALUES (%s, %s, %s)
                ON CONFLICT (rule_id, destination_id) DO UPDATE SET ordinal = EXCLUDED.ordinal
            """, (rid, destination_id, ordinal))
            _audit(cur, auth, "rule.bind_destination", "rule", rid,
                   {"after": {"destination_id": destination_id, "ordinal": ordinal}})
            conn.commit()
        except Exception as e:
            conn.rollback()
            resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
            set_flash(resp, f"Could not bind destination: {e}", "err")
            return resp

    resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
    set_flash(resp, "Destination bound.")
    return resp


@router.post("/{rid}/destinations/{did}/unbind", response_class=Response)
async def remove_binding(
    rid: int, did: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "DELETE FROM rule_destinations WHERE rule_id = %s AND destination_id = %s",
            (rid, did),
        )
        _audit(cur, auth, "rule.unbind_destination", "rule", rid,
               {"before": {"destination_id": did}})
        conn.commit()

    resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
    set_flash(resp, "Destination unbound.")
    return resp


# ---- Helpers -----------------------------------------------------------


def _empty_rule() -> dict:
    return {"id": None, "name": "", "description": "", "scope": "study",
            "predicate": "", "priority": 0, "status": "draft",
            "dispatch_order": "parallel"}


def _validate_rule_form(*, name, predicate, scope, priority, status, dispatch_order) -> dict[str, str]:
    """Run the same validation the API does, but accumulate every error
    so the form re-renders with all problems highlighted at once
    instead of bouncing the user N times."""
    errors: dict[str, str] = {}

    if not name or len(name) > 200:
        errors["name"] = "Name must be 1–200 characters."
    if scope not in ("study", "series"):
        errors["scope"] = "Scope must be 'study' or 'series'."
    if status not in ("draft", "disabled", "enabled"):
        errors["status"] = "Status must be draft, disabled, or enabled."
    if dispatch_order not in ("parallel", "sequential"):
        errors["dispatch_order"] = "Dispatch order must be 'parallel' or 'sequential'."
    if not (-32768 <= priority <= 32767):
        errors["priority"] = "Priority must be between -32768 and 32767."

    # Predicate: lean on the API's shared validator.
    if not predicate or not predicate.strip():
        errors["predicate"] = "Predicate cannot be empty."
    else:
        try:
            _validate_predicate_text(predicate)
        except ValueError as e:
            errors["predicate"] = str(e)

    return errors


def _audit(cur, auth: AuthContext, action: str,
           resource_kind: str, resource_id: int, diff: dict) -> None:
    """Inline admin_audit insert. Mirrors api.audit._record so UI and
    API actions both end up in the same audit stream."""
    cur.execute("""
        INSERT INTO admin_audit (actor, actor_kind, action,
                                 resource_kind, resource_id, diff)
        VALUES (%s, %s, %s, %s, %s, %s::jsonb)
    """, (str(auth), "token", action, resource_kind, str(resource_id),
          json.dumps(diff)))
