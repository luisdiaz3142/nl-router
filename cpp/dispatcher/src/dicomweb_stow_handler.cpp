// Thin wrapper around the shared `run_stow_upload`. The handler itself
// only knows how to:
//   * parse the destination config
//   * resolve the credential into HTTP headers (via the shared helper)
//   * map StowUploadResult kinds into DispatchResult statuses

#include "dicomweb_stow_handler.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <string>

#include "credential.hpp"
#include "handler.hpp"
#include "logging.hpp"
#include "stow_upload.hpp"

namespace nlr {

DicomwebStowDispatchHandler::DicomwebStowDispatchHandler() {
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

DicomwebStowDispatchHandler::~DicomwebStowDispatchHandler() = default;

DispatchResult DicomwebStowDispatchHandler::dispatch(
    const Assignment& a, const Destination& d)
{
    // ---- Parse handler-specific config ----
    std::string url;
    std::string accept           = "application/dicom+json";
    std::string transfer_syntax;
    long timeout_s               = 120;
    try {
        const auto j = nlohmann::json::parse(d.config_json);
        if (!j.contains("url") || !j["url"].is_string()) {
            return DispatchResult::permanent("dicomweb_stow destination missing 'url'");
        }
        url = j["url"].get<std::string>();
        if (j.contains("accept") && j["accept"].is_string()) {
            accept = j["accept"].get<std::string>();
        }
        if (j.contains("transfer_syntax") && j["transfer_syntax"].is_string()) {
            transfer_syntax = j["transfer_syntax"].get<std::string>();
        }
        if (j.contains("timeout_s") && j["timeout_s"].is_number()) {
            timeout_s = j["timeout_s"].get<long>();
        }
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"dicomweb_stow destination config invalid: "} + e.what());
    }

    // ---- Build auth headers from the assignment's decrypted credential ----
    // url may be mutated for api_key with query_param shape.
    struct curl_slist* auth_headers = nullptr;
    {
        const bool url_has_query = url.find('?') != std::string::npos;
        auto cred = apply_credential_to_request(a, auth_headers, url_has_query);
        auth_headers = cred.headers;
        if (!cred.append_to_url.empty()) url.append(cred.append_to_url);
    }

    // ---- Hand off to the shared STOW core ----
    StowUploadParams params;
    params.url             = std::move(url);
    params.accept          = std::move(accept);
    params.transfer_syntax = std::move(transfer_syntax);
    params.timeout_s       = timeout_s;
    params.study_file_root = a.study_file_root;
    params.auth_headers    = auth_headers;
    const auto res = run_stow_upload(params);

    if (auth_headers) curl_slist_free_all(auth_headers);

    // ---- Translate StowUploadResult → DispatchResult ----
    const auto detail = res.to_response_detail_json();
    switch (res.kind) {
        case StowUploadResult::Kind::NoFiles:
        case StowUploadResult::Kind::ReadAllFailed:
            return DispatchResult::permanent(res.error_message, detail);
        case StowUploadResult::Kind::CurlError:
        case StowUploadResult::Kind::Partial:
        case StowUploadResult::Kind::ClientError:
        case StowUploadResult::Kind::ServerError:
            return DispatchResult::transient(res.error_message, detail);
        case StowUploadResult::Kind::Success:
            return DispatchResult::success(detail);
    }
    return DispatchResult::transient("unknown stow upload state", detail);
}

}  // namespace nlr
