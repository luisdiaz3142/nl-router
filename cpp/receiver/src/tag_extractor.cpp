#include "tag_extractor.hpp"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdicent.h>
#include <dcmtk/dcmdata/dcelem.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/ofstd/ofstring.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nlr {

namespace {

// Read a string tag from a dataset. Returns nullopt when the tag is absent
// or empty. DCMTK's findAndGetOFString returns the first VM-1 element; we
// extend to multi-value by joining with backslashes (DICOM's native convention).
std::optional<std::string> read_string(DcmDataset& ds, const DcmTagKey& tag) {
    OFString out;
    // Try multi-value first via findAndGetOFStringArray, which returns the
    // backslash-joined form for VM>1 tags.
    const OFCondition cond = ds.findAndGetOFStringArray(tag, out);
    if (cond.bad() || out.empty()) {
        return std::nullopt;
    }
    return std::string{out.c_str(), out.length()};
}

std::optional<std::int64_t> read_int(DcmDataset& ds, const DcmTagKey& tag) {
    auto s = read_string(ds, tag);
    if (!s.has_value()) return std::nullopt;
    try {
        return static_cast<std::int64_t>(std::stoll(*s));
    } catch (const std::exception&) {
        // DICOM IS values shouldn't fail to parse; if we get one that does,
        // surface as "not present" rather than throwing. The Router will
        // still be able to evaluate predicates that don't reference it.
        return std::nullopt;
    }
}

// JSON-escape a string into the provided buffer.
void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Walk every top-level element in a dataset and emit (PascalCaseName, string-value)
// pairs into the JSON object. Skips:
//   * pixel data and other byte-blob VRs (OB/OW/OF/OD/UN/SQ) — they explode
//     log lines and aren't useful for routing predicates
//   * private tags by default — operators wanting them should configure
//     dcm_extra_tags later (deferred in M1)
//
// The output is a JSON object: {"Modality":"CT","SeriesDescription":"AX 2.0",...}
// with no surrounding whitespace. Values are always strings; numeric tags
// (IS/DS) are kept as-received so consumers can re-parse with the precision
// they need.
std::string build_tags_json(DcmDataset& ds) {
    std::string out;
    out.reserve(1024);
    out.push_back('{');

    bool first = true;
    auto* iter = static_cast<DcmObject*>(ds.nextInContainer(nullptr));
    while (iter != nullptr) {
        if (iter->isLeaf()) {
            auto* elem = OFstatic_cast(DcmElement*, iter);
            const DcmTagKey key = elem->getTag();
            const DcmEVR vr     = elem->getVR();

            // Skip private tags for now.
            const bool is_private = (key.getGroup() & 1) != 0;

            // Skip byte-blob VRs that don't belong in routing predicates.
            const bool is_blob_vr =
                vr == EVR_OB || vr == EVR_OW || vr == EVR_OF || vr == EVR_OD ||
                vr == EVR_UN || vr == EVR_SQ || vr == EVR_pixelSQ;

            if (!is_private && !is_blob_vr) {
                OFString value;
                if (elem->getOFStringArray(value).good() && !value.empty()) {
                    // Resolve the standard data dictionary name. Falls back to
                    // (gggg,eeee) if the tag isn't in the dictionary.
                    const DcmDataDictionary& dict = dcmDataDict.rdlock();
                    const DcmDictEntry* entry = dict.findEntry(key, nullptr);
                    std::string name;
                    if (entry != nullptr && entry->getTagName() != nullptr &&
                        std::string{entry->getTagName()} != DcmTag_ERROR_TagName)
                    {
                        name = entry->getTagName();
                    } else {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%04X%04X",
                                      key.getGroup(), key.getElement());
                        name = buf;
                    }
                    dcmDataDict.rdunlock();

                    if (!first) out.push_back(',');
                    first = false;
                    append_json_string(out, name);
                    out.push_back(':');
                    append_json_string(out, std::string{value.c_str(), value.length()});
                }
            }
        }
        iter = ds.nextInContainer(iter);
    }
    out.push_back('}');
    return out;
}

}  // namespace

