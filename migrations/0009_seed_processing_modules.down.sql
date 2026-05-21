-- Remove the seeded module rows. We delete by `name` because that's
-- what the up migration inserted by; ids may differ across environments.
-- DELETE doesn't cascade through rule_processing_chain, so rule chains
-- referencing these modules will block the delete — that's intentional,
-- since silently breaking active rules would be worse.

DELETE FROM processing_modules
 WHERE name IN ('anonymize_basic', 'standardize_institution_group');
