#include "disk_guard.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include "logging.hpp"

namespace nlr {

DiskGuard::DiskGuard(std::string landing_zone,
                     std::uint8_t warn_pct,
                     std::uint8_t reject_pct,
                     std::uint32_t poll_interval_s,
                     ReceiverMetrics& metrics)
    : landing_zone_(std::move(landing_zone)),
      warn_pct_(warn_pct),
      reject_pct_(reject_pct),
      poll_interval_s_(poll_interval_s),
      metrics_(metrics)
{
    // Clamp to sane values so a misconfig can't make every association
    // get rejected (or admitted) regardless of disk state.
    if (warn_pct_ > 100)  warn_pct_  = 100;
    if (reject_pct_ > 100) reject_pct_ = 100;
    if (warn_pct_ > reject_pct_) {
        // Swap rather than throw — a misconfigured warn>reject is still
        // unambiguously interpretable as "always at least warning" once
        // the lower bound is crossed. Log the surprise and proceed.
        LOG_WARN("disk_guard.config_swapped",
            "warn_pct",   std::to_string(warn_pct_),
            "reject_pct", std::to_string(reject_pct_));
        std::swap(warn_pct_, reject_pct_);
    }
    if (poll_interval_s_ == 0) poll_interval_s_ = 1;
}

DiskGuard::~DiskGuard() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void DiskGuard::start() {
    // Poll once synchronously so the gauges and state reflect reality
    // before the SCP starts accepting traffic. This prevents the first
    // association from being mis-routed during the very-short window
    // between server start and the first scheduled poll tick.
    poll_once();
    thread_ = std::thread([this] { poll_loop_(); });
}

void DiskGuard::stop() noexcept {
    stop_flag_.store(true, std::memory_order_relaxed);
}

void DiskGuard::poll_once() {
    std::error_code ec;
    const auto space = std::filesystem::space(landing_zone_, ec);
    if (ec) {
        // Path may not exist yet (first run). Don't reject in that case —
        // the user should see a clear error elsewhere, and a missing path
        // implies we can't compute a percentage anyway.
        LOG_WARN("disk_guard.space_failed",
            "path",  landing_zone_,
            "error", ec.message());
        return;
    }
    if (space.capacity == 0) {
        LOG_WARN("disk_guard.zero_capacity", "path", landing_zone_);
        return;
    }

    const std::int64_t free_bytes = static_cast<std::int64_t>(space.available);
    const std::int64_t used_bytes =
        static_cast<std::int64_t>(space.capacity - space.available);
    const double used_pct =
        100.0 * static_cast<double>(space.capacity - space.available) /
                static_cast<double>(space.capacity);

    update_state_(used_pct, free_bytes, used_bytes);
}

void DiskGuard::update_state_(double used_pct,
                                std::int64_t free_bytes,
                                std::int64_t used_bytes) {
    State next;
    if (used_pct >= static_cast<double>(reject_pct_))      next = State::Reject;
    else if (used_pct >= static_cast<double>(warn_pct_))   next = State::Warning;
    else                                                    next = State::Normal;

    metrics_.landing_disk_free_bytes.self().set(free_bytes);
    metrics_.landing_disk_used_bytes.self().set(used_bytes);
    metrics_.landing_disk_state.self().set(static_cast<std::int64_t>(next));

    const State prev = state_.exchange(next, std::memory_order_release);
    if (prev != next) {
        // Single log line per transition. "info" on improvement,
        // "warn" on degradation — operators usually filter on warn+.
        const auto state_name = [](State s) -> const char* {
            switch (s) {
                case State::Normal:  return "normal";
                case State::Warning: return "warning";
                case State::Reject:  return "reject";
            }
            return "unknown";
        };
        const bool worsened = (static_cast<int>(next) > static_cast<int>(prev));
        const std::string used_pct_str = [used_pct]() {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f", used_pct);
            return std::string{buf};
        }();
        if (worsened) {
            LOG_WARN("disk_guard.state_changed",
                "from",       state_name(prev),
                "to",         state_name(next),
                "used_pct",   used_pct_str,
                "free_bytes", std::to_string(free_bytes),
                "warn_pct",   std::to_string(warn_pct_),
                "reject_pct", std::to_string(reject_pct_));
        } else {
            LOG_INFO("disk_guard.state_changed",
                "from",       state_name(prev),
                "to",         state_name(next),
                "used_pct",   used_pct_str,
                "free_bytes", std::to_string(free_bytes));
        }
    }
}

void DiskGuard::poll_loop_() {
    using clock = std::chrono::steady_clock;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const auto until = clock::now() + std::chrono::seconds(poll_interval_s_);
        // Periodic wake to honor stop_flag without sleeping the full
        // interval. 250ms is small relative to typical 10s intervals
        // and high relative to thread-wake overhead.
        while (clock::now() < until
               && !stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (stop_flag_.load(std::memory_order_relaxed)) break;
        poll_once();
    }
}

}  // namespace nlr
