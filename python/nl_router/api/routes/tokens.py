"""Token management endpoints.

`POST /api/v1/tokens` mints a fresh API token and returns it exactly
once. After that, the raw token cannot be recovered — only its hash
is stored. Revocation is one-way: tokens are never deleted, just marked
`revoked=TRUE` for audit continuity.
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, ROLE_PERMISSIONS, mint_token, require
from nl_router.api.models import TokenCreate, TokenCreatedOut, TokenOut
from nl_router.db import pool

router = APIRouter(prefix="/tokens", tags=["tokens"])


def _row_to_token(row: dict[str, Any]) -> TokenOut:
    return TokenOut(
        id=row["id"],
        name=row["name"],
        permissions=row["permissions"] or [],
        revoked=row["revoked"],
        expires_at=row["expires_at"],
        last_used_at=row["last_used_at"],
        created_at=row["created_at"],
        created_by=row["created_by"],
    )


def _client_ip(req: Request) -> str | None:
    return req.client.host if req.client else None


def _ua(req: Request) -> str | None:
    return req.headers.get("user-agent")


@router.get("", response_model=list[TokenOut])
def list_tokens(
    include_revoked: bool = False,
    _: AuthContext = Depends(require("tokens.read")),
) -> list[TokenOut]:
    where = "" if include_revoked else "WHERE revoked = FALSE"
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            f"""
            SELECT id, name, permissions, revoked, expires_at,
                   last_used_at, created_at, created_by
              FROM api_tokens
              {where}
             ORDER BY id
            """
        )
        return [_row_to_token(r) for r in cur.fetchall()]


@router.post("", response_model=TokenCreatedOut, status_code=status.HTTP_201_CREATED)
def create_token(
    body: TokenCreate,
    req: Request,
    ctx: AuthContext = Depends(require("tokens.write")),
) -> TokenCreatedOut:
    """Mint a new API token.

    If `permissions` is omitted, the role's default permission set is
    used. The role itself isn't stored on the token row — only the
    materialized permission list is — so changing ROLE_PERMISSIONS later
    doesn't retroactively change existing tokens.
    """
    perms = body.permissions if body.permissions is not None else ROLE_PERMISSIONS[body.role]

    raw, token_hash = mint_token()

    with pool().connection() as conn, conn.cursor() as cur:
        # `created_by` is a FK to users(id). The user system lands in a
        # later M4 slice; tokens minted by an API caller (rather than a
        # human user) record NULL here. The admin_audit row below still
        # captures who minted it (actor = "token:<id>").
        cur.execute(
            """
            INSERT INTO api_tokens (name, token_hash, permissions, expires_at, created_by)
            VALUES (%s, %s, %s::jsonb, %s, NULL)
            RETURNING id, name, permissions, expires_at, created_at
            """,
            (body.name, token_hash, json.dumps(perms), body.expires_at),
        )
        row = cur.fetchone()
        emit_audit(
            conn,
            actor=ctx,
            action="token.create",
            resource_kind="token",
            resource_id=str(row["id"]),
            diff={
                "name": body.name,
                "role": body.role,
                "permissions_count": len(perms),
                "expires_at": body.expires_at.isoformat() if body.expires_at else None,
            },
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()

    return TokenCreatedOut(
        id=row["id"],
        name=row["name"],
        permissions=row["permissions"] or [],
        expires_at=row["expires_at"],
        created_at=row["created_at"],
        raw_token=raw,
    )


@router.delete("/{token_id}", status_code=status.HTTP_204_NO_CONTENT)
def revoke_token(
    token_id: int,
    req: Request,
    ctx: AuthContext = Depends(require("tokens.revoke")),
) -> None:
    """Revoke an API token. Sets revoked=TRUE; the row is preserved for
    audit. Future writes from the token return 401."""
    if token_id == ctx.token_id:
        # Self-revoke is allowed but called out in the audit. (Mostly to
        # support "rotate yourself out" workflows: mint a new token, then
        # revoke the old one.)
        pass

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT id, name, revoked FROM api_tokens WHERE id = %s",
            (token_id,),
        )
        row = cur.fetchone()
        if row is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="token not found")
        if row["revoked"]:
            # Idempotent: already revoked, return 204 without re-emitting audit.
            return
        cur.execute("UPDATE api_tokens SET revoked = TRUE WHERE id = %s", (token_id,))
        emit_audit(
            conn,
            actor=ctx,
            action="token.revoke",
            resource_kind="token",
            resource_id=str(token_id),
            diff={"name": row["name"]},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
