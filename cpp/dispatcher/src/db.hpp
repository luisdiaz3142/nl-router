// nl-dispatch/db.hpp
//
// Postgres client for the dispatcher.
//
// Responsibilities:
//   * List enabled destinations on this server (workers boot from this).
//   * Per destination, claim a batch of route_assignments in status='pending'
//     (or 'failed' with next_retry_at hit) via FOR UPDATE SKIP LOCKED, and
//     atomically transition them to 'dispatching'.
//   * Mark an assignment dispatched / transient-failed / permanently-failed,
//     applying the retry policy when scheduling the next attempt.
//   * Roll work_queue.status up from 'dispatching' → 'dispatched' /
//     'dispatched_partial' / 'failed' when an assignment reaches a terminal
//     state and no non-terminal assignments remain for that row.
//
// All operations are synchronous and run on the calling worker's thread.
// Each worker owns its own Db instance because libpq connections aren't
// thread-safe (a pool/PgBouncer can come later).

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "handler.hpp"

namespace nlr {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Db {
public:
    explicit Db(const std::string& dsn);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // List destinations enabled on this server. For M3 we always return
    // every enabled destination — destinations are not pinned to a node;
    // assignments are. A worker for destination D on node N only handles
    // assignments where work_queue.server_id == N.
    std::vector<Destination> list_enabled_destinations();

    // Claim up to `batch` assignments for one destination on this server,
    // atomically transitioning them to 'dispatching'. Eligibility:
    //   * dispatch_kind matches destination.kind
    //   * status='pending'  OR  (status='failed' AND next_retry_at <= now())
    //   * server_id matches this node
    //
    // worker_id and lease_seconds are recorded for observability and crash
    // recovery (M10's lease sweeper).
    std::vector<Assignment> claim_pending_for_destination(
        std::int64_t destination_id,
        const std::string& server_id,
        const std::string& worker_id,
        std::uint32_t lease_seconds,
        std::uint32_t batch);

    // Mark an assignment dispatched. Records response_detail and updates
    // the parent work_queue row's status if all sibling assignments are
    // also terminal.
    void mark_dispatched(std::int64_t assignment_id,
                          const std::string& response_detail_json);

    // Mark an assignment failed with retry scheduled per the destination's
    // retry_policy. If attempts >= max_attempts OR elapsed > give_up_after_hours,
    // the assignment is marked permanently failed and the parent row
    // status is rolled up.
    //
    // Returns true if the failure was permanent.
    bool mark_failed_with_retry(std::int64_t assignment_id,
                                 const std::string& error,
                                 const std::string& response_detail_json,
                                 const Destination& dst);

    // Mark an assignment permanently failed (e.g. file missing on disk —
    // no retry will help).
    void mark_failed_permanent(std::int64_t assignment_id,
                                const std::string& error,
                                const std::string& response_detail_json);

private:
    void* conn_ {nullptr};

    void connect_();
    void ensure_connected_();
};

}  // namespace nlr
