// nl-receiver/server.hpp
//
// Top-level SCP server. Owns the AssociationHandler, configures its allowed
// presentation contexts (the SOP classes / transfer syntaxes we accept), and
// runs the accept loop until told to stop.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "association_handler.hpp"
#include "config.hpp"
#include "db.hpp"
#include "metrics.hpp"

namespace nlr {

class DiskGuard;
class TlsLayer;

class Server {
public:
    // tls_layer is optional. When non-null the Server runs a second
    // listener on cfg.tls_listen_port wrapping each connection in the
    // configured DcmTLSTransportLayer; otherwise only the plain
    // listener on cfg.listen_port is active.
    Server(const Config& cfg, Db& db, const ReceiverMetrics& metrics,
           const DiskGuard* disk_guard, TlsLayer* tls_layer);

    // Run the accept loops. Returns when stop() is called or a fatal
    // network error occurs on the plain listener. The TLS listener runs
    // in a dedicated thread joined here at shutdown.
    int run();

    // Signal the accept loops to stop. Safe to call from a signal handler
    // (the underlying flag is atomic).
    void stop() noexcept;

private:
    // Configures a DcmSCPConfig with the receiver's standard AETitle,
    // port, presentation contexts, timeouts, etc. Shared by both
    // listeners so the SOP-class / transfer-syntax set is identical.
    void configure_presentation_contexts_(AssociationHandler& handler,
                                           std::uint16_t port);

    int run_listener_(AssociationHandler& handler, const char* tag);

    const Config& cfg_;
    Db&           db_;
    const ReceiverMetrics& metrics_;

    std::unique_ptr<AssociationHandler> plain_handler_;
    std::unique_ptr<AssociationHandler> tls_handler_;       // null if TLS disabled
    std::thread                         tls_thread_;
    std::atomic<bool>                   stop_requested_ {false};
};

}  // namespace nlr
