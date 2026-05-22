#include "metrics.hpp"

namespace nlr {

namespace {
// Claim duration is dominated by Postgres round-trip; sub-second buckets
// give enough resolution to spot a degraded DB, longer ones catch lock
// contention under load.
const std::vector<double> kClaimBuckets =
    {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 5};
}

RouterMetrics RouterMetrics::register_all(nlr::metrics::Registry& r) {
    return RouterMetrics{
        .poll_iterations_total = r.counter(
            "nl_router_poll_iterations_total",
            "Router polling-loop iterations, labeled by outcome",
            {"result"}),                        // success | empty | error
        .claim_duration_seconds = r.histogram(
            "nl_router_claim_duration_seconds",
            "Time spent in claim_received_rows() per iteration",
            kClaimBuckets),
        .rows_routed_total = r.counter(
            "nl_router_rows_routed_total",
            "work_queue rows successfully routed (finalize_routing called)"),
        .rule_matches_total = r.counter(
            "nl_router_rule_matches_total",
            "Times a rule's predicate evaluated true against a row",
            {"rule_name"}),
        .assignments_inserted_total = r.counter(
            "nl_router_assignments_inserted_total",
            "route_assignments rows inserted"),
        .processing_jobs_inserted_total = r.counter(
            "nl_router_processing_jobs_inserted_total",
            "processing_jobs rows inserted"),
        .predicate_eval_failures_total = r.counter(
            "nl_router_predicate_eval_failures_total",
            "Predicate evaluations that threw (typically type errors at runtime)"),
        .insert_failures_total = r.counter(
            "nl_router_insert_failures_total",
            "INSERT failures during route-assignment / processing-job creation"),
        .rule_cache_size = r.gauge(
            "nl_router_rule_cache_size",
            "Currently cached, parse-succeeded enabled rules"),
        .rule_cache_refresh_total = r.counter(
            "nl_router_rule_cache_refresh_total",
            "Rule-cache refresh attempts",
            {"result"}),                        // success | error
        .rule_cache_parse_errors_total = r.counter(
            "nl_router_rule_cache_parse_errors_total",
            "Predicates that failed to parse during cache refresh — these are "
            "silently skipped, so this counter is the way operators learn about them"),
    };
}

}  // namespace nlr
