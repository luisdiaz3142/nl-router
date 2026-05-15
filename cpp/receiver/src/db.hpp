// nl-receiver/db.hpp
//
// Postgres client for the receiver. We insert exactly one statement
// (a single work_queue row at association-end) so the surface is tiny.
//
// libpq is C; we wrap PGconn / PGresult in RAII guards.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "tag_extractor.hpp"

namespace nlr {

// Per-study state collected during an association. The Association handler
// builds one of these per unique study UID seen, then hands it to db::insert
// at association end.
struct StudyRow {
    std::string server_id;                       // from config
    std::string calling_aet;                     // from association params
    std::string called_aet;                      // from association params
    std::string peer_ip;                         // ::1 / 10.0.0.5 / etc.

    // Tags taken from the *first* instance received for this study; in v1
    // these are stable per-study so first-wins is fine. (In subsequent
    // milestones we may want to reconcile across instances.)
    ExtractedTags tags;

    // Counters accumulated across all instances.
    std::int64_t instance_count {0};
    std::int64_t byte_count     {0};

    std::string received_at_iso;                 // ISO-8601 UTC
    std::string closed_at_iso;                   // ISO-8601 UTC

    std::string file_root_path;                  // study directory absolute path
};

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thin libpq wrapper. Constructed once per process; `insert_work_queue_row`
// is safe to call from the association thread. Auto-reconnects on transient
// failures (one retry before bubbling up).
class Db {
public:
    explicit Db(const std::string& dsn);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // INSERT one work_queue row. Returns the new row's id on success.
    // Throws DbError on permanent failure (e.g. constraint violation,
    // missing column).
    std::int64_t insert_work_queue_row(const StudyRow& row);

private:
    void connect_();
    void ensure_connected_();

    std::string dsn_;
    void*       conn_ {nullptr};   // PGconn* — kept as void* to keep libpq
                                    // out of this header's includes
};

}  // namespace nlr
