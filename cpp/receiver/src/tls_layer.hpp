// nl-receiver/tls_layer.hpp
//
// Thin RAII wrapper around DcmTLSTransportLayer. Builds + configures the
// layer per the receiver's TLS config; the caller (Server) attaches it
// to a DcmSCPConfig before spinning up the TLS listener thread.
//
// Lifetime: constructed once at startup, lives for the lifetime of the
// receiver process. SIGHUP-triggered hot reload is deferred — needs
// careful interlock with the TLS listener thread; current rebuild path
// is process restart.
//
// We compile this in unconditionally; if DCMTK was built without OpenSSL
// support, the dcmtls headers wouldn't be installed and the build would
// fail with a clear missing-header diagnostic. The CMake gate
// (NL_RECEIVER_WITH_TLS) lets operators on TLS-less DCMTK builds skip
// TLS support entirely.

#pragma once

#include <memory>
#include <string>

#include <dcmtk/dcmtls/tlslayer.h>

namespace nlr {

struct TlsConfig {
    bool         enabled            {false};
    std::uint16_t listen_port       {2762};
    std::string  cert_file;
    std::string  key_file;
    std::string  ca_file;
    bool         require_client_cert {false};
    // One of: "bcp195_rfc8996" (default, modern), "bcp195_rfc8996_mod",
    // "bcp195_nd", "bcp195_ex". Operators rarely need to change this.
    std::string  profile             {"bcp195_rfc8996"};
};

class TlsLayer {
public:
    // Build + initialize a DcmTLSTransportLayer per the config. Throws
    // std::runtime_error with a precise message on any cert/key/profile
    // failure; the receiver fails to start rather than silently running
    // without the TLS the operator asked for.
    explicit TlsLayer(const TlsConfig& cfg);
    ~TlsLayer();

    TlsLayer(const TlsLayer&)            = delete;
    TlsLayer& operator=(const TlsLayer&) = delete;

    // Pointer suitable for DcmSCPConfig::setTransportLayer(). Lifetime
    // is owned by this object — caller must outlive its SCP.
    DcmTransportLayer* layer() noexcept;

private:
    std::unique_ptr<DcmTLSTransportLayer> tls_;
};

}  // namespace nlr
