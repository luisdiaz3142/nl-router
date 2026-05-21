"""UI: processing modules list / form / delete + per-rule chain bindings.

The processing chain sub-section on the rule edit page also routes
through this module (POST /ui/rules/{rid}/processing-chain[/{step_id}]).
Mirrors the destination-bindings section's structure so the rule
edit template stays uniform."""

from __future__ import annotations

import json
from typing import Annotated

from fastapi import APIRouter, Depends, Form, HTTPException, Request
from fastapi.responses import RedirectResponse, Response

from nl_router.api.auth import AuthContext
from nl_router.db import pool
from nl_router.ui.auth import ui_auth_required
from nl_router.ui.common import render, set_flash


router = APIRouter(prefix="/ui/processing-modules", tags=["ui"],
                   include_in_schema=False)

# Per-rule chain operations live under /ui/rules/{rid}/... but logically
# belong here next to the module CRUD. Mounted separately in app.py.
chain_router = APIRouter(prefix="/ui/rules", tags=["ui"],
                          include_in_schema=False)


# =========================================================================
# /ui/processing-modules — list / form / delete
# =========================================================================


@router.get("", response_class=Response)
async def list_modules(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT m.id, m.name, m.description, m.kind, m.config, m.enabled,
                   m.created_at, m.updated_at,
                   (SELECT COUNT(*) FROM rule_processing_chain rpc
                     WHERE rpc.module_id = m.id) AS rule_count
              FROM processing_modules m
             ORDER BY m.name
        """)
        rows = list(cur.fetchall())
    return render(request, "processing_modules_list.html",
                  auth=auth, modules=rows)


@router.get("/new", response_class=Response)
async def new_module_page(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    return render(
        request, "processing_module_form.html",
        auth=auth, mode="create",
        module={"id": None, "name": "", "description": "", "kind": "",
                "config": "{}", "enabled": True},
        errors={},
    )


@router.post("", response_class=Response)
async def create_module(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name:        Annotated[str, Form()],
    kind:        Annotated[str, Form()],
    config:      Annotated[str, Form()] = "{}",
    description: Annotated[str, Form()] = "",
    enabled:     Annotated[str, Form()] = "on",
):
    errors, config_obj = _validate_module_form(name=name, kind=kind, config=config)
    if errors:
        return render(
            request, "processing_module_form.html",
            auth=auth, mode="create",
            module={"name": name, "kind": kind, "description": description,
                    "config": config, "enabled": enabled == "on"},
            errors=errors,
        )

    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO processing_modules (name, description, kind, config, enabled)
                VALUES (%s, %s, %s, %s::jsonb, %s)
                RETURNING id
            """, (name, description or None, kind,
                  json.dumps(config_obj), enabled == "on"))
            new_id = cur.fetchone()["id"]
            _audit(cur, auth, "processing_module.create", new_id,
                   {"after": {"name": name, "kind": kind,
                              "enabled": enabled == "on"}})
            conn.commit()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                errors["name"] = f"Module name {name!r} already exists."
            else:
                errors["__form__"] = f"Database error: {e}"
            return render(
                request, "processing_module_form.html",
                auth=auth, mode="create",
                module={"name": name, "kind": kind, "description": description,
                        "config": config, "enabled": enabled == "on"},
                errors=errors,
            )

    resp = RedirectResponse(url=f"/ui/processing-modules/{new_id}", status_code=303)
    set_flash(resp, f"Module {name!r} registered.")
    return resp


