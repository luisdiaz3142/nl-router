#include "dicom_handler.hpp"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmnet/scu.h>
#include <dcmtk/ofstd/ofcond.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "handler.hpp"
#include "logging.hpp"

namespace nlr {

namespace {

// Walk a directory tree and collect every .dcm file under it. Order is
// not stable across calls (filesystem-dependent), which doesn't matter
// for routing semantics — each instance is sent independently.
std::vector<std::filesystem::path>
collect_dicom_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec))
    {
        if (it->is_regular_file() && it->path().extension() == ".dcm") {
            out.push_back(it->path());
        }
    }
    return out;
}

// Pull SOPClassUID + TransferSyntaxUID from a DICOM file's file-meta info,
// without parsing the full dataset. We need these to negotiate presentation
// contexts before opening the association.
struct InstanceInfo {
    std::filesystem::path path;
    std::string           sop_class_uid;
    std::string           sop_instance_uid;
    std::string           transfer_syntax_uid;
};

bool read_instance_info(const std::filesystem::path& path, InstanceInfo& out) {
    DcmFileFormat ff;
    // Only read until pixel data so we don't drag every instance into RAM
    // just to read headers.
    OFCondition cond = ff.loadFile(path.c_str(),
                                    EXS_Unknown, EGL_noChange,
                                    DCM_MaxReadLength,
                                    ERM_metaOnly);
    if (cond.bad()) return false;

    auto* fmi = ff.getMetaInfo();
    if (!fmi) return false;

    OFString sop_class, sop_inst, ts;
    if (fmi->findAndGetOFString(DCM_MediaStorageSOPClassUID, sop_class).bad())   return false;
    if (fmi->findAndGetOFString(DCM_MediaStorageSOPInstanceUID, sop_inst).bad()) return false;
    if (fmi->findAndGetOFString(DCM_TransferSyntaxUID, ts).bad())                return false;

    out.path                = path;
    out.sop_class_uid       = std::string{sop_class.c_str()};
    out.sop_instance_uid    = std::string{sop_inst.c_str()};
    out.transfer_syntax_uid = std::string{ts.c_str()};
    return true;
}

// Default transfer syntax list when the destination didn't specify any.
// Implicit + explicit LE cover essentially everything we receive; compressed
// forms are added so the peer can request transcoding-free passthrough for
// pre-compressed instances.
std::vector<std::string> default_transfer_syntaxes() {
    return {
        UID_LittleEndianExplicitTransferSyntax,
        UID_LittleEndianImplicitTransferSyntax,
        UID_JPEGProcess14SV1TransferSyntax,
        UID_JPEG2000LosslessOnlyTransferSyntax,
        UID_JPEGLSLosslessTransferSyntax,
        UID_RLELosslessTransferSyntax,
    };
}

// Encode the per-instance response detail as a JSON object. Tiny enough
// to hand-roll; pulling nlohmann/json into the SCU code path isn't worth
// the include cost.
std::string format_response_json(int sent, int succeeded, int failed,
                                  const std::vector<std::pair<std::string,Uint16>>& failed_uids) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"sent\":"      << sent      << ",";
    oss << "\"succeeded\":" << succeeded << ",";
    oss << "\"failed\":"    << failed;
    if (!failed_uids.empty()) {
        oss << ",\"failed_instances\":[";
        for (std::size_t i = 0; i < failed_uids.size(); ++i) {
            if (i) oss << ',';
            const auto& [uid, status] = failed_uids[i];
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04x", status);
            oss << "{\"sop_uid\":\"" << uid
                << "\",\"status\":\"0x" << buf << "\"}";
        }
        oss << "]";
    }
    oss << "}";
    return oss.str();
}

}  // namespace

