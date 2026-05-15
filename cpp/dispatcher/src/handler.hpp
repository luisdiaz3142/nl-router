// nl-dispatch/handler.hpp
//
// DispatchHandler is the interface every destination kind implements. M3
// only ships the `dicom` kind; M7 (Dispatcher M2) adds dicomweb_stow,
// gcp_dicomweb, http, file, object_storage handlers behind this same
// interface.
//
// Worker design: one DispatchHandler instance per worker thread, one worker
// per destination. The handler is invoked once per assignment with the
// (already-claimed) assignment row and the resolved destination config.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nlr {

// What we know about an assignment when we try to dispatch it.
struct Assignment {
    std::int64_t  id;                   // route_assignments.id
    std::int64_t  work_queue_id;
    std::int64_t  rule_id;
    std::int64_t  destination_id;
    int           attempts;             // current attempt count (pre-increment)
    std::string   study_file_root;      // work_queue.file_root_path
    std::string   study_instance_uid;   // for logging / association SOPs

    // Full extracted tag set (work_queue.tags JSONB rendered as text).
    // Handlers that do `${TagName}` substitution parse this themselves.
    // Empty for assignments produced by handlers that don't read tags.
    std::string   tags_json;

    // ---- Credential (decrypted by the worker if destination.credential_id) ----
    // The worker fetches the credentials row, decrypts the envelope with
    // the loaded KEK, and attaches the plaintext payload + kind here
    // before invoking the handler. If the destination has no credential,
    // both stay empty.
    std::string   credential_kind;       // "basic_http" | "bearer_token" | ...
    std::string   credential_payload;    // decrypted plaintext JSON
};

// Cached snapshot of a destination row (parsed JSONB config + retry policy).
// The worker rebuilds this every refresh interval; handlers receive a
// const reference and may not modify it.
struct Destination {
    std::int64_t id;
    std::string  name;
    std::string  kind;            // "dicom" / "dicomweb_stow" / ... (TEXT in DB)
    bool         enabled;
    std::int16_t dispatch_concurrency;

    // ---- dicom-specific (populated from config JSONB when kind == "dicom") ----
    std::string  host;
    std::uint16_t port {0};
    std::string  called_aet;
    std::string  calling_aet;
    std::uint32_t max_pdu_size {131072};
    bool          tls          {false};
    // Preferred transfer syntaxes (UIDs). Empty = handler picks sensible defaults.
    std::vector<std::string> preferred_transfer_syntaxes;

    // ---- retry policy (parsed from JSONB) ----
    int    rp_max_attempts        {5};
    int    rp_initial_backoff_s   {30};
    double rp_multiplier          {2.0};
    int    rp_max_backoff_s       {3600};
    int    rp_give_up_after_hours {72};

    // ---- generic config (raw JSON text) ----
    // Handlers other than `dicom` (whose fields are pre-parsed above) read
    // their kind-specific config from here. Each handler is responsible
    // for parsing what it needs.
    std::string   config_json;

    // FK to the credentials table; 0 = no credential. The worker uses
    // this to fetch + decrypt the row before each dispatch; the actual
    // payload lives on Assignment (per-dispatch, not persisted).
    std::int64_t  credential_id     {0};
};

// Outcome of dispatching one assignment.
struct DispatchResult {
    enum class Status {
        Success,        // all instances acked OK; mark dispatched
        TransientFail,  // retry per policy
        PermanentFail,  // mark failed without retry (e.g. file missing,
                        //  destination misconfigured)
    };

    Status      status;
    std::string error_message;     // human-readable; surfaced in last_error
    std::string response_detail_json; // JSON object — per-instance statuses etc.

    static DispatchResult success(std::string detail = "{}") {
        return {Status::Success, {}, std::move(detail)};
    }
    static DispatchResult transient(std::string msg, std::string detail = "{}") {
        return {Status::TransientFail, std::move(msg), std::move(detail)};
    }
    static DispatchResult permanent(std::string msg, std::string detail = "{}") {
        return {Status::PermanentFail, std::move(msg), std::move(detail)};
    }
};

class DispatchHandler {
public:
    virtual ~DispatchHandler() = default;

    // Perform the dispatch. Synchronous; called from the worker thread for
    // one assignment at a time. Throws should be caught at the worker
    // boundary and converted to transient failures.
    virtual DispatchResult dispatch(const Assignment& a,
                                     const Destination& d) = 0;
};

// Factory: returns the handler for a given destination kind. Returns nullptr
// for kinds nl-dispatch v1 doesn't support — the worker logs and skips them
// (M7 adds support).
std::unique_ptr<DispatchHandler> make_handler(const std::string& kind);

}  // namespace nlr
