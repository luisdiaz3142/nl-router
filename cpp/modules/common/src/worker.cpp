// Shared module-worker runtime. See worker.hpp.

#include "nl_router/module/worker.hpp"

#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace nl_router::module {

namespace {

// ---- minimal env helpers ----
std::string env_or(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    return fallback;
}
std::string env_required(const char* name, const char* alt = nullptr) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    if (alt) {
        if (const char* v = std::getenv(alt); v && *v) return std::string{v};
    }
    throw std::runtime_error(
        std::string{"required env var not set: "} + name +
        (alt ? std::string{" (or "} + alt + ")" : ""));
}
template <typename T>
T env_int(const char* name, T fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return static_cast<T>(std::atoll(v));
}

// ---- minimal logger (same shape as the daemons but inlined) ----
std::mutex& sink_mutex() { static std::mutex m; return m; }

std::string iso_now() {
    using clk = std::chrono::system_clock;
    const auto now = clk::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) - secs;
    const std::time_t t = clk::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

void log_emit(const std::string& level, const std::string& msg,
               std::initializer_list<std::pair<const char*, std::string>> kv) {
    std::string line = "{\"ts\":\"" + iso_now() + "\",\"lvl\":\"" + level +
                       "\",\"msg\":\"" + msg + "\"";
    for (const auto& [k, v] : kv) {
        line += ",\"";
        line += k;
        line += "\":\"";
        for (char c : v) {
            if (c == '"' || c == '\\') line += '\\';
            line += c;
        }
        line += "\"";
    }
    line += "}\n";
    std::lock_guard<std::mutex> lock(sink_mutex());
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

#define LOG_INFO(msg, ...)  log_emit("info",  msg, {__VA_ARGS__})
#define LOG_WARN(msg, ...)  log_emit("warn",  msg, {__VA_ARGS__})
#define LOG_ERROR(msg, ...) log_emit("error", msg, {__VA_ARGS__})

// ---- libpq RAII ----
struct ResultGuard {
    PGresult* r{nullptr};
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

// ---- worker config ----
struct Config {
    std::string  server_id;
    std::string  database_url;
    std::string  module_kind;
    std::uint32_t poll_interval_ms {500};
    std::uint32_t batch_size       {4};
    std::uint32_t lease_seconds    {300};
};

Config load_config() {
    Config c;
    c.server_id    = env_required("NL_ROUTER_SERVER_ID");
    c.database_url = env_required("NL_ROUTER_DATABASE_URL", "DATABASE_URL");
    c.module_kind  = env_required("NL_ROUTER_MODULE_KIND");
    c.poll_interval_ms = env_int<std::uint32_t>("NL_ROUTER_POLL_INTERVAL_MS", c.poll_interval_ms);
    c.batch_size       = env_int<std::uint32_t>("NL_ROUTER_BATCH_SIZE",      c.batch_size);
    c.lease_seconds    = env_int<std::uint32_t>("NL_ROUTER_LEASE_SECONDS",   c.lease_seconds);
    return c;
}

// ---- SQL ----

// Claim one batch of pending jobs of our kind on this server, atomically
// transitioning them to status='processing' + setting claim columns.
// Returns id, work_queue_id, input_path, output_path, config.
constexpr const char* kClaimSql = R"SQL(
WITH picked AS (
    SELECT id FROM processing_jobs
     WHERE module_kind = $1
       AND server_id   = $2
       AND status      = 'pending'
     ORDER BY created_at
     FOR UPDATE SKIP LOCKED
     LIMIT $3
)
UPDATE processing_jobs pj
   SET status           = 'processing',
       claimed_by       = $4,
       claimed_at       = NOW(),
       claim_expires_at = NOW() + make_interval(secs => $5),
       started_at       = NOW()
  FROM picked
 WHERE pj.id = picked.id
RETURNING pj.id, pj.work_queue_id, pj.input_path, pj.output_path,
          pj.config::text AS config_text
)SQL";

// Mark a job done. After the UPDATE, check whether any non-terminal
// (pending/processing) jobs remain for this work_queue row. If not,
// advance the work_queue row to 'processed' and repoint file_root_path
// at the LAST (highest-ordinal) successful job's output_path so the
// Dispatcher reads from the final processed tree.
//
// We pass $2 = the output_path of the job we just finished so the
// "last job" rollup query can compare ordinals deterministically.
//
// Splitting the post-update rollup into a separate statement matches the
// pattern in the dispatcher (CTE-with-UPDATE visibility rules don't
// see the just-updated row).
constexpr const char* kMarkDoneSql = R"SQL(
UPDATE processing_jobs
   SET status           = 'done',
       output_path      = $2,
       completed_at     = NOW(),
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL
 WHERE id = $1
RETURNING work_queue_id
)SQL";

