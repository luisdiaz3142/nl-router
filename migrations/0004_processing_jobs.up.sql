-- 0004: processing_jobs queue table.
-- Long-running module workers pull from this table filtered by module_kind.
-- Module workers (built-in binaries or operator-supplied containers) implement
-- the same contract: connect to DB, claim via SKIP LOCKED, process, mark done.

BEGIN;

CREATE TABLE processing_jobs (
    id              BIGSERIAL PRIMARY KEY,
    work_queue_id   BIGINT NOT NULL REFERENCES work_queue(id) ON DELETE CASCADE,
    rule_id         BIGINT NOT NULL REFERENCES rules(id),
    module_id       BIGINT NOT NULL REFERENCES processing_modules(id),

    -- denormalized for cheap worker filter (avoid a join on every pickup)
    module_kind     TEXT   NOT NULL,
    ordinal         SMALLINT NOT NULL,            -- position in this row's processing chain
    server_id       TEXT NOT NULL,                -- node that owns the input files

    input_path      TEXT NOT NULL,
    output_path     TEXT,                         -- worker writes on success
    config          JSONB NOT NULL,               -- module defaults merged with rule overrides

    status          job_status NOT NULL DEFAULT 'pending',

    -- worker concurrency (SKIP LOCKED for pickup + claim columns for observability)
    claimed_by       TEXT,
    claimed_at       TIMESTAMPTZ,
    claim_expires_at TIMESTAMPTZ,

    started_at      TIMESTAMPTZ,
    completed_at    TIMESTAMPTZ,
    attempts        INT NOT NULL DEFAULT 0,
    last_error      TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Worker pickup index (partial; only pending jobs of the worker's kind on the worker's node).
CREATE INDEX idx_pj_pending     ON processing_jobs (module_kind, server_id)
    WHERE status = 'pending';
CREATE INDEX idx_pj_workqueue   ON processing_jobs (work_queue_id);
CREATE INDEX idx_pj_claim_expires ON processing_jobs (claim_expires_at)
    WHERE claimed_by IS NOT NULL AND status = 'processing';

COMMIT;
