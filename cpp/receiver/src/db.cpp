#include "db.hpp"

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "logging.hpp"
#include "metrics.hpp"

namespace nlr {

namespace {

// Cast for our void* PGconn handle.
PGconn* as_conn(void* p) { return static_cast<PGconn*>(p); }

// PGresult RAII guard.
struct ResultGuard {
    PGresult* r {nullptr};
    explicit ResultGuard(PGresult* p) : r(p) {}
    ~ResultGuard() { if (r) PQclear(r); }
    ResultGuard(const ResultGuard&) = delete;
    ResultGuard& operator=(const ResultGuard&) = delete;
};

// The work_queue INSERT. Column order MUST match the params array below;
// we use $1..$N positional placeholders to keep the binding code simple and
// avoid sprintf'd SQL.
//
// The schema sets defaults for retry_count, status (via NOT NULL but we
// pass it explicitly), priority, cleanup_hold — those still need explicit
// values for an INSERT that doesn't omit them.
//
// Returns the new row's id via RETURNING.
constexpr const char* kInsertSql = R"SQL(
INSERT INTO work_queue (
    server_id,
    study_instance_uid,
    series_instance_uid,
    accession_number,
    study_id,
    sop_instance_uid,
    patient_id,
    patient_name,
    patient_birth_date,
    patient_sex,
    calling_aet,
    called_aet,
    peer_ip,
    modality,
    station_name,
    station_aet,
    retrieve_aet,
    institution_name,
    manufacturer,
    manufacturer_model_name,
    device_serial_number,
    device_uid,
    software_versions,
    study_description,
    series_description,
    protocol_name,
    body_part_examined,
    referring_physician_name,
    study_date,
    study_time,
    series_date,
    series_time,
    acquisition_date,
    acquisition_time,
    series_number,
    number_of_series,
    instance_number,
    acquisition_number,
    acquisition_type,
    scanning_sequence,
    sequence_name,
    sequence_variant,
    image_type,
    image_comments,
    contrast_bolus_agent,
    slice_thickness,
    magnetic_field_strength,
    code_value,
    code_meaning,
    specific_character_set,
    instance_count,
    byte_count,
    received_at,
    closed_at,
    close_trigger,
    file_root_path,
    status,
    tags
) VALUES (
    $1, $2, $3, $4, $5, $6, $7, $8, _date_or_null($9::text), $10,
    $11, $12, $13::inet, $14, $15, $16, $17, $18, $19, $20,
    $21, $22, $23, $24, $25, $26, $27, $28,
    _date_or_null($29::text), _time_or_null($30::text),
    _date_or_null($31::text), _time_or_null($32::text),
    _date_or_null($33::text), _time_or_null($34::text),
    $35::int, $36::int, $37::int, $38::int,
    $39, $40, $41, $42, $43, $44, $45,
    _numeric_or_null($46::text), _numeric_or_null($47::text),
    $48, $49, $50,
    $51::int, $52::bigint,
    $53::timestamptz, $54::timestamptz, $55::close_trigger,
    $56, $57::work_status, $58::jsonb
)
RETURNING id
)SQL";

// We rely on a couple of tiny helper SQL functions to coerce empty/whitespace
// DICOM DA/TM/DS strings to NULL — DCMTK can hand us "" or "        " for
// absent values that the schema's strict DATE / TIME / NUMERIC types reject.
// These are created lazily on first use; idempotent.
constexpr const char* kCreateHelpersSql = R"SQL(
CREATE OR REPLACE FUNCTION _date_or_null(s text) RETURNS date AS $$
DECLARE clean text;
BEGIN
    clean := nullif(btrim(s), '');
    IF clean IS NULL THEN RETURN NULL; END IF;
    -- DICOM DA: YYYYMMDD (8 chars, all digits). Accept ISO too.
    IF clean ~ '^[0-9]{8}$' THEN
        RETURN to_date(clean, 'YYYYMMDD');
    END IF;
    BEGIN RETURN clean::date; EXCEPTION WHEN OTHERS THEN RETURN NULL; END;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION _time_or_null(s text) RETURNS time AS $$
DECLARE clean text;
BEGIN
    clean := nullif(btrim(s), '');
    IF clean IS NULL THEN RETURN NULL; END IF;
    -- DICOM TM: HHMMSS[.FFFFFF]. Accept "HH:MM:SS" too.
    IF clean ~ '^[0-9]{6}(\.[0-9]+)?$' THEN
        RETURN to_timestamp(left(clean,6), 'HH24MISS')::time;
    END IF;
    BEGIN RETURN clean::time; EXCEPTION WHEN OTHERS THEN RETURN NULL; END;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE OR REPLACE FUNCTION _numeric_or_null(s text) RETURNS numeric AS $$
DECLARE clean text;
BEGIN
    clean := nullif(btrim(s), '');
    IF clean IS NULL THEN RETURN NULL; END IF;
    BEGIN RETURN clean::numeric; EXCEPTION WHEN OTHERS THEN RETURN NULL; END;
END;
$$ LANGUAGE plpgsql IMMUTABLE;
)SQL";