ExtractedTags extract(DcmDataset& ds) {
    ExtractedTags t;

    // ---- Required identity tags (throw if missing) ----
    if (auto v = read_string(ds, DCM_StudyInstanceUID); v.has_value()) {
        t.study_instance_uid = *v;
    } else {
        throw std::runtime_error("StudyInstanceUID missing from dataset");
    }
    t.series_instance_uid = read_string(ds, DCM_SeriesInstanceUID);
    t.sop_instance_uid    = read_string(ds, DCM_SOPInstanceUID);
    if (!t.series_instance_uid.has_value() || !t.sop_instance_uid.has_value()) {
        throw std::runtime_error(
            "SeriesInstanceUID or SOPInstanceUID missing from dataset");
    }

    t.accession_number  = read_string(ds, DCM_AccessionNumber);
    t.study_id          = read_string(ds, DCM_StudyID);

    // ---- Patient ----
    t.patient_id         = read_string(ds, DCM_PatientID);
    t.patient_name       = read_string(ds, DCM_PatientName);
    t.patient_birth_date = read_string(ds, DCM_PatientBirthDate);
    t.patient_sex        = read_string(ds, DCM_PatientSex);

    // ---- Equipment / origin ----
    t.modality                 = read_string(ds, DCM_Modality);
    t.station_name             = read_string(ds, DCM_StationName);
    t.institution_name         = read_string(ds, DCM_InstitutionName);
    t.manufacturer             = read_string(ds, DCM_Manufacturer);
    t.manufacturer_model_name  = read_string(ds, DCM_ManufacturerModelName);
    t.device_serial_number     = read_string(ds, DCM_DeviceSerialNumber);
    t.device_uid               = read_string(ds, DCM_DeviceUID);
    t.software_versions        = read_string(ds, DCM_SoftwareVersions);
    // StationAETitle / RetrieveAETitle are common in DICOMDIR / Q/R contexts
    // but rare in C-STORE datasets. We still try.
    t.station_aet              = read_string(ds, DCM_StationAETitle);
    t.retrieve_aet             = read_string(ds, DCM_RetrieveAETitle);

    // ---- Study / series ----
    t.study_description        = read_string(ds, DCM_StudyDescription);
    t.series_description       = read_string(ds, DCM_SeriesDescription);
    t.protocol_name            = read_string(ds, DCM_ProtocolName);
    t.body_part_examined       = read_string(ds, DCM_BodyPartExamined);
    t.referring_physician_name = read_string(ds, DCM_ReferringPhysicianName);

    // ---- Timestamps ----
    t.study_date       = read_string(ds, DCM_StudyDate);
    t.study_time       = read_string(ds, DCM_StudyTime);
    t.series_date      = read_string(ds, DCM_SeriesDate);
    t.series_time      = read_string(ds, DCM_SeriesTime);
    t.acquisition_date = read_string(ds, DCM_AcquisitionDate);
    t.acquisition_time = read_string(ds, DCM_AcquisitionTime);

    // ---- Numbering ----
    t.series_number       = read_int(ds, DCM_SeriesNumber);
    t.number_of_series    = read_int(ds, DCM_NumberOfSeriesRelatedInstances);
    t.instance_number     = read_int(ds, DCM_InstanceNumber);
    t.acquisition_number  = read_int(ds, DCM_AcquisitionNumber);

    // ---- Acquisition / sequence ----
    t.acquisition_type     = read_string(ds, DCM_AcquisitionType);
    t.scanning_sequence    = read_string(ds, DCM_ScanningSequence);
    t.sequence_name        = read_string(ds, DCM_SequenceName);
    t.sequence_variant     = read_string(ds, DCM_SequenceVariant);
    t.image_type           = read_string(ds, DCM_ImageType);
    t.image_comments       = read_string(ds, DCM_ImageComments);
    t.contrast_bolus_agent = read_string(ds, DCM_ContrastBolusAgent);

    // ---- Numeric (kept as DICOM DS strings to preserve precision) ----
    t.slice_thickness         = read_string(ds, DCM_SliceThickness);
    t.magnetic_field_strength = read_string(ds, DCM_MagneticFieldStrength);

    // ---- Coded procedure ----
    t.code_value   = read_string(ds, DCM_CodeValue);
    t.code_meaning = read_string(ds, DCM_CodeMeaning);

    // ---- Charset ----
    t.specific_character_set = read_string(ds, DCM_SpecificCharacterSet);

    // ---- Full tag set JSON ----
    t.tags_json = build_tags_json(ds);
    return t;
}

}  // namespace nlr
