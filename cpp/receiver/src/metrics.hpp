// nl-receiver/metrics.hpp
//
// Receiver-specific metric definitions. Holds references to the family
// objects registered with the process-wide registry; everywhere else in
// the receiver just touches the typed handles.
//
// Catalog (per design plan):
//   nl_receiver_associations_total{result,peer_type,calling_aet} counter
//   nl_receiver_associations_active                          gauge
//   nl_receiver_workers_busy                                 gauge
//   nl_receiver_workers_total                                gauge
//   nl_receiver_instances_received_total{calling_aet,modality} counter
//   nl_receiver_bytes_received_total{calling_aet}            counter
//   nl_receiver_association_duration_seconds                 histogram
//   nl_receiver_studies_closed_total{trigger}                counter
//   nl_receiver_landing_disk_used_bytes                      gauge
//   nl_receiver_landing_disk_free_bytes                      gauge
//   nl_receiver_db_insert_duration_seconds                   histogram
//   nl_receiver_db_insert_errors_total                       counter
//
// In v1 the workers_* gauges report 1 / associations_active because the
// listener is still single-threaded. They become meaningful when the
// bounded-pool listener lands (still M10, follow-up commit).

#pragma once

#include "nl_router/metrics/registry.hpp"

namespace nlr {

struct ReceiverMetrics {
    nlr::metrics::CounterFamily&   associations_total;       // {result, peer_type, calling_aet}
    nlr::metrics::GaugeFamily&     associations_active;
    nlr::metrics::GaugeFamily&     workers_busy;
    nlr::metrics::GaugeFamily&     workers_total;
    nlr::metrics::CounterFamily&   instances_received_total; // {calling_aet, modality}
    nlr::metrics::CounterFamily&   bytes_received_total;     // {calling_aet}
    nlr::metrics::HistogramFamily& association_duration_seconds;
    nlr::metrics::CounterFamily&   studies_closed_total;     // {trigger}
    nlr::metrics::GaugeFamily&     landing_disk_used_bytes;
    nlr::metrics::GaugeFamily&     landing_disk_free_bytes;
    nlr::metrics::GaugeFamily&     landing_disk_state;       // 0=normal,1=warn,2=reject
    nlr::metrics::HistogramFamily& db_insert_duration_seconds;
    nlr::metrics::CounterFamily&   db_insert_errors_total;

    // Register all families with the given registry. Idempotent — safe to
    // call multiple times. Returns a populated ReceiverMetrics with refs
    // bound to the registry.
    static ReceiverMetrics register_all(nlr::metrics::Registry& registry);
};

}  // namespace nlr
