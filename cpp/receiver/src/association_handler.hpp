// nl-receiver/association_handler.hpp
//
// Per-association handler. Inherits from DcmSCP and overrides the C-STORE
// receive path to:
//   1. Write each instance to disk in the planned UID-tree layout.
//   2. Aggregate per-study state (tag extract, byte/instance counters).
//   3. At association end, insert one work_queue row per unique study seen
//      (close_trigger='assoc_end').
//
// C-ECHO falls through to DcmSCP's default handler.
//
// v1 is single-threaded: one association at a time. The bounded-pool /
// listener split lands in M10 along with the metrics endpoint and TLS.

#pragma once

#include <dcmtk/dcmnet/scp.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "config.hpp"
#include "db.hpp"
#include "metrics.hpp"

namespace nlr {

class DiskGuard;

class AssociationHandler : public DcmSCP {
public:
    AssociationHandler(const Config& cfg, Db& db,
                       const ReceiverMetrics& metrics,
                       const DiskGuard* disk_guard);
    ~AssociationHandler() override = default;

    AssociationHandler(const AssociationHandler&)            = delete;
    AssociationHandler& operator=(const AssociationHandler&) = delete;

protected:
    // Called by DcmSCP after an association request is parsed but before
    // it's accepted. We override to refuse new associations when the
    // landing zone is too full (DiskGuard reports Reject). DCMTK then
    // sends a proper A-ASSOCIATE-RJ on the wire.
    void notifyAssociationRequest(const T_ASC_Parameters& params,
                                  DcmSCPActionType& desired_action) override;

    // Called by DcmSCP when a DIMSE message arrives. We dispatch:
    //   * C-ECHO  -> base class (default behavior is fine)
    //   * C-STORE -> handle_store()
    OFCondition handleIncomingCommand(T_DIMSE_Message* msg,
                                      const DcmPresentationContextInfo& ctx_info) override;

    // Called by DcmSCP at the end of the association. We emit one work_queue
    // row per study we accumulated, then reset state for the next association.
    void notifyAssociationTermination() override;

private:
    OFCondition handle_store(T_DIMSE_C_StoreRQ& req,
                             const DcmPresentationContextInfo& ctx_info);

    // Per-study state accumulated during the association. Keyed by
    // StudyInstanceUID.
    struct StudyState {
        StudyRow      row;
        std::string   landing_date;          // YYYY-MM-DD chosen at first instance
    };

    const Config& cfg_;
    Db&           db_;
    const ReceiverMetrics& metrics_;
    const DiskGuard*       disk_guard_;
    std::map<std::string, StudyState> studies_;
    std::chrono::system_clock::time_point assoc_start_;

    // Network context snapshot (peer ip + AETs) captured at association start
    // and reused across all instances in the association.
    std::string peer_ip_;
    std::string calling_aet_;
    std::string called_aet_;

    // Did we already snapshot the network context for this association?
    bool snapshot_done_ {false};

    void snapshot_network_context_();
    static std::string iso_now();
};

}  // namespace nlr
