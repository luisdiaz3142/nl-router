// nl-receiver/server.hpp
//
// Top-level SCP server. Owns the AssociationHandler, configures its allowed
// presentation contexts (the SOP classes / transfer syntaxes we accept), and
// runs the accept loop until told to stop.

#pragma once

#include <atomic>
#include <memory>

#include "association_handler.hpp"
#include "config.hpp"
#include "db.hpp"

namespace nlr {

class Server {
public:
    Server(const Config& cfg, Db& db);

    // Run the accept loop. Returns when stop() is called or a fatal network
    // error occurs.
    int run();

    // Signal the accept loop to stop. Safe to call from a signal handler
    // (the underlying flag is atomic).
    void stop() noexcept;

private:
    void configure_presentation_contexts_();

    const Config& cfg_;
    Db&           db_;
    std::unique_ptr<AssociationHandler> handler_;
    std::atomic<bool> stop_requested_ {false};
};

}  // namespace nlr
