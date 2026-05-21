"""processing_modules CRUD + per-rule processing_chain endpoints.

The processing_modules table is the registry of installed module
binaries. Each row's `kind` selects the worker pool that picks up
processing_jobs for it; the `name` is the operator-visible label
referenced by rule_processing_chain rows.

The rule_processing_chain table is the per-rule ordered list of
modules that run between Router and Dispatcher. We expose it through
two route shapes:
  * Module-level:  /api/v1/processing-modules/[id]
  * Chain-level:   /api/v1/rules/{rid}/processing-chain[/{module_id}]

The chain routes are mounted under the rules prefix because they only
make sense in the context of a rule and that's where the bindings
data lives.
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, require
from nl_router.api.models import (
    ChainStepIn,
    ChainStepOut,
    ProcessingModuleCreate,
    ProcessingModuleOut,
    ProcessingModuleUpdate,
)
from nl_router.db import pool


# =========================================================================
# /api/v1/processing-modules
# =========================================================================

router = APIRouter(prefix="/processing-modules", tags=["processing_modules"])


def _row_to_module(row: dict) -> ProcessingModuleOut:
    return ProcessingModuleOut(
        id=row["id"],
        name=row["name"],
        description=row["description"],
        kind=row["kind"],
        config=row["config"] or {},
        enabled=row["enabled"],
        created_at=row["created_at"],
        updated_at=row["updated_at"],
    )


@router.get("", response_model=list[ProcessingModuleOut])
def list_modules(
    request: Request,
    _: AuthContext = Depends(require("modules.read")),
) -> list[ProcessingModuleOut]:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, kind, config, enabled,
                   created_at, updated_at
              FROM processing_modules
             ORDER BY name
        """)
        return [_row_to_module(r) for r in cur.fetchall()]


@router.get("/{mid}", response_model=ProcessingModuleOut)
def get_module(
    mid: int,
    request: Request,
    _: AuthContext = Depends(require("modules.read")),
) -> ProcessingModuleOut:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, kind, config, enabled,
                   created_at, updated_at
              FROM processing_modules
             WHERE id = %s
        """, (mid,))
        row = cur.fetchone()
        if row is None:
            raise HTTPException(404, f"processing_module {mid} not found")
    return _row_to_module(row)


@router.post("", response_model=ProcessingModuleOut,
             status_code=status.HTTP_201_CREATED)
def create_module(
    body: ProcessingModuleCreate,
    request: Request,
    ctx: AuthContext = Depends(require("modules.write")),
) -> ProcessingModuleOut:
    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO processing_modules (name, description, kind, config, enabled)
                VALUES (%s, %s, %s, %s::jsonb, %s)
                RETURNING id, name, description, kind, config, enabled,
                          created_at, updated_at
            """, (body.name, body.description, body.kind,
                  json.dumps(body.config), body.enabled))
            row = cur.fetchone()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                raise HTTPException(409, f"processing_module name {body.name!r} already exists")
            raise
        emit_audit(
            conn,
            actor=ctx,
            action="processing_module.create",
            resource_kind="processing_module",
            resource_id=str(row["id"]),
            diff={"after": {"name": body.name, "kind": body.kind,
                            "enabled": body.enabled}},
        )
        conn.commit()
    return _row_to_module(row)


