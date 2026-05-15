// nl-receiver/storage.hpp
//
// Filesystem layout helpers for received instances.
//
// Layout (per the design plan):
//   <landing_zone>/<YYYY-MM-DD>/<study_uid>/<series_uid>/<sop_uid>.dcm
//
// Per-server: each receiver writes only to its own local disk. The
// work_queue row's server_id + file_root_path together tell downstream
// modules where to find the files.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace nlr {

// Compute the path for an instance file. Does not touch the filesystem.
//
// On entry:
//   landing_zone  - absolute path to /var/lib/nl-router/incoming (or test path)
//   study_uid     - StudyInstanceUID  (0020,000D)
//   series_uid    - SeriesInstanceUID (0020,000E)
//   sop_uid       - SOPInstanceUID    (0008,0018)
//
// The date directory uses the receive day, not the study date — keeps daily
// directory counts bounded and makes operator inspection easier.
std::filesystem::path instance_path(
    const std::filesystem::path& landing_zone,
    std::string_view date_yyyymmdd,
    std::string_view study_uid,
    std::string_view series_uid,
    std::string_view sop_uid
);

// Compute the study root path (parent of all series directories for that
// study). This is the value we store in work_queue.file_root_path.
std::filesystem::path study_root(
    const std::filesystem::path& landing_zone,
    std::string_view date_yyyymmdd,
    std::string_view study_uid
);

// Today's date as YYYY-MM-DD (UTC). Cached for the lifetime of an
// association — instances arriving across midnight are kept under the
// directory chosen at association start, not split.
std::string today_yyyymmdd();

// Ensure all parent directories of `target` exist. Idempotent; safe to call
// from multiple threads (relies on std::filesystem::create_directories which
// tolerates concurrent creates).
//
// Throws std::filesystem::filesystem_error on failure.
void ensure_parents(const std::filesystem::path& target);

}  // namespace nlr
