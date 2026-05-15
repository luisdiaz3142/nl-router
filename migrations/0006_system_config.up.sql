-- 0006: system_config table with seed defaults.
-- Simple key/value store with JSONB values. Modules read once at startup,
-- cache in-memory, refresh on LISTEN/NOTIFY for the 'system_config' channel.

BEGIN;

CREATE TABLE system_config (
    key         TEXT PRIMARY KEY,
    value       JSONB NOT NULL,
    description TEXT,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_by  TEXT
);

-- ---------------------------------------------------------------------------
-- Seed defaults. Operators tune via UI/CLI; runtime values are validated by
-- the consuming module before use.
-- ---------------------------------------------------------------------------

-- Receiver
INSERT INTO system_config (key, value, description) VALUES
    ('receiver.max_associations',         '100',        'Hard cap on concurrent inbound DICOM associations per receiver process.'),
    ('receiver.association_timeout_ms',   '30000',      'Idle association kill timeout in milliseconds.'),
    ('receiver.pdu_max_size',             '131072',     'Maximum PDU size advertised to peers.'),
    ('receiver.study_idle_close_s',       '60',         'Default seconds of inactivity before closing a study (study_timer trigger).'),
    ('receiver.series_idle_close_s',      '60',         'Default seconds of inactivity before closing a series (series_timer trigger).'),
    ('receiver.disk_warn_pct',            '85',         'Landing-zone disk usage percent at which to log warnings and emit alerts.'),
    ('receiver.disk_reject_pct',          '95',         'Landing-zone disk usage percent at which to A-ASSOCIATE-RJ new associations.'),
    ('receiver.disk_poll_interval_s',     '10',         'Interval for statfs polling of the landing zone.');

-- Router
INSERT INTO system_config (key, value, description) VALUES
    ('router.poll_interval_ms',  '500',  'Fallback poll interval when LISTEN/NOTIFY is not driving wake-ups.'),
    ('router.batch_size',        '32',   'Maximum work_queue rows claimed per pickup transaction.'),
    ('router.lease_seconds',     '60',   'Worker claim lease in seconds; reclaimed by sweeper if exceeded.');

-- Dispatcher
INSERT INTO system_config (key, value, description) VALUES
    ('dispatcher.default_concurrency', '4',   'Default per-destination thread-pool size when destination does not override.'),
    ('dispatcher.reaper_interval_s',   '30',  'Interval for reclaiming expired claims on route_assignments.');

-- Cleaner
INSERT INTO system_config (key, value, description) VALUES
    ('cleaner.scan_interval_s',  '60',   'Cleaner scan interval per node.'),
    ('cleaner.file_batch',       '100',  'Files cleaned per cleaner cycle per node.'),
    ('cleaner.prune_batch',      '500',  'Rows pruned per leader cycle.');

-- Retention defaults (overridable per-rule via rules.retention_override)
INSERT INTO system_config (key, value, description) VALUES
    ('retention.dispatched_h',         '24',    'Hours to retain files for dispatched rows.'),
    ('retention.partial_h',            '168',   'Hours to retain files for dispatched_partial rows (1 week).'),
    ('retention.failed_h',             '720',   'Hours to retain files for failed rows (30 days).'),
    ('retention.prune_dispatched_d',   '365',   'Days to retain DB rows for dispatched studies before pruning.'),
    ('retention.prune_partial_d',      '1095',  'Days to retain DB rows for dispatched_partial studies (3 years).'),
    ('retention.prune_failed_d',       '1825',  'Days to retain DB rows for failed studies (5 years).');

-- Credentials
INSERT INTO system_config (key, value, description) VALUES
    ('credentials.cache_ttl_seconds', '300',  'In-memory decryption cache TTL per process (5 minutes default).'),
    ('credentials.audit_min_interval_s', '60', 'Minimum interval between last_used_at metadata refreshes per credential.');

-- API
INSERT INTO system_config (key, value, description) VALUES
    ('api.session_ttl_minutes',    '480',  'Browser session lifetime for OIDC/local logins (8 hours).'),
    ('api.login_max_attempts',     '5',    'Per-username login attempts before lockout window.'),
    ('api.login_lockout_minutes',  '15',   'Lockout window after exceeding login_max_attempts.');

COMMIT;
