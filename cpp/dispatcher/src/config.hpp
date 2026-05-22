// nl-dispatch/config.hpp
//
// Env-only bootstrap config for the dispatcher daemon. Like the router, the
// runtime knobs (poll interval, destination refresh) will move to
// system_config in M4; v1 reads them from env too.

#pragma once

#include <cstdint>
#include <string>

namespace nlr {

struct Config {
    // Identifies this node. Dispatcher only picks up route_assignments
    // whose server_id matches — keeps work local to the node that owns the
    // landing-zone files.
    std::string server_id;

    // libpq DSN.
    std::string database_url;

    // Idle poll cycle when a destination worker has nothing to do.
    std::uint32_t poll_interval_ms {500};

    // Maximum route_assignments rows claimed per destination per cycle.
    // Bigger values reduce transaction overhead; smaller values give better
    // recovery if a worker crashes mid-batch.
    std::uint32_t batch_size {16};

    // Claim lease in seconds.
    std::uint32_t lease_seconds {300};

    // How often to refresh the destinations list (new destinations show up;
    // disabled destinations have their workers stopped).
    std::uint32_t destination_refresh_s {30};

    // Prometheus /metrics HTTP exposer. Port 0 disables.
    std::uint16_t metrics_port      {9182};
    std::string   metrics_bind_addr {"0.0.0.0"};

    // Logging verbosity.
    std::string log_level {"info"};
};

// Required: NL_ROUTER_SERVER_ID, NL_ROUTER_DATABASE_URL (or DATABASE_URL).
// Optional: NL_ROUTER_POLL_INTERVAL_MS, NL_ROUTER_BATCH_SIZE,
//           NL_ROUTER_LEASE_SECONDS, NL_ROUTER_DESTINATION_REFRESH_S,
//           NL_ROUTER_LOG_LEVEL.
Config load_config();

}  // namespace nlr
