-- 0005 down

BEGIN;

ALTER TABLE destinations
    DROP CONSTRAINT IF EXISTS fk_destinations_credential;

DROP TABLE IF EXISTS credentials;

COMMIT;
