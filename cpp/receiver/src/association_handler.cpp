#include "association_handler.hpp"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmnet/diutil.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/dcmnet/scp.h>
#include <dcmtk/ofstd/ofcond.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

#include "disk_guard.hpp"
#include "logging.hpp"
#include "storage.hpp"
#include "tag_extractor.hpp"

namespace nlr {

AssociationHandler::AssociationHandler(const Config& cfg, Db& db,
                                        const ReceiverMetrics& metrics,
                                        const DiskGuard* disk_guard,
                                        std::string peer_type)
    : cfg_(cfg), db_(db), metrics_(metrics), disk_guard_(disk_guard),
      peer_type_(std::move(peer_type)) {}

void AssociationHandler::notifyAssociationRequest(
    const T_ASC_Parameters& params,
    DcmSCPActionType& desired_action)
{
    // If the disk is in Reject state, refuse before negotiating
    // presentation contexts. DCMTK emits A-ASSOCIATE-RJ on the wire
    // using the reason code mapped from DCMSCP_TOO_MANY_ASSOCIATIONS,
    // which is the closest standard reason ("local-limit-exceeded").
    if (disk_guard_ != nullptr &&
        disk_guard_->state() == DiskGuard::State::Reject)
    {
        // Pull peer identity out of T_ASC_Parameters for the log line.
        // We can't use getPeerAETitle() yet — that becomes available
        // only after acceptance.
        const std::string calling{params.DULparams.callingAPTitle};
        const std::string called{params.DULparams.calledAPTitle};
        const std::string peer{params.DULparams.callingPresentationAddress};

        LOG_WARN("association.reject_disk_full",
            "calling_aet", calling,
            "called_aet",  called,
            "peer",        peer);

        metrics_.associations_total.labels({"rejected_disk_full", peer_type_}).inc();
        desired_action = DCMSCP_ACTION_REFUSE_ASSOCIATION;
        return;
    }

    // Default path: let DcmSCP proceed with normal negotiation. We don't
    // call the base implementation because the base is a no-op hook;
    // leaving desired_action at its incoming value is the documented way
    // to accept.
    (void)params;
    (void)desired_action;
}

// Normalize a peer address string to a numeric IP literal suitable for
// inserting into the work_queue.peer_ip INET column.
//
// DCMTK's getPeerIP() can return either a hostname (when reverse-DNS is on)
// or a numeric IP. We turn anything non-numeric into its first resolved
// address (IPv4 preferred, falling back to IPv6). Returns "0.0.0.0" if
// resolution fails entirely — we don't want a single bad lookup to drop a
// study.
static std::string normalize_ip(const std::string& raw) {
    if (raw.empty()) return "0.0.0.0";

    // Already numeric?
    in_addr  v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET,  raw.c_str(), &v4) == 1 ||
        inet_pton(AF_INET6, raw.c_str(), &v6) == 1) {
        return raw;
    }

    // Resolve. getaddrinfo gives us an addrinfo list; we walk and convert.
    addrinfo  hints{};
    addrinfo* result = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(raw.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return "0.0.0.0";
    }

    std::string ipv4_out, ipv6_out;
    char buf[INET6_ADDRSTRLEN];
    for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const auto* sa = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                ipv4_out = buf;
                break;        // prefer IPv4 for legacy peer logs
            }
        } else if (ai->ai_family == AF_INET6 && ipv6_out.empty()) {
            const auto* sa = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
            if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf))) {
                ipv6_out = buf;
            }
        }
    }
    freeaddrinfo(result);

    if (!ipv4_out.empty()) return ipv4_out;
    if (!ipv6_out.empty()) return ipv6_out;
    return "0.0.0.0";
}

std::string AssociationHandler::iso_now() {
    using clock = std::chrono::system_clock;
    const auto now  = clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) - secs;
    const std::time_t t = clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms.count() << "+00";
    return oss.str();
}

