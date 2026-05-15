"""admin_audit emission helper.

Every state-changing API call writes one row to admin_audit. The diff JSONB
captures the before/after structure; credential payloads are explicitly
redacted upstream so they never appear here.
"""

from __future__ import annotations

import json
from typing import Any

from psycopg import Connection

from nl_router.api.auth import AuthContext


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
                client_ip,
                user_agent,
            ),
        )
