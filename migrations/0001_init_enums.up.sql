-- 0001: Initial enum types for nl-router.
-- These types are referenced by subsequent migrations.

BEGIN;

CREATE TYPE work_status AS ENUM (
    'received',
    'routing',
    'routed',
    'processing',
    'processed',
    'dispatching',
    'dispatched',
    'dispatched_partial',
    'failed',
    'cleaned'
);

CREATE TYPE close_trigger AS ENUM (
    'study_timer',
    'series_timer',
    'assoc_end'
);

CREATE TYPE rule_status AS ENUM (
    'draft',
    'disabled',
    'enabled'
);

CREATE TYPE rule_scope AS ENUM (
    'study',
    'series'
);

CREATE TYPE job_status AS ENUM (
    'pending',
    'processing',
    'done',
    'failed'
);

CREATE TYPE user_role AS ENUM (
    'admin',
    'operator',
    'viewer',
    'service'
);

COMMIT;