void AssociationHandler::snapshot_network_context_() {
    if (snapshot_done_) return;

    // DcmSCP exposes the called AET (us) via getCalledAETitle() and the
    // calling AET (the peer) via getPeerAETitle(). Peer IP via getPeerIP().
    const OFString called  = getCalledAETitle();
    const OFString calling = getPeerAETitle();
    const OFString peer    = getPeerIP();
    calling_aet_ = std::string{calling.c_str(), calling.length()};
    called_aet_  = std::string{called.c_str(),  called.length()};
    peer_ip_     = normalize_ip(std::string{peer.c_str(), peer.length()});

    assoc_start_ = std::chrono::system_clock::now();
    snapshot_done_ = true;

    // We bump "accepted" here rather than in DcmSCP::negotiateAssociation
    // because that hook isn't cleanly overridable in DCMTK 3.6.x; reaching
    // handleIncomingCommand means negotiation completed and the peer is
    // talking DIMSE, which is the operational definition of "accepted."
    metrics_.associations_total.labels({"accepted", peer_type_}).inc();
    metrics_.associations_active.self().inc();
    // v1 single-threaded model — busy reflects the single in-flight assoc.
    metrics_.workers_busy.self().inc();

    LOG_INFO("association.accept",
        "calling_aet", calling_aet_,
        "called_aet",  called_aet_,
        "peer_ip",     peer_ip_,
        "peer_type",   peer_type_);
}

OFCondition AssociationHandler::handleIncomingCommand(
    T_DIMSE_Message* msg,
    const DcmPresentationContextInfo& ctx_info)
{
    snapshot_network_context_();

    if (msg->CommandField == DIMSE_C_STORE_RQ) {
        return handle_store(msg->msg.CStoreRQ, ctx_info);
    }

    // C-ECHO and anything else: defer to DcmSCP's defaults (handles C-ECHO,
    // returns "command not supported" for the rest).
    return DcmSCP::handleIncomingCommand(msg, ctx_info);
}

OFCondition AssociationHandler::handle_store(
    T_DIMSE_C_StoreRQ& req,
    const DcmPresentationContextInfo& ctx_info)
{
    // Receive the dataset into memory. DcmSCP::receiveSTORERequest writes it
    // directly to a file or to memory; we pass a nullptr filename so it
    // returns an in-memory DcmFileFormat we can both write to disk AND
    // extract tags from in one pass.
    DcmFileFormat ff;
    DcmDataset* dataset = nullptr;
    OFString tempfile;     // not used; we manage storage ourselves

    OFCondition cond = DcmSCP::receiveSTORERequest(req, ctx_info.presentationContextID,
                                                    dataset);
    if (cond.bad() || dataset == nullptr) {
        LOG_ERROR("store.receive_failed", "error", cond.text() ? cond.text() : "unknown");
        return cond;
    }

    // Wrap the dataset in a FileFormat so DCMTK writes a proper part-10 file
    // (preamble + DICM magic + file meta information).
    ff.getDataset()->copyFrom(*dataset);

    // ---- Extract tags first ----
    ExtractedTags tags;
    try {
        tags = extract(*ff.getDataset());
    } catch (const std::exception& e) {
        LOG_ERROR("store.extract_failed", "error", e.what());
        return DIMSE_BADDATA;
    }

    // ---- Compute target path and write ----
    auto& study_state = studies_[tags.study_instance_uid];
    if (study_state.landing_date.empty()) {
        // First instance for this study in this association: snapshot the
        // landing date and the receive-time. Subsequent instances for the
        // same study reuse both.
        study_state.landing_date = today_yyyymmdd();
        study_state.row.received_at_iso = iso_now();
        study_state.row.server_id   = cfg_.server_id;
        study_state.row.calling_aet = calling_aet_;
        study_state.row.called_aet  = called_aet_;
        study_state.row.peer_ip     = peer_ip_;
        study_state.row.tags        = tags;
        study_state.row.file_root_path =
            study_root(cfg_.landing_zone, study_state.landing_date,
                       tags.study_instance_uid).string();
    }

    const auto file_path = instance_path(
        cfg_.landing_zone,
        study_state.landing_date,
        tags.study_instance_uid,
        *tags.series_instance_uid,
        *tags.sop_instance_uid);

    try {
        ensure_parents(file_path);
    } catch (const std::exception& e) {
        LOG_ERROR("store.mkdir_failed",
                  "path", file_path.string(),
                  "error", e.what());
        // No DCMTK-defined "couldn't create directory" code; map to a generic
        // I/O error so the SCU sees a non-success C-STORE status.
        return EC_IllegalCall;
    }

    // Save the file. We use the same transfer syntax we received it in to
    // avoid lossy recompression. DcmFileFormat::saveFile handles preamble,
    // file meta info, and dataset writing in one call.
    const E_TransferSyntax xfer = ff.getDataset()->getOriginalXfer();
    cond = ff.saveFile(file_path.c_str(), xfer);
    if (cond.bad()) {
        LOG_ERROR("store.write_failed",
                  "path",  file_path.string(),
                  "error", cond.text() ? cond.text() : "unknown");
        return cond;
    }

    // Counters.
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(file_path, ec);
    if (!ec) {
        study_state.row.byte_count += static_cast<std::int64_t>(bytes);
    }
    study_state.row.instance_count += 1;

    // Metric labels: empty modality/calling_aet would create an
    // "unknown" bucket — keep label values stable but non-empty.
    const std::string modality =
        tags.modality ? *tags.modality : std::string{"unknown"};
    metrics_.instances_received_total.labels({calling_aet_, modality}).inc();
    if (!ec) {
        metrics_.bytes_received_total.labels({calling_aet_})
            .inc(static_cast<std::int64_t>(bytes));
    }

    LOG_DEBUG("store.instance_written",
        "study_uid",  tags.study_instance_uid,
        "series_uid", *tags.series_instance_uid,
        "sop_uid",    *tags.sop_instance_uid,
        "path",       file_path.string(),
        "bytes",      std::to_string(bytes));

    // Send the C-STORE response. When we override handleIncomingCommand we
    // bypass the framework's automatic response, so we have to do this
    // explicitly — otherwise the SCU sits in DIMSE_BLOCKING and eventually
    // times out / aborts.
    cond = sendSTOREResponse(ctx_info.presentationContextID, req,
                             STATUS_Success);
    if (cond.bad()) {
        LOG_WARN("store.send_response_failed",
                 "error", cond.text() ? cond.text() : "unknown");
    }
    return cond;
}

