// nl-clean/db.hpp
//
// Postgres client for the cleaner.
//
// Responsibilities:
//   * Load retention TTLs from system_config at startup.
//   * Per-cycle: list eligible terminal-status rows on this node whose
//     TTL has elapsed, delete their files (caller's job), then mark them
//     'cleaned'.
//   * Periodically attempt a Postgres advisory lock; whoever holds it
//     runs the leader-only DELETE that prunes ancient 'cleaned' rows.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlr {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Effective per-status TTLs loaded from system_config. Defaults match the
// schema seed; explicit reads here pick up operator overrides.
struct RetentionConfig {
    int dispatched_h         {24};        // hours, per terminal status
    int partial_h            {168};
    int failed_h             {720};
    int prune_dispatched_d   {365};       // days, after status='cleaned'
    int prune_partial_d      {1095};
    int prune_failed_d       {1825};
};

// One eligible row, returned by list_eligible_rows().
struct EligibleRow {
    std::int64_t id;
    std::string  status;             // 'dispatched' | 'dispatched_partial' | 'failed'
    std::string  file_root_path;
};

// Constant lock id used for row-prune leader election. Any nonzero
// 8-byte int works as long as everyone agrees. 0x4E4C525F434C4E52 =
// "NLR_CLNR" in ASCII; uniquely scoped to this concern.
constexpr std::int64_t kPruneLockId = 0x4E4C525F434C4E52LL;

class Db {
public:
    explicit Db(const std::string& dsn);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // Load retention TTLs from the system_config table. Missing keys fall
    // back to the seed defaults (idempotent — safe on a stale DB).
    RetentionConfig load_retention();

    // Return rows on this node whose terminal-state TTL has elapsed and
    // which are eligible for file cleanup (status terminal, not held).
    std::vector<EligibleRow> list_eligible_rows(
        const std::string& server_id,
        const RetentionConfig& ttl,
        std::uint32_t limit);

    // Mark a single row 'cleaned' (files have been deleted by the caller).
    void mark_cleaned(std::int64_t work_queue_id);

    // Try to acquire the prune-leader advisory lock. Returns true if we
    // got it; the caller must release_prune_lock() before next iteration.
    bool try_acquire_prune_lock();
    void release_prune_lock();

    // Leader-only: delete 'cleaned' rows older than the prune TTL.
    // Returns the number of rows deleted in this batch.
    //
    // v1 simplification: we don't preserve the original terminal status,
    // so we use the LONGEST prune TTL globally (prune_failed_d). Operators
    // wanting finer-grained pruning by original status need a schema
    // change (pre_clean_status column) — a planned follow-up.
    int prune_cleaned_rows(int days, std::uint32_t batch);

private:
    void* conn_ {nullptr};
    void ensure_connected_();
};

}  // namespace nlr
