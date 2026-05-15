// nl-dispatch/worker.hpp
//
// One Worker per enabled destination, running on its own thread. The
// Worker polls for assignments addressed to its destination, hands each
// to the appropriate DispatchHandler, and updates the DB based on the
// result. Independent per-destination workers ensure a slow / down
// destination can't block dispatch to a healthy one.
//
// v1 is single-threaded per destination. dispatch_concurrency > 1 is
// honored in M10 by giving each destination a thread pool of that size.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "config.hpp"
#include "db.hpp"
#include "handler.hpp"

namespace nlr {

class Worker {
public:
    Worker(const Config& cfg, Destination destination, const std::string& worker_id);
    ~Worker();

    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;

    // Start the worker thread.
    void start();

    // Signal the worker to stop after the current iteration; non-blocking.
    void stop() noexcept;

    // Block until the worker thread exits.
    void join();

    std::int64_t destination_id() const noexcept { return destination_.id; }
    const std::string& destination_name() const noexcept { return destination_.name; }

    // Update the worker's view of its destination (called from the server
    // thread when a destination refresh shows changed config/retry policy).
    // The worker picks up the new config on its next iteration.
    void update_destination(Destination updated);

private:
    void run_();

    Config         cfg_;
    Destination    destination_;            // mutex-guarded mutable snapshot
    std::mutex     destination_m_;
    std::unique_ptr<Db> db_;                // owned by this worker (libpq isn't thread-safe)
    std::unique_ptr<DispatchHandler> handler_;
    std::string    worker_id_;
    std::atomic<bool> stop_requested_ {false};
    std::thread    thread_;
};

}  // namespace nlr
