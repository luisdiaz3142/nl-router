#include "server.hpp"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/dcmnet/scp.h>
#include <dcmtk/dcmnet/scpcfg.h>
#include <dcmtk/dcmnet/scppool.h>

#include <string>
#include <vector>

#include "logging.hpp"
#include "tls_layer.hpp"

namespace nlr {

namespace {

// SOP class UIDs we accept C-STORE for. This is the common-modality set —
// CT, MR, CR/DX/MG/US/NM/XA, plus secondary capture and presentation states.
// Expand as new modalities show up; nothing here is load-bearing on the
// schema, just on what the SCP advertises during association negotiation.
const std::vector<const char*> kAcceptedStorageSopClasses = {
    UID_CTImageStorage,
    UID_MRImageStorage,
    UID_ComputedRadiographyImageStorage,
    UID_DigitalXRayImageStorageForPresentation,
    UID_DigitalXRayImageStorageForProcessing,
    UID_DigitalMammographyXRayImageStorageForPresentation,
    UID_DigitalMammographyXRayImageStorageForProcessing,
    UID_UltrasoundImageStorage,
    UID_UltrasoundMultiframeImageStorage,
    UID_NuclearMedicineImageStorage,
    UID_XRayAngiographicImageStorage,
    UID_XRayRadiofluoroscopicImageStorage,
    UID_PositronEmissionTomographyImageStorage,
    UID_SecondaryCaptureImageStorage,
    UID_MultiframeSingleBitSecondaryCaptureImageStorage,
    UID_MultiframeGrayscaleByteSecondaryCaptureImageStorage,
    UID_MultiframeGrayscaleWordSecondaryCaptureImageStorage,
    UID_MultiframeTrueColorSecondaryCaptureImageStorage,
    UID_EnhancedCTImageStorage,
    UID_EnhancedMRImageStorage,
    UID_EnhancedXAImageStorage,
    UID_EnhancedXRFImageStorage,
    UID_BreastTomosynthesisImageStorage,
    UID_RTImageStorage,
    UID_RTDoseStorage,
    UID_RTStructureSetStorage,
    UID_RTPlanStorage,
    UID_GrayscaleSoftcopyPresentationStateStorage,
    UID_ColorSoftcopyPresentationStateStorage,
    UID_KeyObjectSelectionDocumentStorage,
    UID_EncapsulatedPDFStorage,
    UID_EncapsulatedCDAStorage,
};

// Transfer syntaxes we accept. Required: implicit + explicit little-endian.
// Common compressed forms are advertised so most modalities can negotiate
// their native syntax without transcoding on send.
const std::vector<const char*> kAcceptedTransferSyntaxes = {
    UID_LittleEndianExplicitTransferSyntax,
    UID_LittleEndianImplicitTransferSyntax,
    UID_DeflatedExplicitVRLittleEndianTransferSyntax,
    UID_JPEGProcess1TransferSyntax,
    UID_JPEGProcess2_4TransferSyntax,
    UID_JPEGProcess14SV1TransferSyntax,
    UID_JPEGProcess14TransferSyntax,
    UID_JPEGLSLosslessTransferSyntax,
    UID_JPEGLSLossyTransferSyntax,
    UID_JPEG2000LosslessOnlyTransferSyntax,
    UID_JPEG2000TransferSyntax,
    UID_RLELosslessTransferSyntax,
};

}  // namespace

Server::Server(const Config& cfg, Db& db, const ReceiverMetrics& metrics,
                const DiskGuard* disk_guard, TlsLayer* tls_layer)
    : cfg_(cfg), db_(db), metrics_(metrics)
{
    // Wire up the per-process context the pool workers share. Done
    // exactly once on the main thread before any pool starts accepting.
    AssociationHandler::init_shared_context(cfg_, db_, metrics_, disk_guard);

    // ---- Plain pool -----------------------------------------------------
    plain_pool_ = std::make_unique<PlainPool>();
    plain_pool_->setMaxThreads(cfg_.max_associations);
    configure_pool_config_(plain_pool_->getConfig(), cfg_.listen_port);

    // ---- TLS pool (optional) -------------------------------------------
    if (cfg_.tls_enabled && tls_layer != nullptr) {
        tls_pool_ = std::make_unique<TlsPool>();
        tls_pool_->setMaxThreads(cfg_.max_associations);
        configure_pool_config_(tls_pool_->getConfig(), cfg_.tls_listen_port);
        // setTransportLayer is non-owning; TlsLayer outlives this Server.
        tls_pool_->getConfig().setTransportLayer(tls_layer->layer());
    }

    // workers_total is a static gauge — sum of both pools' max sizes.
    // workers_busy ticks inside each handler's notify/terminate hooks
    // (incremented on association accept, decremented at end) so this
    // covers both plain and TLS naturally.
    const std::int64_t total = static_cast<std::int64_t>(cfg_.max_associations)
                                + (tls_pool_ ? cfg_.max_associations : 0u);
    metrics_.workers_total.self().set(total);
}

Server::~Server() = default;

void Server::configure_pool_config_(DcmSCPConfig& scfg, std::uint16_t port) {
    scfg.setAETitle(cfg_.host_aet.c_str());
    scfg.setPort(port);
    scfg.setMaxReceivePDULength(cfg_.max_pdu_size);
    scfg.setConnectionBlockingMode(DUL_BLOCK);
    scfg.setACSETimeout(cfg_.association_timeout_s);
    scfg.setDIMSETimeout(cfg_.association_timeout_s);
    scfg.setDIMSEBlockingMode(DIMSE_BLOCKING);

    // Disable reverse-DNS on accepted connections. By default DcmSCP
    // returns a hostname via getPeerIP() (it runs gethostbyaddr). We need
    // a numeric IP so it can be inserted into the work_queue.peer_ip
    // INET column without further lookup.
    scfg.setHostLookupEnabled(OFFalse);

    // C-ECHO (Verification): accept on default transfer syntaxes.
    OFList<OFString> echo_ts;
    echo_ts.push_back(UID_LittleEndianExplicitTransferSyntax);
    echo_ts.push_back(UID_LittleEndianImplicitTransferSyntax);
    scfg.addPresentationContext(UID_VerificationSOPClass, echo_ts);

    // C-STORE: each accepted SOP class gets the full transfer-syntax list.
    OFList<OFString> xfer_list;
    for (auto* ts : kAcceptedTransferSyntaxes) xfer_list.push_back(ts);
    for (auto* sop : kAcceptedStorageSopClasses) {
        scfg.addPresentationContext(sop, xfer_list);
    }

    LOG_INFO("scp.configure",
        "aet",                cfg_.host_aet,
        "port",               std::to_string(port),
        "sop_classes",        std::to_string(kAcceptedStorageSopClasses.size()),
        "transfer_syntaxes",  std::to_string(kAcceptedTransferSyntaxes.size()),
        "max_pdu",            std::to_string(cfg_.max_pdu_size),
        "max_associations",   std::to_string(cfg_.max_associations));
}

int Server::drive_pool_(DcmBaseSCPPool& pool, const char* tag) {
    // DcmSCPPool::listen() is a blocking loop: it spawns workers on
    // demand, accepts on the listening socket, dispatches each accepted
    // association to an idle worker, and only returns on a fatal
    // network error. We surface that condition; the caller turns it into
    // an exit code.
    const OFCondition cond = pool.listen();
    if (cond.bad()) {
        LOG_WARN("scp.pool_listen_error",
            "listener", tag,
            "error",    cond.text() ? cond.text() : "unknown");
        return 2;
    }
    return 0;
}

int Server::run() {
    LOG_INFO("scp.listen",
        "host_aet",         cfg_.host_aet,
        "port",             std::to_string(cfg_.listen_port),
        "peer_type",        "plain",
        "max_associations", std::to_string(cfg_.max_associations));

    if (tls_pool_) {
        LOG_INFO("scp.listen",
            "host_aet",         cfg_.host_aet,
            "port",             std::to_string(cfg_.tls_listen_port),
            "peer_type",        "tls",
            "max_associations", std::to_string(cfg_.max_associations));
        tls_thread_ = std::thread([this] {
            (void)drive_pool_(*tls_pool_, "tls");
        });
    }

    const int plain_rc = drive_pool_(*plain_pool_, "plain");

    stop_requested_.store(true, std::memory_order_relaxed);
    if (tls_thread_.joinable()) tls_thread_.join();

    LOG_INFO("scp.shutdown");
    return plain_rc;
}

void Server::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    // DcmSCPPool exposes stopAfterCurrentAssociations() for graceful
    // shutdown — current associations finish, no new ones accepted.
    if (plain_pool_) plain_pool_->stopAfterCurrentAssociations();
    if (tls_pool_)   tls_pool_->stopAfterCurrentAssociations();
}

}  // namespace nlr
