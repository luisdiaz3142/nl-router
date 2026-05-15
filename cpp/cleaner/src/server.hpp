// nl-clean/server.hpp
//
// Periodic scan loop: file cleanup on this node + leader-only row pruning.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "config.hpp"
#include "db.hpp"

namespace nlr {

class Server {
public:
    explicit Server(const Config& cfg);

    int run();
    void stop() noexcept;

private:
    std::size_t file_cleanup_pass_();
    int row_prune_pass_();

    Config             cfg_;
    std::unique_ptr<Db> db_;
    RetentionConfig    retention_;
    std::atomic<bool>  stop_requested_ {false};
};

}  // namespace nlr
