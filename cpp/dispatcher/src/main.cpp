// nl-dispatch — outbound DICOM dispatcher for nl-router.
//
// Spawns one worker thread per enabled destination, each polling
// route_assignments for that destination + this server. v1 supports the
// 'dicom' kind only (DCMTK SCU); other kinds get logged and skipped.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>

#include "config.hpp"
#include "logging.hpp"
#include "metrics.hpp"
#include "server.hpp"

#include "nl_router/metrics/exposer.hpp"
#include "nl_router/metrics/registry.hpp"

namespace {

std::atomic<nlr::Server*> g_server{nullptr};

void on_signal(int sig) {
    if (auto* s = g_server.load()) s->stop();
    std::signal(sig, SIG_DFL);
}

void install_signal_handlers() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    try {
        const auto cfg = nlr::load_config();
        nlr::log::set_level(nlr::log::parse_level(cfg.log_level));

        LOG_INFO("startup",
            "server_id",    cfg.server_id,
            "metrics_port", std::to_string(cfg.metrics_port),
            "metrics_bind", cfg.metrics_bind_addr,
            "log_level",    cfg.log_level);

        auto& registry = nlr::metrics::Registry::global();
        nlr::DispatcherMetrics metrics = nlr::DispatcherMetrics::register_all(registry);
        nlr::metrics::Exposer exposer(registry, cfg.metrics_port, cfg.metrics_bind_addr);
        exposer.start();

        nlr::Server server(cfg, metrics);
        g_server.store(&server);
        install_signal_handlers();

        const int rc = server.run();
        g_server.store(nullptr);

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
