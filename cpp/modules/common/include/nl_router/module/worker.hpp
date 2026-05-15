// nl_router/module/worker.hpp
//
// Shared "module worker" runtime for nl-router processing modules. A
// module binary (e.g. `nl-mod-anonymize-basic`) embeds this runtime and
// supplies a single function — `process(input_path, output_path,
// config_json)` — that does the actual transformation. The runtime
// handles:
//
//   * Env-driven config (NL_ROUTER_SERVER_ID, NL_ROUTER_DATABASE_URL,
//     NL_ROUTER_MODULE_KIND, NL_ROUTER_POLL_INTERVAL_MS,
//     NL_ROUTER_BATCH_SIZE, NL_ROUTER_LEASE_SECONDS, NL_ROUTER_LOG_LEVEL).
//   * Postgres connection + claim/finish/fail SQL.
//   * The poll loop: pull `processing_jobs.kind = NL_ROUTER_MODULE_KIND
//     AND server_id = us AND status = 'pending'` rows via FOR UPDATE
//     SKIP LOCKED, mark 'processing', hand each to the user's process()
//     function, mark done/failed based on the return value.
//   * After every job: if no remaining 'pending'/'processing' jobs exist
//     for that work_queue row, advance work_queue:
//        - status='processing' → 'processed'
//        - file_root_path     ← this row's output_path  (final stage)
//     so the Dispatcher's claim filter (status IN ('routed','processed'))
//     picks up the row immediately after.
//   * SIGINT / SIGTERM → graceful exit after the current job.

#pragma once

#include <functional>
#include <string>

namespace nl_router::module {

// What the per-module process() function returns.
struct ProcessResult {
    enum class Status {
        Success,    // marks job 'done'
        Failure,    // marks job 'failed'  (increments attempts; permanent for v1)
    };

    Status      status;
    std::string error_message;   // surfaced as processing_jobs.last_error

    static ProcessResult success() { return {Status::Success, {}}; }
    static ProcessResult failure(std::string msg) {
        return {Status::Failure, std::move(msg)};
    }
};

// Signature for the user-supplied processing function.
//
//   input_path  - directory containing the .dcm files from the previous
//                 chain step (or the original landing zone for ordinal=0).
//                 Modules MUST NOT modify files under input_path.
//   output_path - directory the module creates and writes processed files
//                 to. Modules SHOULD preserve the relative subpath of each
//                 input so the dispatcher can walk the result like it
//                 walks the landing zone.
//   config_json - the merged module-default + per-rule-override config
//                 from processing_jobs.config (verbatim JSON text).
//
// Throws: callers handle std::exception and turn it into a job failure.
using ProcessFn = std::function<ProcessResult(
    const std::string& input_path,
    const std::string& output_path,
    const std::string& config_json)>;

// Run the worker until SIGINT/SIGTERM. The argv values aren't used in v1
// (everything is env-driven), but the signature mirrors main() so callers
// can hand argc/argv through unchanged.
//
// Returns the process exit code (0 on graceful shutdown).
int run_worker(int argc, char** argv, const ProcessFn& process);

}  // namespace nl_router::module