constexpr const char* kMarkFailedSql = R"SQL(
UPDATE processing_jobs
   SET status           = 'failed',
       last_error       = $2,
       attempts         = attempts + 1,
       completed_at     = NOW(),
       claimed_by       = NULL,
       claimed_at       = NULL,
       claim_expires_at = NULL
 WHERE id = $1
RETURNING work_queue_id
)SQL";

// Rollup: if no pending/processing jobs remain for this work_queue, the
// row's processing phase is complete. Decide its terminal status:
//   * any 'failed' job → status='failed' (set failed_phase='processor')
//   * all 'done'        → status='processed', and file_root_path is set
//                          to the output_path of the highest-ordinal done job
//
// Args: $1 work_queue_id
constexpr const char* kRollupSql = R"SQL(
WITH stats AS (
    SELECT
      sum(CASE WHEN status IN ('pending','processing') THEN 1 ELSE 0 END) AS open_n,
      sum(CASE WHEN status = 'failed' THEN 1 ELSE 0 END)                  AS failed_n,
      (SELECT output_path FROM processing_jobs
        WHERE work_queue_id = $1 AND status = 'done'
        ORDER BY ordinal DESC LIMIT 1)                                    AS final_output
      FROM processing_jobs
     WHERE work_queue_id = $1
)
UPDATE work_queue w
   SET status         = CASE WHEN stats.failed_n > 0 THEN 'failed'::work_status
                              ELSE 'processed'::work_status END,
       processed_at   = NOW(),
       file_root_path = COALESCE(stats.final_output, w.file_root_path),
       failed_phase   = CASE WHEN stats.failed_n > 0 THEN 'processor'
                              ELSE NULL END,
       last_error     = CASE WHEN stats.failed_n > 0
                              THEN (SELECT last_error FROM processing_jobs
                                      WHERE work_queue_id = $1 AND status='failed'
                                      ORDER BY ordinal LIMIT 1)
                              ELSE NULL END
  FROM stats
 WHERE w.id = $1
   AND stats.open_n = 0
)SQL";

}  // namespace

