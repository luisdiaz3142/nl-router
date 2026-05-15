// nl-dispatch/stow_upload.hpp
//
// Shared core of the DICOMweb STOW-RS upload path. Both
// `dicomweb_stow_handler` and `gcp_dicomweb_handler` use this — the
// difference between them is only how the URL and Authorization header
// are produced.
//
// Each call:
//   1. Walks `study_file_root` for `*.dcm`.
//   2. Reads every file into memory.
//   3. Builds a multipart/related body per DICOM PS3.18 §6.6.1.1 / §10.5.
//   4. POSTs to `url` with the caller-supplied auth headers.
//   5. Returns a structured result the handler can translate into a
//      DispatchResult.

#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nlr {

struct StowUploadParams {
    std::string         url;                   // full target URL
    std::string         accept           {"application/dicom+json"};
    std::string         transfer_syntax;       // optional; advertised in C-T
    long                timeout_s        {120};
    std::string         study_file_root;       // local directory of .dcm files

    // Auth headers already built by the caller (e.g. Authorization: Bearer X).
    // We DO NOT touch this list; just append our protocol headers and POST.
    // Caller retains ownership and is responsible for curl_slist_free_all.
    struct curl_slist*  auth_headers     {nullptr};

    // Optional: extra static headers from destination config (e.g. X-Site).
    // Appended verbatim alongside auth_headers.
    // Format: each entry is a complete "Name: value" string.
    std::vector<std::string> extra_headers;
};

struct StowUploadResult {
    // HTTP-layer outcome.
    long          http_status {0};   // 0 if curl_easy_perform failed
    int           curl_code   {0};   // CURLcode as int
    std::string   curl_error;        // populated on curl failure

    // STOW-level outcome.
    int           sent        {0};
    int           succeeded   {0};
    int           failed      {0};
    bool          partial     {false};   // 202 from peer

    // Response info.
    std::size_t   body_bytes  {0};
    std::string   response_content_type;

    // Caller-friendly classification.
    enum class Kind { NoFiles, ReadAllFailed, CurlError, Success, Partial, ClientError, ServerError };
    Kind          kind        {Kind::CurlError};
    std::string   error_message;     // populated for non-Success results

    // JSON serialization for route_assignments.response_detail.
    std::string to_response_detail_json() const;
};

// Run a STOW upload. Returns the structured result. Never throws —
// all failures land in StowUploadResult::kind + error_message.
StowUploadResult run_stow_upload(const StowUploadParams& p);

}  // namespace nlr
