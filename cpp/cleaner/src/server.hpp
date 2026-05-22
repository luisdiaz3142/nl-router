// nl-clean/server.hpp
//
// Periodic scan loop: file cleanup on this node + leader-only row pruning.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "config.hpp"
#include "db.hpp"
#include "metrics.hpp"

namespace nlr {

class Server {
public:
    Server(const Config& cfg, const CleanerMetrics& metrics);

    int run();
    void stop() noexcept;

private:
    std::size_t file_cleanup_pass_();
    int row_prune_pass_();

    Config                  cfg_;
    std::unique_ptr<Db>     db_;
    RetentionConfig         retention_;
    const CleanerMetrics&   metrics_;
    std::atomic<bool>       stop_requested_ {false};
};

}  // namespace nlr