void AssociationHandler::notifyAssociationTermination() {
    if (!snapshot_done_) {
        // We accepted the association but never saw a C-STORE (e.g., the
        // peer only did a C-ECHO). Nothing to flush — and crucially, we
        // never bumped associations_active either, so don't decrement.
        DcmSCP::notifyAssociationTermination();
        return;
    }

    const std::string closed_at = iso_now();
    int rows_inserted = 0;

    for (auto& [study_uid, state] : studies_) {
        state.row.closed_at_iso = closed_at;
        try {
            const auto id = db_.insert_work_queue_row(state.row);
            LOG_INFO("work_queue.row_inserted",
                "id",            std::to_string(id),
                "study_uid",     study_uid,
                "instance_count", std::to_string(state.row.instance_count),
                "byte_count",    std::to_string(state.row.byte_count));
            ++rows_inserted;
            // v1 only ships the assoc_end trigger; series/study idle
            // timers land alongside the bounded-pool listener.
            metrics_.studies_closed_total.labels({"assoc_end"}).inc();
        } catch (const std::exception& e) {
            LOG_ERROR("work_queue.insert_failed",
                "study_uid", study_uid,
                "error",     e.what());
        }
    }

    // Duration: monotonic clock would be cleaner but assoc_start_ is wall-
    // clock from snapshot_network_context_(); subtracting a system_clock
    // pair is fine for an O(seconds-to-minutes) measurement on a single
    // host. We accept the (tiny) NTP-adjustment risk because the dashboard
    // uses these buckets for outlier detection, not nanosecond accuracy.
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::system_clock::now() - assoc_start_).count();
    metrics_.association_duration_seconds.self().observe(dt);
    metrics_.associations_active.self().dec();
    metrics_.workers_busy.self().dec();

    LOG_INFO("association.close",
        "calling_aet",       calling_aet_,
        "studies",           std::to_string(studies_.size()),
        "rows_inserted",     std::to_string(rows_inserted));

    studies_.clear();
    snapshot_done_ = false;

    DcmSCP::notifyAssociationTermination();
}

}  // namespace nlr
