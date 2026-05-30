// nl-dcm-probe — DICOM C-ECHO probe.
//
// Shell-out target for the management API's destination
// test-connection feature. M19 originally landed the test-connection
// button with a TCP-only probe for the `dicom` kind because the API
// process doesn't link DCMTK. M31 closes the gap with this tiny CLI:
// statically linked against the same DCMTK as the daemons, takes a
// config on argv, opens a real DIMSE association and sends a
// C-ECHO, exits 0 / 1 / 2.
//
// Contract:
//   argv     : <host> <port> <called_aet> <calling_aet>
//                [timeout_s] [max_pdu_size]
//              Defaults: timeout_s=8, max_pdu_size=131072.
//   stdout   : nothing (caller measures wall-clock by timing the
//              subprocess; the probe just runs and exits)
//   stderr   : single-line error message on failure
//   exit 0   : association opened, C-ECHO accepted, released cleanly
//   exit 1   : DIMSE failure (peer reachable but C-ECHO rejected /
//              association refused / negotiation failed / etc.) —
//              stderr has the reason
//   exit 2   : usage / I/O / internal error
//
// Deliberately small — no TLS, no auth, no JSON parsing. The
// test-connection button uses this as a "is the peer reachable AND
// does it speak DIMSE Verification?" probe. TLS for dispatch is
// deferred (design plan); when it lands we extend this contract.
// Likewise no preferred-transfer-syntaxes — Verification only
// requires Implicit VR LE, and offering more would be wasted PDUs.

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/scu.h>
#include <dcmtk/ofstd/ofcond.h>
#include <dcmtk/oflog/oflog.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr int kDefaultMaxPduSize = 131072;
constexpr int kDefaultTimeoutS   = 8;

int fail(int code, const std::string& msg) {
    std::cerr << msg << '\n';
    return code;
}

int parse_int_strict(const char* s) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (s == end || (end && *end != '\0') || v < 0 || v > 0x7FFFFFFF) {
        return -1;
    }
    return static_cast<int>(v);
}

}  // namespace

int main(int argc, char** argv) {
    // ---- 0. Silence DCMTK's stderr logger -------------------------------
    // The dispatcher's daemon logs DIMSE-level activity on purpose,
    // but a one-shot probe should be silent on success. The contract
    // says stderr is empty unless we failed; OFLOG defaults to INFO
    // on stderr which would otherwise emit "I: Requesting Association"
    // and friends.
    OFLog::configure(OFLogger::FATAL_LOG_LEVEL);

    // ---- 1. argv ---------------------------------------------------------
    if (argc < 5 || argc > 7) {
        return fail(2,
            "usage: nl-dcm-probe <host> <port> <called_aet> <calling_aet> "
            "[timeout_s] [max_pdu_size]");
    }

    const std::string host        = argv[1];
    const int         port        = parse_int_strict(argv[2]);
    const std::string called_aet  = argv[3];
    const std::string calling_aet = argv[4];
    const int timeout_s = (argc >= 6) ? parse_int_strict(argv[5]) : kDefaultTimeoutS;
    const int max_pdu_size = (argc >= 7) ? parse_int_strict(argv[6]) : kDefaultMaxPduSize;

    if (host.empty() || port <= 0 || port > 65535 ||
        called_aet.empty() || calling_aet.empty() ||
        timeout_s <= 0 || max_pdu_size <= 0) {
        return fail(2, "invalid argument (host non-empty, port 1-65535, AETs non-empty, "
                       "timeout_s>0, max_pdu_size>0)");
    }

    // ---- 2. Configure the SCU -------------------------------------------
    DcmSCU scu;
    scu.setPeerHostName(host.c_str());
    scu.setPeerPort(static_cast<Uint16>(port));
    scu.setPeerAETitle(called_aet.c_str());
    scu.setAETitle(calling_aet.c_str());
    scu.setMaxReceivePDULength(static_cast<Uint32>(max_pdu_size));
    scu.setACSETimeout(timeout_s);
    scu.setConnectionTimeout(timeout_s);
    scu.setDIMSEBlockingMode(DIMSE_NONBLOCKING);
    scu.setDIMSETimeout(timeout_s);

    // Verification SOP Class with Implicit VR LE — the universal
    // transfer syntax every C-ECHO-capable peer must accept.
    OFList<OFString> xfers;
    xfers.push_back(OFString{UID_LittleEndianImplicitTransferSyntax});
    OFCondition cond = scu.addPresentationContext(UID_VerificationSOPClass, xfers);
    if (cond.bad()) {
        return fail(2, std::string{"addPresentationContext: "} +
                       (cond.text() ? cond.text() : "unknown"));
    }

    // ---- 3. Open the association ----------------------------------------
    cond = scu.initNetwork();
    if (cond.bad()) {
        return fail(1, std::string{"initNetwork: "} +
                       (cond.text() ? cond.text() : "unknown"));
    }

    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        const std::string err = cond.text() ? cond.text() : "unknown";
        // Try to close gracefully even though we don't have a valid
        // association — DCMTK accepts this and just cleans up state.
        scu.closeAssociation(DCMSCU_RELEASE_ASSOCIATION);
        return fail(1, "negotiateAssociation: " + err);
    }

    // Verify Verification was actually accepted (peer can refuse it
    // even if the association comes up). Look up the presentation
    // context for Verification and make sure DCMTK returned a non-zero
    // id (= peer accepted it).
    T_ASC_PresentationContextID pc =
        scu.findPresentationContextID(UID_VerificationSOPClass, "");
    if (pc == 0) {
        scu.releaseAssociation();
        return fail(1, "peer accepted association but refused Verification SOP class");
    }

    // ---- 4. Send C-ECHO -------------------------------------------------
    cond = scu.sendECHORequest(pc);
    if (cond.bad()) {
        const std::string err = cond.text() ? cond.text() : "unknown";
        scu.releaseAssociation();
        return fail(1, "C-ECHO: " + err);
    }

    // ---- 5. Release the association -------------------------------------
    scu.releaseAssociation();
    return 0;
}
