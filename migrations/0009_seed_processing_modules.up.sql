-- Pre-register the built-in processing modules so operators can attach
-- them to rules immediately without an INSERT step. One row per kind
-- the .deb actually ships a binary for; new modules added in future
-- migrations should follow the same shape.
--
-- ON CONFLICT (name) DO NOTHING makes the migration idempotent for
-- environments that already had these rows inserted by hand during M6
-- development.

INSERT INTO processing_modules (name, description, kind, config, enabled)
VALUES
    (
        'anonymize_basic',
        'Built-in: strip / replace arbitrary DICOM tags via DCMTK data dictionary names.',
        'anonymize_basic',
        '{}'::jsonb,
        TRUE
    ),
    (
        'standardize_institution_group',
        'Built-in: sets (0008,1040) InstitutionalDepartmentName to the configured value (default "NVRA"). Creates the tag if missing.',
        'standardize_institution_group',
        '{"value": "NVRA"}'::jsonb,
        TRUE
    )
ON CONFLICT (name) DO NOTHING;
