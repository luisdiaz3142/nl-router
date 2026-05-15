// nl-dispatch/gcp_dicomweb_handler.hpp
//
// GCP Healthcare API DICOMweb STOW-RS dispatcher.
//
// Destination config:
//   {
//     "project_id":  "my-gcp-project",
//     "location":    "us-central1",
//     "dataset":     "radiology",
//     "dicom_store": "main",
//     "scope":       "https://www.googleapis.com/auth/cloud-platform",
//     "endpoint_base": "https://healthcare.googleapis.com/v1"  (optional)
//   }
//
// Credential: a `gcp_service_account` credential whose payload is the
// JSON file downloaded from GCP. Authority is the destination's
// credential_id; the handler exchanges the service-account JSON for a
// short-lived OAuth2 access token at dispatch time and caches it until
// just before expiry.

#pragma once

#include "gcp_oauth.hpp"
#include "handler.hpp"

namespace nlr {

class GcpDicomwebDispatchHandler final : public DispatchHandler {
public:
    GcpDicomwebDispatchHandler();
    ~GcpDicomwebDispatchHandler() override;

    DispatchResult dispatch(const Assignment& a, const Destination& d) override;

private:
    // One token cache per handler instance (== per worker thread). The
    // cache key incorporates the service-account JSON hash + scope, so
    // rotation or scope change invalidates automatically.
    GcpTokenCache token_cache_;
};

}  // namespace nlr
