#include "db.hpp"

#include <libpq-fe.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

std::string cell(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return {};
    return std::string{PQgetvalue(r, row, col),
                       static_cast<std::size_t>(PQgetlength(r, row, col))};
}

// system_config values are JSONB scalars (integer-looking text like "24")
// stored as a JSON value. Strip leading/trailing whitespace and quotes
// before parsing as int.
int parse_int_scalar(const std::string& raw, int fallback) {
    if (raw.empty()) return fallback;
    std::string s = raw;
    // Trim whitespace.
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    // Strip outer JSON quotes if present.
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    try {
        return std::stoi(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

// SQL: list rows whose terminal-state TTL has elapsed.
//
// "Terminal-state time" is the row's dispatched_at (set by the dispatcher's
// work_queue rollup). Router-phase failures don't set dispatched_at, so we
// fall back to closed_at — the receive-time stamp — as a stable backstop.
// That bounds even pathological "failed but never went through dispatch"
// rows by the longest TTL (failed: 30 days).
//
// $1 server_id   text
// $2 dispatched_h int  $3 partial_h int  $4 failed_h int
// $5 limit       int
constexpr const char* kListEligibleSql = R"SQL(
SELECT id, status::text, file_root_path
  FROM work_queue
 WHERE server_id     = $1
   AND status IN ('dispatched','dispatched_partial','failed')
   AND cleanup_hold  = FALSE
   AND CASE status
         WHEN 'dispatched'         THEN
              now() - COALESCE(dispatched_at, closed_at) > make_interval(hours => $2)
         WHEN 'dispatched_partial' THEN
              now() - COALESCE(dispatched_at, closed_at) > make_interval(hours => $3)
         WHEN 'failed'             THEN
              now() - COALESCE(dispatched_at, closed_at) > make_interval(hours => $4)
       END
 ORDER BY COALESCE(dispatched_at, closed_at)
 LIMIT $5
)SQL";

constexpr const char* kMarkCleanedSql = R"SQL(
UPDATE work_queue
   SET status     = 'cleaned',
       cleaned_at = NOW()
 WHERE id = $1
)SQL";

// LIMIT inside a DELETE isn't directly supported; pattern below is the
// usual workaround. Bounded at $2 rows per cycle so a long backlog drains
// over time without holding row locks for ages.
constexpr const char* kPruneSql = R"SQL(
DELETE FROM work_queue
 WHERE id IN (
    SELECT id FROM work_queue
     WHERE status     = 'cleaned'
       AND cleaned_at < now() - make_interval(days => $1)
     ORDER BY cleaned_at
     LIMIT $2
 )
)SQL";

}  // namespace

// ---- ctor / connection management ---------------------------------------

Db::Db(const std::string& dsn) {
    PGconn* c = PQconnectdb(dsn.c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        const std::string err = PQerrorMessage(c);
        PQfinish(c);
        throw DbError("connection failed: " + err);
    }
    conn_ = c;
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

// ---- API ---------------------------------------------------------------

RetentionConfig Db::load_retention() {
    ensure_connected_();
    RetentionConfig out;

    ResultGuard r{
        PQexec(as_conn(conn_),
            "SELECT key, value::text FROM system_config "
            "WHERE key IN ('retention.dispatched_h','retention.partial_h','retention.failed_h',"
            "              'retention.prune_dispatched_d','retention.prune_partial_d','retention.prune_failed_d')")
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"load_retention: "} + PQerrorMessage(as_conn(conn_)));
    }

    std::unordered_map<std::string, std::string> kv;
    const int n = PQntuples(r.r);
    for (int i = 0; i < n; ++i) {
        kv[cell(r.r, i, 0)] = cell(r.r, i, 1);
    }

    out.dispatched_h       = parse_int_scalar(kv["retention.dispatched_h"],       out.dispatched_h);
    out.partial_h          = parse_int_scalar(kv["retention.partial_h"],          out.partial_h);
    out.failed_h           = parse_int_scalar(kv["retention.failed_h"],           out.failed_h);
    out.prune_dispatched_d = parse_int_scalar(kv["retention.prune_dispatched_d"], out.prune_dispatched_d);
    out.prune_partial_d    = parse_int_scalar(kv["retention.prune_partial_d"],    out.prune_partial_d);
    out.prune_failed_d     = parse_int_scalar(kv["retention.prune_failed_d"],     out.prune_failed_d);

    LOG_INFO("retention.loaded",
        "dispatched_h",       std::to_string(out.dispatched_h),
        "partial_h",          std::to_string(out.partial_h),
        "failed_h",           std::to_string(out.failed_h),
        "prune_dispatched_d", std::to_string(out.prune_dispatched_d),
        "prune_partial_d",    std::to_string(out.prune_partial_d),
        "prune_failed_d",     std::to_string(out.prune_failed_d));
    return out;
}

std::vector<EligibleRow> Db::list_eligible_rows(
    const std::string& server_id,
    const RetentionConfig& ttl,
    std::uint32_t limit)
{
    ensure_connected_();

    const std::string dispatched_h_s = std::to_string(ttl.dispatched_h);
    const std::string partial_h_s    = std::to_string(ttl.partial_h);
    const std::string failed_h_s     = std::to_string(ttl.failed_h);
    const std::string limit_s        = std::to_string(limit);

    const char* params[] = {
        server_id.c_str(),
        dispatched_h_s.c_str(),
        partial_h_s.c_str(),
        failed_h_s.c_str(),
        limit_s.c_str(),
    };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kListEligibleSql,
                     5, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"list_eligible_rows: "} +
                      PQerrorMessage(as_conn(conn_)));
    }

    const int n = PQntuples(r.r);
    std::vector<EligibleRow> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        EligibleRow row;
        row.id             = std::stoll(cell(r.r, i, 0));
        row.status         = cell(r.r, i, 1);
        row.file_root_path = cell(r.r, i, 2);
        out.push_back(std::move(row));
    }
    return out;
}

void Db::mark_cleaned(std::int64_t work_queue_id) {
    ensure_connected_();
    const std::string id_s = std::to_string(work_queue_id);
    const char* params[1] = { id_s.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kMarkCleanedSql,
                     1, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"mark_cleaned: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
}

bool Db::try_acquire_prune_lock() {
    ensure_connected_();
    const std::string lock_s = std::to_string(kPruneLockId);
    const char* params[1] = { lock_s.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), "SELECT pg_try_advisory_lock($1::bigint)",
                     1, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK || PQntuples(r.r) != 1) {
        throw DbError(std::string{"try_acquire_prune_lock: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    return cell(r.r, 0, 0) == "t";
}

void Db::release_prune_lock() {
    ensure_connected_();
    const std::string lock_s = std::to_string(kPruneLockId);
    const char* params[1] = { lock_s.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), "SELECT pg_advisory_unlock($1::bigint)",
                     1, nullptr, params, nullptr, nullptr, 0)
    };
    // We don't fail on a missed unlock — advisory locks auto-release on
    // session end, so this is best-effort.
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        LOG_WARN("db.advisory_unlock_failed",
            "error", PQerrorMessage(as_conn(conn_)));
    }
}

int Db::prune_cleaned_rows(int days, std::uint32_t batch) {
    ensure_connected_();
    const std::string days_s  = std::to_string(days);
    const std::string batch_s = std::to_string(batch);
    const char* params[] = { days_s.c_str(), batch_s.c_str() };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kPruneSql,
                     2, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"prune_cleaned_rows: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    return std::atoi(PQcmdTuples(r.r));
}

}  // namespace nlr
