// nl-dispatch/server.hpp
//
// Manages the per-destination worker pool. On startup we read the list of
// enabled destinations and spawn one Worker per. Periodically we re-read
// the list; new destinations get fresh workers, and disabled destinations
// (or destinations removed from the table) have their workers stopped.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

#include "config.hpp"
#include "db.hpp"
#include "worker.hpp"

namespace nlr {

class Server {
public:
    explicit Server(const Config& cfg);

    int run();
    void stop() noexcept;

private:
    void refresh_destinations_();
    void stop_all_workers_();
    std::string make_worker_id_(std::int64_t destination_id) const;

    Config cfg_;
    std::unique_ptr<Db> meta_db_;            // used by the server thread for
                                              //  the destination-list query
    std::unordered_map<std::int64_t, std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stop_requested_ {false};
    std::chrono::steady_clock::time_point last_refresh_;
};

}  // namespace nlr
