// nl-receiver/tag_extractor.hpp
//
// Extract DICOM tags from a received instance's dataset into a typed struct
// suitable for inserting into work_queue. Equivalent in role to dicomdiablo's
// `getdcmtags` C++ helper, but in-process: we don't spawn a child binary for
// each instance.
//
// We pull the same scalar set that work_queue promotes (per the design plan)
// plus a JSON-as-string blob containing every other tag we saw, for the
// JSONB column. The JSON is built by hand here (small/known schema) rather
// than via a JSON library.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

class DcmDataset;   // forward-declare from DCMTK

namespace nlr {

// Extracted scalar tags. Optional fields are nullopt when absent in the
// dataset (DICOM tags are mostly optional in any given SOP class). Strings
// preserve DICOM-multi-value backslash separation as received.
struct ExtractedTags {
    // ---- DICOM identity ----
    std::string                   study_instance_uid;        // required by schema
    std::optional<std::string>    series_instance_uid;
    std::optional<std::string>    accession_number;
    std::optional<std::string>    study_id;
    std::optional<std::string>    sop_instance_uid;

    // ---- Patient ----
    std::optional<std::string>    patient_id;
    std::optional<std::string>    patient_name;
    std::optional<std::string>    patient_birth_date;   // DICOM DA: YYYYMMDD
    std::optional<std::string>    patient_sex;          // M/F/O

    // ---- Equipment / origin ----
    std::optional<std::string>    modality;
    std::optional<std::string>    station_name;
    std::optional<std::string>    station_aet;
    std::optional<std::string>    retrieve_aet;
    std::optional<std::string>    institution_name;
    std::optional<std::string>    manufacturer;
    std::optional<std::string>    manufacturer_model_name;
    std::optional<std::string>    device_serial_number;
    std::optional<std::string>    device_uid;
    std::optional<std::string>    software_versions;

    // ---- Study / series ----
    std::optional<std::string>    study_description;
    std::optional<std::string>    series_description;
    std::optional<std::string>    protocol_name;
    std::optional<std::string>    body_part_examined;
    std::optional<std::string>    referring_physician_name;

    // ---- Timestamps (raw DICOM strings) ----
    std::optional<std::string>    study_date;          // YYYYMMDD
    std::optional<std::string>    study_time;          // HHMMSS[.FFFFFF]
    std::optional<std::string>    series_date;
    std::optional<std::string>    series_time;
    std::optional<std::string>    acquisition_date;
    std::optional<std::string>    acquisition_time;

    // ---- Numbering ----
    std::optional<std::int64_t>   series_number;
    std::optional<std::int64_t>   number_of_series;
    std::optional<std::int64_t>   instance_number;
    std::optional<std::int64_t>   acquisition_number;

    // ---- Acquisition / sequence ----
    std::optional<std::string>    acquisition_type;
    std::optional<std::string>    scanning_sequence;
    std::optional<std::string>    sequence_name;
    std::optional<std::string>    sequence_variant;
    std::optional<std::string>    image_type;          // multi-value, backslash-joined
    std::optional<std::string>    image_comments;
    std::optional<std::string>    contrast_bolus_agent;

    // ---- Numeric (NUMERIC(10,4) in DB; kept as string here to preserve DICOM DS precision) ----
    std::optional<std::string>    slice_thickness;
    std::optional<std::string>    magnetic_field_strength;

    // ---- Coded procedure ----
    std::optional<std::string>    code_value;
    std::optional<std::string>    code_meaning;

    // ---- Charset ----
    std::optional<std::string>    specific_character_set;

    // ---- All extracted tags as a JSON object literal ----
    // Encoded once during extraction so the DB insert is a single text bind.
    // Keys are PascalCase DICOM names (Modality, SeriesDescription, ...).
    std::string                   tags_json;
};

// Extract scalar tags + JSON blob from a DCMTK dataset.
//
// Required-presence rule: throws std::runtime_error if StudyInstanceUID,
// SeriesInstanceUID, or SOPInstanceUID are missing from the dataset — those
// are mandatory in every SOP class and a missing one means we received a
// malformed instance.
ExtractedTags extract(DcmDataset& dataset);

}  // namespace nlr
