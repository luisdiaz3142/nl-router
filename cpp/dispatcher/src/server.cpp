#include "server.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "logging.hpp"
#include "nl_router/crypto/envelope.hpp"
#include "nl_router/crypto/kek.hpp"

namespace nlr {

namespace {

std::string short_hostname() {
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) return "unknown";
    return host;
}

}  // namespace

Server::Server(const Config& cfg, const DispatcherMetrics& metrics)
    : cfg_(cfg), metrics_(metrics) {
    meta_db_ = std::make_unique<Db>(cfg_.database_url);
    last_refresh_ = std::chrono::steady_clock::now() - std::chrono::hours(1);

    // Try to load the KEK at startup. If unavailable, we still start —
    // destinations without credentials work fine — but log a warning so
    // operators know credential-requiring destinations will fail.
    try {
        kek_ = ::nl_router::crypto::load_kek();
        LOG_INFO("dispatcher.kek_loaded");
    } catch (const ::nl_router::crypto::KEKUnavailableError& e) {
        LOG_WARN("dispatcher.kek_unavailable",
            "reason", e.what(),
            "impact", "destinations with credential_id will fail per-assignment until a KEK is configured");
    }

    LOG_INFO("dispatcher.startup",
        "server_id",            cfg_.server_id,
        "poll_interval_ms",     std::to_string(cfg_.poll_interval_ms),
        "batch_size",           std::to_string(cfg_.batch_size),
        "lease_seconds",        std::to_string(cfg_.lease_seconds),
        "destination_refresh_s", std::to_string(cfg_.destination_refresh_s));
}

std::string Server::make_worker_id_(std::int64_t destination_id) const {
    return "nl-dispatch@" + short_hostname() + ":" +
           std::to_string(getpid()) + "/d" + std::to_string(destination_id);
}

void Server::refresh_destinations_() {
    std::vector<Destination> latest;
    try {
        latest = meta_db_->list_enabled_destinations();
    } catch (const std::exception& e) {
        LOG_ERROR("dispatcher.refresh_failed", "error", e.what());
        return;
    }

    std::unordered_set<std::int64_t> latest_ids;
    for (auto& d : latest) latest_ids.insert(d.id);

    // Start workers for new destinations; update workers for existing ones.
    for (auto& d : latest) {
        auto it = workers_.find(d.id);
        if (it == workers_.end()) {
            const auto wid = make_worker_id_(d.id);
            auto w = std::make_unique<Worker>(cfg_, d, wid, kek_, metrics_);
            w->start();
            workers_[d.id] = std::move(w);
            LOG_INFO("dispatcher.worker_started",
                "destination_id", std::to_string(d.id),
                "destination",    d.name,
                "kind",           d.kind);
        } else {
            it->second->update_destination(d);
        }
    }

    // Stop workers for destinations that are gone or disabled.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (latest_ids.find(it->first) == latest_ids.end()) {
            LOG_INFO("dispatcher.worker_stopping",
                "destination_id", std::to_string(it->first),
                "destination",    it->second->destination_name());
            it->second->stop();
            it->second->join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }

    metrics_.destinations_active.self().set(
        static_cast<std::int64_t>(workers_.size()));

    LOG_INFO("dispatcher.refresh_done",
        "destinations", std::to_string(latest.size()),
        "workers",      std::to_string(workers_.size()));
}

int Server::run() {
    // Eager first refresh so workers exist immediately.
    refresh_destinations_();
    last_refresh_ = std::chrono::steady_clock::now();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // The server thread mostly sleeps; periodically refresh the
        // destination list. Workers do all the real work on their own
        // threads.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_refresh_).count();
        if (cfg_.destination_refresh_s > 0 &&
            elapsed >= static_cast<long>(cfg_.destination_refresh_s))
        {
            refresh_destinations_();
            last_refresh_ = now;
        }

        // Sleep in short increments so signal handlers don't have to wait
        // the full destination_refresh_s interval to be observed.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    stop_all_workers_();
    LOG_INFO("dispatcher.shutdown");
    return 0;
}

void Server::stop_all_workers_() {
    for (auto& [id, w] : workers_) w->stop();
    for (auto& [id, w] : workers_) w->join();
    workers_.clear();
}

void Server::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

}  // namespace nlr
