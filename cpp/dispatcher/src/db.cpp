#include "db.hpp"

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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

std::string cell(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return {};
    return std::string{PQgetvalue(r, row, col),
                       static_cast<std::size_t>(PQgetlength(r, row, col))};
}

// ---- destinations ------------------------------------------------------

constexpr const char* kListDestinationsSql = R"SQL(
SELECT id, name, kind, enabled, dispatch_concurrency,
       config::text       AS config_text,
       retry_policy::text AS retry_text,
       credential_id
  FROM destinations
 WHERE enabled = TRUE
 ORDER BY id
)SQL";

void apply_dicom_config(Destination& d, const std::string& config_text) {
    try {
        auto j = nlohmann::json::parse(config_text);
        if (j.contains("host")  && j["host"].is_string())  d.host       = j["host"].get<std::string>();
        if (j.contains("port")  && j["port"].is_number())  d.port       = j["port"].get<std::uint16_t>();
        if (j.contains("called_aet")  && j["called_aet"].is_string())  d.called_aet  = j["called_aet"].get<std::string>();
        if (j.contains("calling_aet") && j["calling_aet"].is_string()) d.calling_aet = j["calling_aet"].get<std::string>();
        if (j.contains("max_pdu_size") && j["max_pdu_size"].is_number())
            d.max_pdu_size = j["max_pdu_size"].get<std::uint32_t>();
        if (j.contains("tls") && j["tls"].is_boolean()) d.tls = j["tls"].get<bool>();
        if (j.contains("preferred_transfer_syntaxes") && j["preferred_transfer_syntaxes"].is_array()) {
            for (auto& ts : j["preferred_transfer_syntaxes"]) {
                if (ts.is_string()) d.preferred_transfer_syntaxes.push_back(ts.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("db.destination.config_parse_failed",
            "destination_id", std::to_string(d.id),
            "error",          e.what());
    }
}

void apply_retry_policy(Destination& d, const std::string& retry_text) {
    try {
        auto j = nlohmann::json::parse(retry_text);
        if (j.contains("max_attempts"))         d.rp_max_attempts        = j["max_attempts"].get<int>();
        if (j.contains("initial_backoff_s"))    d.rp_initial_backoff_s   = j["initial_backoff_s"].get<int>();
        if (j.contains("multiplier"))           d.rp_multiplier          = j["multiplier"].get<double>();
        if (j.contains("max_backoff_s"))        d.rp_max_backoff_s       = j["max_backoff_s"].get<int>();
        if (j.contains("give_up_after_hours"))  d.rp_give_up_after_hours = j["give_up_after_hours"].get<int>();
    } catch (const std::exception& e) {
        LOG_WARN("db.destination.retry_parse_failed",
            "destination_id", std::to_string(d.id),
            "error",          e.what());
    }
}

// ---- claim assignments -------------------------------------------------
//
// `dispatch_kind` is denormalized on route_assignments so we filter purely
// on that column (no join needed). Status eligibility includes 'pending'
// and 'failed' where next_retry_at has hit.
//
// Args: $1 destination_id, $2 server_id, $3 batch, $4 worker_id, $5 lease_s
constexpr const char* kClaimAssignmentsSql = R"SQL(
WITH picked AS (
    SELECT ra.id
      FROM route_assignments ra
     WHERE ra.destination_id = $1
       AND ra.server_id      = $2
       AND (ra.status = 'pending'
            OR (ra.status = 'failed' AND ra.next_retry_at IS NOT NULL
                                     AND ra.next_retry_at <= NOW()))
     ORDER BY ra.next_retry_at NULLS FIRST, ra.created_at
     FOR UPDATE SKIP LOCKED
     LIMIT $3
)
UPDATE route_assignments ra
   SET status           = 'dispatching',
       claimed_by       = $4,
       claimed_at       = NOW(),
       claim_expires_at = NOW() + make_interval(secs => $5)
  FROM picked
 WHERE ra.id = picked.id
RETURNING ra.id, ra.work_queue_id, ra.rule_id, ra.destination_id, ra.attempts,
          (SELECT w.file_root_path     FROM work_queue w WHERE w.id = ra.work_queue_id),
          (SELECT w.study_instance_uid FROM work_queue w WHERE w.id = ra.work_queue_id),
          (SELECT w.tags::text         FROM work_queue w WHERE w.id = ra.work_queue_id)
)SQL";

// ---- terminal transitions ---------------------------------------------

// On success: mark the assignment dispatched. The work_queue rollup runs
// as a second statement in the same transaction (kRollupWorkQueueSql) so
// the rollup query sees the just-updated assignment row — Postgres CTEs
// don't make WITH-clause modifications visible to sibling sub-queries.
constexpr const char* kMarkDispatchedSql = R"SQL(
UPDATE route_assignments
   SET status           = 'dispatched',
       dispatched_at    = NOW(),
       response_detail  = $2::jsonb,
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL,
       next_retry_at    = NULL
 WHERE id = $1
RETURNING work_queue_id
)SQL";

// Rollup the parent work_queue row's status based on its assignments'
// current states. Caller passes the work_queue_id ($1).
//
//   * Any non-terminal sibling exists → don't change work_queue.status
//   * All siblings 'dispatched'       → work_queue.status = 'dispatched'
//   * All siblings 'failed' (perm)    → work_queue.status = 'failed'
//   * Mixed                            → 'dispatched_partial'
//
// "Permanently failed" means status='failed' AND next_retry_at IS NULL —
// the dispatcher clears next_retry_at when it gives up; pending retries
// (next_retry_at set) keep the parent row in 'dispatching'.
constexpr const char* kRollupWorkQueueSql = R"SQL(
WITH counts AS (
    SELECT
      sum(CASE WHEN status='dispatched' THEN 1 ELSE 0 END) AS dispatched_n,
      sum(CASE WHEN status='failed' AND next_retry_at IS NULL THEN 1 ELSE 0 END) AS failed_perm_n,
      sum(CASE WHEN status NOT IN ('dispatched','failed')
                 OR (status='failed' AND next_retry_at IS NOT NULL)
               THEN 1 ELSE 0 END) AS non_terminal_n
      FROM route_assignments
     WHERE work_queue_id = $1::bigint
)
UPDATE work_queue w
   SET status        = CASE
                         WHEN c.failed_perm_n = 0 THEN 'dispatched'::work_status
                         WHEN c.dispatched_n = 0  THEN 'failed'::work_status
                         ELSE 'dispatched_partial'::work_status
                       END,
       dispatched_at = NOW()
  FROM counts c
 WHERE w.id = $1::bigint AND c.non_terminal_n = 0
)SQL";

