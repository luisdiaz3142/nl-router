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
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>

#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmnet/dimse.h>

#include "config.hpp"
#include "db.hpp"
#include "logging.hpp"
#include "server.hpp"

namespace {

// Global pointer for the signal handler. We avoid any heavyweight handling
// inside the handler — just flip the stop flag on the server.
std::atomic<nlr::Server*> g_server{nullptr};

void on_signal(int sig) {
    auto* s = g_server.load();
    if (s) s->stop();
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
            "server_id",    cfg.server_id,
            "host_aet",     cfg.host_aet,
            "listen_port",  std::to_string(cfg.listen_port),
            "landing_zone", cfg.landing_zone,
            "log_level",    cfg.log_level);

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

        nlr::Db db(cfg.database_url);
        nlr::Server server(cfg, db);

        g_server.store(&server);
        install_signal_handlers();

        const int rc = server.run();
        g_server.store(nullptr);

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
