-- 0002: work_queue (the central pipeline table) and route_assignments (per-destination outputs).
-- work_queue is the input contract for the Router and the audit-grade record of every
-- received study/series as it moves through the phases.

BEGIN;

CREATE TABLE work_queue (
    id BIGSERIAL PRIMARY KEY,
    server_id TEXT NOT NULL,

    -- DICOM identity
    study_instance_uid  TEXT NOT NULL,
    series_instance_uid TEXT,                     -- NULL when row groups a study
    accession_number    TEXT,
    study_id            TEXT,                     -- DICOM (0020,0010), distinct from study_instance_uid
    sop_instance_uid    TEXT,                     -- when row is at instance granularity

    -- patient (raw; HIPAA-cleared deployment assumed)
    patient_id          TEXT,
    patient_name        TEXT,
    patient_birth_date  DATE,
    patient_sex         CHAR(1),

    -- network context
    calling_aet  TEXT NOT NULL,
    called_aet   TEXT NOT NULL,
    peer_ip      INET NOT NULL,

    -- study/series metadata commonly used in routing predicates
    -- (full dicomdiablo predicate parity; everything here is rule-addressable)
    modality                 TEXT,
    station_name             TEXT,
    station_aet              TEXT,
    retrieve_aet             TEXT,
    institution_name         TEXT,
    manufacturer             TEXT,
    manufacturer_model_name  TEXT,
    device_serial_number     TEXT,
    device_uid               TEXT,
    software_versions        TEXT,
    study_description        TEXT,
    series_description       TEXT,
    protocol_name            TEXT,
    body_part_examined       TEXT,
    referring_physician_name TEXT,

    -- timestamps from DICOM
    study_date               DATE,
    study_time               TIME,
    series_date              DATE,
    series_time              TIME,
    acquisition_date         DATE,
    acquisition_time         TIME,

    -- numbering
    series_number            INT,
    number_of_series         INT,
    instance_number          INT,
    acquisition_number       INT,

    -- acquisition / sequence (MR/CT routing axes)
    acquisition_type         TEXT,
    scanning_sequence        TEXT,
    sequence_name            TEXT,
    sequence_variant         TEXT,
    image_type               TEXT,                -- DICOM multi-value, backslash-joined as received
    image_comments           TEXT,
    contrast_bolus_agent     TEXT,

    -- numeric routing thresholds (DICOM DS values)
    slice_thickness          NUMERIC(10,4),
    magnetic_field_strength  NUMERIC(10,4),

    -- coded procedure
    code_value               TEXT,
    code_meaning             TEXT,

    -- charset
    specific_character_set   TEXT,

    -- ingestion stats
    instance_count INT         NOT NULL,
    byte_count     BIGINT      NOT NULL,
    received_at    TIMESTAMPTZ NOT NULL,
    closed_at      TIMESTAMPTZ NOT NULL,
    close_trigger  close_trigger NOT NULL,

    -- on-disk pointer (local to this server_id; per-node local disk)
    file_root_path TEXT NOT NULL,

    -- phase tracking
    status         work_status NOT NULL,
    routed_at      TIMESTAMPTZ,
    processed_at   TIMESTAMPTZ,
    dispatched_at  TIMESTAMPTZ,
    cleaned_at     TIMESTAMPTZ,

    -- worker concurrency (SKIP LOCKED for pickup + columns for observability)
    claimed_by       TEXT,
    claimed_at       TIMESTAMPTZ,
    claim_expires_at TIMESTAMPTZ,

    -- failure / retry
    last_error    TEXT,
    failed_phase  TEXT,
    retry_count   INT NOT NULL DEFAULT 0,
    next_retry_at TIMESTAMPTZ,

    -- prioritization
    priority      SMALLINT NOT NULL DEFAULT 0,

    -- cleanup hold (operator can pin a row out of cleanup; holds never auto-expire)
    cleanup_hold        BOOLEAN NOT NULL DEFAULT FALSE,
    cleanup_hold_reason TEXT,
    cleanup_hold_by     TEXT,
    cleanup_hold_at     TIMESTAMPTZ,

    -- full extracted tag set (long tail beyond scalars above)
    tags JSONB NOT NULL
);

-- Indexes: hot routing-predicate columns plus phase pickup paths.
CREATE INDEX idx_wq_status_server   ON work_queue (status, server_id);
CREATE INDEX idx_wq_study_uid       ON work_queue (study_instance_uid);
CREATE INDEX idx_wq_calling_aet     ON work_queue (calling_aet);
CREATE INDEX idx_wq_modality        ON work_queue (modality);
CREATE INDEX idx_wq_protocol_name   ON work_queue (protocol_name);
CREATE INDEX idx_wq_institution     ON work_queue (institution_name);
CREATE INDEX idx_wq_acquisition_dt  ON work_queue (acquisition_date);
CREATE INDEX idx_wq_received_at     ON work_queue (received_at);
CREATE INDEX idx_wq_priority        ON work_queue (priority DESC, received_at)
    WHERE status IN ('received','routed','processed');
CREATE INDEX idx_wq_next_retry      ON work_queue (next_retry_at) WHERE status = 'failed';
CREATE INDEX idx_wq_claim_expires   ON work_queue (claim_expires_at) WHERE claimed_by IS NOT NULL;
CREATE INDEX idx_wq_tags_gin        ON work_queue USING GIN (tags);
CREATE INDEX idx_wq_cleanup_cand    ON work_queue (server_id, status)
    WHERE status IN ('dispatched','dispatched_partial','failed') AND cleanup_hold = FALSE;


-- route_assignments: one row per (work_queue_row, destination). The Router writes these
-- when rules fire; the Dispatcher consumes them.

CREATE TABLE route_assignments (
    id BIGSERIAL PRIMARY KEY,
    work_queue_id  BIGINT NOT NULL REFERENCES work_queue(id) ON DELETE CASCADE,
    rule_id        BIGINT NOT NULL,            -- FK added in 0003 once rules table exists
    destination_id BIGINT NOT NULL,            -- FK added in 0003 once destinations table exists

    -- denormalized for cheap worker filter (avoid joins on pickup)
    dispatch_kind  TEXT NOT NULL,
    server_id      TEXT NOT NULL,

    status         TEXT NOT NULL,              -- pending | dispatching | dispatched | failed
    attempts       INT  NOT NULL DEFAULT 0,
    last_error     TEXT,
    next_retry_at  TIMESTAMPTZ,
    dispatched_at  TIMESTAMPTZ,

    -- worker concurrency
    claimed_by       TEXT,
    claimed_at       TIMESTAMPTZ,
    claim_expires_at TIMESTAMPTZ,

    -- per-instance C-STORE statuses for dicom; HTTP response detail for http; etc.
    response_detail  JSONB,

    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_ra_work_queue   ON route_assignments (work_queue_id);
CREATE INDEX idx_ra_status       ON route_assignments (status);
CREATE INDEX idx_ra_pending_kind ON route_assignments (dispatch_kind, server_id, next_retry_at)
    WHERE status IN ('pending','failed');
CREATE INDEX idx_ra_next_retry   ON route_assignments (next_retry_at) WHERE status = 'failed';

COMMIT;
