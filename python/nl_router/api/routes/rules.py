"""CRUD endpoints for routing rules + rule↔destination bindings.

These thinly wrap SQL on the `rules` table and the `rule_destinations`
join. The router daemon picks up changes on its next rule-cache refresh
(default 15s) — there's no NOTIFY-driven invalidation yet. That arrives
in a follow-up.
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, require
from nl_router.api.models import (
    RuleCreate,
    RuleDestinationBind,
    RuleDestinationOut,
    RuleOut,
    RuleUpdate,
)
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
    """Create a rule.

    The predicate goes through a structural preflight (length + paren
    depth + balanced quotes) on the API boundary — see
    `_validate_predicate_text` in api/models.py. Anything that passes
    preflight is stored as-is; the router daemon parses semantically
    on its next rule-cache refresh, and logs a warning if the body
    fails to compile (which only happens for unknown function /
    method names that the preflight can't see)."""
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


# ===========================================================================
# Rule ↔ destination bindings (rule_destinations join table)
# ===========================================================================


def _row_to_binding(row: dict[str, Any]) -> RuleDestinationOut:
    return RuleDestinationOut(
        id=row["id"],
        rule_id=row["rule_id"],
        destination_id=row["destination_id"],
        destination_name=row["destination_name"],
        destination_kind=row["destination_kind"],
        ordinal=row["ordinal"],
        retry_policy_override=row.get("retry_policy_override"),
    )


@router.get("/{rule_id}/destinations", response_model=list[RuleDestinationOut])
def list_rule_destinations(
    rule_id: int,
    _: AuthContext = Depends(require("rules.read")),
) -> list[RuleDestinationOut]:
    """List destinations bound to a rule, ordered by ordinal."""
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM rules WHERE id = %s", (rule_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
        cur.execute(
            """
            SELECT rd.id, rd.rule_id, rd.destination_id, rd.ordinal,
                   rd.retry_policy_override,
                   d.name AS destination_name, d.kind AS destination_kind
              FROM rule_destinations rd
              JOIN destinations d ON d.id = rd.destination_id
             WHERE rd.rule_id = %s
             ORDER BY rd.ordinal, d.name
            """,
            (rule_id,),
        )
        return [_row_to_binding(r) for r in cur.fetchall()]


@router.put(
    "/{rule_id}/destinations/{destination_id}",
    response_model=RuleDestinationOut,
    status_code=status.HTTP_200_OK,
)
def bind_destination(
    rule_id: int,
    destination_id: int,
    body: RuleDestinationBind,
    req: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> RuleDestinationOut:
    """Bind a destination to a rule (create-or-update).

    PUT semantics: if the binding already exists, ordinal /
    retry_policy_override are updated; if not, a new binding row is
    inserted. Either way the response is the resulting binding.
    """
    override_json = json.dumps(body.retry_policy_override) if body.retry_policy_override else None

    with pool().connection() as conn, conn.cursor() as cur:
        # Existence checks let us return clean 404s before the FK insert
        # would otherwise translate them to opaque 23503 errors.
        cur.execute("SELECT 1 FROM rules WHERE id = %s", (rule_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
        cur.execute("SELECT 1 FROM destinations WHERE id = %s", (destination_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                                detail="destination not found")

        # Upsert. ON CONFLICT (rule_id, destination_id) uses the unique
        # constraint declared in migration 0003.
        cur.execute(
            """
            INSERT INTO rule_destinations (rule_id, destination_id, ordinal, retry_policy_override)
            VALUES (%s, %s, %s, %s::jsonb)
            ON CONFLICT (rule_id, destination_id) DO UPDATE
              SET ordinal               = EXCLUDED.ordinal,
                  retry_policy_override = EXCLUDED.retry_policy_override
            RETURNING id, rule_id, destination_id, ordinal, retry_policy_override
            """,
            (rule_id, destination_id, body.ordinal, override_json),
        )
        upserted = cur.fetchone()

        # Re-fetch with the destination join so the response shape matches list.
        cur.execute(
            """
            SELECT rd.id, rd.rule_id, rd.destination_id, rd.ordinal,
                   rd.retry_policy_override,
                   d.name AS destination_name, d.kind AS destination_kind
              FROM rule_destinations rd
              JOIN destinations d ON d.id = rd.destination_id
             WHERE rd.id = %s
            """,
            (upserted["id"],),
        )
        row = cur.fetchone()

        emit_audit(
            conn,
            actor=ctx,
            action="rule.destination.bind",
            resource_kind="rule",
            resource_id=str(rule_id),
            diff={
                "destination_id": destination_id,
                "ordinal": body.ordinal,
                "retry_policy_override": body.retry_policy_override,
            },
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_binding(row)


@router.delete(
    "/{rule_id}/destinations/{destination_id}",
    status_code=status.HTTP_204_NO_CONTENT,
)
def unbind_destination(
    rule_id: int,
    destination_id: int,
    req: Request,
    ctx: AuthContext = Depends(require("rules.write")),
) -> None:
    """Remove a destination from a rule. Idempotent: 204 either way as
    long as the rule exists."""
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM rules WHERE id = %s", (rule_id,))
        if cur.fetchone() is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="rule not found")
        cur.execute(
            "DELETE FROM rule_destinations WHERE rule_id = %s AND destination_id = %s",
            (rule_id, destination_id),
        )
        emit_audit(
            conn,
            actor=ctx,
            action="rule.destination.unbind",
            resource_kind="rule",
            resource_id=str(rule_id),
            diff={"destination_id": destination_id},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
