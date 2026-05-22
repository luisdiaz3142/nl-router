#include "metrics.hpp"

namespace nlr {

namespace {
// File pass: bounded by file_batch * per-row I/O. Sub-second per row on
// fast disks, can climb under a deep backlog. Bucketed to spot growing
// scan times before the loop falls behind the inbound rate.
const std::vector<double> kFilePassBuckets =
    {0.01, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60};

// Prune pass: single DELETE … LIMIT bounded by prune_batch. Usually
// short; long tails point at lock contention or autovacuum drag.
const std::vector<double> kPrunePassBuckets =
    {0.01, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30};
}

CleanerMetrics CleanerMetrics::register_all(nlr::metrics::Registry& r) {
    return CleanerMetrics{
        .scan_iterations_total = r.counter(
            "nl_cleaner_scan_iterations_total",
            "Cleaner scan loop iterations, by outcome",
            {"result"}),                            // success | empty | error
        .rows_cleaned_total = r.counter(
            "nl_cleaner_rows_cleaned_total",
            "work_queue rows transitioned to 'cleaned' (files removed)",
            {"status"}),                            // dispatched | dispatched_partial | failed
        .bytes_freed_total = r.counter(
            "nl_cleaner_bytes_freed_total",
            "Total bytes freed from local landing zone by file cleanup"),
        .files_deleted_total = r.counter(
            "nl_cleaner_files_deleted_total",
            "Total individual files deleted from local landing zone"),
        .rows_pruned_total = r.counter(
            "nl_cleaner_rows_pruned_total",
            "work_queue rows DELETEd by the leader-only row-prune pass"),
        .leader = r.gauge(
            "nl_cleaner_leader",
            "1 if this cleaner currently holds the prune advisory lock, else 0"),
        .file_pass_duration_seconds = r.histogram(
            "nl_cleaner_file_pass_duration_seconds",
            "Wall-clock duration of one file_cleanup_pass()",
            kFilePassBuckets),
        .prune_pass_duration_seconds = r.histogram(
            "nl_cleaner_prune_pass_duration_seconds",
            "Wall-clock duration of one row_prune_pass() (leader only)",
            kPrunePassBuckets),
    };
}

}  // namespace nlr
