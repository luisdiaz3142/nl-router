// nl-dispatch/http_handler.hpp
//
// Generic HTTP webhook dispatcher. Sends one HTTP request per study
// (NOT per instance — operators expecting per-instance webhooks should
// configure the receiver to emit one work_queue row per instance, or
// build a custom destination kind).
//
// Destination config (JSONB):
//   {
//     "url_template":  "https://hook.example.com/dicom/${StudyInstanceUID}",
//     "method":        "POST",
//     "headers":       {"Content-Type":"application/json", "X-Site":"main"},
//     "body_template": "{\"study_uid\":\"${StudyInstanceUID}\","
//                      " \"patient_id\":\"${PatientID}\"}",
//     "include_dicom_files": false,    // future; multipart upload
//     "timeout_s":     30
//   }
//
// Credential (optional):
//   * basic_http   → Authorization: Basic <base64(user:pass)>
//   * bearer_token → Authorization: Bearer <token>
//   * api_key      → custom header or query param
//   * mTLS / other → deferred

#pragma once

#include "handler.hpp"

namespace nlr {

class HttpDispatchHandler final : public DispatchHandler {
public:
    HttpDispatchHandler();
    ~HttpDispatchHandler() override;

    DispatchResult dispatch(const Assignment& a, const Destination& d) override;
};

}  // namespace nlr
