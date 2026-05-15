#include "server.hpp"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/dcmnet/scp.h>
#include <dcmtk/dcmnet/scpcfg.h>

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
    // Plain listener — always present.
    plain_handler_ = std::make_unique<AssociationHandler>(
        cfg_, db_, metrics_, disk_guard, std::string{"plain"});
    configure_presentation_contexts_(*plain_handler_, cfg_.listen_port);

    // TLS listener — only if explicitly enabled and a layer is provided.
    if (cfg_.tls_enabled && tls_layer != nullptr) {
        tls_handler_ = std::make_unique<AssociationHandler>(
            cfg_, db_, metrics_, disk_guard, std::string{"tls"});
        configure_presentation_contexts_(*tls_handler_, cfg_.tls_listen_port);

        DcmSCPConfig& tls_scfg = tls_handler_->getConfig();
        // setTransportLayer is non-owning; the TlsLayer instance must
        // outlive this handler. main.cpp guarantees that ordering.
        tls_scfg.setTransportLayer(tls_layer->layer());
    }

    // The single-threaded baseline reports one worker per listener.
    // The bounded-pool upgrade (still M10 scope, follow-up commit) will
    // set this to max_associations × #listeners and bump workers_busy
    // from inside the pool dispatch.
    metrics_.workers_total.self().set(tls_handler_ ? 2 : 1);
}

void Server::configure_presentation_contexts_(AssociationHandler& handler,
                                                std::uint16_t port) {
    DcmSCPConfig& scfg = handler.getConfig();
    scfg.setAETitle(cfg_.host_aet.c_str());
    scfg.setPort(port);
    scfg.setMaxReceivePDULength(cfg_.max_pdu_size);
    scfg.setConnectionBlockingMode(DUL_BLOCK);
    scfg.setACSETimeout(cfg_.association_timeout_s);
    scfg.setDIMSETimeout(cfg_.association_timeout_s);
    scfg.setDIMSEBlockingMode(DIMSE_BLOCKING);

    // Disable reverse-DNS on accepted connections. By default DcmSCP returns
    // a hostname via getPeerIP() (it runs gethostbyaddr). We need a numeric
    // IP so it can be inserted into the work_queue.peer_ip INET column
    // without further lookup. Belt-and-suspenders: association_handler also
    // resolves via getaddrinfo if a hostname slips through.
    scfg.setHostLookupEnabled(OFFalse);

    // C-ECHO (Verification): accept on default transfer syntaxes. OFList has
    // no initializer-list constructor; build via push_back.
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
        "max_pdu",            std::to_string(cfg_.max_pdu_size));
}

int Server::run_listener_(AssociationHandler& handler, const char* tag) {
    // DcmSCP::listen() is a single-association blocking loop. It returns
    // when the association ends (success or error); we loop here to accept
    // additional associations until told to stop.
    //
    // DCMTK doesn't expose a stable, version-portable "port unavailable" error
    // code distinct from per-association faults. We treat repeated immediate
    // errors as a fatal listen failure via a small failure counter.
    int consecutive_errors = 0;
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        OFCondition cond = handler.listen();
        if (cond.bad()) {
            LOG_WARN("scp.listen_error",
                "listener", tag,
                "error",    cond.text() ? cond.text() : "unknown");
            if (++consecutive_errors >= 5) {
                LOG_ERROR("scp.listen_giving_up",
                    "listener",           tag,
                    "consecutive_errors", std::to_string(consecutive_errors));
                return 2;
            }
        } else {
            consecutive_errors = 0;
        }
    }
    return 0;
}

int Server::run() {
    LOG_INFO("scp.listen",
        "host_aet", cfg_.host_aet,
        "port",     std::to_string(cfg_.listen_port),
        "peer_type", "plain");

    // If TLS is enabled, spin up a dedicated listener thread. The plain
    // listener runs on the main thread because that's where M1 already
    // ran it; the TLS listener is the new addition. Either listener can
    // accept independently.
    if (tls_handler_) {
        LOG_INFO("scp.listen",
            "host_aet", cfg_.host_aet,
            "port",     std::to_string(cfg_.tls_listen_port),
            "peer_type", "tls");
        tls_thread_ = std::thread([this] {
            (void)run_listener_(*tls_handler_, "tls");
        });
    }

    const int plain_rc = run_listener_(*plain_handler_, "plain");

    // Plain listener returned (stop or fatal). Tell TLS listener to stop
    // and wait for it.
    stop_requested_.store(true, std::memory_order_relaxed);
    if (tls_thread_.joinable()) tls_thread_.join();

    LOG_INFO("scp.shutdown");
    return plain_rc;
}

void Server::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    // DcmSCP doesn't have a clean "stop listening" hook; the next association
    // attempt or the current accept blocking call will return when the OS
    // socket is closed during process shutdown.
}

}  // namespace nlr
