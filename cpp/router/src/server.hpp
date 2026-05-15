// nl-route/server.hpp
//
// The router's polling loop. Glues together RuleCache, the DSL evaluator,
// and the DB client.

#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "config.hpp"
#include "db.hpp"
#include "rule_cache.hpp"

namespace nlr {

class Server {
public:
    Server(const Config& cfg, Db& db);

    // Run the poll loop until stop() is called. Returns 0 on graceful
    // shutdown, non-zero on fatal error.
    int run();

    // Flip the stop flag. Safe to call from a signal handler.
    void stop() noexcept;

private:
    // Process one batch: claim rows, evaluate rules against each, write
    // assignments, mark routed/failed. Returns number of rows processed.
    std::size_t evaluate_batch_();

    // For one row: build context, walk enabled rules, insert assignments
    // for matches, and mark the row routed or failed.
    void evaluate_row_(const ClaimedRow& row);

    const Config& cfg_;
    Db&           db_;
    RuleCache     rule_cache_;
    std::atomic<bool> stop_requested_ {false};

    std::chrono::steady_clock::time_point last_rule_refresh_;
    std::string worker_id_;
};

}  // namespace nlr
