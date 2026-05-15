// nl-receiver/config.hpp
//
// Bootstrap configuration for the receiver process.
//
// Mirrors the design plan's local config layering: a small TOML file at
// /etc/nl-router/config.toml provides defaults; environment variables override
// for ad-hoc / containerized deployments. We keep the loader env-only in v1
// to avoid pulling a TOML dependency into the receiver; the Python management
// CLI writes/edits the file and the receiver reads its own env.

#pragma once

#include <cstdint>
#include <string>

namespace nlr {

struct Config {
    // Identifies this node in work_queue.server_id and admin_audit.
    std::string server_id;

    // Postgres DSN for the central config + work_queue database. libpq-style.
    std::string database_url;

    // DICOM SCP listener.
    std::string host_aet      {"NL_ROUTER"};
    std::uint16_t listen_port {11112};

    // Landing zone root. Each instance writes to:
    //   <landing_zone>/YYYY-MM-DD/<study_uid>/<series_uid>/<sop_uid>.dcm
    std::string landing_zone  {"/var/lib/nl-router/incoming"};

    // Maximum PDU size advertised in association negotiation.
    std::uint32_t max_pdu_size {131072};

    // Per-association idle timeout in seconds. DCMTK closes the association
    // if no activity within this window. Prevents hung peers from holding
    // a slot. (v1 uses this as the only timeout; bounded thread pool with
    // configurable per-association limits land in M10.)
    std::uint32_t association_timeout_s {30};

    // Prometheus /metrics HTTP exposer. Setting port to 0 disables the
    // exposer entirely — useful for tests and CI runs. bind_addr controls
    // the listen interface (0.0.0.0 = all). The endpoint serves text-
    // exposition format on GET /metrics and a small "ok" on GET / and
    // GET /healthz for load-balancer health checks.
    std::uint16_t metrics_port      {9180};
    std::string   metrics_bind_addr {"0.0.0.0"};

    // How often the disk-stats poller updates the landing-zone gauges, in
    // seconds. Slice 2 of M10 layers two-threshold admission control on
    // top of these gauges; for now they're informational only.
    std::uint32_t disk_poll_interval_s {10};

    // Logging verbosity. "info" or "debug". Reloaded on SIGHUP in v2.
    std::string log_level {"info"};
};

// Load configuration from environment variables.
//
// Required:
//   NL_ROUTER_SERVER_ID     - identifier for this node
//   NL_ROUTER_DATABASE_URL  - libpq DSN (also accepts DATABASE_URL)
//
// Optional (defaults shown above):
//   NL_ROUTER_HOST_AET, NL_ROUTER_LISTEN_PORT, NL_ROUTER_LANDING_ZONE,
//   NL_ROUTER_MAX_PDU_SIZE, NL_ROUTER_ASSOCIATION_TIMEOUT_S,
//   NL_ROUTER_METRICS_PORT, NL_ROUTER_METRICS_BIND_ADDR,
//   NL_ROUTER_DISK_POLL_INTERVAL_S,
//   NL_ROUTER_LOG_LEVEL
//
// Throws std::runtime_error on missing required values or invalid integers.
Config load_config();

}  // namespace nlr
