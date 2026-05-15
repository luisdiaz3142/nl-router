#include "disk.hpp"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace nlr {

namespace {

// Is `child` lexically inside `parent`? Used as a safety check so we
// never walk up past the landing zone, even if a corrupt file_root_path
// points outside.
bool is_within(const std::filesystem::path& parent,
               const std::filesystem::path& child) {
    auto p = parent.lexically_normal();
    auto c = child.lexically_normal();
    auto p_it = p.begin();
    auto c_it = c.begin();
    while (p_it != p.end() && c_it != c.end()) {
        if (*p_it != *c_it) return false;
        ++p_it; ++c_it;
    }
    return p_it == p.end();   // every parent component appeared at the start of child
}

// Remove a directory iff it's empty. No-op if it has contents (someone
// else's study live in the same date dir) or doesn't exist.
void try_remove_empty_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    // `is_empty` distinguishes "non-empty" from "doesn't exist"; we treat
    // both as "leave it alone".
    if (!std::filesystem::exists(dir, ec) || ec) return;
    if (!std::filesystem::is_directory(dir, ec) || ec) return;
    if (!std::filesystem::is_empty(dir, ec) || ec) return;
    std::filesystem::remove(dir, ec);   // ignore errors here too
}

}  // namespace

CleanupResult delete_study_directory(
    const std::filesystem::path& landing_zone,
    const std::filesystem::path& study_root)
{
    CleanupResult result;

    // Refuse to act on paths outside the landing zone. Otherwise a corrupted
    // file_root_path could direct us to /etc, /home, anything.
    if (!is_within(landing_zone, study_root)) {
        throw std::filesystem::filesystem_error(
            "study_root not under landing_zone",
            study_root, landing_zone,
            std::make_error_code(std::errc::operation_not_permitted));
    }

    std::error_code ec;
    if (!std::filesystem::exists(study_root, ec)) {
        result.path_missing = true;
        return result;
    }
    if (ec) {
        // Permission denied or similar — treat as a real error.
        throw std::filesystem::filesystem_error("stat failed", study_root, ec);
    }

    // Count files + bytes before deletion so we can emit accurate metrics.
    if (std::filesystem::is_directory(study_root, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator(study_root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator{};
             it.increment(ec))
        {
            if (!it->is_regular_file(ec) || ec) continue;
            const auto sz = it->file_size(ec);
            if (!ec) {
                ++result.files_deleted;
                result.bytes_freed += static_cast<std::uint64_t>(sz);
            }
        }
        ec.clear();

        const auto removed = std::filesystem::remove_all(study_root, ec);
        if (ec) {
            throw std::filesystem::filesystem_error(
                "remove_all failed", study_root, ec);
        }
        // remove_all returns the count of files+dirs it removed; we used
        // our own file_count above for byte accounting.
        (void)removed;
    } else {
        // Single file at study_root path? Unusual but handle gracefully.
        const auto sz = std::filesystem::file_size(study_root, ec);
        if (!ec) {
            result.files_deleted = 1;
            result.bytes_freed = static_cast<std::uint64_t>(sz);
        }
        std::filesystem::remove(study_root, ec);
        if (ec) {
            throw std::filesystem::filesystem_error(
                "remove failed", study_root, ec);
        }
    }

    // Walk upward, removing now-empty parent dirs, but never past landing_zone.
    auto parent = study_root.parent_path();
    while (!parent.empty() && is_within(landing_zone, parent) && parent != landing_zone) {
        try_remove_empty_dir(parent);
        // Stop as soon as the directory still exists (i.e. is non-empty).
        std::error_code stat_ec;
        if (std::filesystem::exists(parent, stat_ec)) break;
        parent = parent.parent_path();
    }

    return result;
}

}  // namespace nlr
