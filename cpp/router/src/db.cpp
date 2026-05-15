#include "db.hpp"

#include <libpq-fe.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "logging.hpp"

namespace nlr {

namespace {

PGconn* as_conn(void* p) { return static_cast<PGconn*>(p); }

struct ResultGuard {
    PGresult* r {nullptr};
    explicit ResultGuard(PGresult* p) : r(p) {}
    ~ResultGuard() { if (r) PQclear(r); }
    ResultGuard(const ResultGuard&)            = delete;
    ResultGuard& operator=(const ResultGuard&) = delete;
};

// ---- Rule listing -------------------------------------------------------
// md5 of the predicate text is computed in Postgres so we get cache-key
// stability across processes (and so the router doesn't need an md5 lib).
constexpr const char* kListEnabledRulesSql = R"SQL(
SELECT id,
       name,
       scope::text,
       dispatch_order,
       priority,
       predicate,
       md5(predicate) AS predicate_hash
  FROM rules
 WHERE status = 'enabled'
 ORDER BY priority DESC, name
)SQL";

// ---- Claim a batch ------------------------------------------------------
// FOR UPDATE SKIP LOCKED is the standard work-queue pattern. We update the
// claim columns in the same statement (UPDATE ... FROM ... RETURNING) so
// other routers picking up concurrently see the rows as no longer
// 'received'.
//
// Args:
//   $1 = server_id  (which node owns the files; routing is local)
//   $2 = batch      (max rows to claim)
//   $3 = worker_id  (this process's identifier)
//   $4 = lease_s    (claim_expires_at offset from now())
constexpr const char* kClaimReceivedRowsSql = R"SQL(
WITH picked AS (
    SELECT id
      FROM work_queue
     WHERE status = 'received' AND server_id = $1
     ORDER BY priority DESC, received_at
     FOR UPDATE SKIP LOCKED
     LIMIT $2
)
UPDATE work_queue w
   SET status           = 'routing',
       claimed_by       = $3,
       claimed_at       = NOW(),
       claim_expires_at = NOW() + make_interval(secs => $4)
  FROM picked
 WHERE w.id = picked.id
RETURNING w.id,
          w.calling_aet,
          w.called_aet,
          host(w.peer_ip) AS peer_ip_text,
          w.tags::text     AS tags_text,
          w.file_root_path
)SQL";

// ---- Insert route_assignments for one rule ------------------------------
constexpr const char* kInsertAssignmentsSql = R"SQL(
INSERT INTO route_assignments (
    work_queue_id, rule_id, destination_id,
    dispatch_kind, server_id, status
)
SELECT $1, rd.rule_id, rd.destination_id,
       d.kind, $3, 'pending'
  FROM rule_destinations rd
  JOIN destinations d ON d.id = rd.destination_id
 WHERE rd.rule_id = $2
   AND d.enabled = TRUE
)SQL";

// ---- Insert processing_jobs from rule_processing_chain ------------------
// We compute input_path/output_path here in SQL using a CTE so the chain
// is ordered consistently with rule_processing_chain.ordinal. Each row's
// input is the previous row's output (lag()); the first row's input is
// the study root that the Router passes in.
//
// Args:
//   $1 work_queue_id   bigint
//   $2 rule_id         bigint
//   $3 server_id       text
//   $4 study_file_root text     (initial input for ordinal=0)
//   $5 processing_root text     (e.g. /var/lib/nl-router/processing)
constexpr const char* kInsertProcessingJobsSql = R"SQL(
WITH chain AS (
    SELECT rpc.module_id,
           rpc.ordinal,
           rpc.config_override,
           pm.kind   AS module_kind,
           pm.config AS module_config_default
      FROM rule_processing_chain rpc
      JOIN processing_modules pm ON pm.id = rpc.module_id
     WHERE rpc.rule_id = $2
       AND pm.enabled  = TRUE
     ORDER BY rpc.ordinal
),
paths AS (
    SELECT
        c.*,
        format('%s/%s/%s-%s', $5::text, $1::text, c.ordinal::text, c.module_kind)
            AS my_output,
        lag(format('%s/%s/%s-%s', $5::text, $1::text, c.ordinal::text, c.module_kind))
            OVER (ORDER BY c.ordinal)
            AS prev_output
      FROM chain c
)
INSERT INTO processing_jobs (
    work_queue_id, rule_id, module_id, module_kind, ordinal,
    server_id, input_path, output_path, config, status
)
SELECT $1::bigint, $2::bigint,
       paths.module_id, paths.module_kind, paths.ordinal,
       $3,
       COALESCE(paths.prev_output, $4)               AS input_path,
       paths.my_output                                AS output_path,
       COALESCE(paths.config_override, paths.module_config_default, '{}'::jsonb) AS config,
       'pending'::job_status
  FROM paths
)SQL";

// Finalize routing: advance to 'processing' if any jobs were inserted for
// this work_queue row, else 'routed'. EXISTS lookup keeps the cost
// bounded.
constexpr const char* kFinalizeRoutingSql = R"SQL(
UPDATE work_queue
   SET status = CASE
                  WHEN EXISTS (SELECT 1 FROM processing_jobs
                                 WHERE work_queue_id = $1)
                       THEN 'processing'::work_status
                  ELSE 'routed'::work_status
                END,
       routed_at        = NOW(),
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL,
       last_error       = NULL,
       failed_phase     = NULL
 WHERE id = $1
)SQL";

