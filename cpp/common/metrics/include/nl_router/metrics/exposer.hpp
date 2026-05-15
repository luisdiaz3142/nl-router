// nl-router/metrics/exposer.hpp
//
// Minimal HTTP server that serves Prometheus's text-exposition format on
// a single endpoint (default: GET /metrics). One blocking accept thread,
// one connection at a time — adequate for the ~15s scrape cadence
// Prometheus uses by default.
//
// We deliberately avoid pulling in a third-party HTTP server (civetweb,
// crow, httplib) — the protocol surface we need is tiny:
//   * accept a TCP connection
//   * read until \r\n\r\n
//   * recognize "GET /metrics" (anything else → 404)
//   * write HTTP/1.1 200 OK + body + close
//
// Bind address is "0.0.0.0" by default; configurable via the second arg.
//
// Lifetime: construct once, call start(), the destructor calls stop() and
// joins. stop() shuts down the listening socket so the accept thread
// unblocks promptly.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace nlr::metrics {

class Registry;

class Exposer {
public:
    // port=0 disables the exposer (start() is a no-op). Useful for tests.
    Exposer(Registry& registry,
            std::uint16_t port,
            std::string bind_addr = "0.0.0.0");
    ~Exposer();

    Exposer(const Exposer&)            = delete;
    Exposer& operator=(const Exposer&) = delete;

    void start();
    void stop() noexcept;

    // Port we actually bound (useful when constructed with port=0 elsewhere
    // — though we treat port=0 specially as "disabled" not "ephemeral").
    std::uint16_t port() const noexcept { return port_; }

private:
    void accept_loop_();

    Registry&            registry_;
    std::uint16_t        port_;
    std::string          bind_addr_;
    int                  listen_fd_ {-1};
    std::atomic<bool>    stop_requested_ {false};
    std::thread          thread_;
};

}  // namespace nlr::metrics
