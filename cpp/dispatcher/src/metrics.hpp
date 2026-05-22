// nl-dispatch/metrics.hpp
//
// Dispatcher metric catalog. Per-destination + per-kind labels let
// dashboards graph "which destination is slow / failing" and "is the
// DICOMweb path healthier than the S3 path right now."
//
// Catalog:
//   nl_dispatcher_assignments_total{kind, result}  counter
//     result ∈ {dispatched, transient_fail, permanent_fail, cred_fail}
//   nl_dispatcher_dispatch_duration_seconds{kind}  histogram
//   nl_dispatcher_destinations_active              gauge
//   nl_dispatcher_worker_busy{destination_id}      gauge
//   nl_dispatcher_credential_fetch_failures_total  counter

#pragma once

#include "nl_router/metrics/registry.hpp"

namespace nlr {

struct DispatcherMetrics {
    nlr::metrics::CounterFamily&   assignments_total;            // {kind, result}
    nlr::metrics::HistogramFamily& dispatch_duration_seconds;    // {kind}
    nlr::metrics::GaugeFamily&     destinations_active;
    nlr::metrics::GaugeFamily&     worker_busy;                  // {destination_id}
    nlr::metrics::CounterFamily&   credential_fetch_failures_total;

    static DispatcherMetrics register_all(nlr::metrics::Registry& registry);
};

}  // namespace nlr