// ---- Status transitions -------------------------------------------------
constexpr const char* kMarkFailedSql = R"SQL(
UPDATE work_queue
   SET status           = 'failed',
       failed_phase     = 'router',
       last_error       = $2,
       retry_count      = retry_count + 1,
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL
 WHERE id = $1
)SQL";

// Pull a text cell from a result row. Returns empty string for NULL — the
// router's callers only call this on columns it knows are non-null.
std::string cell(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return {};
    return std::string{PQgetvalue(r, row, col),
                       static_cast<std::size_t>(PQgetlength(r, row, col))};
}

}  // namespace

// ---- ctors / connection management --------------------------------------

Db::Db(const std::string& dsn) {
    PGconn* c = PQconnectdb(dsn.c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        const std::string err = PQerrorMessage(c);
        PQfinish(c);
        throw DbError("connection failed: " + err);
    }
    conn_ = c;
    LOG_INFO("db.connected", "host", PQhost(c), "db", PQdb(c));
}

Db::~Db() {
    if (conn_) PQfinish(as_conn(conn_));
}

void Db::ensure_connected_() {
    auto* c = as_conn(conn_);
    if (PQstatus(c) == CONNECTION_OK) return;
    LOG_WARN("db.reconnect", "reason", PQerrorMessage(c));
    PQreset(c);
    if (PQstatus(c) != CONNECTION_OK) {
        throw DbError(std::string{"reconnect failed: "} + PQerrorMessage(c));
    }
}

// ---- API ----------------------------------------------------------------

std::vector<DbRule> Db::list_enabled_rules() {
    ensure_connected_();

    ResultGuard r{PQexec(as_conn(conn_), kListEnabledRulesSql)};
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"list_enabled_rules: "} +
                      PQerrorMessage(as_conn(conn_)));
    }

    const int n = PQntuples(r.r);
    std::vector<DbRule> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        DbRule rule;
        rule.id              = std::stoll(cell(r.r, i, 0));
        rule.name            = cell(r.r, i, 1);
        rule.scope           = cell(r.r, i, 2);
        rule.dispatch_order  = cell(r.r, i, 3);
        rule.priority        = static_cast<std::int16_t>(std::stoi(cell(r.r, i, 4)));
        rule.predicate       = cell(r.r, i, 5);
        rule.predicate_hash  = cell(r.r, i, 6);
        out.push_back(std::move(rule));
    }
    return out;
}

std::vector<ClaimedRow> Db::claim_received_rows(const std::string& server_id,
                                                 const std::string& worker_id,
                                                 std::uint32_t lease_seconds,
                                                 std::uint32_t batch) {
    ensure_connected_();

    const std::string batch_str    = std::to_string(batch);
    const std::string lease_str    = std::to_string(lease_seconds);

    const char* params[]   = { server_id.c_str(), batch_str.c_str(),
                               worker_id.c_str(), lease_str.c_str() };
    const int   nparams    = 4;

    ResultGuard r{
        PQexecParams(as_conn(conn_), kClaimReceivedRowsSql,
                     nparams, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"claim_received_rows: "} +
                      PQerrorMessage(as_conn(conn_)));
    }

    const int n = PQntuples(r.r);
    std::vector<ClaimedRow> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ClaimedRow row;
        row.id          = std::stoll(cell(r.r, i, 0));
        row.calling_aet    = cell(r.r, i, 1);
        row.called_aet     = cell(r.r, i, 2);
        row.peer_ip        = cell(r.r, i, 3);
        row.tags_json      = cell(r.r, i, 4);
        row.file_root_path = cell(r.r, i, 5);
        out.push_back(std::move(row));
    }
    return out;
}

int Db::insert_processing_jobs(std::int64_t work_queue_id,
                                std::int64_t rule_id,
                                const std::string& server_id,
                                const std::string& study_file_root,
                                const std::string& processing_root) {
    ensure_connected_();
    const std::string wq  = std::to_string(work_queue_id);
    const std::string rid = std::to_string(rule_id);
    const char* params[] = {
        wq.c_str(), rid.c_str(), server_id.c_str(),
        study_file_root.c_str(), processing_root.c_str()
    };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kInsertProcessingJobsSql,
                     5, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"insert_processing_jobs: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    return std::stoi(PQcmdTuples(r.r));
}

void Db::finalize_routing(std::int64_t work_queue_id) {
    ensure_connected_();
    const std::string idstr = std::to_string(work_queue_id);
    const char* params[] = { idstr.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kFinalizeRoutingSql,
                     1, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"finalize_routing: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
}

int Db::insert_route_assignments(std::int64_t work_queue_id,
                                  std::int64_t rule_id,
                                  const std::string& server_id) {
    ensure_connected_();

    const std::string wq   = std::to_string(work_queue_id);
    const std::string rid  = std::to_string(rule_id);

    const char* params[] = { wq.c_str(), rid.c_str(), server_id.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kInsertAssignmentsSql,
                     3, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"insert_route_assignments: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    return std::stoi(PQcmdTuples(r.r));
}

void Db::mark_failed(std::int64_t work_queue_id, const std::string& error) {
    ensure_connected_();
    const std::string idstr = std::to_string(work_queue_id);
    const char* params[] = { idstr.c_str(), error.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kMarkFailedSql,
                     2, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"mark_failed: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
}

}  // namespace nlr
