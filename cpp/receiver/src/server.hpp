// nl-receiver/server.hpp
//
// Top-level SCP server. Owns one DcmSCPPool per transport (plain and,
// optionally, TLS), each pool sized to cfg.max_associations worker
// threads. Configures shared presentation contexts (SOP classes /
// transfer syntaxes) once and hands a DcmSharedSCPConfig to each pool.
//
// Threading model: each pool's listener thread accepts TCP connections
// and dispatches accepted associations to its idle workers. With N
// workers per transport, up to N associations run concurrently per
// transport (so up to 2N if both transports are at capacity).

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <dcmtk/dcmnet/scp.h>
#include <dcmtk/dcmnet/scppool.h>

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
    // pool on cfg.tls_listen_port wrapping each connection in the
    // configured DcmTLSTransportLayer; otherwise only the plain pool
    // on cfg.listen_port is active.
    Server(const Config& cfg, Db& db, const ReceiverMetrics& metrics,
           const DiskGuard* disk_guard, TlsLayer* tls_layer);

    ~Server();

    // Run the accept loops. Returns when stop() is called or a fatal
    // network error occurs on the plain pool. The TLS pool runs in a
    // dedicated thread joined here at shutdown.
    int run();

    // Signal both pools to stop. Safe to call from a signal handler
    // (the underlying flag is atomic).
    void stop() noexcept;

private:
    // Type aliases — keep the template gymnastics out of the .cpp.
    using PlainPool = DcmSCPPool<PlainAssociationHandler>;
    using TlsPool   = DcmSCPPool<TlsAssociationHandler>;

    // Adds the receiver's standard AETitle, port, presentation contexts,
    // and timeouts to a pool's config. Called once per pool at
    // construction; all pool workers share this config under the hood.
    void configure_pool_config_(DcmSCPConfig& scfg, std::uint16_t port);

    // Drives a pool's blocking listen() loop. Used as the body of the
    // dedicated TLS-listener thread; the plain pool runs on the main
    // thread inside run().
    int drive_pool_(DcmBaseSCPPool& pool, const char* tag);

    const Config& cfg_;
    Db&           db_;
    const ReceiverMetrics& metrics_;

    std::unique_ptr<PlainPool> plain_pool_;
    std::unique_ptr<TlsPool>   tls_pool_;     // null if TLS disabled
    std::thread                tls_thread_;
    std::atomic<bool>          stop_requested_ {false};
};

}  // namespace nlr
