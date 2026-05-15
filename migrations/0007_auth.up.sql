-- 0007: Auth tables — users, API tokens, login attempts.
-- Users authenticate via OIDC (preferred), local password (fallback), or both.
-- API tokens carry an explicit permission set independent of user role.

BEGIN;

CREATE TABLE users (
    id              BIGSERIAL PRIMARY KEY,
    username        TEXT NOT NULL UNIQUE,
    email           TEXT,
    display_name    TEXT,
    password_hash   TEXT,                       -- Argon2id; NULL when user is OIDC-only
    oidc_issuer     TEXT,                       -- iss claim; NULL when user is local-only
    oidc_subject    TEXT,                       -- sub claim
    role            user_role NOT NULL,
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_login_at   TIMESTAMPTZ,
    CHECK (password_hash IS NOT NULL OR oidc_subject IS NOT NULL)
);

CREATE UNIQUE INDEX idx_users_oidc
    ON users (oidc_issuer, oidc_subject)
    WHERE oidc_subject IS NOT NULL;

CREATE TABLE api_tokens (
    id              BIGSERIAL PRIMARY KEY,
    name            TEXT NOT NULL,
    token_hash      TEXT NOT NULL UNIQUE,        -- only hash stored; raw token shown once at creation
    permissions     JSONB NOT NULL,              -- explicit permission set, not derived from role
    created_by      BIGINT REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_used_at    TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ,                 -- NULL = no expiry
    revoked         BOOLEAN NOT NULL DEFAULT FALSE
);

CREATE INDEX idx_api_tokens_active ON api_tokens (token_hash) WHERE revoked = FALSE;
CREATE INDEX idx_api_tokens_created_by ON api_tokens (created_by);

CREATE TABLE login_attempts (
    id           BIGSERIAL PRIMARY KEY,
    username     TEXT NOT NULL,
    client_ip    INET NOT NULL,
    success      BOOLEAN NOT NULL,
    attempted_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_login_attempts_recent ON login_attempts (username, attempted_at DESC);
CREATE INDEX idx_login_attempts_ip     ON login_attempts (client_ip, attempted_at DESC);

COMMIT;
