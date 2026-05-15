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
#include "disk_guard.hpp"
#include "logging.hpp"
#include "metrics.hpp"
#include "server.hpp"
#include "tls_layer.hpp"

#include "nl_router/metrics/exposer.hpp"
#include "nl_router/metrics/registry.hpp"

namespace {

// Global pointer for the signal handler. We avoid any heavyweight handling
// inside the handler — just flip the stop flag on the server.
std::atomic<nlr::Server*>     g_server{nullptr};
std::atomic<nlr::DiskGuard*>  g_disk_guard{nullptr};

void on_signal(int sig) {
    if (auto* s = g_server.load())     s->stop();
    if (auto* d = g_disk_guard.load()) d->stop();
    // Re-raise default to ensure the next signal terminates immediately if
    // graceful shutdown stalls.
    std::signal(sig, SIG_DFL);
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
            "max_associations", std::to_string(cfg.max_associations),
            "landing_zone",     cfg.landing_zone,
            "metrics_port",     std::to_string(cfg.metrics_port),
            "metrics_bind",     cfg.metrics_bind_addr,
            "disk_poll_int_s",  std::to_string(cfg.disk_poll_interval_s),
            "disk_warn_pct",    std::to_string(cfg.disk_warn_pct),
            "disk_reject_pct",  std::to_string(cfg.disk_reject_pct),
            "tls_enabled",      cfg.tls_enabled ? std::string{"true"} : std::string{"false"},
            "tls_listen_port",  cfg.tls_enabled ? std::to_string(cfg.tls_listen_port)
                                                : std::string{"-"},
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

        // Disk back-pressure guard. Starts a polling thread that updates
        // landing_disk_{used,free}_bytes + landing_disk_state and drives
        // the AssociationHandler's reject logic.
        nlr::DiskGuard disk_guard(cfg.landing_zone,
                                   cfg.disk_warn_pct,
                                   cfg.disk_reject_pct,
                                   cfg.disk_poll_interval_s,
                                   metrics);
        disk_guard.start();
        g_disk_guard.store(&disk_guard);

        // ---- Optional TLS layer ----
        // Constructed only when tls_enabled=true; failures (bad
        // cert/key/profile) throw and prevent the receiver from starting
        // rather than silently degrading to plain-only.
        std::unique_ptr<nlr::TlsLayer> tls_layer;
        if (cfg.tls_enabled) {
            nlr::TlsConfig tcfg;
            tcfg.enabled             = true;
            tcfg.listen_port         = cfg.tls_listen_port;
            tcfg.cert_file           = cfg.tls_cert_file;
            tcfg.key_file            = cfg.tls_key_file;
            tcfg.ca_file             = cfg.tls_ca_file;
            tcfg.require_client_cert = cfg.tls_require_client_cert;
            tcfg.profile             = cfg.tls_profile;
            tls_layer = std::make_unique<nlr::TlsLayer>(tcfg);
        }

        nlr::Server server(cfg, db, metrics, &disk_guard, tls_layer.get());

        g_server.store(&server);
        install_signal_handlers();

        const int rc = server.run();
        g_server.store(nullptr);

        disk_guard.stop();
        g_disk_guard.store(nullptr);
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
