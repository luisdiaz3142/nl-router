"""Token authentication + RBAC for the management API.

v1 supports API tokens only. Users + OIDC land in a follow-up M4 slice.

Tokens are minted as `nlr_<base64url-32-bytes>` and shown to the operator
exactly once at creation time. We store only the SHA-256 hash in the
`api_tokens` table; on each request the Bearer header value is hashed and
looked up.

RBAC: each token row carries an explicit `permissions` JSONB array of
`<resource>.<verb>` strings (e.g. `rules.write`, `destinations.read`).
Route handlers declare what permissions they require; the dependency
rejects unauthorized callers with 403.
"""

from __future__ import annotations

import hashlib
import secrets
from dataclasses import dataclass

from fastapi import Depends, Header, HTTPException, status
from typing_extensions import Annotated

from nl_router.db import pool


# ---- Permission helpers --------------------------------------------------

# Predefined permission bundles. Tokens may have an arbitrary explicit
# permission set, but the CLI's "create token" command uses these named
# roles as shorthand.
ROLE_PERMISSIONS: dict[str, list[str]] = {
    "admin": [
        "rules.read", "rules.write", "rules.delete",
        "destinations.read", "destinations.write", "destinations.delete",
        "destinations.test",
        "credentials.read_metadata", "credentials.write", "credentials.rotate", "credentials.delete",
        "modules.read", "modules.write",
        "workqueue.read", "workqueue.hold", "workqueue.retry",
        "system_config.read", "system_config.write",
        "audit.read",
        "tokens.read", "tokens.write", "tokens.revoke",
        "users.read", "users.write",
    ],
    "operator": [
        "rules.read", "rules.write", "rules.delete",
        "destinations.read", "destinations.write", "destinations.delete",
        "destinations.test",
        "credentials.read_metadata", "credentials.write", "credentials.rotate",
        "modules.read", "modules.write",
        "workqueue.read", "workqueue.hold", "workqueue.retry",
        "system_config.read",
        "audit.read",
    ],
    "viewer": [
        "rules.read", "destinations.read",
        "credentials.read_metadata", "modules.read",
        "workqueue.read", "system_config.read", "audit.read",
    ],
    "service": [
        # Empty by default; tokens of this role carry per-token explicit perms.
    ],
}


def mint_token() -> tuple[str, str]:
    """Generate a fresh API token. Returns (raw_token, sha256_hash).

    The raw token is shown to the operator once; only the hash is stored
    and looked up on auth.

    secrets.token_urlsafe(32) yields 32 random bytes (256 bits of
    entropy from os.urandom) encoded as URL-safe base64; the printed
    form is ``nlr_<43-char-base64url>``.
    """
    raw_token = "nlr_" + secrets.token_urlsafe(32)
    token_hash = hashlib.sha256(raw_token.encode()).hexdigest()
    return raw_token, token_hash


def hash_token(raw_token: str) -> str:
    """SHA-256 hex digest of a raw token (matches the form stored in DB)."""
    return hashlib.sha256(raw_token.encode()).hexdigest()


# ---- Auth dependency -----------------------------------------------------


@dataclass(frozen=True)
class AuthContext:
    """The identity behind a request, derived from a Bearer token."""

    token_id: int
    token_name: str
    permissions: frozenset[str]

    def has_perm(self, perm: str) -> bool:
        return perm in self.permissions

    def __str__(self) -> str:                   # used in admin_audit.actor
        return f"token:{self.token_id}"


def _bearer_from_header(authorization: str | None) -> str:
    """Extract the bearer token value from an Authorization header.

    Accepts both `Authorization: Bearer <token>` and a raw `<token>` value
    (the latter is convenient for curl one-liners and shell scripts).
    """
    if not authorization:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="missing Authorization header",
            headers={"WWW-Authenticate": "Bearer"},
        )
    parts = authorization.strip().split(None, 1)
    if len(parts) == 2 and parts[0].lower() == "bearer":
        return parts[1]
    if len(parts) == 1:
        return parts[0]
    raise HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail="malformed Authorization header",
        headers={"WWW-Authenticate": "Bearer"},
    )


class InvalidToken(Exception):
    """Raised by validate_raw_token when the token is unknown / revoked
    / expired. Header-based and cookie-based dependencies convert this
    into the appropriate response (401 JSON / 302 redirect respectively)."""


def validate_raw_token(raw_token: str) -> AuthContext:
    """Validate a raw token string against the api_tokens table.

    Shared by both the Authorization-header dependency (api/auth.py)
    and the nlr_session-cookie dependency (ui/auth.py). Updates
    `last_used_at` at most once per minute per token (throttle prevents
    a write storm at API request rates).

    Raises:
        InvalidToken: token is missing/empty/unknown/revoked/expired.
    """
    if not raw_token:
        raise InvalidToken("empty token")
    token_hash = hash_token(raw_token)

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            """
            SELECT id, name, permissions
              FROM api_tokens
             WHERE token_hash = %s
               AND revoked = FALSE
               AND (expires_at IS NULL OR expires_at > now())
            """,
            (token_hash,),
        )
        row = cur.fetchone()
        if row is None:
            raise InvalidToken("invalid or revoked token")
        # Throttled write-back so authenticated traffic doesn't pin a
        # row lock per request.
        try:
            cur.execute(
                "UPDATE api_tokens "
                "   SET last_used_at = now() "
                " WHERE id = %s "
                "   AND (last_used_at IS NULL "
                "        OR last_used_at < now() - interval '1 minute')",
                (row["id"],),
            )
            conn.commit()
        except Exception:
            conn.rollback()

    return AuthContext(
        token_id=row["id"],
        token_name=row["name"],
        permissions=frozenset(row["permissions"] or []),
    )


async def auth_required(
    authorization: Annotated[str | None, Header(alias="Authorization")] = None,
) -> AuthContext:
    """Dependency: validate the Bearer token and return an AuthContext.

    Returns 401 if the header is missing/malformed or the token is
    unknown or revoked.
    """
    raw = _bearer_from_header(authorization)
    try:
        return validate_raw_token(raw)
    except InvalidToken as e:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail=str(e),
            headers={"WWW-Authenticate": "Bearer"},
        ) from e


def require(*perms: str):
    """Dependency factory: enforce that the auth context has every permission.

    Usage:
        @router.post("/rules", dependencies=[Depends(require("rules.write"))])
        def create_rule(...): ...
    """

    async def _checker(ctx: AuthContext = Depends(auth_required)) -> AuthContext:
        missing = [p for p in perms if not ctx.has_perm(p)]
        if missing:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail=f"missing permission(s): {', '.join(missing)}",
            )
        return ctx

    return _checker
