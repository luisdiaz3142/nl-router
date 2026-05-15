-- 0005: credentials table.
-- App-level envelope encryption: payloads are encrypted via AES-256-GCM before
-- insert. KEK lives outside Postgres (env var or file). DB only ever holds
-- ciphertext + nonce. kind is TEXT (not ENUM) so new credential kinds can be
-- added without schema migrations.

BEGIN;

CREATE TABLE credentials (
    id          BIGSERIAL PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    description TEXT,
    kind        TEXT NOT NULL,
    -- crypto algorithm version (lets us migrate from AES-256-GCM to a future
    -- algorithm without code branches in consumers)
    enc_version SMALLINT NOT NULL,
    nonce       BYTEA NOT NULL,
    ciphertext  BYTEA NOT NULL,
    -- non-sensitive metadata: expires_at, last_used_at, last_used_count, etc.
    metadata    JSONB,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    created_by  TEXT,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_by  TEXT
);

CREATE INDEX idx_credentials_kind ON credentials (kind);

-- Back-fill the FK on destinations.credential_id now that credentials exists.
ALTER TABLE destinations
    ADD CONSTRAINT fk_destinations_credential
        FOREIGN KEY (credential_id) REFERENCES credentials(id);

COMMIT;
