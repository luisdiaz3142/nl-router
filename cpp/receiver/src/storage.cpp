#include "storage.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace nlr {

namespace {

// DICOM UIDs are dotted-decimal strings up to 64 chars (e.g.
// "1.2.840.10008.1.2.1"). They're already filesystem-safe — only digits and
// dots. We still validate to fail loudly on malformed input rather than
// silently writing to weird paths.
//
// Specifically: reject empty UIDs and anything containing path separators or
// null bytes. We don't normalize case (UIDs are case-sensitive in DICOM).
void validate_uid(std::string_view uid, const char* what) {
    if (uid.empty()) {
        throw std::runtime_error(std::string{what} + ": empty UID");
    }
    for (char c : uid) {
        if (c == '/' || c == '\\' || c == '\0') {
            throw std::runtime_error(std::string{what} + ": UID contains path separator: " +
                                     std::string{uid});
        }
    }
}

}  // namespace

std::filesystem::path instance_path(
    const std::filesystem::path& landing_zone,
    std::string_view date_yyyymmdd,
    std::string_view study_uid,
    std::string_view series_uid,
    std::string_view sop_uid
) {
    validate_uid(study_uid,  "study_instance_uid");
    validate_uid(series_uid, "series_instance_uid");
    validate_uid(sop_uid,    "sop_instance_uid");

    return landing_zone
        / std::string{date_yyyymmdd}
        / std::string{study_uid}
        / std::string{series_uid}
        / (std::string{sop_uid} + ".dcm");
}

std::filesystem::path study_root(
    const std::filesystem::path& landing_zone,
    std::string_view date_yyyymmdd,
    std::string_view study_uid
) {
    validate_uid(study_uid, "study_instance_uid");
    return landing_zone / std::string{date_yyyymmdd} / std::string{study_uid};
}

std::string today_yyyymmdd() {
    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%d");
    return oss.str();
}

void ensure_parents(const std::filesystem::path& target) {
    const auto parent = target.parent_path();
    if (parent.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw std::filesystem::filesystem_error(
            "create_directories failed", parent, ec);
    }
}

}  // namespace nlr
