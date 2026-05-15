// nl-dispatch/dicomweb_stow_handler.hpp
//
// DICOMweb STOW-RS dispatcher (DICOM PS3.18 §10.5).
//
// Builds a multipart/related body containing every .dcm instance under
// the study root and POSTs it to a DICOMweb-compatible endpoint. The
// peer returns a DICOM JSON or XML status report; we summarize it into
// route_assignments.response_detail.
//
// Destination config (JSONB):
//   {
//     "url":              "https://pacs.example.com/dicom-web/studies",
//     "accept":           "application/dicom+json",          // optional
//     "transfer_syntax":  "1.2.840.10008.1.2.1",             // optional; advertised in C-T
//     "timeout_s":        120                                 // optional
//   }
//
// Credentials: any HTTP-shaped kind (basic_http, bearer_token, api_key,
// mtls in a follow-up). Service-account JSON is not appropriate here —
// use the `gcp_dicomweb` destination kind instead, which performs the
// OAuth2 token exchange.
//
// Errors:
//   curl error    → transient
//   200/202        → success (200 = all stored, 202 = partial)
//   400/409 etc.   → transient with config-issue note (retry policy
//                     give_up_after_hours bounds them)
//   5xx            → transient
//   No instances on disk → permanent

#pragma once

#include "handler.hpp"

namespace nlr {

class DicomwebStowDispatchHandler final : public DispatchHandler {
public:
    DicomwebStowDispatchHandler();
    ~DicomwebStowDispatchHandler() override;

    DispatchResult dispatch(const Assignment& a, const Destination& d) override;
};

}  // namespace nlr
