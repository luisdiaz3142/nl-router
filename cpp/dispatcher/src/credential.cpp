#include "credential.hpp"

#include <nlohmann/json.hpp>

#include <string>

#include "logging.hpp"
#include "template.hpp"

namespace nlr {

namespace {

// RFC 4648 base64 (with padding). Mirrors the inline encoder formerly in
// http_handler.cpp; kept here so both HTTP handlers share one implementation.
std::string base64_encode(const std::string& in) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(alpha[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(alpha[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4 != 0) out.push_back('=');
    return out;
}

}  // namespace

CredentialApplyResult apply_credential_to_request(
    const Assignment&     a,
    struct curl_slist*    headers,
    bool                  current_url_has_query)
{
    CredentialApplyResult res;
    res.headers = headers;

    if (a.credential_kind.empty()) return res;
    if (a.credential_payload.empty()) {
        LOG_WARN("dispatch.cred_no_payload",
            "assignment_id", std::to_string(a.id),
            "kind",          a.credential_kind);
        return res;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(a.credential_payload);
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("dispatch.cred_parse_failed",
            "assignment_id", std::to_string(a.id),
            "error",         e.what());
        return res;
    }

    if (a.credential_kind == "basic_http") {
        const auto user = j.value("username", "");
        const auto pass = j.value("password", "");
        const std::string token = base64_encode(user + ":" + pass);
        const std::string hdr = "Authorization: Basic " + token;
        res.headers = curl_slist_append(res.headers, hdr.c_str());
    } else if (a.credential_kind == "bearer_token") {
        const auto tok = j.value("token", "");
        const std::string hdr = "Authorization: Bearer " + tok;
        res.headers = curl_slist_append(res.headers, hdr.c_str());
    } else if (a.credential_kind == "api_key") {
        const auto val = j.value("value", "");
        if (j.contains("header") && j["header"].is_string()) {
            const std::string hdr = j["header"].get<std::string>() + ": " + val;
            res.headers = curl_slist_append(res.headers, hdr.c_str());
        } else if (j.contains("query_param") && j["query_param"].is_string()) {
            const auto key = j["query_param"].get<std::string>();
            std::string suffix;
            suffix.push_back(current_url_has_query ? '&' : '?');
            suffix.append(url_encode(key));
            suffix.push_back('=');
            suffix.append(url_encode(val));
            res.append_to_url = std::move(suffix);
        } else {
            LOG_WARN("dispatch.cred_api_key_missing_location",
                "assignment_id", std::to_string(a.id));
        }
    } else if (a.credential_kind == "gcp_service_account") {
        // gcp_dicomweb handler implements its own OAuth2 flow — never
        // attach a service-account JSON directly to a generic HTTP call.
        LOG_WARN("dispatch.cred_gcp_for_generic_http",
            "assignment_id", std::to_string(a.id),
            "advice",        "gcp_service_account credentials should be paired with the gcp_dicomweb destination kind");
    } else {
        // mtls_cert, tls_cert: caller-specific wiring (CURLOPT_SSLCERT
        // etc.) lands when we need it. Not silently dropped — log so
        // operators see the misconfiguration.
        LOG_WARN("dispatch.cred_unsupported_kind",
            "assignment_id", std::to_string(a.id),
            "kind",          a.credential_kind);
    }
    return res;
}

}  // namespace nlr
