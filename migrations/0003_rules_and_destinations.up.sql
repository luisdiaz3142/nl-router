-- 0003: Rules, destinations, processing modules, and their join tables.
-- Establishes the routing-rule data model and back-fills FKs on route_assignments.

BEGIN;

-- ---------------------------------------------------------------------------
-- rules: predicate-based routing rules with draft/disabled/enabled lifecycle.
-- ---------------------------------------------------------------------------
CREATE TABLE rules (
    id            BIGSERIAL PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE,
    description   TEXT,
    status        rule_status NOT NULL DEFAULT 'draft',
    scope         rule_scope  NOT NULL,
    predicate     TEXT NOT NULL,                  -- dicomdiablo-compatible expression DSL
    priority      SMALLINT NOT NULL DEFAULT 0,

    -- dispatch order across this rule's destinations
    dispatch_order TEXT NOT NULL DEFAULT 'parallel'
        CHECK (dispatch_order IN ('parallel','sequential')),

    -- per-rule overrides
    retention_override JSONB,
    -- shape: {"dispatched_h":24,"partial_h":168,"failed_h":720,
    --         "prune_dispatched_d":365,"prune_partial_d":1095,"prune_failed_d":1825}

    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    created_by    TEXT,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_by    TEXT
);

CREATE INDEX idx_rules_enabled_scope ON rules (scope) WHERE status = 'enabled';

-- ---------------------------------------------------------------------------
-- destinations: outbound endpoints with kind-specific config and retry policy.
-- kind is TEXT (not ENUM) so custom destination kinds can be added without
-- schema migrations.
-- ---------------------------------------------------------------------------
CREATE TABLE destinations (
    id            BIGSERIAL PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE,
    description   TEXT,
    kind          TEXT NOT NULL,
    enabled       BOOLEAN NOT NULL DEFAULT TRUE,
    config        JSONB NOT NULL,                 -- kind-specific shape, validated app-side
    credential_id BIGINT,                         -- FK added in 0005 once credentials exist

    -- per-destination concurrency cap (dispatcher thread-pool size for this destination)
    dispatch_concurrency SMALLINT NOT NULL DEFAULT 4,

    -- retry policy default for this destination
    retry_policy JSONB NOT NULL DEFAULT
        '{"max_attempts":5,"initial_backoff_s":30,"multiplier":2.0,"max_backoff_s":3600,"give_up_after_hours":72}'::jsonb,

    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    created_by    TEXT,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_by    TEXT
);

CREATE INDEX idx_destinations_kind ON destinations (kind);

-- ---------------------------------------------------------------------------
-- rule_destinations: many-to-many (rules can fan out to multiple destinations).
-- ---------------------------------------------------------------------------
CREATE TABLE rule_destinations (
    id             BIGSERIAL PRIMARY KEY,
    rule_id        BIGINT NOT NULL REFERENCES rules(id) ON DELETE CASCADE,
    destination_id BIGINT NOT NULL REFERENCES destinations(id),
    ordinal        SMALLINT NOT NULL DEFAULT 0,
    retry_policy_override JSONB,                  -- rare per-pairing override
    UNIQUE (rule_id, destination_id)
);

CREATE INDEX idx_rd_rule ON rule_destinations (rule_id);

-- ---------------------------------------------------------------------------
-- processing_modules: registry of processing module kinds (built-in + custom).
-- kind is TEXT (not ENUM) so operators can register their own.
-- ---------------------------------------------------------------------------
CREATE TABLE processing_modules (
    id          BIGSERIAL PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    description TEXT,
    kind        TEXT NOT NULL,
    config      JSONB NOT NULL DEFAULT '{}'::jsonb,
    enabled     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_processing_modules_kind ON processing_modules (kind);

-- ---------------------------------------------------------------------------
-- rule_processing_chain: ordered processing steps per rule, with optional
-- per-rule config overrides.
-- ---------------------------------------------------------------------------
CREATE TABLE rule_processing_chain (
    id              BIGSERIAL PRIMARY KEY,
    rule_id         BIGINT NOT NULL REFERENCES rules(id) ON DELETE CASCADE,
    module_id       BIGINT NOT NULL REFERENCES processing_modules(id),
    ordinal         SMALLINT NOT NULL,
    config_override JSONB,
    UNIQUE (rule_id, ordinal)
);

CREATE INDEX idx_rpc_rule ON rule_processing_chain (rule_id);

-- ---------------------------------------------------------------------------
-- Now that rules and destinations exist, back-fill the foreign keys on
-- route_assignments (declared in 0002 without REFERENCES to keep migration
-- order minimal).
-- ---------------------------------------------------------------------------
ALTER TABLE route_assignments
    ADD CONSTRAINT fk_ra_rule
        FOREIGN KEY (rule_id) REFERENCES rules(id),
    ADD CONSTRAINT fk_ra_destination
        FOREIGN KEY (destination_id) REFERENCES destinations(id);

-- ---------------------------------------------------------------------------
-- Seed built-in processing modules (kind names match the v1 module set).
-- These rows represent the registered module kinds; operators reference them
-- from rule_processing_chain. Workers are started separately (systemd units).
-- ---------------------------------------------------------------------------
INSERT INTO processing_modules (name, kind, description, config) VALUES
    ('anonymize_basic',      'anonymize_basic',      'Strip/replace configurable tags. Site-specific quick de-identification.', '{}'::jsonb),
    ('anonymize_dicom_psi',  'anonymize_dicom_psi',  'DICOM PS 3.15 Basic Application Confidentiality Profile.', '{}'::jsonb),
    ('tag_modifier',         'tag_modifier',         'Generic tag set/remove/replace. Translates institution names, normalizes descriptions.', '{}'::jsonb),
    ('transcode',            'transcode',            'Transfer syntax conversion (decompress, JPEG/JPEG-LS, lossless).', '{}'::jsonb),
    ('uid_remap',            'uid_remap',            'Assign new UIDs preserving series/study relationships.', '{}'::jsonb),
    ('http_callout',         'http_callout',         'POST metadata to external endpoint, optionally wait for response.', '{}'::jsonb),
    ('custom_script',        'custom_script',        'Invoke operator-supplied binary/script. Catchall extension point.', '{}'::jsonb);

COMMIT;
