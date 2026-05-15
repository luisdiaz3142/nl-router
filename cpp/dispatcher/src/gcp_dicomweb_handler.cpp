#include "gcp_dicomweb_handler.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <string>

#include "handler.hpp"
#include "gcp_oauth.hpp"
#include "logging.hpp"
#include "stow_upload.hpp"

namespace nlr {

namespace {

// Compose the GCP Healthcare DICOMweb /studies URL.
//
// Default endpoint base is healthcare.googleapis.com/v1. Operators with
// regional or beta endpoints set endpoint_base in config.
std::string compose_studies_url(const std::string& endpoint_base,
                                  const std::string& project_id,
                                  const std::string& location,
                                  const std::string& dataset,
                                  const std::string& dicom_store) {
    return endpoint_base +
           "/projects/"      + project_id +
           "/locations/"     + location +
           "/datasets/"      + dataset +
           "/dicomStores/"   + dicom_store +
           "/dicomWeb/studies";
}

}  // namespace

GcpDicomwebDispatchHandler::GcpDicomwebDispatchHandler() {
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

GcpDicomwebDispatchHandler::~GcpDicomwebDispatchHandler() = default;

DispatchResult GcpDicomwebDispatchHandler::dispatch(
    const Assignment& a, const Destination& d)
{
    // ---- 1) Parse config ----
    std::string project_id, location, dataset, dicom_store;
    std::string scope         = "https://www.googleapis.com/auth/cloud-platform";
    std::string endpoint_base = "https://healthcare.googleapis.com/v1";
    std::string transfer_syntax;
    long timeout_s            = 120;
    try {
        const auto j = nlohmann::json::parse(d.config_json);
        project_id  = j.value("project_id",  "");
        location    = j.value("location",    "");
        dataset     = j.value("dataset",     "");
        dicom_store = j.value("dicom_store", "");
        if (j.contains("scope") && j["scope"].is_string()) {
            scope = j["scope"].get<std::string>();
        }
        if (j.contains("endpoint_base") && j["endpoint_base"].is_string()) {
            endpoint_base = j["endpoint_base"].get<std::string>();
        }
        if (j.contains("transfer_syntax") && j["transfer_syntax"].is_string()) {
            transfer_syntax = j["transfer_syntax"].get<std::string>();
        }
        if (j.contains("timeout_s") && j["timeout_s"].is_number()) {
            timeout_s = j["timeout_s"].get<long>();
        }
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"gcp_dicomweb destination config invalid: "} + e.what());
    }
    if (project_id.empty() || location.empty() || dataset.empty() || dicom_store.empty()) {
        return DispatchResult::permanent(
            "gcp_dicomweb destination requires project_id, location, dataset, dicom_store");
    }

    // ---- 2) Validate credential ----
    if (a.credential_payload.empty() || a.credential_kind != "gcp_service_account") {
        return DispatchResult::permanent(
            "gcp_dicomweb requires a credential of kind 'gcp_service_account'");
    }

    // ---- 3) Acquire OAuth2 access token (cached) ----
    std::string access_token;
    try {
        access_token = token_cache_.get(a.credential_payload, scope);
    } catch (const GcpOAuthError& e) {
        // Auth failure: classify as transient — token endpoint outages
        // do happen, and the retry policy's give_up_after_hours backstop
        // bounds the wait if it's a real misconfiguration.
        return DispatchResult::transient(
            std::string{"gcp_oauth: "} + e.what());
    } catch (const std::exception& e) {
        return DispatchResult::transient(
            std::string{"gcp_oauth: "} + e.what());
    }

    // ---- 4) Build auth header for STOW ----
    struct curl_slist* auth_headers = nullptr;
    {
        const std::string hdr = "Authorization: Bearer " + access_token;
        auth_headers = curl_slist_append(auth_headers, hdr.c_str());
    }

    // ---- 5) Hand off to the shared STOW core ----
    StowUploadParams params;
    params.url             = compose_studies_url(
        endpoint_base, project_id, location, dataset, dicom_store);
    params.accept          = "application/dicom+json";
    params.transfer_syntax = std::move(transfer_syntax);
    params.timeout_s       = timeout_s;
    params.study_file_root = a.study_file_root;
    params.auth_headers    = auth_headers;
    const auto res = run_stow_upload(params);

    if (auth_headers) curl_slist_free_all(auth_headers);

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
