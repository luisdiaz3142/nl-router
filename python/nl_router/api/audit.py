"""admin_audit emission helper.

Every state-changing API call writes one row to admin_audit. The diff JSONB
captures the before/after structure; credential payloads are explicitly
redacted upstream so they never appear here.
"""

from __future__ import annotations

import ipaddress
import json
from typing import Any

from psycopg import Connection

from nl_router.api.auth import AuthContext
from nl_router.api.metrics import AUDIT_EVENTS_TOTAL


def _coerce_inet(value: str | None) -> str | None:
    """Return `value` if it parses as an IPv4/IPv6 address, else None.

    admin_audit.client_ip is a Postgres INET column; passing a non-IP
    string (e.g. "testclient" from FastAPI's TestClient, or a malformed
    X-Forwarded-For header from a misbehaving reverse proxy) makes the
    INSERT fail with `invalid input syntax for type inet`. We coerce
    once at the boundary: drop the value if it doesn't parse. The
    audit row still lands, just without the client_ip.
    """
    if value is None:
        return None
    try:
        ipaddress.ip_address(value)
        return value
    except ValueError:
        return None


def emit_audit(
    conn: Connection,
    *,
    actor: AuthContext,
    action: str,                 # e.g. "rule.create", "rule.update", "destination.delete"
    resource_kind: str | None,
    resource_id: str | None,
    diff: dict[str, Any] | None,
    client_ip: str | None = None,
    user_agent: str | None = None,
) -> None:
    """Insert one admin_audit row inside the caller's transaction.

    The caller is responsible for committing.
    """
    with conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO admin_audit (
                actor, actor_kind, action, resource_kind, resource_id,
                diff, client_ip, user_agent
            ) VALUES (%s, %s, %s, %s, %s, %s::jsonb, %s, %s)
            """,
            (
                str(actor),
                "token",
                action,
                resource_kind,
                resource_id,
                json.dumps(diff) if diff is not None else None,
                _coerce_inet(client_ip),
                user_agent,
            ),
        )
    # Bump after the INSERT so a SQL failure (which would already raise)
    # doesn't inflate the counter. If the caller's transaction later
    # rolls back, we have a slight over-count — acceptable for an
    # observability counter; the alternative (after-commit hooks) buys
    # nothing operationally.
    AUDIT_EVENTS_TOTAL.labels(action=action).inc()