// Bind helper: append a string-or-NULL to a parameter vector.
void bind_opt(std::vector<const char*>& params,
              std::vector<std::string>&  storage,
              const std::optional<std::string>& v) {
    if (v.has_value()) {
        storage.push_back(*v);
        params.push_back(storage.back().c_str());
    } else {
        params.push_back(nullptr);
    }
}
void bind_str(std::vector<const char*>& params,
              std::vector<std::string>&  storage,
              std::string v) {
    storage.push_back(std::move(v));
    params.push_back(storage.back().c_str());
}
void bind_opt_int(std::vector<const char*>& params,
                  std::vector<std::string>&  storage,
                  const std::optional<std::int64_t>& v) {
    if (v.has_value()) {
        storage.push_back(std::to_string(*v));
        params.push_back(storage.back().c_str());
    } else {
        params.push_back(nullptr);
    }
}
void bind_int(std::vector<const char*>& params,
              std::vector<std::string>&  storage,
              std::int64_t v) {
    storage.push_back(std::to_string(v));
    params.push_back(storage.back().c_str());
}

}  // namespace

// ---- Db ------------------------------------------------------------------

Db::Db(const std::string& dsn) : dsn_(dsn) {
    connect_();

    // Install (idempotently) the per-tag coercion helpers. CREATE OR REPLACE
    // makes this safe to run on every startup.
    ResultGuard r{PQexec(as_conn(conn_), kCreateHelpersSql)};
    if (PQresultStatus(r.r) != PGRES_COMMAND_OK) {
        const std::string err = PQerrorMessage(as_conn(conn_));
        throw DbError("failed to install helper functions: " + err);
    }
}

Db::~Db() {
    if (conn_) PQfinish(as_conn(conn_));
}

