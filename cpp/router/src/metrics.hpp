// nl-route/metrics.hpp
//
// Router-specific metric definitions. Same pattern as the receiver:
// register all families once at startup, hold typed references in the
// struct, and the hot path touches them directly.
//
// Catalog:
//   nl_router_poll_iterations_total{result}        counter  (success/empty/error)
//   nl_router_claim_duration_seconds               histogram
//   nl_router_rows_routed_total                    counter
//   nl_router_rule_matches_total{rule_name}        counter
//   nl_router_assignments_inserted_total           counter
//   nl_router_processing_jobs_inserted_total       counter
//   nl_router_predicate_eval_failures_total        counter
//   nl_router_insert_failures_total                counter
//   nl_router_rule_cache_size                      gauge
//   nl_router_rule_cache_refresh_total{result}     counter  (success/error)
//   nl_router_rule_cache_parse_errors_total        counter

#pragma once

#include "nl_router/metrics/registry.hpp"

namespace nlr {

struct RouterMetrics {
    nlr::metrics::CounterFamily&   poll_iterations_total;        // {result}
    nlr::metrics::HistogramFamily& claim_duration_seconds;
    nlr::metrics::CounterFamily&   rows_routed_total;
    nlr::metrics::CounterFamily&   rule_matches_total;           // {rule_name}
    nlr::metrics::CounterFamily&   assignments_inserted_total;
    nlr::metrics::CounterFamily&   processing_jobs_inserted_total;
    nlr::metrics::CounterFamily&   predicate_eval_failures_total;
    nlr::metrics::CounterFamily&   insert_failures_total;
    nlr::metrics::GaugeFamily&     rule_cache_size;
    nlr::metrics::CounterFamily&   rule_cache_refresh_total;     // {result}
    nlr::metrics::CounterFamily&   rule_cache_parse_errors_total;

    static RouterMetrics register_all(nlr::metrics::Registry& registry);
};

}  // namespace nlr
