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

Server::Server(const Config& cfg, const CleanerMetrics& metrics)
    : cfg_(cfg), metrics_(metrics) {
    db_ = std::make_unique<Db>(cfg_.database_url);
    retention_ = db_->load_retention();
    // Start out as a non-leader; row_prune_pass_() will flip this when it
    // acquires the advisory lock, and clear it back to 0 on release.
    metrics_.leader.self().set(0);
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
        bool errored = false;
        try {
            cleaned = file_cleanup_pass_();
        } catch (const std::exception& e) {
            errored = true;
            LOG_ERROR("cleaner.file_pass_failed", "error", e.what());
        }

        int pruned = 0;
        try {
            pruned = row_prune_pass_();
        } catch (const std::exception& e) {
            errored = true;
            LOG_ERROR("cleaner.prune_pass_failed", "error", e.what());
        }

        // Classify the iteration for the scan_iterations_total counter.
        // 'empty' iterations are normal and dominate at steady state; the
        // error rate is the alertable signal.
        const char* result = errored ? "error"
                                     : (cleaned == 0 && pruned == 0) ? "empty" : "success";
        metrics_.scan_iterations_total.labels({result}).inc();

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
    const auto pass_start = std::chrono::steady_clock::now();
    const auto rows = db_->list_eligible_rows(cfg_.server_id, retention_, cfg_.file_batch);
    if (rows.empty()) {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - pass_start).count();
        metrics_.file_pass_duration_seconds.self().observe(elapsed);
        return 0;
    }

    LOG_DEBUG("cleaner.batch", "eligible", std::to_string(rows.size()));

    const std::filesystem::path landing{cfg_.landing_zone};
    std::size_t succeeded = 0;
    std::uint64_t bytes_total = 0;
    std::uint64_t files_total = 0;

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
        files_total += result.files_deleted;
        // Per-row counters keep the by-status dashboard panels honest even
        // when one cycle is dominated by a single status (e.g. partial
        // backlogs left behind by a flapping destination).
        metrics_.rows_cleaned_total.labels({row.status}).inc();
        metrics_.bytes_freed_total.self().inc(static_cast<std::int64_t>(result.bytes_freed));
        metrics_.files_deleted_total.self().inc(static_cast<std::int64_t>(result.files_deleted));
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
            "files_freed", std::to_string(files_total),
            "bytes_freed", std::to_string(bytes_total));
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - pass_start).count();
    metrics_.file_pass_duration_seconds.self().observe(elapsed);
    return succeeded;
}

int Server::row_prune_pass_() {
    if (!db_->try_acquire_prune_lock()) {
        // Someone else is the leader this cycle. Not an error.
        metrics_.leader.self().set(0);
        return 0;
    }

    metrics_.leader.self().set(1);
    const auto pass_start = std::chrono::steady_clock::now();

    int total = 0;
    try {
        // v1 simplification: use the longest prune TTL globally because we
        // don't preserve the original terminal status post-cleanup. Adding
        // a pre_clean_status column to enable differentiated pruning is a
        // planned follow-up.
        const int longest_d = retention_.prune_failed_d;
        total = db_->prune_cleaned_rows(longest_d, cfg_.prune_batch);
        if (total > 0) {
            metrics_.rows_pruned_total.self().inc(total);
            LOG_INFO("cleaner.pruned",
                "rows", std::to_string(total),
                "ttl_days", std::to_string(longest_d));
        }
    } catch (...) {
        db_->release_prune_lock();
        metrics_.leader.self().set(0);
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - pass_start).count();
        metrics_.prune_pass_duration_seconds.self().observe(elapsed);
        throw;
    }
    db_->release_prune_lock();
    // Clear leader gauge between cycles — the advisory lock is released so
    // any node could become leader on the next iteration. Holding the
    // gauge at 1 across cycles would falsely imply ongoing leadership.
    metrics_.leader.self().set(0);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - pass_start).count();
    metrics_.prune_pass_duration_seconds.self().observe(elapsed);
    return total;
}

}  // namespace nlr
