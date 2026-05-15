#include "tls_layer.hpp"

#include <dcmtk/dcmtls/tlsciphr.h>
#include <dcmtk/dcmtls/tlsdefin.h>
#include <dcmtk/dcmnet/assoc.h>

#include <stdexcept>

#include "logging.hpp"

namespace nlr {

namespace {

// Map our string knob to DCMTK's enum. Default is the modern RFC 8996
// profile (TLS 1.2 minimum, modern ciphers). Operators rarely override.
DcmTLSSecurityProfile parse_profile(const std::string& s) {
    if (s == "bcp195_rfc8996")      return TSP_Profile_BCP_195_RFC_8996;
    if (s == "bcp195_rfc8996_mod")  return TSP_Profile_BCP_195_RFC_8996_Modified;
    if (s == "bcp195_nd")           return TSP_Profile_BCP195_ND;
    if (s == "bcp195_ex")           return TSP_Profile_BCP195_Extended;
    if (s == "bcp195")              return TSP_Profile_BCP195;   // retired but kept for compat
    throw std::runtime_error("unknown tls.profile: " + s);
}

// All DCMTK file-format paths in the receiver are PEM; we don't expose
// ASN1/DER as a config knob because almost no operator ships keys that
// way and it would be one more cliff for misconfig.
constexpr DcmKeyFileFormat kKeyFormat = DCF_Filetype_PEM;

// Helper: throw if the OFCondition is bad, with the operation name in
// the message so failures point straight at the misconfigured field.
void check(const OFCondition& cond, const char* op) {
    if (cond.bad()) {
        throw std::runtime_error(
            std::string{"tls "} + op + ": " +
            (cond.text() ? cond.text() : "unknown error"));
    }
}

}  // namespace

TlsLayer::TlsLayer(const TlsConfig& cfg) {
    if (!cfg.enabled) {
        throw std::logic_error("TlsLayer constructed with enabled=false");
    }
    if (cfg.cert_file.empty() || cfg.key_file.empty()) {
        throw std::runtime_error(
            "tls.cert_file and tls.key_file are required when tls.enabled=true");
    }

    // Acceptor role — we're the SCP. RAND file is OpenSSL's seed source;
    // nullptr lets DCMTK use the OS default (/dev/urandom or equivalent).
    tls_ = std::make_unique<DcmTLSTransportLayer>(NET_ACCEPTOR, nullptr, OFTrue);
    // DcmTLSTransportLayer exposes init status via operator!() (no
    // explicit method); !*tls_ is true when the OpenSSL context failed.
    if (!*tls_) {
        throw std::runtime_error("DcmTLSTransportLayer init failed (OpenSSL?)");
    }

    const DcmTLSSecurityProfile profile = parse_profile(cfg.profile);

    // Profile must be set BEFORE addCipherSuite/activateCipherSuites; it
    // also affects which TLS versions are negotiated. setCertificateFile
    // takes the profile so DCMTK can validate key/cert algorithm against
    // the cipher set.
    check(tls_->setTLSProfile(profile), "setTLSProfile");

    check(tls_->setPrivateKeyFile(cfg.key_file.c_str(), kKeyFormat),
          "setPrivateKeyFile");
    check(tls_->setCertificateFile(cfg.cert_file.c_str(), kKeyFormat, profile),
          "setCertificateFile");

    if (!tls_->checkPrivateKeyMatchesCertificate()) {
        throw std::runtime_error(
            "tls.key_file does not match tls.cert_file (public/private key mismatch)");
    }

    if (cfg.require_client_cert) {
        if (cfg.ca_file.empty()) {
            throw std::runtime_error(
                "tls.require_client_cert=true requires tls.ca_file");
        }
        check(tls_->addTrustedCertificateFile(cfg.ca_file.c_str(), kKeyFormat),
              "addTrustedCertificateFile");
        tls_->setCertificateVerification(DCV_requireCertificate);
    } else if (!cfg.ca_file.empty()) {
        // CA file provided but mTLS not required: load it anyway so the
        // server can verify presented client certs opportunistically.
        check(tls_->addTrustedCertificateFile(cfg.ca_file.c_str(), kKeyFormat),
              "addTrustedCertificateFile");
        tls_->setCertificateVerification(DCV_checkCertificate);
    } else {
        tls_->setCertificateVerification(DCV_ignoreCertificate);
    }

    // Finalize the cipher list now that the profile + cert are set.
    check(tls_->activateCipherSuites(), "activateCipherSuites");

    LOG_INFO("tls.layer_initialized",
        "profile",            cfg.profile,
        "require_client_cert", cfg.require_client_cert ? std::string{"true"}
                                                       : std::string{"false"},
        "cert_file",          cfg.cert_file,
        "key_file",           cfg.key_file,
        "ca_file",            cfg.ca_file.empty() ? std::string{"-"} : cfg.ca_file);
}

TlsLayer::~TlsLayer() = default;

DcmTransportLayer* TlsLayer::layer() noexcept {
    return tls_.get();
}

}  // namespace nlr
