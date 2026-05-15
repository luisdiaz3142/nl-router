#include "server.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <thread>

#include "disk.hpp"
#include "logging.hpp"

namespace nlr {

Server::Server(const Config& cfg) : cfg_(cfg) {
    db_ = std::make_unique<Db>(cfg_.database_url);
    retention_ = db_->load_retention();
    LOG_INFO("cleaner.startup",
        "server_id",       cfg_.server_id,
        "landing_zone",    cfg_.landing_zone,
        "scan_interval_s", std::to_string(cfg_.scan_interval_s),
        "file_batch",      std::to_string(cfg_.file_batch),
        "prune_batch",     std::to_string(cfg_.prune_batch));
}

int Server::run() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        std::size_t cleaned = 0;
        try {
            cleaned = file_cleanup_pass_();
        } catch (const std::exception& e) {
            LOG_ERROR("cleaner.file_pass_failed", "error", e.what());
        }

        int pruned = 0;
        try {
            pruned = row_prune_pass_();
        } catch (const std::exception& e) {
            LOG_ERROR("cleaner.prune_pass_failed", "error", e.what());
        }

        if (cleaned == 0 && pruned == 0) {
            // Nothing happened this cycle. Sleep the full interval.
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.scan_interval_s));
        } else {
            // Something happened — re-scan immediately in case there's more
            // (e.g. operator just bulk-released holds). Brief sleep to keep
            // the loop from busy-spinning under steady load.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    LOG_INFO("cleaner.shutdown");
    return 0;
}

void Server::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

std::size_t Server::file_cleanup_pass_() {
    const auto rows = db_->list_eligible_rows(cfg_.server_id, retention_, cfg_.file_batch);
    if (rows.empty()) return 0;

    LOG_DEBUG("cleaner.batch", "eligible", std::to_string(rows.size()));

    const std::filesystem::path landing{cfg_.landing_zone};
    std::size_t succeeded = 0;
    std::uint64_t bytes_total = 0;

    for (const auto& row : rows) {
        if (stop_requested_.load(std::memory_order_relaxed)) break;

        CleanupResult result;
        try {
            result = delete_study_directory(landing, std::filesystem::path{row.file_root_path});
        } catch (const std::exception& e) {
            // I/O failure: log and skip — next cycle will retry. We never
            // mark this row 'cleaned' since the files are still there.
            LOG_ERROR("cleaner.file_delete_failed",
                "work_queue_id", std::to_string(row.id),
                "path",          row.file_root_path,
                "error",         e.what());
            continue;
        }

        try {
            db_->mark_cleaned(row.id);
        } catch (const std::exception& e) {
            LOG_ERROR("cleaner.mark_cleaned_failed",
                "work_queue_id", std::to_string(row.id),
                "error",         e.what());
            continue;
        }

        ++succeeded;
        bytes_total += result.bytes_freed;
        LOG_INFO("cleaner.cleaned",
            "work_queue_id", std::to_string(row.id),
            "status",        row.status,
            "path",          row.file_root_path,
            "files",         std::to_string(result.files_deleted),
            "bytes",         std::to_string(result.bytes_freed),
            "path_missing",  result.path_missing ? "true" : "false");
    }

    if (succeeded > 0) {
        LOG_INFO("cleaner.cycle_summary",
            "cleaned",     std::to_string(succeeded),
            "bytes_freed", std::to_string(bytes_total));
    }
    return succeeded;
}

int Server::row_prune_pass_() {
    if (!db_->try_acquire_prune_lock()) {
        // Someone else is the leader this cycle. Not an error.
        return 0;
    }

    int total = 0;
    try {
        // v1 simplification: use the longest prune TTL globally because we
        // don't preserve the original terminal status post-cleanup. Adding
        // a pre_clean_status column to enable differentiated pruning is a
        // planned follow-up.
        const int longest_d = retention_.prune_failed_d;
        total = db_->prune_cleaned_rows(longest_d, cfg_.prune_batch);
        if (total > 0) {
            LOG_INFO("cleaner.pruned",
                "rows", std::to_string(total),
                "ttl_days", std::to_string(longest_d));
        }
    } catch (...) {
        db_->release_prune_lock();
        throw;
    }
    db_->release_prune_lock();
    return total;
}

}  // namespace nlr
