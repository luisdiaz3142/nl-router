#include "file_handler.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "handler.hpp"
#include "logging.hpp"
#include "template.hpp"

namespace nlr {

namespace {

// Walk every .dcm file under the study root, preserving its relative
// subpath when preserve_hierarchy=true. Returns (source_path, relative_path)
// pairs. relative_path is empty for the flatten mode.
struct CopyEntry {
    std::filesystem::path source;
    std::filesystem::path rel;        // relative to study root
};

std::vector<CopyEntry> walk(const std::filesystem::path& root) {
    std::vector<CopyEntry> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec))
    {
        if (!it->is_regular_file(ec) || ec) continue;
        if (it->path().extension() == ".dcm") {
            CopyEntry e;
            e.source = it->path();
            e.rel    = std::filesystem::relative(it->path(), root, ec);
            if (ec) e.rel = it->path().filename();
            out.push_back(std::move(e));
        }
    }
    return out;
}

// Copy one file with optional fsync. Returns bytes copied. Throws
// filesystem_error on real failures (permission denied, ENOSPC, ...).
std::uint64_t copy_file(const std::filesystem::path& src,
                         const std::filesystem::path& dst,
                         bool do_fsync) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) throw std::filesystem::filesystem_error("create_directories", dst.parent_path(), ec);

    // copy_file overwrite-replace is the safest default — operators sending
    // the same study twice see the second copy land cleanly rather than
    // erroring out on existing file.
    std::filesystem::copy_file(src, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throw std::filesystem::filesystem_error("copy_file", src, dst, ec);

    if (do_fsync) {
        // POSIX fsync the data so a power loss doesn't leave the operator
        // thinking the study was archived when it actually wasn't.
        const int fd = ::open(dst.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    const auto sz = std::filesystem::file_size(dst, ec);
    return ec ? 0 : static_cast<std::uint64_t>(sz);
}

}  // namespace

DispatchResult FileDispatchHandler::dispatch(const Assignment& a, const Destination& d) {
    if (a.study_file_root.empty()) {
        return DispatchResult::permanent("no study_file_root recorded on work_queue row");
    }
    const std::filesystem::path src_root{a.study_file_root};

    // ---- Parse handler-specific config ----
    std::string path_template;
    bool preserve_hierarchy = true;
    bool do_fsync           = false;
    try {
        const auto j = nlohmann::json::parse(d.config_json);
        if (!j.contains("path_template") || !j["path_template"].is_string()) {
            return DispatchResult::permanent("file destination missing 'path_template'");
        }
        path_template = j["path_template"].get<std::string>();
        if (j.contains("preserve_hierarchy") && j["preserve_hierarchy"].is_boolean()) {
            preserve_hierarchy = j["preserve_hierarchy"].get<bool>();
        }
        if (j.contains("fsync") && j["fsync"].is_boolean()) {
            do_fsync = j["fsync"].get<bool>();
        }
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"file destination config invalid: "} + e.what());
    }

    // ---- Expand `${TagName}` references ----
    const auto resolved = expand_template(path_template, a.tags_json);
    if (resolved.empty()) {
        return DispatchResult::permanent(
            "path_template expanded to empty string (all referenced tags missing?)");
    }

    // ---- Walk source + copy each file ----
    const auto files = walk(src_root);
    if (files.empty()) {
        return DispatchResult::permanent(
            "no .dcm files under " + src_root.string());
    }
    const std::filesystem::path dst_root{resolved};

    int copied = 0;
    int failed = 0;
    std::uint64_t bytes_copied = 0;
    for (const auto& f : files) {
        std::filesystem::path dst = preserve_hierarchy
            ? (dst_root / f.rel)
            : (dst_root / f.source.filename());
        try {
            bytes_copied += copy_file(f.source, dst, do_fsync);
            ++copied;
        } catch (const std::exception& e) {
            ++failed;
            LOG_WARN("dispatch.file.copy_failed",
                "src",   f.source.string(),
                "dst",   dst.string(),
                "error", e.what());
        }
    }

    // Build the response_detail JSON. Hand-rolled to avoid pulling json
    // formatting into the hot path.
    std::ostringstream detail;
    detail << "{\"sent\":" << files.size()
           << ",\"succeeded\":" << copied
           << ",\"failed\":" << failed
           << ",\"bytes\":" << bytes_copied
           << ",\"dst_root\":\"" << dst_root.string() << "\""
           << "}";

    if (failed > 0) {
        return DispatchResult::transient(
            std::to_string(failed) + " of " + std::to_string(files.size()) +
            " files failed to copy", detail.str());
    }
    return DispatchResult::success(detail.str());
}

}  // namespace nlr
