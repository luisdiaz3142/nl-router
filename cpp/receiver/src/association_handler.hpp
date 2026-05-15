// nl-receiver/association_handler.hpp
//
// Per-association handler. Each pool worker thread creates an instance
// of one of the AssociationHandler subclasses, runs it for the lifetime
// of a single accepted DICOM association, then returns the slot to the
// pool. Up to N associations (max_associations) can be in flight
// simultaneously.
//
// Responsibilities, per association:
//   1. Write each received SOP instance to disk in the UID-tree layout.
//   2. Aggregate per-study state (tag extract, byte/instance counters).
//   3. At association end, insert one work_queue row per unique study
//      seen (close_trigger='assoc_end').
//   4. Refuse new associations when DiskGuard is in Reject state.
//
// C-ECHO falls through to DcmSCP's default handler.
//
// Threading model
// ---------------
// AssociationHandler extends DcmThreadSCP (the threaded-worker variant
// of DcmSCP introduced in DCMTK 3.6.1). DcmSCPPool owns N pre-spawned
// threads, each holding an AssociationHandler instance; the pool's
// listener thread accepts TCP connections and dispatches each accepted
// T_ASC_Association to the next idle worker via condvar. Excess
// connections beyond pool capacity wait briefly on the kernel's listen
// backlog and then time out at the SCU side — matching the design
// plan's "reject A-ASSOCIATE-RJ when local-limit-exceeded" semantics.
//
// Because pool workers are default-constructed by DcmSCPPool, the
// shared per-process context (Config, Db, Metrics, DiskGuard) is
// injected once at startup via init_shared_context() and held as
// static state. The peer_type label ("plain" / "tls") is per-subclass
// since plain and TLS each get their own pool.

#pragma once

#include <dcmtk/dcmnet/scpthrd.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "config.hpp"
#include "db.hpp"
#include "metrics.hpp"

namespace nlr {

class DiskGuard;

class AssociationHandler : public DcmThreadSCP {
public:
    AssociationHandler() = default;            // pool default-constructs
    ~AssociationHandler() override = default;

    AssociationHandler(const AssociationHandler&)            = delete;
    AssociationHandler& operator=(const AssociationHandler&) = delete;

    // Wire up the long-lived per-process context the workers share. Must
    // be called exactly once before any pool starts accepting. Idempotent
    // for tests; not safe to call concurrently with active workers.
    static void init_shared_context(const Config& cfg,
                                    Db& db,
                                    const ReceiverMetrics& metrics,
                                    const DiskGuard* disk_guard);

protected:
    // "plain" / "tls" — set by the subclass; included in metric labels
    // and the association.accept log line so dashboards can see TLS
    // adoption per association.
    virtual const std::string& peer_type() const = 0;

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

    // Shared static context injected at startup. Pointers (not refs) so
    // the default constructor doesn't need initialization syntax for
    // them. Accessors guarantee the pointers are populated.
    static const Config*           s_cfg_;
    static Db*                     s_db_;
    static const ReceiverMetrics*  s_metrics_;
    static const DiskGuard*        s_disk_guard_;

    static const Config&          cfg()        { return *s_cfg_; }
    static Db&                    db()         { return *s_db_; }
    static const ReceiverMetrics& metrics()    { return *s_metrics_; }
    static const DiskGuard*       disk_guard() { return s_disk_guard_; }

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

// Concrete subclasses — one per transport. Each pool is templated on
// one of these so the peer_type() label is fixed per pool at compile
// time and shows up correctly in metrics + logs.
class PlainAssociationHandler : public AssociationHandler {
protected:
    const std::string& peer_type() const override;
};

class TlsAssociationHandler : public AssociationHandler {
protected:
    const std::string& peer_type() const override;
};

}  // namespace nlr