@router.patch("/{mid}", response_model=ProcessingModuleOut)
def update_module(
    mid: int,
    body: ProcessingModuleUpdate,
    request: Request,
    ctx: AuthContext = Depends(require("modules.write")),
) -> ProcessingModuleOut:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT * FROM processing_modules WHERE id = %s", (mid,))
        prior = cur.fetchone()
        if prior is None:
            raise HTTPException(404, f"processing_module {mid} not found")

        # Build SET clause dynamically based on which fields the caller sent.
        sets: list[str] = []
        params: list[Any] = []
        for field in ("name", "description", "enabled"):
            v = getattr(body, field)
            if v is not None:
                sets.append(f"{field} = %s")
                params.append(v)
        if body.config is not None:
            sets.append("config = %s::jsonb")
            params.append(json.dumps(body.config))
        if not sets:
            # Nothing to update — just return the current row.
            return _row_to_module(prior)
        sets.append("updated_at = NOW()")
        params.append(mid)

        try:
            cur.execute(
                "UPDATE processing_modules SET " + ", ".join(sets) +
                " WHERE id = %s RETURNING *", params,
            )
            row = cur.fetchone()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                raise HTTPException(409, "name conflicts with an existing module")
            raise

        emit_audit(
            conn,
            actor=ctx,
            action="processing_module.update",
            resource_kind="processing_module",
            resource_id=str(mid),
            diff={
                "before": {"name": prior["name"], "enabled": prior["enabled"]},
                "after":  {"name": row["name"],   "enabled": row["enabled"]},
            },
        )
        conn.commit()
    return _row_to_module(row)


@router.delete("/{mid}", status_code=status.HTTP_204_NO_CONTENT)
def delete_module(
    mid: int,
    request: Request,
    ctx: AuthContext = Depends(require("modules.write")),
) -> None:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT name FROM processing_modules WHERE id = %s", (mid,))
        prior = cur.fetchone()
        if prior is None:
            raise HTTPException(404, f"processing_module {mid} not found")
        try:
            cur.execute("DELETE FROM processing_modules WHERE id = %s", (mid,))
        except Exception as e:
            conn.rollback()
            # FK violation: still referenced by rule_processing_chain.
            raise HTTPException(409, f"cannot delete: {e}. Unbind from rules first.")
        emit_audit(
            conn,
            actor=ctx,
            action="processing_module.delete",
            resource_kind="processing_module",
            resource_id=str(mid),
            diff={"before": {"name": prior["name"]}},
        )
        conn.commit()


# =========================================================================
# /api/v1/rules/{rid}/processing-chain
# =========================================================================
#
# Lives in this file (not rules.py) because it's a separate resource;
# we just mount it under the rules prefix for URL ergonomics.

chain_router = APIRouter(prefix="/rules", tags=["processing_modules"])


@chain_router.get("/{rid}/processing-chain", response_model=list[ChainStepOut])
def get_chain(
    rid: int,
    request: Request,
    _: AuthContext = Depends(require("rules.read")),
) -> list[ChainStepOut]:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM rules WHERE id = %s", (rid,))
        if cur.fetchone() is None:
            raise HTTPException(404, f"rule {rid} not found")
        cur.execute("""
            SELECT rpc.id, rpc.rule_id, rpc.module_id, rpc.ordinal,
                   rpc.config_override,
                   m.name AS module_name, m.kind AS module_kind
              FROM rule_processing_chain rpc
              JOIN processing_modules m ON m.id = rpc.module_id
             WHERE rpc.rule_id = %s
             ORDER BY rpc.ordinal, rpc.id
        """, (rid,))
        return [
            ChainStepOut(
                id=r["id"], rule_id=r["rule_id"], module_id=r["module_id"],
                module_name=r["module_name"], module_kind=r["module_kind"],
                ordinal=r["ordinal"], config_override=r["config_override"],
            )
            for r in cur.fetchall()
        ]


@chain_router.post("/{rid}/processing-chain",
                    response_model=ChainStepOut,
                    status_code=status.HTTP_201_CREATED)
