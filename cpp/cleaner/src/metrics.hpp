// nl-clean/metrics.hpp
//
// Cleaner metric catalog. Two distinct workstreams get their own
// counters — file cleanup happens on every node; row pruning only on
// the advisory-lock leader. The `leader` gauge lets dashboards
// answer "which node is currently doing the prune work?" without
// poking at Postgres directly.
//
// Catalog:
//   nl_cleaner_scan_iterations_total{result}     counter
//     result ∈ {success, empty, error}
//   nl_cleaner_rows_cleaned_total{status}        counter
//     status carries the pre-cleanup terminal status (dispatched, partial, failed)
//   nl_cleaner_bytes_freed_total                 counter
//   nl_cleaner_files_deleted_total               counter
//   nl_cleaner_rows_pruned_total                 counter
//   nl_cleaner_leader                            gauge  (0 or 1)
//   nl_cleaner_file_pass_duration_seconds        histogram
//   nl_cleaner_prune_pass_duration_seconds       histogram

#pragma once

#include "nl_router/metrics/registry.hpp"

namespace nlr {

struct CleanerMetrics {
    nlr::metrics::CounterFamily&   scan_iterations_total;         // {result}
    nlr::metrics::CounterFamily&   rows_cleaned_total;            // {status}
    nlr::metrics::CounterFamily&   bytes_freed_total;
    nlr::metrics::CounterFamily&   files_deleted_total;
    nlr::metrics::CounterFamily&   rows_pruned_total;
    nlr::metrics::GaugeFamily&     leader;
    nlr::metrics::HistogramFamily& file_pass_duration_seconds;
    nlr::metrics::HistogramFamily& prune_pass_duration_seconds;

    static CleanerMetrics register_all(nlr::metrics::Registry& registry);
};

}  // namespace nlr
