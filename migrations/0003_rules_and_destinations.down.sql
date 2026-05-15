-- 0003 down

BEGIN;

ALTER TABLE route_assignments
    DROP CONSTRAINT IF EXISTS fk_ra_destination,
    DROP CONSTRAINT IF EXISTS fk_ra_rule;

DROP TABLE IF EXISTS rule_processing_chain;
DROP TABLE IF EXISTS processing_modules;
DROP TABLE IF EXISTS rule_destinations;
DROP TABLE IF EXISTS destinations;
DROP TABLE IF EXISTS rules;

COMMIT;