// On transient failure: mark the assignment 'failed' but set next_retry_at
// so the worker re-claims it later. Do NOT roll up work_queue.status yet
// (more attempts to come).
constexpr const char* kMarkFailedRetrySql = R"SQL(
UPDATE route_assignments
   SET status           = 'failed',
       attempts         = $2::int,
       last_error       = $3,
       response_detail  = $4::jsonb,
       next_retry_at    = $5::timestamptz,
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL
 WHERE id = $1
)SQL";

// Permanent failure: clear next_retry_at; the rollup runs as a second
// statement (kRollupWorkQueueSql) so it sees the updated assignment row.
constexpr const char* kMarkFailedPermanentSql = R"SQL(
UPDATE route_assignments
   SET status           = 'failed',
       attempts         = COALESCE(attempts, 0) + 1,
       last_error       = $2,
       response_detail  = $3::jsonb,
       next_retry_at    = NULL,
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL
 WHERE id = $1
RETURNING work_queue_id
)SQL";

// ---- backoff math ------------------------------------------------------

// Compute the next-retry interval in seconds given the retry policy and
// the new attempt count (1-based: first failure has attempts=1).
//
// backoff = min(initial * multiplier^(attempts-1), max_backoff)
int next_retry_seconds(const Destination& d, int attempts) {
    const double mult = std::pow(d.rp_multiplier, std::max(0, attempts - 1));
    double secs = static_cast<double>(d.rp_initial_backoff_s) * mult;
    if (secs > static_cast<double>(d.rp_max_backoff_s)) {
        secs = static_cast<double>(d.rp_max_backoff_s);
    }
    if (secs < 1.0) secs = 1.0;
    return static_cast<int>(secs);
}

}  // namespace

// ---- ctors --------------------------------------------------------------

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

