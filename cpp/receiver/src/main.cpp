// nl-receiver — DICOM SCP for nl-router.
//
// M1 baseline: single-threaded SCP, association-end close trigger, writes
// instances to local disk and inserts one work_queue row per study at
// association end.
//
// Run with env-only config:
//   NL_ROUTER_SERVER_ID=node-a \
//   NL_ROUTER_DATABASE_URL=postgres://... \
//   NL_ROUTER_LANDING_ZONE=/var/lib/nl-router/incoming \
//   nl-receiver

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmnet/dimse.h>

#include "config.hpp"
#include "db.hpp"
#include "logging.hpp"
#include "metrics.hpp"
#include "server.hpp"

#include "nl_router/metrics/exposer.hpp"
#include "nl_router/metrics/registry.hpp"

namespace {

// Global pointer for the signal handler. We avoid any heavyweight handling
// inside the handler — just flip the stop flag on the server.
std::atomic<nlr::Server*> g_server{nullptr};

// Disk-stats poller flag. Set true at shutdown so the poll loop exits.
std::atomic<bool> g_disk_poll_stop{false};

void on_signal(int sig) {
    auto* s = g_server.load();
    if (s) s->stop();
    g_disk_poll_stop.store(true);
    // Re-raise default to ensure the next signal terminates immediately if
    // graceful shutdown stalls.
    std::signal(sig, SIG_DFL);
}

// Poll filesystem usage on the landing zone at a configurable cadence and
// update the gauges. Uses std::filesystem::space — POSIX statfs under the
// hood on Linux/macOS. Slice 2 of M10 layers a back-pressure threshold on
// top of these gauges; here they're informational only.
void disk_poll_loop(const std::string& landing_zone,
                     std::uint32_t interval_s,
                     nlr::ReceiverMetrics& metrics) {
    using clock = std::chrono::steady_clock;
    while (!g_disk_poll_stop.load(std::memory_order_relaxed)) {
        std::error_code ec;
        const auto space = std::filesystem::space(landing_zone, ec);
        if (!ec) {
            metrics.landing_disk_free_bytes.self()
                .set(static_cast<std::int64_t>(space.available));
            metrics.landing_disk_used_bytes.self()
                .set(static_cast<std::int64_t>(space.capacity - space.available));
        }
        // Sleep with periodic wake to honor the stop flag promptly.
        const auto until = clock::now() + std::chrono::seconds(interval_s);
        while (clock::now() < until
               && !g_disk_poll_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

void install_signal_handlers() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    // Ignore SIGPIPE — TCP peers can disconnect mid-write and we want libpq
    // and DCMTK to surface the error via return codes, not via process death.
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    try {
        const auto cfg = nlr::load_config();
        nlr::log::set_level(nlr::log::parse_level(cfg.log_level));

        LOG_INFO("startup",
            "server_id",        cfg.server_id,
            "host_aet",         cfg.host_aet,
            "listen_port",      std::to_string(cfg.listen_port),
            "landing_zone",     cfg.landing_zone,
            "metrics_port",     std::to_string(cfg.metrics_port),
            "metrics_bind",     cfg.metrics_bind_addr,
            "disk_poll_int_s",  std::to_string(cfg.disk_poll_interval_s),
            "log_level",        cfg.log_level);

        // Make sure the landing zone exists. The cleaner handles eventual
        // deletion; we just need the root present so first-instance writes
        // don't fail.
        std::error_code ec;
        std::filesystem::create_directories(cfg.landing_zone, ec);
        if (ec) {
            LOG_ERROR("startup.landing_zone_failed",
                "path",  cfg.landing_zone,
                "error", ec.message());
            return 2;
        }

        // DCMTK requires its data dictionary to be loaded. On Homebrew installs
        // the dictionary lives at $(brew --prefix dcmtk)/share/dcmtk/dicom.dic;
        // DCMTK looks it up via the DCMDICTPATH env var. If neither the env
        // nor the default path works, log a fatal error early.
        if (!dcmDataDict.isDictionaryLoaded()) {
            LOG_ERROR("startup.dcm_dict_missing",
                "hint", "set DCMDICTPATH to dicom.dic (e.g. $(brew --prefix dcmtk)/share/dcmtk/dicom.dic)");
            return 2;
        }

        // ---- Metrics registry + /metrics exposer ----
        auto& registry = nlr::metrics::Registry::global();
        nlr::ReceiverMetrics metrics = nlr::ReceiverMetrics::register_all(registry);
        nlr::metrics::Exposer exposer(registry, cfg.metrics_port, cfg.metrics_bind_addr);
        exposer.start();

        nlr::Db db(cfg.database_url);
        db.set_metrics(&metrics);

        nlr::Server server(cfg, db, metrics);

        // Kick off the disk-stats poller. Detached because it has no
        // dependencies beyond the gauges and the stop flag.
        std::thread disk_thread(disk_poll_loop, cfg.landing_zone,
                                 cfg.disk_poll_interval_s, std::ref(metrics));

        g_server.store(&server);
        install_signal_handlers();

        const int rc = server.run();
        g_server.store(nullptr);

        g_disk_poll_stop.store(true);
        if (disk_thread.joinable()) disk_thread.join();
        exposer.stop();

        LOG_INFO("shutdown.complete", "exit_code", std::to_string(rc));
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "{\"ts\":\"\",\"lvl\":\"error\",\"msg\":\"fatal\",\"error\":\""
                  << e.what() << "\"}\n";
        return 1;
    } catch (...) {
        std::cerr << "{\"ts\":\"\",\"lvl\":\"error\",\"msg\":\"fatal\",\"error\":\"unknown\"}\n";
        return 1;
    }
}
