-- 0001 down: drop enum types in reverse order.

BEGIN;

DROP TYPE IF EXISTS user_role;
DROP TYPE IF EXISTS job_status;
DROP TYPE IF EXISTS rule_scope;
DROP TYPE IF EXISTS rule_status;
DROP TYPE IF EXISTS close_trigger;
DROP TYPE IF EXISTS work_status;

COMMIT;
