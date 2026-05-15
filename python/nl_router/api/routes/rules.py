"""CRUD endpoints for routing rules.

These thinly wrap SQL on the `rules` table. The router daemon picks up
changes on its next rule-cache refresh (default 15s) — there's no
NOTIFY-driven invalidation yet. That arrives in a follow-up.
"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, require
from nl_router.api.models import RuleCreate, RuleOut, RuleUpdate
from nl_router.db import pool

router = APIRouter(prefix="/rules", tags=["rules"])


# ---- helpers -------------------------------------------------------------


def _row_to_rule(row: dict[str, Any]) -> RuleOut:
    return RuleOut(
        id=row["id"],
        name=row["name"],
        description=row["description"],
        scope=row["scope"],
        predicate=row["predicate"],
        priority=row["priority"],
        status=row["status"],
        dispatch_order=row["dispatch_order"],
        created_at=row["created_at"],
        created_by=row["created_by"],
        updated_at=row["updated_at"],
        updated_by=row["updated_by"],
    )


def _select_rule(cur, rule_id: int) -> dict[str, Any] | None:
    cur.execute("SELECT * FROM rules WHERE id = %s", (rule_id,))
    return cur.fetchone()


def _client_ip(req: Request) -> str | None:
    return req.client.host if req.client else None


def _ua(req: Request) -> str | None:
    return req.headers.get("user-agent")


# ---- routes --------------------------------------------------------------


@router.get("", response_model=list[RuleOut])
def list_rules(
    status_filter: str | None = None,
    scope: str | None = None,
    _: AuthContext = Depends(require("rules.read")),
) -> list[RuleOut]:
    """List rules with optional filters by lifecycle status and scope."""
    sql = "SELECT * FROM rules"
    clauses: list[str] = []
    params: list[Any] = []
    if status_filter is not None:
        clauses.append("status = %s")
        params.append(status_filter)
    if scope is not None:
        clauses.append("scope = %s")
        params.append(scope)
    if clauses:
        sql += " WHERE " + " AND ".join(clauses)
    sql += " ORDER BY priority DESC, name"

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        return [_row_to_rule(r) for r in cur.fetchall()]


@router.get("/{rule_id}", response_model=RuleOut)
def get_rule(
    rule_id: int,
    _: AuthContext = Depends(require("rules.read")),
) -> RuleOut:
    with pool().connection() as conn, conn.cursor() as cur:
        row = _select_rule(cur, rule_id)
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
    return _row_to_rule(row)


@router.post("", response_model=RuleOut, status_code=status.HTTP_201_CREATED)
def create_rule(
    body: RuleCreate,
    req: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> RuleOut:
    """Create a rule. The predicate is stored as-is; the router daemon will
    log a warning if it fails to parse on its next refresh cycle. A
    server-side parse-validate endpoint lands in a follow-up."""
    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO rules (
                    name, description, scope, predicate, priority, status,
                    dispatch_order, created_by, updated_by
                ) VALUES (%s, %s, %s::rule_scope, %s, %s, %s::rule_status, %s, %s, %s)
                RETURNING *
                """,
                (
                    body.name, body.description, body.scope, body.predicate,
                    body.priority, body.status, body.dispatch_order,
                    str(ctx), str(ctx),
                ),
            )
        except Exception as e:
            conn.rollback()
            # Translate the most likely DB error (unique-name violation)
            # into a clean 409. Anything else falls through as 500.
            if "duplicate" in str(e).lower() or "unique" in str(e).lower():
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail=f"rule with name {body.name!r} already exists",
                )
            raise
        row = cur.fetchone()
        emit_audit(
            conn,
            actor=ctx,
            action="rule.create",
            resource_kind="rule",
            resource_id=str(row["id"]),
            diff={"after": body.model_dump()},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_rule(row)


@router.patch("/{rule_id}", response_model=RuleOut)
def update_rule(
    rule_id: int,
    body: RuleUpdate,
    req: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> RuleOut:
    updates = body.model_dump(exclude_unset=True)
    if not updates:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST,
                            detail="empty update body")

    # Build a parameterized SET clause. We allowlist column names via the
    # Pydantic model — `body` can only contain known fields, so the f-string
    # interpolation here is safe.
    cast_map = {
        "scope":          "rule_scope",
        "status":         "rule_status",
    }
    set_parts: list[str] = []
    params: list[Any] = []
    for col, val in updates.items():
        if col in cast_map:
            set_parts.append(f"{col} = %s::{cast_map[col]}")
        else:
            set_parts.append(f"{col} = %s")
        params.append(val)
    set_parts.append("updated_by = %s")
    params.append(str(ctx))
    set_parts.append("updated_at = now()")

    sql = f"UPDATE rules SET {', '.join(set_parts)} WHERE id = %s RETURNING *"
    params.append(rule_id)

    with pool().connection() as conn, conn.cursor() as cur:
        # Snapshot the prior values for the audit diff.
        prior = _select_rule(cur, rule_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
        cur.execute(sql, params)
        row = cur.fetchone()
        # Limit the diff to the fields that actually changed.
        before = {k: prior[k] for k in updates.keys() if k in prior}
        emit_audit(
            conn,
            actor=ctx,
            action="rule.update",
            resource_kind="rule",
            resource_id=str(rule_id),
            diff={"before": _scalarize(before), "after": _scalarize(updates)},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_rule(row)


@router.delete("/{rule_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_rule(
    rule_id: int,
    req: Request,
    ctx: AuthContext = Depends(require("rules.delete")),
) -> None:
    """Delete a rule. Cascade in the schema removes its rule_destinations
    and rule_processing_chain bindings; existing route_assignments rows
    stay (they reference the rule_id but ON DELETE has no cascade — the
    audit history of a routed study survives rule deletion)."""
    with pool().connection() as conn, conn.cursor() as cur:
        prior = _select_rule(cur, rule_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
        cur.execute("DELETE FROM rules WHERE id = %s", (rule_id,))
        emit_audit(
            conn,
            actor=ctx,
            action="rule.delete",
            resource_kind="rule",
            resource_id=str(rule_id),
            diff={"before": {"name": prior["name"], "predicate": prior["predicate"]}},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()


# ---- helpers --------------------------------------------------------------


def _scalarize(d: dict[str, Any]) -> dict[str, Any]:
    """Make a dict JSON-safe for audit storage. Mostly a no-op; only the
    enum values arrive as Python strings already."""
    return {k: (str(v) if hasattr(v, "value") else v) for k, v in d.items()}