int run_worker(int /*argc*/, char** /*argv*/, const ProcessFn& process) {
    Config cfg;
    try {
        cfg = load_config();
    } catch (const std::exception& e) {
        std::cerr << "{\"lvl\":\"error\",\"msg\":\"config\",\"error\":\"" << e.what() << "\"}\n";
        return 1;
    }

    // Signal handling.
    static std::atomic<bool> stop_requested{false};
    std::signal(SIGINT,  [](int){ stop_requested.store(true); });
    std::signal(SIGTERM, [](int){ stop_requested.store(true); });
    std::signal(SIGPIPE, SIG_IGN);

    // DB connection.
    PGconn* conn = PQconnectdb(cfg.database_url.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn);
        PQfinish(conn);
        LOG_ERROR("db.connect_failed", {"error", err});
        return 1;
    }
    LOG_INFO("module.start",
        {"server_id",   cfg.server_id},
        {"module_kind", cfg.module_kind},
        {"poll_ms",     std::to_string(cfg.poll_interval_ms)},
        {"batch",       std::to_string(cfg.batch_size)},
        {"lease_s",     std::to_string(cfg.lease_seconds)});

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname) - 1);
    const std::string worker_id =
        std::string{"nl-mod-"} + cfg.module_kind + "@" + hostname + ":" +
        std::to_string(getpid());

    const std::string batch_s = std::to_string(cfg.batch_size);
    const std::string lease_s = std::to_string(cfg.lease_seconds);

    while (!stop_requested.load(std::memory_order_relaxed)) {
        // Claim batch.
        const char* claim_params[] = {
            cfg.module_kind.c_str(),
            cfg.server_id.c_str(),
            batch_s.c_str(),
            worker_id.c_str(),
            lease_s.c_str()
        };
        ResultGuard r{
            PQexecParams(conn, kClaimSql, 5, nullptr, claim_params,
                         nullptr, nullptr, 0)
        };
        if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
            LOG_ERROR("module.claim_failed",
                {"error", PQerrorMessage(conn)});
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg.poll_interval_ms));
            continue;
        }
        const int n = PQntuples(r.r);
        if (n == 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg.poll_interval_ms));
            continue;
        }

        for (int i = 0; i < n; ++i) {
            if (stop_requested.load(std::memory_order_relaxed)) break;

            const std::int64_t job_id        = std::stoll(cell(r.r, i, 0));
            const std::int64_t work_queue_id = std::stoll(cell(r.r, i, 1));
            const std::string  input_path    = cell(r.r, i, 2);
            const std::string  output_path   = cell(r.r, i, 3);
            const std::string  config_json   = cell(r.r, i, 4);

            // Invoke the user-supplied processing function.
            ProcessResult result = ProcessResult::failure("uninitialized");
            try {
                result = process(input_path, output_path, config_json);
            } catch (const std::exception& e) {
                result = ProcessResult::failure(std::string{"uncaught: "} + e.what());
            } catch (...) {
                result = ProcessResult::failure("uncaught: unknown");
            }

            const std::string id_s = std::to_string(job_id);
            try {
                // Wrap UPDATE + rollup in one transaction so the rollup
                // sees the just-updated row.
                ResultGuard begin{PQexec(conn, "BEGIN")};
                (void)begin;

                if (result.status == ProcessResult::Status::Success) {
                    const char* params[] = { id_s.c_str(), output_path.c_str() };
                    ResultGuard upd{
                        PQexecParams(conn, kMarkDoneSql, 2, nullptr,
                                     params, nullptr, nullptr, 0)
                    };
                    if (PQresultStatus(upd.r) != PGRES_TUPLES_OK) {
                        throw std::runtime_error(
                            std::string{"mark_done: "} + PQerrorMessage(conn));
                    }
                    LOG_INFO("module.job_done",
                        {"job_id",        id_s},
                        {"work_queue_id", std::to_string(work_queue_id)},
                        {"output_path",   output_path});
                } else {
                    const char* params[] = { id_s.c_str(), result.error_message.c_str() };
                    ResultGuard upd{
                        PQexecParams(conn, kMarkFailedSql, 2, nullptr,
                                     params, nullptr, nullptr, 0)
                    };
                    if (PQresultStatus(upd.r) != PGRES_TUPLES_OK) {
                        throw std::runtime_error(
                            std::string{"mark_failed: "} + PQerrorMessage(conn));
                    }
                    LOG_WARN("module.job_failed",
                        {"job_id",        id_s},
                        {"work_queue_id", std::to_string(work_queue_id)},
                        {"error",         result.error_message});
                }

                // Rollup if this was the last open job for the row.
                const std::string wq_s = std::to_string(work_queue_id);
                const char* roll_params[] = { wq_s.c_str() };
                ResultGuard roll{
                    PQexecParams(conn, kRollupSql, 1, nullptr,
                                 roll_params, nullptr, nullptr, 0)
                };
                if (PQresultStatus(roll.r) != PGRES_COMMAND_OK) {
                    throw std::runtime_error(
                        std::string{"rollup: "} + PQerrorMessage(conn));
                }

                ResultGuard commit{PQexec(conn, "COMMIT")};
                if (PQresultStatus(commit.r) != PGRES_COMMAND_OK) {
                    throw std::runtime_error(
                        std::string{"commit: "} + PQerrorMessage(conn));
                }
            } catch (const std::exception& e) {
                ResultGuard rb{PQexec(conn, "ROLLBACK")};
                LOG_ERROR("module.status_update_failed",
                    {"job_id", id_s},
                    {"error",  e.what()});
            }
        }
    }

    LOG_INFO("module.stop", {"module_kind", cfg.module_kind});
    PQfinish(conn);
    return 0;
}

}  // namespace nl_router::module
