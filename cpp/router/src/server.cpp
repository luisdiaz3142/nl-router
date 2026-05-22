#include "server.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <unistd.h>

#include "context_builder.hpp"
#include "logging.hpp"
#include "nl_router/dsl/dsl.hpp"

namespace nlr {

namespace {

// Unique identifier for this process, used as work_queue.claimed_by. We
// include the hostname's tail and the pid so logs can correlate a claim
// to its router instance.
std::string make_worker_id() {
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        std::snprintf(host, sizeof(host), "unknown");
    }
    return std::string{"nl-route@"} + host + ":" + std::to_string(getpid());
}

}  // namespace

Server::Server(const Config& cfg, Db& db, const RouterMetrics& metrics)
    : cfg_(cfg), db_(db), metrics_(metrics), rule_cache_(db),
      last_rule_refresh_(std::chrono::steady_clock::now()),
      worker_id_(make_worker_id())
{
    LOG_INFO("router.startup",
        "server_id",       cfg_.server_id,
        "worker_id",       worker_id_,
        "poll_interval_ms", std::to_string(cfg_.poll_interval_ms),
        "batch_size",      std::to_string(cfg_.batch_size),
        "lease_seconds",   std::to_string(cfg_.lease_seconds));
}

int Server::run() {
    // Eager initial cache load so the first iteration has rules to evaluate.
    try {
        rule_cache_.refresh();
        metrics_.rule_cache_refresh_total.labels({"success"}).inc();
        metrics_.rule_cache_size.self().set(
            static_cast<std::int64_t>(rule_cache_.size()));
    } catch (const std::exception& e) {
        metrics_.rule_cache_refresh_total.labels({"error"}).inc();
        LOG_ERROR("router.initial_rule_load_failed", "error", e.what());
        return 2;
    }

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Refresh the rule cache periodically.
        const auto now = std::chrono::steady_clock::now();
        const auto since_refresh = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_rule_refresh_).count();
        if (cfg_.rule_cache_refresh_s > 0 &&
            since_refresh >= static_cast<long>(cfg_.rule_cache_refresh_s))
        {
            try {
                rule_cache_.refresh();
                metrics_.rule_cache_refresh_total.labels({"success"}).inc();
                metrics_.rule_cache_size.self().set(
                    static_cast<std::int64_t>(rule_cache_.size()));
                last_rule_refresh_ = now;
            } catch (const std::exception& e) {
                metrics_.rule_cache_refresh_total.labels({"error"}).inc();
                LOG_WARN("router.rule_refresh_failed", "error", e.what());
                // Keep running with the stale cache rather than stop.
                last_rule_refresh_ = now;
            }
        }

        std::size_t processed = 0;
        try {
            processed = evaluate_batch_();
            metrics_.poll_iterations_total.labels({
                processed > 0 ? "success" : "empty"
            }).inc();
        } catch (const std::exception& e) {
            metrics_.poll_iterations_total.labels({"error"}).inc();
            LOG_ERROR("router.batch_failed", "error", e.what());
            // Sleep to avoid a tight error loop if the DB is misbehaving.
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_interval_ms));
            continue;
        }

        if (processed == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_interval_ms));
        }
    }

    LOG_INFO("router.shutdown", "worker_id", worker_id_);
    return 0;
}

void Server::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

std::size_t Server::evaluate_batch_() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto rows = db_.claim_received_rows(cfg_.server_id, worker_id_,
                                         cfg_.lease_seconds, cfg_.batch_size);
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(
        clock::now() - t0).count();
    metrics_.claim_duration_seconds.self().observe(dt);
    if (rows.empty()) return 0;

    LOG_DEBUG("router.batch_claimed",
        "count", std::to_string(rows.size()));

    for (const auto& row : rows) {
        evaluate_row_(row);
    }
    return rows.size();
}