DispatchResult DicomDispatchHandler::dispatch(const Assignment& a,
                                                const Destination& d) {
    if (a.study_file_root.empty()) {
        return DispatchResult::permanent("no study_file_root recorded on work_queue row");
    }
    const std::filesystem::path root{a.study_file_root};

    // ---- 1) Enumerate the files on disk ---------------------------------
    auto files = collect_dicom_files(root);
    if (files.empty()) {
        // Cleaner already ran, or the receiver didn't actually write
        // anything before flushing the row. Either way, the files are
        // gone; no retry will help.
        return DispatchResult::permanent("no .dcm files under " + root.string());
    }

    // ---- 2) Read headers; bucket by (sop_class, transfer_syntax) --------
    //
    // DCMTK presentation contexts are limited (the standard allows up to
    // 128 per association). We bucket unique (sop_class, ts) pairs and
    // propose one context per bucket. Files that fail to read are skipped
    // and counted as failures.
    std::vector<InstanceInfo> infos;
    infos.reserve(files.size());
    std::unordered_map<std::string, std::vector<std::string>> sop_to_xfers;  // sop_class -> [xfer]
    int header_read_failures = 0;
    for (const auto& f : files) {
        InstanceInfo info;
        if (!read_instance_info(f, info)) {
            ++header_read_failures;
            LOG_WARN("dispatch.dicom.header_read_failed", "path", f.string());
            continue;
        }
        auto& xfers = sop_to_xfers[info.sop_class_uid];
        if (std::find(xfers.begin(), xfers.end(), info.transfer_syntax_uid) == xfers.end()) {
            xfers.push_back(info.transfer_syntax_uid);
        }
        infos.push_back(std::move(info));
    }

    if (infos.empty()) {
        return DispatchResult::permanent(
            "all instances under " + root.string() + " failed header parse");
    }

    // ---- 3) Configure the SCU ------------------------------------------
    DcmSCU scu;
    scu.setPeerHostName(d.host.c_str());
    scu.setPeerPort(d.port);
    scu.setPeerAETitle(d.called_aet.c_str());
    scu.setAETitle(d.calling_aet.c_str());
    scu.setMaxReceivePDULength(d.max_pdu_size);
    scu.setACSETimeout(30);          // seconds
    scu.setConnectionTimeout(30);

    // Determine the transfer-syntax list we offer per SOP class. If the
    // destination specified preferred_transfer_syntaxes, those go first;
    // we then append every syntax we found in our local files (the peer
    // will pick one acceptable for each SOP class). Falling back to the
    // default list ensures we always offer at least implicit + explicit LE.
    const auto& preferred = d.preferred_transfer_syntaxes.empty()
        ? default_transfer_syntaxes()
        : d.preferred_transfer_syntaxes;

    int contexts_added = 0;
    for (auto& [sop_class, found_xfers] : sop_to_xfers) {
        // Combine preferred first, then any local-file syntaxes not already
        // in the preferred list. De-dup. DCMTK silently truncates beyond
        // 128 contexts, so we cap conservatively at 120 per SCU instance.
        std::vector<OFString> offered;
        offered.reserve(preferred.size() + found_xfers.size());
        for (const auto& ts : preferred) offered.emplace_back(ts.c_str());
        for (const auto& ts : found_xfers) {
            if (std::find(offered.begin(), offered.end(), OFString{ts.c_str()})
                == offered.end()) {
                offered.emplace_back(ts.c_str());
            }
        }
        OFList<OFString> ofl;
        for (auto& s : offered) ofl.push_back(s);

        if (scu.addPresentationContext(sop_class.c_str(), ofl).bad()) {
            LOG_WARN("dispatch.dicom.add_pc_failed", "sop_class", sop_class);
            continue;
        }
        if (++contexts_added >= 120) break;
    }
    if (contexts_added == 0) {
        return DispatchResult::permanent("no presentation contexts to offer");
    }

    // ---- 4) Open association --------------------------------------------
    OFCondition cond = scu.initNetwork();
    if (cond.bad()) {
        return DispatchResult::transient(
            std::string{"initNetwork: "} + (cond.text() ? cond.text() : "unknown"));
    }
    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        const std::string err = cond.text() ? cond.text() : "unknown";
        scu.closeAssociation(DCMSCU_RELEASE_ASSOCIATION);
        // Negotiation failures can be transient (peer down, network glitch)
        // or permanent (peer doesn't speak our SOP classes). We treat as
        // transient and let the retry policy decide; permanent
        // misconfiguration trips the give_up_after_hours backstop.
        return DispatchResult::transient("negotiateAssociation: " + err);
    }

    // ---- 5) Send each instance -----------------------------------------
    int sent = 0, succeeded = 0, failed = 0;
    std::vector<std::pair<std::string,Uint16>> failed_uids;

    for (const auto& info : infos) {
        DcmFileFormat ff;
        cond = ff.loadFile(info.path.c_str());
        if (cond.bad()) {
            ++failed;
            failed_uids.emplace_back(info.sop_instance_uid, 0xC000);
            LOG_WARN("dispatch.dicom.file_load_failed",
                "path",  info.path.string(),
                "error", cond.text() ? cond.text() : "unknown");
            continue;
        }

        // Find a presentation context that the peer accepted for this
        // (sop_class, ts) combination. Falling back to "any context for
        // the sop_class" handles peers that accepted a different syntax.
        T_ASC_PresentationContextID pc_id = scu.findPresentationContextID(
            info.sop_class_uid.c_str(),
            info.transfer_syntax_uid.c_str());
        if (pc_id == 0) {
            pc_id = scu.findPresentationContextID(
                info.sop_class_uid.c_str(), "");
        }
        if (pc_id == 0) {
            ++failed;
            // Status 0x0122 = "Refused: SOP Class Not Supported" — fits a
            // missing presentation context.
            failed_uids.emplace_back(info.sop_instance_uid, 0x0122);
            LOG_WARN("dispatch.dicom.no_pc",
                "sop_class",  info.sop_class_uid,
                "transfer_syntax", info.transfer_syntax_uid);
            continue;
        }

        Uint16 rsp_status = 0xC000;
        cond = scu.sendSTORERequest(pc_id, info.path.c_str(), ff.getDataset(),
                                     rsp_status);
        ++sent;
        if (cond.good() && rsp_status == STATUS_Success) {
            ++succeeded;
        } else {
            ++failed;
            failed_uids.emplace_back(info.sop_instance_uid, rsp_status);
            LOG_WARN("dispatch.dicom.cstore_failed",
                "sop_uid", info.sop_instance_uid,
                "status",  std::to_string(rsp_status),
                "error",   cond.text() ? cond.text() : "ok-but-non-success-status");
        }
    }

    // ---- 6) Release the association ------------------------------------
    cond = scu.releaseAssociation();
    if (cond.bad()) {
        LOG_WARN("dispatch.dicom.release_failed",
            "error", cond.text() ? cond.text() : "unknown");
    }

    const std::string detail = format_response_json(
        sent, succeeded, failed + header_read_failures, failed_uids);

    if (failed > 0 || header_read_failures > 0) {
        // Any per-instance failure is a transient failure for the whole
        // assignment; retry policy decides whether to re-attempt. Per-
        // destination dedup is the dispatcher's responsibility in a v2
        // (M7) refactor; v1 simply re-sends the whole set on retry.
        return DispatchResult::transient(
            "1+ instances failed during C-STORE", detail);
    }
    if (succeeded == 0) {
        return DispatchResult::transient("no instances delivered", detail);
    }
    return DispatchResult::success(detail);
}

}  // namespace nlr
