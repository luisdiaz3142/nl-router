-- 0008: admin_audit table.
-- Append-only log of state-changing API calls. Credential payload values are
-- never captured in diff; only structural facts ("rotated", "created", etc.).

BEGIN;

CREATE TABLE admin_audit (
    id            BIGSERIAL PRIMARY KEY,
    actor         TEXT NOT NULL,                 -- user id or token id (rendered as string)
    actor_kind    TEXT NOT NULL,                 -- 'user' | 'token'
    action        TEXT NOT NULL,                 -- 'rule.create' | 'rule.update' | 'credential.rotate' | 'hold.set' | ...
    resource_kind TEXT,
    resource_id   TEXT,
    diff          JSONB,                         -- before/after; credentials always redacted
    client_ip     INET,
    user_agent    TEXT,
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_admin_audit_actor    ON admin_audit (actor, occurred_at DESC);
CREATE INDEX idx_admin_audit_resource ON admin_audit (resource_kind, resource_id, occurred_at DESC);
CREATE INDEX idx_admin_audit_action   ON admin_audit (action, occurred_at DESC);

COMMIT;
