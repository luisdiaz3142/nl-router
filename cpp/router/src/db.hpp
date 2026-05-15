// nl-route/db.hpp
//
// Postgres client for the routing daemon.
//
// Responsibilities:
//   * List enabled rules (RuleCache uses this on refresh).
//   * Claim a batch of work_queue rows in status='received' via
//     SELECT FOR UPDATE SKIP LOCKED, atomically marking them 'routing'.
//   * Fetch the routing context for a claimed row (calling_aet, called_aet,
//     peer_ip, tags JSONB).
//   * Insert route_assignments rows for matching (rule, destination) pairs.
//   * Advance a row to 'routed' on success or 'failed' on rule error.
//
// All operations are synchronous and run on the router's polling thread.
// M10 adds dedicated worker threads sharing a libpq connection pool.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlr {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One row in the rules table (only fields the router uses).
struct DbRule {
    std::int64_t id;
    std::string  name;
    std::string  scope;
    std::string  dispatch_order;
    std::int16_t priority;
    std::string  predicate;
    std::string  predicate_hash;   // md5 of the predicate text
};

// One claimed work_queue row, fetched with everything the router needs to
// build the eval context and route it.
struct ClaimedRow {
    std::int64_t id;
    std::string  calling_aet;
    std::string  called_aet;
    std::string  peer_ip;
    std::string  tags_json;        // jsonb rendered as text
};

class Db {
public:
    explicit Db(const std::string& dsn);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // Return all rules in status='enabled', regardless of scope. Caller
    // (RuleCache) buckets and parses.
    std::vector<DbRule> list_enabled_rules();

    // Claim up to `batch` work_queue rows in status='received' for this
    // server, atomically transitioning them to status='routing'. The rows
    // are populated with the eval-context fields.
    //
    // server_id pins routing to the node that owns the landing-zone files
    // (per the design plan's local-disk-per-node model).
    //
    // `worker_id` and `lease_seconds` are written to claim_* columns for
    // observability and crash recovery.
    std::vector<ClaimedRow> claim_received_rows(const std::string& server_id,
                                                 const std::string& worker_id,
                                                 std::uint32_t lease_seconds,
                                                 std::uint32_t batch);

    // Insert one route_assignments row per (rule, destination) pair for the
    // given work_queue row. Returns the number of assignments inserted.
    //
    // The SELECT-from-INSERT joins rule_destinations + destinations so the
    // dispatch_kind column is denormalized for cheap dispatcher pickup.
    // server_id is preserved on the assignment so dispatcher workers route
    // only their own node's assignments.
    int insert_route_assignments(std::int64_t work_queue_id,
                                  std::int64_t rule_id,
                                  const std::string& server_id);

    // Mark a row 'routed' (success) and clear claim columns.
    void mark_routed(std::int64_t work_queue_id);

    // Mark a row 'failed' with an error message (router-phase failure,
    // typically a rule that threw at eval time). Clears claim columns.
    void mark_failed(std::int64_t work_queue_id, const std::string& error);

private:
    void* conn_ {nullptr};  // PGconn* — kept opaque to avoid leaking libpq
                             //  out of this header

    void connect_();
    void ensure_connected_();
};

}  // namespace nlr
