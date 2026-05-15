"""CRUD endpoints for destinations.

The destinations.kind column is TEXT (not ENUM), so operators can declare
custom kinds for their own dispatchers. Built-in kinds with shipped
handlers (M3: `dicom`; M7: `dicomweb_stow`, `gcp_dicomweb`, `http`,
`file`, `object_storage`) work out of the box; custom kinds need an
operator-supplied worker registered against the route_assignments table.
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, require
from nl_router.api.models import DestinationCreate, DestinationOut, DestinationUpdate
from nl_router.db import pool

router = APIRouter(prefix="/destinations", tags=["destinations"])


def _row_to_dest(row: dict[str, Any]) -> DestinationOut:
    return DestinationOut(
        id=row["id"],
        name=row["name"],
        description=row["description"],
        kind=row["kind"],
        enabled=row["enabled"],
        config=row["config"] or {},
        credential_id=row["credential_id"],
        dispatch_concurrency=row["dispatch_concurrency"],
        retry_policy=row.get("retry_policy"),
        created_at=row["created_at"],
        updated_at=row["updated_at"],
    )


def _select_dest(cur, dest_id: int) -> dict[str, Any] | None:
    cur.execute("SELECT * FROM destinations WHERE id = %s", (dest_id,))
    return cur.fetchone()


def _client_ip(req: Request) -> str | None:
    return req.client.host if req.client else None


def _ua(req: Request) -> str | None:
    return req.headers.get("user-agent")


@router.get("", response_model=list[DestinationOut])
def list_destinations(
    kind: str | None = None,
    enabled: bool | None = None,
    _: AuthContext = Depends(require("destinations.read")),
) -> list[DestinationOut]:
    sql = "SELECT * FROM destinations"
    clauses: list[str] = []
    params: list[Any] = []
    if kind is not None:
        clauses.append("kind = %s")
        params.append(kind)
    if enabled is not None:
        clauses.append("enabled = %s")
        params.append(enabled)
    if clauses:
        sql += " WHERE " + " AND ".join(clauses)
    sql += " ORDER BY name"

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        return [_row_to_dest(r) for r in cur.fetchall()]


@router.get("/{dest_id}", response_model=DestinationOut)
def get_destination(
    dest_id: int,
    _: AuthContext = Depends(require("destinations.read")),
) -> DestinationOut:
    with pool().connection() as conn, conn.cursor() as cur:
        row = _select_dest(cur, dest_id)
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="destination not found")
    return _row_to_dest(row)


@router.post("", response_model=DestinationOut, status_code=status.HTTP_201_CREATED)
def create_destination(
    body: DestinationCreate,
    req: Request,
    ctx: AuthContext = Depends(require("destinations.write")),
) -> DestinationOut:
    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO destinations (
                    name, description, kind, enabled, config,
                    dispatch_concurrency, credential_id, retry_policy
                ) VALUES (
                    %s, %s, %s, %s, %s::jsonb,
                    %s, %s,
                    COALESCE(%s::jsonb,
                             '{"max_attempts":5,"initial_backoff_s":30,"multiplier":2.0,"max_backoff_s":3600,"give_up_after_hours":72}'::jsonb)
                )
                RETURNING *
                """,
                (
                    body.name, body.description, body.kind, body.enabled,
                    json.dumps(body.config),
                    body.dispatch_concurrency, body.credential_id,
                    json.dumps(body.retry_policy) if body.retry_policy else None,
                ),
            )
        except Exception as e:
            conn.rollback()
            if "duplicate" in str(e).lower() or "unique" in str(e).lower():
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail=f"destination with name {body.name!r} already exists",
                )
            raise
        row = cur.fetchone()
        emit_audit(
            conn,
            actor=ctx,
            action="destination.create",
            resource_kind="destination",
            resource_id=str(row["id"]),
            diff={"after": body.model_dump()},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_dest(row)


@router.patch("/{dest_id}", response_model=DestinationOut)
def update_destination(
    dest_id: int,
    body: DestinationUpdate,
    req: Request,
    ctx: AuthContext = Depends(require("destinations.write")),
) -> DestinationOut:
    updates = body.model_dump(exclude_unset=True)
    if not updates:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST,
                            detail="empty update body")

    # JSONB columns need ::jsonb casts and json.dumps serialization.
    jsonb_cols = {"config", "retry_policy"}
    set_parts: list[str] = []
    params: list[Any] = []
    for col, val in updates.items():
        if col in jsonb_cols:
            set_parts.append(f"{col} = %s::jsonb")
            params.append(json.dumps(val) if val is not None else None)
        else:
            set_parts.append(f"{col} = %s")
            params.append(val)
    set_parts.append("updated_at = now()")
    sql = f"UPDATE destinations SET {', '.join(set_parts)} WHERE id = %s RETURNING *"
    params.append(dest_id)

    with pool().connection() as conn, conn.cursor() as cur:
        prior = _select_dest(cur, dest_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="destination not found")
        cur.execute(sql, params)
        row = cur.fetchone()
        before = {k: prior[k] for k in updates.keys() if k in prior}
        emit_audit(
            conn,
            actor=ctx,
            action="destination.update",
            resource_kind="destination",
            resource_id=str(dest_id),
            diff={"before": _jsonify(before), "after": _jsonify(updates)},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_dest(row)


@router.delete("/{dest_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_destination(
    dest_id: int,
    req: Request,
    ctx: AuthContext = Depends(require("destinations.delete")),
) -> None:
    """Delete a destination. The DB FK on rule_destinations is RESTRICT by
    default — if any rule_destinations or route_assignments rows still
    reference this destination, the delete is refused with 409. Operators
    should remove the rule bindings first."""
    with pool().connection() as conn, conn.cursor() as cur:
        prior = _select_dest(cur, dest_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="destination not found")
        try:
            cur.execute("DELETE FROM destinations WHERE id = %s", (dest_id,))
        except Exception as e:
            conn.rollback()
            if "foreign key" in str(e).lower():
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail="destination is still bound to one or more rules / assignments",
                )
            raise
        emit_audit(
            conn,
            actor=ctx,
            action="destination.delete",
            resource_kind="destination",
            resource_id=str(dest_id),
            diff={"before": {"name": prior["name"], "kind": prior["kind"]}},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()


def _jsonify(d: dict[str, Any]) -> dict[str, Any]:
    """Replace non-JSON-natural types so the audit diff serializes cleanly."""
    out: dict[str, Any] = {}
    for k, v in d.items():
        if isinstance(v, (dict, list, str, int, float, bool)) or v is None:
            out[k] = v
        else:
            out[k] = str(v)
    return out