void Db::connect_() {
    PGconn* c = PQconnectdb(dsn_.c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        const std::string err = PQerrorMessage(c);
        PQfinish(c);
        throw DbError("connection failed: " + err);
    }
    conn_ = c;
    LOG_INFO("db.connected", "host", PQhost(c), "db", PQdb(c));
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

std::int64_t Db::insert_work_queue_row(const StudyRow& row) {
    // Serialize on the single PGconn — libpq is not thread-safe at the
    // connection level. The lock spans ensure_connected_, param binding,
    // PQexecParams, and result inspection because all of those touch
    // the shared connection. A per-thread connection pool would let
    // calls run truly in parallel; INSERT latency on Postgres is
    // dominated by transaction commit (~5-10ms) so the trade-off
    // isn't materially constraining at 100-worker scale.
    std::lock_guard<std::mutex> lk(mu_);
    ensure_connected_();

    // Latency observation around the actual PQexecParams call (excluding
    // the param-binding loop, which is allocation-dominated and not the
    // metric we care about). Captured here so we can record on both
    // success and exception paths.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto record_latency = [&]() {
        if (metrics_) {
            const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                clock::now() - t0).count();
            metrics_->db_insert_duration_seconds.self().observe(dt);
        }
    };

    // Build the params array. 58 bound params, in the same order as $1..$58
    // in kInsertSql.
    std::vector<std::string>  storage;
    std::vector<const char*>  params;
    storage.reserve(64);
    params.reserve(64);

    const auto& t = row.tags;

    bind_str(params, storage, row.server_id);                      // $1
    bind_str(params, storage, t.study_instance_uid);               // $2
    bind_opt(params, storage, t.series_instance_uid);              // $3
    bind_opt(params, storage, t.accession_number);                 // $4
    bind_opt(params, storage, t.study_id);                         // $5
    bind_opt(params, storage, t.sop_instance_uid);                 // $6
    bind_opt(params, storage, t.patient_id);                       // $7
    bind_opt(params, storage, t.patient_name);                     // $8
    bind_opt(params, storage, t.patient_birth_date);               // $9 -> _date_or_null
    bind_opt(params, storage, t.patient_sex);                      // $10
    bind_str(params, storage, row.calling_aet);                    // $11
    bind_str(params, storage, row.called_aet);                     // $12
    bind_str(params, storage, row.peer_ip);                        // $13 -> inet
    bind_opt(params, storage, t.modality);                         // $14
    bind_opt(params, storage, t.station_name);                     // $15
    bind_opt(params, storage, t.station_aet);                      // $16
    bind_opt(params, storage, t.retrieve_aet);                     // $17
    bind_opt(params, storage, t.institution_name);                 // $18
    bind_opt(params, storage, t.manufacturer);                     // $19
    bind_opt(params, storage, t.manufacturer_model_name);          // $20
    bind_opt(params, storage, t.device_serial_number);             // $21
    bind_opt(params, storage, t.device_uid);                       // $22
    bind_opt(params, storage, t.software_versions);                // $23
    bind_opt(params, storage, t.study_description);                // $24
    bind_opt(params, storage, t.series_description);               // $25
    bind_opt(params, storage, t.protocol_name);                    // $26
    bind_opt(params, storage, t.body_part_examined);               // $27
    bind_opt(params, storage, t.referring_physician_name);         // $28
    bind_opt(params, storage, t.study_date);                       // $29 -> _date_or_null
    bind_opt(params, storage, t.study_time);                       // $30 -> _time_or_null
    bind_opt(params, storage, t.series_date);                      // $31 -> _date_or_null
    bind_opt(params, storage, t.series_time);                      // $32 -> _time_or_null
    bind_opt(params, storage, t.acquisition_date);                 // $33 -> _date_or_null
    bind_opt(params, storage, t.acquisition_time);                 // $34 -> _time_or_null
    bind_opt_int(params, storage, t.series_number);                // $35
    bind_opt_int(params, storage, t.number_of_series);             // $36
    bind_opt_int(params, storage, t.instance_number);              // $37
    bind_opt_int(params, storage, t.acquisition_number);           // $38
    bind_opt(params, storage, t.acquisition_type);                 // $39
    bind_opt(params, storage, t.scanning_sequence);                // $40
    bind_opt(params, storage, t.sequence_name);                    // $41
    bind_opt(params, storage, t.sequence_variant);                 // $42
    bind_opt(params, storage, t.image_type);                       // $43
    bind_opt(params, storage, t.image_comments);                   // $44
    bind_opt(params, storage, t.contrast_bolus_agent);             // $45
    bind_opt(params, storage, t.slice_thickness);                  // $46 -> _numeric_or_null
    bind_opt(params, storage, t.magnetic_field_strength);          // $47 -> _numeric_or_null
    bind_opt(params, storage, t.code_value);                       // $48
    bind_opt(params, storage, t.code_meaning);                     // $49
    bind_opt(params, storage, t.specific_character_set);           // $50
    bind_int(params, storage, row.instance_count);                 // $51
    bind_int(params, storage, row.byte_count);                     // $52
    bind_str(params, storage, row.received_at_iso);                // $53
    bind_str(params, storage, row.closed_at_iso);                  // $54
    bind_str(params, storage, "assoc_end");                        // $55 close_trigger
    bind_str(params, storage, row.file_root_path);                 // $56
    bind_str(params, storage, "received");                         // $57 status
    bind_str(params, storage, t.tags_json);                        // $58 jsonb

    ResultGuard r{
        PQexecParams(
            as_conn(conn_),
            kInsertSql,
            static_cast<int>(params.size()),
            nullptr,                  // paramTypes (libpq infers from SQL)
            params.data(),
            nullptr,                  // paramLengths (NULL = strlen)
            nullptr,                  // paramFormats (NULL = text)
            0                         // resultFormat: text
        )
    };

    if (PQresultStatus(r.r) != PGRES_TUPLES_OK) {
        record_latency();
        if (metrics_) metrics_->db_insert_errors_total.self().inc();
        const std::string err = PQerrorMessage(as_conn(conn_));
        throw DbError("INSERT work_queue failed: " + err);
    }

    record_latency();
    const char* id_text = PQgetvalue(r.r, 0, 0);
    return std::stoll(id_text ? id_text : "0");
}

}  // namespace nlr