std::vector<Destination> Db::list_enabled_destinations() {
    ensure_connected_();

    ResultGuard r{PQexec(as_conn(conn_), kListDestinationsSql)};
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"list_enabled_destinations: "} +
                      PQerrorMessage(as_conn(conn_)));
    }

    const int n = PQntuples(r.r);
    std::vector<Destination> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Destination d;
        d.id   = std::stoll(cell(r.r, i, 0));
        d.name = cell(r.r, i, 1);
        d.kind = cell(r.r, i, 2);
        d.enabled = cell(r.r, i, 3) == "t";
        d.dispatch_concurrency = static_cast<std::int16_t>(std::stoi(cell(r.r, i, 4)));
        const auto config_text = cell(r.r, i, 5);
        const auto retry_text  = cell(r.r, i, 6);
        d.config_json = config_text;
        const auto cred_text = cell(r.r, i, 7);
        if (!cred_text.empty()) {
            try { d.credential_id = std::stoll(cred_text); }
            catch (...) { d.credential_id = 0; }
        }
        if (d.kind == "dicom") apply_dicom_config(d, config_text);
        apply_retry_policy(d, retry_text);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Assignment> Db::claim_pending_for_destination(
    std::int64_t destination_id,
    const std::string& server_id,
    const std::string& worker_id,
    std::uint32_t lease_seconds,
    std::uint32_t batch)
{
    ensure_connected_();

    const std::string did_s   = std::to_string(destination_id);
    const std::string batch_s = std::to_string(batch);
    const std::string lease_s = std::to_string(lease_seconds);
    const char* params[]      = { did_s.c_str(), server_id.c_str(), batch_s.c_str(),
                                  worker_id.c_str(), lease_s.c_str() };

    ResultGuard r{
        PQexecParams(as_conn(conn_), kClaimAssignmentsSql,
                     5, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"claim_pending_for_destination: "} +
                      PQerrorMessage(as_conn(conn_)));
    }

    const int n = PQntuples(r.r);
    std::vector<Assignment> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Assignment a;
        a.id                  = std::stoll(cell(r.r, i, 0));
        a.work_queue_id       = std::stoll(cell(r.r, i, 1));
        a.rule_id             = std::stoll(cell(r.r, i, 2));
        a.destination_id      = std::stoll(cell(r.r, i, 3));
        a.attempts            = std::stoi(cell(r.r, i, 4));
        a.study_file_root     = cell(r.r, i, 5);
        a.study_instance_uid  = cell(r.r, i, 6);
        a.tags_json           = cell(r.r, i, 7);
        out.push_back(std::move(a));
    }
    return out;
}

std::optional<Db::CredentialEnvelope>
Db::fetch_credential_envelope(std::int64_t credential_id) {
    ensure_connected_();

    const std::string id_s = std::to_string(credential_id);
    const char* params[1] = { id_s.c_str() };
    // libpq's text protocol returns BYTEA as the standard "\\x<hex>" form,
    // which we'd have to decode. Easier: ask Postgres to encode the bytea
    // columns as base64 text strings inline.
    ResultGuard r{
        PQexecParams(as_conn(conn_),
            "SELECT id, kind, enc_version,"
            "       encode(nonce, 'base64')      AS nonce_b64,"
            "       encode(ciphertext, 'base64') AS ct_b64"
            "  FROM credentials WHERE id = $1",
            1, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        throw DbError(std::string{"fetch_credential_envelope: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    if (PQntuples(r.r) == 0) return std::nullopt;

    auto b64_decode = [](const std::string& s) -> std::vector<std::uint8_t> {
        // Strip whitespace; reuse OpenSSL's BIO base64 decoder for
        // standard '+', '/', '=' base64 (Postgres encode() emits classical
        // base64, not urlsafe).
        std::string compact;
        compact.reserve(s.size());
        for (char c : s) if (c != '\n' && c != '\r' && c != ' ') compact.push_back(c);
        // Pad to multiple of 4.
        while (compact.size() % 4 != 0) compact.push_back('=');

        // Tiny base64 decoder (classical alphabet).
        static int table[256];
        static bool init = false;
        if (!init) {
            for (int i = 0; i < 256; ++i) table[i] = -1;
            for (int i = 0; i < 26; ++i) {
                table[static_cast<unsigned>('A' + i)] = i;
                table[static_cast<unsigned>('a' + i)] = i + 26;
            }
            for (int i = 0; i < 10; ++i) table[static_cast<unsigned>('0' + i)] = i + 52;
            table[static_cast<unsigned>('+')] = 62;
            table[static_cast<unsigned>('/')] = 63;
            init = true;
        }

        std::vector<std::uint8_t> out;
        out.reserve((compact.size() / 4) * 3);
        for (std::size_t i = 0; i < compact.size(); i += 4) {
            const char a = compact[i], b = compact[i+1], c = compact[i+2], d = compact[i+3];
            const int va = table[static_cast<unsigned char>(a)];
            const int vb = table[static_cast<unsigned char>(b)];
            const int vc = (c == '=') ? 0 : table[static_cast<unsigned char>(c)];
            const int vd = (d == '=') ? 0 : table[static_cast<unsigned char>(d)];
            if (va < 0 || vb < 0 || (c != '=' && vc < 0) || (d != '=' && vd < 0)) {
                return {};        // malformed; let caller treat as "no creds"
            }
            const std::uint32_t triple =
                (static_cast<std::uint32_t>(va) << 18) |
                (static_cast<std::uint32_t>(vb) << 12) |
                (static_cast<std::uint32_t>(vc) <<  6) |
                 static_cast<std::uint32_t>(vd);
            out.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFFu));
            if (c != '=') out.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFFu));
            if (d != '=') out.push_back(static_cast<std::uint8_t>( triple        & 0xFFu));
        }
        return out;
    };

    CredentialEnvelope env;
    env.id          = std::stoll(cell(r.r, 0, 0));
    env.kind        = cell(r.r, 0, 1);
    env.enc_version = static_cast<std::int16_t>(std::stoi(cell(r.r, 0, 2)));
    env.nonce       = b64_decode(cell(r.r, 0, 3));
    env.ciphertext  = b64_decode(cell(r.r, 0, 4));
    return env;
}