@router.get("/{mid}", response_class=Response)
async def edit_module_page(
    mid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, kind, config, enabled,
                   created_at, updated_at
              FROM processing_modules
             WHERE id = %s
        """, (mid,))
        m = cur.fetchone()
        if not m:
            raise HTTPException(404, "module not found")

        cur.execute("""
            SELECT rpc.rule_id, rpc.ordinal, rpc.config_override,
                   r.name AS rule_name, r.status::text AS rule_status
              FROM rule_processing_chain rpc
              JOIN rules r ON r.id = rpc.rule_id
             WHERE rpc.module_id = %s
             ORDER BY r.name, rpc.ordinal
        """, (mid,))
        used_by = list(cur.fetchall())

    m = dict(m)
    m["config"] = json.dumps(m["config"], indent=2) if m["config"] else "{}"

    return render(
        request, "processing_module_form.html",
        auth=auth, mode="edit", module=m, errors={},
        used_by=used_by,
    )


@router.post("/{mid}", response_class=Response)
async def update_module(
    mid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name:        Annotated[str, Form()],
    kind:        Annotated[str, Form()],
    config:      Annotated[str, Form()] = "{}",
    description: Annotated[str, Form()] = "",
    enabled:     Annotated[str, Form()] = "",
):
    errors, config_obj = _validate_module_form(name=name, kind=kind, config=config)
    if errors:
        return render(
            request, "processing_module_form.html",
            auth=auth, mode="edit",
            module={"id": mid, "name": name, "kind": kind,
                    "description": description, "config": config,
                    "enabled": enabled == "on"},
            errors=errors, used_by=[],
        )

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT * FROM processing_modules WHERE id = %s", (mid,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "module not found")
        cur.execute("""
            UPDATE processing_modules SET
                name        = %s,
                description = %s,
                kind        = %s,
                config      = %s::jsonb,
                enabled     = %s,
                updated_at  = NOW()
             WHERE id = %s
        """, (name, description or None, kind, json.dumps(config_obj),
              enabled == "on", mid))
        _audit(cur, auth, "processing_module.update", mid, {
            "before": {"name": prior["name"], "kind": prior["kind"],
                       "enabled": prior["enabled"]},
            "after":  {"name": name, "kind": kind, "enabled": enabled == "on"},
        })
        conn.commit()

    resp = RedirectResponse(url=f"/ui/processing-modules/{mid}", status_code=303)
    set_flash(resp, f"Module {name!r} saved.")
    return resp


@router.post("/{mid}/delete", response_class=Response)
async def delete_module(
    mid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT name FROM processing_modules WHERE id = %s", (mid,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "module not found")
        try:
            cur.execute("DELETE FROM processing_modules WHERE id = %s", (mid,))
        except Exception as e:
            conn.rollback()
            resp = RedirectResponse(url=f"/ui/processing-modules/{mid}",
                                     status_code=303)
            set_flash(resp,
                f"Cannot delete: {e}. Unbind from rules first.", "err")
            return resp
        _audit(cur, auth, "processing_module.delete", mid,
               {"before": {"name": prior["name"]}})
        conn.commit()

    resp = RedirectResponse(url="/ui/processing-modules", status_code=303)
    set_flash(resp, f"Module {prior['name']!r} deleted.")
    return resp


# =========================================================================
# /ui/rules/{rid}/processing-chain — chain bindings on the rule edit page
# =========================================================================


@chain_router.post("/{rid}/processing-chain", response_class=Response)
async def add_chain_step(
    rid: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    module_id:       Annotated[int, Form()],
    ordinal:         Annotated[int, Form()] = 0,
    config_override: Annotated[str, Form()] = "",
):
    override_obj: dict | None = None
    if config_override.strip():
        try:
            override_obj = json.loads(config_override)
            if not isinstance(override_obj, dict):
                resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
                set_flash(resp, "config_override must be a JSON object", "err")
                return resp
        except json.JSONDecodeError as e:
            resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
            set_flash(resp, f"Invalid JSON: {e.msg} (line {e.lineno}, col {e.colno})",
                       "err")
            return resp

    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO rule_processing_chain (
                    rule_id, module_id, ordinal, config_override
                ) VALUES (%s, %s, %s, %s::jsonb)
            """, (rid, module_id, ordinal,
                  json.dumps(override_obj) if override_obj is not None else None))
            _audit(cur, auth, "rule.chain_add", rid,
                   {"after": {"module_id": module_id, "ordinal": ordinal,
                              "config_override": override_obj}})
            conn.commit()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
            if "duplicate" in msg or "unique" in msg:
                set_flash(resp,
                    f"Ordinal {ordinal} is already taken on this rule. "
                    f"Pick a different ordinal or remove the existing step.",
                    "err")
            else:
                set_flash(resp, f"Could not add chain step: {e}", "err")
            return resp

    resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
    set_flash(resp, "Processing step added.")
    return resp


@chain_router.post("/{rid}/processing-chain/{step_id}/delete",
                    response_class=Response)
async def delete_chain_step(
    rid: int, step_id: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT module_id, ordinal FROM rule_processing_chain "
            "WHERE id = %s AND rule_id = %s",
            (step_id, rid),
        )
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "chain step not found")
        cur.execute(
            "DELETE FROM rule_processing_chain WHERE id = %s AND rule_id = %s",
            (step_id, rid),
        )
        _audit(cur, auth, "rule.chain_remove", rid,
               {"before": {"module_id": prior["module_id"],
                           "ordinal": prior["ordinal"]}})
        conn.commit()

    resp = RedirectResponse(url=f"/ui/rules/{rid}", status_code=303)
    set_flash(resp, "Processing step removed.")
    return resp


# ---- Helpers -----------------------------------------------------------


def _validate_module_form(*, name: str, kind: str, config: str):
    errors: dict[str, str] = {}
    config_obj: dict = {}
    if not name or len(name) > 200:
        errors["name"] = "Name must be 1–200 characters."
    if not kind or len(kind) > 200:
        errors["kind"] = "Kind must be 1–200 characters and match the binary at /usr/libexec/nl-router/modules/<kind>."
    if not config.strip():
        config_obj = {}
    else:
        try:
            config_obj = json.loads(config)
            if not isinstance(config_obj, dict):
                errors["config"] = "Config must be a JSON object."
        except json.JSONDecodeError as e:
            errors["config"] = f"Invalid JSON: {e.msg} at line {e.lineno}, col {e.colno}"
    return errors, config_obj


def _audit(cur, auth: AuthContext, action: str,
           resource_id: int, diff: dict) -> None:
    """Inline admin_audit insert mirroring the API's emit_audit shape."""
    resource_kind = "rule" if action.startswith("rule.") else "processing_module"
    cur.execute("""
        INSERT INTO admin_audit (actor, actor_kind, action,
                                 resource_kind, resource_id, diff)
        VALUES (%s, %s, %s, %s, %s, %s::jsonb)
    """, (str(auth), "token", action, resource_kind,
          str(resource_id), json.dumps(diff)))
