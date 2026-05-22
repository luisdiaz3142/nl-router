#include "metrics.hpp"

namespace nlr {

namespace {
// Dispatch durations span seconds (file kind) to minutes (multi-instance
// DICOM C-STORE to a slow archive). Sized accordingly.
const std::vector<double> kDispatchBuckets =
    {0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600};
}

DispatcherMetrics DispatcherMetrics::register_all(nlr::metrics::Registry& r) {
    return DispatcherMetrics{
        .assignments_total = r.counter(
            "nl_dispatcher_assignments_total",
            "route_assignments outcomes, by destination kind and terminal result",
            {"kind", "result"}),                   // dispatched|transient_fail|permanent_fail|cred_fail
        .dispatch_duration_seconds = r.histogram(
            "nl_dispatcher_dispatch_duration_seconds",
            "Wall-clock duration of one assignment's handler.dispatch() call",
            kDispatchBuckets,
            {"kind"}),
        .destinations_active = r.gauge(
            "nl_dispatcher_destinations_active",
            "Destinations with a worker currently running"),
        .worker_busy = r.gauge(
            "nl_dispatcher_worker_busy",
            "1 if the per-destination worker has an in-flight dispatch right now, else 0",
            {"destination_id"}),
        .credential_fetch_failures_total = r.counter(
            "nl_dispatcher_credential_fetch_failures_total",
            "Decrypt or fetch failures during credential resolution. "
            "Counts cred_fail in assignments_total too — this counter is the "
            "credential-specific cut for alerting separately on KEK / credential "
            "store problems."),
    };
}

}  // namespace nlr
