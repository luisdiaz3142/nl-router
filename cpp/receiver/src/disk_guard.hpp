// nl-receiver/disk_guard.hpp
//
// Polls the landing-zone filesystem and exposes a 3-level state that the
// SCP queries before accepting an association.
//
// Thresholds (used%):
//   < warn_pct          → Normal      (accept, no logs)
//   warn_pct .. reject  → Warning     (accept, periodic warn log + gauge=1)
//   ≥ reject_pct        → Reject      (A-ASSOCIATE-RJ on new associations)
//
// In-flight associations are not interrupted — they already hold disk and
// returning a mid-write error would only orphan a partial study. Disk-full
// mid-write is handled at the C-STORE response layer by DCMTK's write
// errors; we surface them via existing error metrics.
//
// Recovery: the cleaner (M9) frees disk over time; once usage drops below
// warn_pct the state returns to Normal and new associations are accepted
// without operator intervention.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "metrics.hpp"

namespace nlr {

class DiskGuard {
public:
    enum class State : int {
        Normal  = 0,
        Warning = 1,
        Reject  = 2,
    };

    DiskGuard(std::string landing_zone,
              std::uint8_t warn_pct,
              std::uint8_t reject_pct,
              std::uint32_t poll_interval_s,
              ReceiverMetrics& metrics);
    ~DiskGuard();

    DiskGuard(const DiskGuard&)            = delete;
    DiskGuard& operator=(const DiskGuard&) = delete;

    void start();
    void stop() noexcept;

    // Hot-path query. Safe from any thread, no allocation.
    State state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    // For tests: poll once synchronously without the thread.
    void poll_once();

private:
    void poll_loop_();
    void update_state_(double used_pct, std::int64_t free_bytes, std::int64_t used_bytes);

    std::string  landing_zone_;
    std::uint8_t warn_pct_;
    std::uint8_t reject_pct_;
    std::uint32_t poll_interval_s_;
    ReceiverMetrics& metrics_;

    std::atomic<State> state_      {State::Normal};
    std::atomic<bool>  stop_flag_  {false};
    std::thread        thread_;
};

}  // namespace nlr