void Server::evaluate_row_(const ClaimedRow& row) {
    // Build the eval context from the row's data.
    ::nl_router::dsl::Context ctx;
    try {
        RowContextInput input{
            row.calling_aet,
            row.called_aet,
            row.peer_ip,
            row.tags_json,
        };
        ctx = build_context(input);
    } catch (const std::exception& e) {
        LOG_ERROR("router.context_build_failed",
            "work_queue_id", std::to_string(row.id),
            "error",         e.what());
        db_.mark_failed(row.id, std::string{"context build: "} + e.what());
        return;
    }

    // Evaluate study-scope rules. (M1 receiver always emits close_trigger
    // 'assoc_end' and groups at study level, so we only consult study-scope
    // rules here. Series-scope handling lands when the receiver supports
    // series-idle close.)
    const auto rules = rule_cache_.for_scope("study");
    int matches            = 0;
    int assignments_total  = 0;
    bool eval_error        = false;
    std::string error_msg;

    for (const auto& cr : *rules) {
        bool matched = false;
        try {
            matched = ::nl_router::dsl::evaluate(*cr.ast, ctx);
        } catch (const std::exception& e) {
            // A bad predicate (e.g. type error) shouldn't fail the whole
            // row — log it and skip the rule. The work_queue row still
            // advances to 'routed' so we don't get stuck in a retry loop
            // on an authoring mistake.
            metrics_.predicate_eval_failures_total.self().inc();
            LOG_WARN("router.predicate_eval_failed",
                "work_queue_id", std::to_string(row.id),
                "rule_id",       std::to_string(cr.id),
                "rule_name",     cr.name,
                "error",         e.what());
            continue;
        }
        if (!matched) continue;
        ++matches;
        metrics_.rule_matches_total.labels({cr.name}).inc();

        try {
            const int n = db_.insert_route_assignments(row.id, cr.id, cfg_.server_id);
            assignments_total += n;
            metrics_.assignments_inserted_total.self().inc(n);

            // Per-rule processing chain. Each entry in
            // rule_processing_chain becomes one processing_jobs row,
            // with input/output paths pre-computed so the modules don't
            // have to negotiate ordering at runtime.
            const int p = db_.insert_processing_jobs(
                row.id, cr.id, cfg_.server_id,
                row.file_root_path, cfg_.processing_root);
            metrics_.processing_jobs_inserted_total.self().inc(p);

            LOG_INFO("router.rule_matched",
                "work_queue_id",  std::to_string(row.id),
                "rule_id",        std::to_string(cr.id),
                "rule_name",      cr.name,
                "assignments",    std::to_string(n),
                "processing_jobs", std::to_string(p));
        } catch (const std::exception& e) {
            // DB insert failure here is recoverable for other rules, but we
            // should mark the row failed so the operator notices.
            metrics_.insert_failures_total.self().inc();
            eval_error = true;
            error_msg  = std::string{"insert assignments/jobs: "} + e.what();
            LOG_ERROR("router.insert_failed",
                "work_queue_id", std::to_string(row.id),
                "rule_id",       std::to_string(cr.id),
                "error",         e.what());
            break;  // stop processing rules for this row
        }
    }

    try {
        if (eval_error) {
            db_.mark_failed(row.id, error_msg);
        } else {
            // finalize_routing inspects processing_jobs and picks
            // 'processing' (Processor takes it next) or 'routed'
            // (Dispatcher can act immediately).
            db_.finalize_routing(row.id);
            metrics_.rows_routed_total.self().inc();
        }
    } catch (const std::exception& e) {
        // Worst case: claim columns stay set until the lease expires and a
        // sweeper (M10) reclaims the row. Log loudly.
        LOG_ERROR("router.status_update_failed",
            "work_queue_id", std::to_string(row.id),
            "error",         e.what());
        return;
    }

    LOG_INFO("router.row_routed",
        "work_queue_id",     std::to_string(row.id),
        "matches",           std::to_string(matches),
        "assignments_total", std::to_string(assignments_total));
}

}  // namespace nlr
