// nl-clean/config.hpp
//
// Env-only bootstrap config for the cleaner daemon. Runtime knobs that
// should live in system_config (TTLs, scan interval) are loaded from there
// at startup with sensible env-overrides for ad-hoc testing.

#pragma once

#include <cstdint>
#include <string>

namespace nlr {

struct Config {
    // This node's identifier. Cleaner only deletes files on its own node
    // (matches the local-disk-per-node storage model).
    std::string server_id;

    // libpq DSN.
    std::string database_url;

    // Local landing-zone root. Used to compute parent-dir cleanup so old
    // date directories don't pile up after their child studies are gone.
    std::string landing_zone {"/var/lib/nl-router/incoming"};

    // How often the cleaner scans work_queue. Pulled from system_config at
    // startup; env override available for tests / dev.
    std::uint32_t scan_interval_s {60};

    // Max work_queue rows cleaned per cycle on this node. Bigger values
    // free disk faster after a backlog; smaller values keep individual
    // scan iterations bounded.
    std::uint32_t file_batch {100};

    // Max work_queue rows pruned per cycle on the leader. DELETE … LIMIT
    // through a subquery; bounded so the leader iteration doesn't grow
    // unboundedly with the cleaned-row backlog.
    std::uint32_t prune_batch {500};

    // Logging verbosity.
    std::string log_level {"info"};
};

// Required: NL_ROUTER_SERVER_ID, NL_ROUTER_DATABASE_URL (or DATABASE_URL).
// Optional: NL_ROUTER_LANDING_ZONE, NL_ROUTER_SCAN_INTERVAL_S,
//           NL_ROUTER_FILE_BATCH, NL_ROUTER_PRUNE_BATCH, NL_ROUTER_LOG_LEVEL.
Config load_config();

}  // namespace nlr