// Helper: run kRollupWorkQueueSql for a known work_queue_id. Used by both
// the success path and the permanent-failure path.
static void rollup_work_queue(PGconn* conn, const std::string& work_queue_id) {
    const char* params[1] = { work_queue_id.c_str() };
    ResultGuard r{
        PQexecParams(conn, kRollupWorkQueueSql,
                     1, nullptr, params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"rollup_work_queue: "} + PQerrorMessage(conn));
    }
}

void Db::mark_dispatched(std::int64_t assignment_id,
                          const std::string& response_detail_json) {
    ensure_connected_();
    auto* c = as_conn(conn_);

    // Transactional pair: UPDATE the assignment, then UPDATE the parent
    // work_queue row using its now-current sibling state.
    {
        ResultGuard begin{PQexec(c, "BEGIN")};
        if (PQresultStatus(begin.r) != PGRES_COMMAND_OK) {
            throw DbError(std::string{"BEGIN: "} + PQerrorMessage(c));
        }
    }

    try {
        const std::string id_s = std::to_string(assignment_id);
        const char* params[]   = { id_s.c_str(), response_detail_json.c_str() };
        ResultGuard upd{
            PQexecParams(c, kMarkDispatchedSql,
                         2, nullptr, params, nullptr, nullptr, 0)
        };
        if (PQresultStatus(upd.r) != PGRES_TUPLES_OK) {
            throw DbError(std::string{"mark_dispatched: "} + PQerrorMessage(c));
        }
        if (PQntuples(upd.r) == 0) {
            // Lease expired or row gone — silently swallow; the worker will
            // re-claim if it's still pending.
            ResultGuard rb{PQexec(c, "ROLLBACK")};
            return;
        }
        const std::string wq_id = cell(upd.r, 0, 0);
        rollup_work_queue(c, wq_id);

        ResultGuard commit{PQexec(c, "COMMIT")};
        if (PQresultStatus(commit.r) != PGRES_COMMAND_OK) {
            throw DbError(std::string{"COMMIT: "} + PQerrorMessage(c));
        }
    } catch (...) {
        ResultGuard rb{PQexec(c, "ROLLBACK")};
        throw;
    }
}

