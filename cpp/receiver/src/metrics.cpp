#include "metrics.hpp"

namespace nlr {

// Bucket choices — picked to give actionable resolution where measurements
// actually cluster, not arbitrary log-spaced defaults.
//
// association_duration_seconds: most associations finish in < 5s for small
// studies; large multi-frame studies can run for a minute. The 0.1s and
// 0.5s buckets catch C-ECHO and tiny C-STORE; 300s is the cliff above
// which something is wrong.
namespace {
const std::vector<double> kAssocBuckets =
    {0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300};

// db_insert_duration_seconds: pure Postgres INSERT latency. Local DB is
// usually < 5ms; > 100ms is a problem worth seeing in the dashboard.
const std::vector<double> kDbBuckets =
    {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 5};
}

ReceiverMetrics ReceiverMetrics::register_all(nlr::metrics::Registry& r) {
    return ReceiverMetrics{
        .associations_total = r.counter(
            "nl_receiver_associations_total",
            "Total inbound DICOM associations, by acceptance result, peer "
            "transport, and calling AET. The calling_aet label is the "
            "primary axis operators use to answer 'which scanner is being "
            "rejected / connecting most.' Cardinality is bounded by the "
            "real-world AE-Title set at the site (typically < 50).",
            {"result", "peer_type", "calling_aet"}),
        .associations_active = r.gauge(
            "nl_receiver_associations_active",
            "Number of currently active DICOM associations"),
        .workers_busy = r.gauge(
            "nl_receiver_workers_busy",
            "Number of association-handling worker threads currently busy"),
        .workers_total = r.gauge(
            "nl_receiver_workers_total",
            "Total association-handling worker threads in the pool"),
        .instances_received_total = r.counter(
            "nl_receiver_instances_received_total",
            "C-STORE instances received and written to disk",
            {"calling_aet", "modality"}),
        .bytes_received_total = r.counter(
            "nl_receiver_bytes_received_total",
            "Total bytes written to the landing zone",
            {"calling_aet"}),
        .association_duration_seconds = r.histogram(
            "nl_receiver_association_duration_seconds",
            "Wall-clock duration of completed associations",
            kAssocBuckets),
        .studies_closed_total = r.counter(
            "nl_receiver_studies_closed_total",
            "Studies flushed to work_queue, labeled by close trigger",
            {"trigger"}),
        .landing_disk_used_bytes = r.gauge(
            "nl_receiver_landing_disk_used_bytes",
            "Bytes currently used on the filesystem hosting the landing zone"),
        .landing_disk_free_bytes = r.gauge(
            "nl_receiver_landing_disk_free_bytes",
            "Bytes currently available on the filesystem hosting the landing zone"),
        .landing_disk_state = r.gauge(
            "nl_receiver_landing_disk_state",
            "Landing-zone disk-pressure state: 0=normal, 1=warning, 2=reject"),
        .db_insert_duration_seconds = r.histogram(
            "nl_receiver_db_insert_duration_seconds",
            "Latency of work_queue INSERT statements",
            kDbBuckets),
        .db_insert_errors_total = r.counter(
            "nl_receiver_db_insert_errors_total",
            "work_queue INSERT failures (any cause)"),
    };
}

}  // namespace nlr