def add_chain_step(
    rid: int,
    body: ChainStepIn,
    request: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> ChainStepOut:
    """Insert a step. Idempotent on (rule_id, ordinal) — the table's
    UNIQUE constraint catches duplicates; we translate that to 409."""
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT id FROM rules WHERE id = %s", (rid,))
        if cur.fetchone() is None:
            raise HTTPException(404, f"rule {rid} not found")
        cur.execute("SELECT name, kind FROM processing_modules WHERE id = %s",
                    (body.module_id,))
        mod = cur.fetchone()
        if mod is None:
            raise HTTPException(404, f"processing_module {body.module_id} not found")

        try:
            cur.execute("""
                INSERT INTO rule_processing_chain (
                    rule_id, module_id, ordinal, config_override
                ) VALUES (%s, %s, %s, %s::jsonb)
                RETURNING id, rule_id, module_id, ordinal, config_override
            """, (rid, body.module_id, body.ordinal,
                  json.dumps(body.config_override) if body.config_override is not None else None))
            row = cur.fetchone()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                raise HTTPException(
                    409,
                    f"rule {rid} already has a step at ordinal {body.ordinal}. "
                    f"PATCH that step or pick a different ordinal."
                )
            raise

        emit_audit(
            conn,
            actor=ctx,
            action="rule.chain_add",
            resource_kind="rule",
            resource_id=str(rid),
            diff={"after": {"module_id": body.module_id,
                            "module_name": mod["name"],
                            "ordinal": body.ordinal,
                            "config_override": body.config_override}},
        )
        conn.commit()

    return ChainStepOut(
        id=row["id"], rule_id=row["rule_id"], module_id=row["module_id"],
        module_name=mod["name"], module_kind=mod["kind"],
        ordinal=row["ordinal"], config_override=row["config_override"],
    )


@chain_router.patch("/{rid}/processing-chain/{step_id}",
                     response_model=ChainStepOut)
def update_chain_step(
    rid: int, step_id: int,
    body: ChainStepIn,
    request: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> ChainStepOut:
    """Update a step — reorder, swap modules, change config_override.
    ChainStepIn is reused (same shape on PATCH); ordinal collisions
    surface as 409."""
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM rule_processing_chain WHERE id = %s AND rule_id = %s",
            (step_id, rid),
        )
        prior = cur.fetchone()
        if prior is None:
            raise HTTPException(404, f"chain step {step_id} not found on rule {rid}")

        cur.execute("SELECT name, kind FROM processing_modules WHERE id = %s",
                    (body.module_id,))
        mod = cur.fetchone()
        if mod is None:
            raise HTTPException(404, f"processing_module {body.module_id} not found")

        try:
            cur.execute("""
                UPDATE rule_processing_chain
                   SET module_id       = %s,
                       ordinal         = %s,
                       config_override = %s::jsonb
                 WHERE id = %s AND rule_id = %s
                RETURNING id, rule_id, module_id, ordinal, config_override
            """, (body.module_id, body.ordinal,
                  json.dumps(body.config_override) if body.config_override is not None else None,
                  step_id, rid))
            row = cur.fetchone()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                raise HTTPException(409, f"another step already occupies ordinal {body.ordinal}")
            raise

        emit_audit(
            conn,
            actor=ctx,
            action="rule.chain_update",
            resource_kind="rule",
            resource_id=str(rid),
            diff={
                "before": {"module_id": prior["module_id"],
                           "ordinal": prior["ordinal"],
                           "config_override": prior["config_override"]},
                "after":  {"module_id": body.module_id,
                           "ordinal": body.ordinal,
                           "config_override": body.config_override},
            },
        )
        conn.commit()

    return ChainStepOut(
        id=row["id"], rule_id=row["rule_id"], module_id=row["module_id"],
        module_name=mod["name"], module_kind=mod["kind"],
        ordinal=row["ordinal"], config_override=row["config_override"],
    )


@chain_router.delete("/{rid}/processing-chain/{step_id}",
                      status_code=status.HTTP_204_NO_CONTENT)
def delete_chain_step(
    rid: int, step_id: int,
    request: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> None:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT module_id, ordinal FROM rule_processing_chain "
            "WHERE id = %s AND rule_id = %s",
            (step_id, rid),
        )
        prior = cur.fetchone()
        if prior is None:
            raise HTTPException(404, f"chain step {step_id} not found on rule {rid}")
        cur.execute(
            "DELETE FROM rule_processing_chain WHERE id = %s AND rule_id = %s",
            (step_id, rid),
        )
        emit_audit(
            conn,
            actor=ctx,
            action="rule.chain_remove",
            resource_kind="rule",
            resource_id=str(rid),
            diff={"before": {"module_id": prior["module_id"],
                             "ordinal": prior["ordinal"]}},
        )
        conn.commit()
