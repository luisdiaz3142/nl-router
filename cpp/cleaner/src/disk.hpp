// nl-clean/disk.hpp
//
// Filesystem operations for the cleaner. Pure functions; all I/O is
// strictly local to this node's landing zone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace nlr {

// Result of a single study-directory cleanup.
struct CleanupResult {
    std::size_t   files_deleted   {0};
    std::uint64_t bytes_freed     {0};
    // True if the directory wasn't present to begin with (e.g. cleaner
    // running on a different node, or files were manually removed). Not
    // an error.
    bool          path_missing    {false};
};

// Recursively delete every file under `study_root`, then remove the
// directory itself. If the parent date directory is empty after the
// study is gone, remove it too — but stop walking up at `landing_zone`
// (we never delete the landing root, even if it ends up empty).
//
// Idempotent: a missing study_root counts as success with path_missing=true.
// Throws std::filesystem::filesystem_error on a real I/O failure (permission
// denied, hardware error, etc.); the caller turns that into a per-row
// failure log without halting the daemon.
CleanupResult delete_study_directory(
    const std::filesystem::path& landing_zone,
    const std::filesystem::path& study_root);

}  // namespace nlr
