"""Credential CRUD endpoints with envelope encryption.

The plaintext payload is sent on create / rotate and validated against
the kind's schema (see `nl_router.api.kinds`). It's then encrypted with
the configured KEK via `nl_router.crypto` and written to the
`credentials` table as ciphertext + nonce + version. The payload is
NEVER returned in any response, listing, audit diff, or error message.

Routes
------
POST    /api/v1/credentials              create
GET     /api/v1/credentials              list (metadata only)
GET     /api/v1/credentials/{id}         get one (metadata only)
PATCH   /api/v1/credentials/{id}         rotate payload and/or update metadata
DELETE  /api/v1/credentials/{id}         delete
GET     /api/v1/credentials/kinds        list known credential kinds

Permissions
-----------
List/get:                  `credentials.read_metadata`
Create/rotate (write/rotate): `credentials.write`  /  `credentials.rotate`
Delete:                    `credentials.delete`

Failure modes
-------------
* KEK not configured  → 503 (operators see a clear error rather than
                         silently saving garbage).
* Malformed payload   → 422 (validated by Pydantic before encryption).
* Unique-name clash   → 409.
* FK from a destination that references the credential blocks DELETE
                        → 409 (matches the destinations.delete behavior).
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, status
from psycopg.types.json import Jsonb
from pydantic import ValidationError

from nl_router.api.audit import emit_audit
from nl_router.api.auth import AuthContext, require
from nl_router.api.kinds import known_kinds, validate_payload
from nl_router.api.models import CredentialCreate, CredentialOut, CredentialRotate
from nl_router.crypto import CryptoError, KEKUnavailableError, encrypt
from nl_router.db import pool

router = APIRouter(prefix="/credentials", tags=["credentials"])


def _client_ip(req: Request) -> str | None:
    return req.client.host if req.client else None


def _ua(req: Request) -> str | None:
    return req.headers.get("user-agent")


def _format_validation_errors(e: ValidationError) -> list[dict[str, Any]]:
    """Strip non-JSON-serializable context (e.g. embedded ValueError
    instances from model_post_init) out of pydantic's `.errors()` so the
    422 body renders cleanly."""
    out: list[dict[str, Any]] = []
    for err in e.errors():
        out.append({
            "loc":  list(err.get("loc", ())),
            "msg":  err.get("msg"),
            "type": err.get("type"),
            "input": err.get("input") if isinstance(err.get("input"), (str, int, float, bool, type(None))) else None,
        })
    return out


def _row_to_out(row: dict[str, Any]) -> CredentialOut:
    return CredentialOut(
        id=row["id"],
        name=row["name"],
        description=row["description"],
        kind=row["kind"],
        enc_version=row["enc_version"],
        metadata=row.get("metadata"),
        created_at=row["created_at"],
        created_by=row["created_by"],
        updated_at=row["updated_at"],
        updated_by=row["updated_by"],
    )


def _select_credential(cur, cred_id: int) -> dict[str, Any] | None:
    cur.execute(
        """
        SELECT id, name, description, kind, enc_version, metadata,
               created_at, created_by, updated_at, updated_by
          FROM credentials
         WHERE id = %s
        """,
        (cred_id,),
    )
    return cur.fetchone()


# ---- Kinds catalog -------------------------------------------------------


@router.get("/kinds", response_model=list[str])
def list_known_kinds(
    _: AuthContext = Depends(require("credentials.read_metadata")),
) -> list[str]:
    """List the credential kinds this server understands. Operators use
    this to learn the valid `kind` values before creating a credential."""
    return known_kinds()


# ---- Standard CRUD -------------------------------------------------------


@router.get("", response_model=list[CredentialOut])
def list_credentials(
    kind: str | None = None,
    _: AuthContext = Depends(require("credentials.read_metadata")),
) -> list[CredentialOut]:
    """List credentials. Payloads are NEVER returned — only metadata."""
    sql = (
        "SELECT id, name, description, kind, enc_version, metadata, "
        "       created_at, created_by, updated_at, updated_by "
        "  FROM credentials"
    )
    params: list[Any] = []
    if kind is not None:
        sql += " WHERE kind = %s"
        params.append(kind)
    sql += " ORDER BY name"
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        return [_row_to_out(r) for r in cur.fetchall()]


@router.get("/{cred_id}", response_model=CredentialOut)
def get_credential(
    cred_id: int,
    _: AuthContext = Depends(require("credentials.read_metadata")),
) -> CredentialOut:
    with pool().connection() as conn, conn.cursor() as cur:
        row = _select_credential(cur, cred_id)
    if row is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                            detail="credential not found")
    return _row_to_out(row)


@router.post("", response_model=CredentialOut, status_code=status.HTTP_201_CREATED)
def create_credential(
    body: CredentialCreate,
    req: Request,
    ctx: AuthContext = Depends(require("credentials.write")),
) -> CredentialOut:
    """Create a credential. Validates the payload against the kind's
    schema, encrypts it with the configured KEK, and stores ciphertext +
    nonce + version. The plaintext is dropped immediately after encryption.
    """
    # ---- Validate the payload shape ----
    try:
        validated = validate_payload(body.kind, body.payload)
    except ValidationError as e:
        # Project to plain dicts; raw .errors() may include ValueError
        # context objects that FastAPI's JSON encoder can't serialize.
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=_format_validation_errors(e),
        )
    except ValueError as e:
        # Unknown kind, or per-kind post_init validation (e.g. api_key needing
        # exactly one of header/query_param).
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=str(e),
        )

    # ---- Encrypt ----
    plaintext = json.dumps(validated, separators=(",", ":")).encode("utf-8")
    try:
        envelope = encrypt(plaintext)
    except KEKUnavailableError as e:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=f"KEK not configured: {e}",
        )
    except CryptoError as e:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"encryption failed: {e}",
        )

    # ---- Insert ----
    metadata: dict[str, Any] = {}
    if body.expires_at is not None:
        metadata["expires_at"] = body.expires_at.isoformat()

    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO credentials (
                    name, description, kind, enc_version, nonce, ciphertext,
                    metadata, created_by, updated_by
                ) VALUES (%s, %s, %s, %s, %s, %s, %s::jsonb, %s, %s)
                RETURNING id, name, description, kind, enc_version, metadata,
                          created_at, created_by, updated_at, updated_by
                """,
                (
                    body.name, body.description, body.kind,
                    envelope.enc_version, envelope.nonce, envelope.ciphertext,
                    json.dumps(metadata), str(ctx), str(ctx),
                ),
            )
        except Exception as e:
            conn.rollback()
            if "duplicate" in str(e).lower() or "unique" in str(e).lower():
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail=f"credential with name {body.name!r} already exists",
                )
            raise
        row = cur.fetchone()

        # Audit: NEVER include the payload. Record only the structural facts.
        emit_audit(
            conn,
            actor=ctx,
            action="credential.create",
            resource_kind="credential",
            resource_id=str(row["id"]),
            diff={
                "after": {
                    "name": body.name,
                    "kind": body.kind,
                    "enc_version": envelope.enc_version,
                    "has_expires_at": body.expires_at is not None,
                },
            },
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_out(row)


@router.patch("/{cred_id}", response_model=CredentialOut)
def rotate_or_update_credential(
    cred_id: int,
    body: CredentialRotate,
    req: Request,
    ctx: AuthContext = Depends(require("credentials.rotate")),
) -> CredentialOut:
    """PATCH a credential. If `payload` is provided, the row is
    re-encrypted with a fresh nonce (true rotation). Metadata-only updates
    (`description`, `expires_at`) leave nonce/ciphertext alone.

    Mixed updates (payload + metadata in one request) are allowed; we do
    both transactionally.
    """
    if body.payload is None and body.description is None and body.expires_at is None:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST,
                            detail="empty update body")

    with pool().connection() as conn, conn.cursor() as cur:
        prior = _select_credential(cur, cred_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                                detail="credential not found")

        set_parts: list[str] = []
        params: list[Any] = []
        audit_diff: dict[str, Any] = {"before": {}, "after": {}}

        # ---- Payload rotation ----
        if body.payload is not None:
            try:
                validated = validate_payload(prior["kind"], body.payload)
            except ValidationError as e:
                raise HTTPException(
                    status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                    detail=_format_validation_errors(e),
                )
            except ValueError as e:
                raise HTTPException(
                    status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                    detail=str(e),
                )

            plaintext = json.dumps(validated, separators=(",", ":")).encode("utf-8")
            try:
                envelope = encrypt(plaintext)
            except KEKUnavailableError as e:
                raise HTTPException(
                    status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
                    detail=f"KEK not configured: {e}",
                )

            set_parts.append("enc_version = %s")
            params.append(envelope.enc_version)
            set_parts.append("nonce = %s")
            params.append(envelope.nonce)
            set_parts.append("ciphertext = %s")
            params.append(envelope.ciphertext)
            audit_diff["after"]["payload"] = "<rotated>"

        # ---- Metadata-only updates ----
        if body.description is not None:
            set_parts.append("description = %s")
            params.append(body.description)
            audit_diff["before"]["description"] = prior["description"]
            audit_diff["after"]["description"]  = body.description

        if body.expires_at is not None:
            # `expires_at` lives inside metadata JSONB.
            new_meta = dict(prior.get("metadata") or {})
            new_meta["expires_at"] = body.expires_at.isoformat()
            set_parts.append("metadata = %s::jsonb")
            params.append(json.dumps(new_meta))
            audit_diff["before"]["expires_at"] = (prior.get("metadata") or {}).get("expires_at")
            audit_diff["after"]["expires_at"]  = body.expires_at.isoformat()

        set_parts.append("updated_at = now()")
        set_parts.append("updated_by = %s")
        params.append(str(ctx))

        sql = (
            f"UPDATE credentials SET {', '.join(set_parts)} "
            f"WHERE id = %s "
            f"RETURNING id, name, description, kind, enc_version, metadata, "
            f"          created_at, created_by, updated_at, updated_by"
        )
        params.append(cred_id)
        cur.execute(sql, params)
        row = cur.fetchone()

        emit_audit(
            conn,
            actor=ctx,
            action="credential.rotate" if body.payload is not None else "credential.update",
            resource_kind="credential",
            resource_id=str(cred_id),
            diff=audit_diff,
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
    return _row_to_out(row)


@router.delete("/{cred_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_credential(
    cred_id: int,
    req: Request,
    ctx: AuthContext = Depends(require("credentials.delete")),
) -> None:
    """Delete a credential. Refused with 409 if any destination still
    references it (the destinations.credential_id FK is RESTRICT)."""
    with pool().connection() as conn, conn.cursor() as cur:
        prior = _select_credential(cur, cred_id)
        if prior is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND,
                                detail="credential not found")
        try:
            cur.execute("DELETE FROM credentials WHERE id = %s", (cred_id,))
        except Exception as e:
            conn.rollback()
            if "foreign key" in str(e).lower():
                raise HTTPException(
                    status_code=status.HTTP_409_CONFLICT,
                    detail="credential is still referenced by one or more destinations",
                )
            raise
        emit_audit(
            conn,
            actor=ctx,
            action="credential.delete",
            resource_kind="credential",
            resource_id=str(cred_id),
            diff={"before": {"name": prior["name"], "kind": prior["kind"]}},
            client_ip=_client_ip(req),
            user_agent=_ua(req),
        )
        conn.commit()