bool Db::mark_failed_with_retry(std::int64_t assignment_id,
                                 const std::string& error,
                                 const std::string& response_detail_json,
                                 const Destination& dst) {
    ensure_connected_();

    // Fetch the current attempts + created_at to decide retry vs permanent.
    // (Could be done in a single SQL statement; keeping it as two for
    // readability.)
    const std::string id_s = std::to_string(assignment_id);
    const char* meta_params[1] = { id_s.c_str() };
    ResultGuard meta{
        PQexecParams(as_conn(conn_),
            "SELECT attempts, created_at FROM route_assignments WHERE id = $1",
            1, nullptr, meta_params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(meta.r) != PGRES_TUPLES_OK || PQntuples(meta.r) != 1) {
        throw DbError(std::string{"mark_failed: fetch meta: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    const int prev_attempts = std::stoi(cell(meta.r, 0, 0));
    const std::string created_at_text = cell(meta.r, 0, 1);
    const int new_attempts = prev_attempts + 1;

    // Have we hit max_attempts? Has the give-up window elapsed?
    bool give_up = (new_attempts >= dst.rp_max_attempts);
    if (!give_up) {
        // Cheap server-side check for elapsed time. Could parse Postgres
        // timestamps client-side, but the DB does it cleaner.
        const std::string hours_s = std::to_string(dst.rp_give_up_after_hours);
        const char* age_params[2] = { created_at_text.c_str(), hours_s.c_str() };
        ResultGuard age{
            PQexecParams(as_conn(conn_),
                "SELECT EXTRACT(EPOCH FROM (NOW() - $1::timestamptz)) / 3600 > $2::int",
                2, nullptr, age_params, nullptr, nullptr, 0)
        };
        if (PQresultStatus(age.r) == PGRES_TUPLES_OK && PQntuples(age.r) == 1) {
            give_up = (cell(age.r, 0, 0) == "t");
        }
    }

    if (give_up) {
        const char* perm_params[] = {
            id_s.c_str(), error.c_str(), response_detail_json.c_str()
        };
        ResultGuard r{
            PQexecParams(as_conn(conn_), kMarkFailedPermanentSql,
                         3, nullptr, perm_params, nullptr, nullptr, 0)
        };
        if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
            throw DbError(std::string{"mark_failed_permanent: "} +
                          PQerrorMessage(as_conn(conn_)));
        }
        return true;
    }

    const int backoff_s = next_retry_seconds(dst, new_attempts);
    // We can't pass an "interval expression" as a bound parameter. Compute
    // the absolute timestamp client-side via the server (one round trip).
    const std::string backoff_str = std::to_string(backoff_s);
    const char* ts_params[1] = { backoff_str.c_str() };
    ResultGuard ts{
        PQexecParams(as_conn(conn_),
            "SELECT (NOW() + make_interval(secs => $1::int))::text",
            1, nullptr, ts_params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(ts.r) != PGRES_TUPLES_OK || PQntuples(ts.r) != 1) {
        throw DbError(std::string{"mark_failed: compute next_retry: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    const std::string next_retry_at_text = cell(ts.r, 0, 0);
    const std::string attempts_s         = std::to_string(new_attempts);

    const char* retry_params[] = {
        id_s.c_str(), attempts_s.c_str(), error.c_str(),
        response_detail_json.c_str(), next_retry_at_text.c_str()
    };
    ResultGuard r{
        PQexecParams(as_conn(conn_), kMarkFailedRetrySql,
                     5, nullptr, retry_params, nullptr, nullptr, 0)
    };
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        throw DbError(std::string{"mark_failed_retry: "} +
                      PQerrorMessage(as_conn(conn_)));
    }
    return false;
}

void Db::mark_failed_permanent(std::int64_t assignment_id,
                                const std::string& error,
                                const std::string& response_detail_json) {
    ensure_connected_();
    auto* c = as_conn(conn_);

    {
        ResultGuard begin{PQexec(c, "BEGIN")};
        if (PQresultStatus(begin.r) != PGRES_COMMAND_OK) {
            throw DbError(std::string{"BEGIN: "} + PQerrorMessage(c));
        }
    }
    try {
        const std::string id_s = std::to_string(assignment_id);
        const char* params[]   = { id_s.c_str(), error.c_str(), response_detail_json.c_str() };
        ResultGuard upd{
            PQexecParams(c, kMarkFailedPermanentSql,
                         3, nullptr, params, nullptr, nullptr, 0)
        };
        if (PQresultStatus(upd.r) != PGRES_TUPLES_OK) {
            throw DbError(std::string{"mark_failed_permanent: "} + PQerrorMessage(c));
        }
        if (PQntuples(upd.r) == 0) {
            ResultGuard rb{PQexec(c, "ROLLBACK")};
            return;
        }
        const std::string wq_id = cell(upd.r, 0, 0);
        rollup_work_queue(c, wq_id);

        ResultGuard commit{PQexec(c, "COMMIT")};
        if (PQresultStatus(commit.r) != PGRES_COMMAND_OK) {
            throw DbError(std::string{"COMMIT: "} + PQerrorMessage(c));
        }
    } catch (...) {
        ResultGuard rb{PQexec(c, "ROLLBACK")};
        throw;
    }
}

}  // namespace nlr
