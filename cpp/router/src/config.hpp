// nl-route/config.hpp
//
// Env-only bootstrap config for the routing daemon. Knobs that should live
// in central system_config (poll interval, batch size, claim lease seconds)
// are read from there in M4; v1 reads them from env too so the binary can
// run standalone for testing.

#pragma once

#include <cstdint>
#include <string>

namespace nlr {

struct Config {
    // Identifies this node. Router only picks up work_queue rows whose
    // server_id matches — keeps work pinned to the node that owns the
    // landing-zone files.
    std::string server_id;

    // libpq DSN to the central config + work_queue database.
    std::string database_url;

    // Root directory where processing modules write their intermediate
    // outputs. Each work_queue row gets a subdirectory; each ordinal in
    // the chain gets a sub-subdirectory. The Router pre-computes the
    // full path tree at job-creation time and writes it into
    // processing_jobs.input_path / output_path so workers don't have to
    // negotiate the ordering at runtime.
    std::string processing_root {"/var/lib/nl-router/processing"};

    // How long an idle poll cycle sleeps when work_queue is empty.
    // LISTEN/NOTIFY-based wake-up lands in a follow-up; for now we just poll.
    std::uint32_t poll_interval_ms {500};

    // Maximum work_queue rows claimed per polling transaction. Smaller values
    // give better fan-out across multiple router processes; larger values
    // reduce transaction overhead.
    std::uint32_t batch_size {32};

    // Worker claim lease in seconds. Sweeper reclaims rows whose
    // claim_expires_at < now() — currently the same router process on the
    // next iteration. (M10 adds a dedicated sweeper thread.)
    std::uint32_t lease_seconds {60};

    // How often to refresh the in-memory rule cache from the database.
    // 0 = refresh every poll iteration. NOTIFY-based invalidation lands later.
    std::uint32_t rule_cache_refresh_s {15};

    // Prometheus /metrics HTTP exposer. Port 0 disables — useful for
    // tests and when an operator deliberately doesn't want a metrics
    // port open. 9181 is the design-plan default for the router;
    // receiver=9180, dispatcher=9182, cleaner=9183, API=9184.
    std::uint16_t metrics_port      {9181};
    std::string   metrics_bind_addr {"0.0.0.0"};

    // Logging verbosity. "info" or "debug".
    std::string log_level {"info"};
};

// Load configuration from environment variables.
//
// Required: NL_ROUTER_SERVER_ID, NL_ROUTER_DATABASE_URL (or DATABASE_URL).
// Optional (defaults shown above): NL_ROUTER_POLL_INTERVAL_MS,
//   NL_ROUTER_BATCH_SIZE, NL_ROUTER_LEASE_SECONDS,
//   NL_ROUTER_RULE_CACHE_REFRESH_S, NL_ROUTER_METRICS_PORT,
//   NL_ROUTER_METRICS_BIND_ADDR, NL_ROUTER_LOG_LEVEL.
//
// Throws std::runtime_error on missing required values or invalid integers.
Config load_config();

}  // namespace nlr
