#include "http_handler.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "credential.hpp"
#include "handler.hpp"
#include "logging.hpp"
#include "template.hpp"

namespace nlr {

namespace {

// libcurl write callback — accumulate response bytes into a std::string.
std::size_t write_to_string(void* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

}  // namespace

HttpDispatchHandler::HttpDispatchHandler() {
    // Lazy-initialize libcurl globals once. curl_global_cleanup() is not
    // called — libcurl's docs explicitly note it's optional and racy with
    // other threads using libcurl (which our worker pool will).
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

HttpDispatchHandler::~HttpDispatchHandler() = default;

DispatchResult HttpDispatchHandler::dispatch(const Assignment& a, const Destination& d) {
    // ---- Parse config ----
    std::string url_template, body_template;
    std::string method = "POST";
    long timeout_s = 30;
    nlohmann::json headers_json;
    try {
        const auto j = nlohmann::json::parse(d.config_json);
        if (!j.contains("url_template") || !j["url_template"].is_string()) {
            return DispatchResult::permanent("http destination missing 'url_template'");
        }
        url_template = j["url_template"].get<std::string>();
        if (j.contains("method") && j["method"].is_string()) {
            method = j["method"].get<std::string>();
        }
        if (j.contains("body_template") && j["body_template"].is_string()) {
            body_template = j["body_template"].get<std::string>();
        }
        if (j.contains("headers") && j["headers"].is_object()) {
            headers_json = j["headers"];
        }
        if (j.contains("timeout_s") && j["timeout_s"].is_number()) {
            timeout_s = j["timeout_s"].get<long>();
        }
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"http destination config invalid: "} + e.what());
    }

    // ---- Expand templates ----
    std::string url  = expand_template(url_template,  a.tags_json);
    std::string body = expand_template(body_template, a.tags_json);
    if (url.empty()) {
        return DispatchResult::permanent("url_template expanded to empty string");
    }

    // ---- Build curl easy handle ----
    CURL* curl = curl_easy_init();
    if (!curl) {
        return DispatchResult::transient("curl_easy_init failed");
    }
    struct curl_slist* headers_list = nullptr;

    // Custom headers from config first; credential header may overwrite Authorization.
    for (auto it = headers_json.begin(); it != headers_json.end(); ++it) {
        if (it.value().is_string()) {
            const std::string hdr = it.key() + ": " + it.value().get<std::string>();
            headers_list = curl_slist_append(headers_list, hdr.c_str());
        }
    }
    {
        const bool url_has_query = url.find('?') != std::string::npos;
        auto cred = apply_credential_to_request(a, headers_list, url_has_query);
        headers_list = cred.headers;
        if (!cred.append_to_url.empty()) url.append(cred.append_to_url);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(timeout_s, 10));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    if (headers_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // ---- Execute ----
    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    char err_buf[CURL_ERROR_SIZE] = {};
    if (rc != CURLE_OK) {
        std::strncpy(err_buf, curl_easy_strerror(rc), sizeof(err_buf) - 1);
    }

    if (headers_list) curl_slist_free_all(headers_list);
    curl_easy_cleanup(curl);

    // ---- Interpret outcome ----
    std::ostringstream detail;
    detail << "{\"http_status\":" << http_status
           << ",\"curl_code\":" << static_cast<int>(rc)
           << ",\"body_bytes\":" << response_body.size()
           << "}";

    if (rc != CURLE_OK) {
        // Network-layer failure (DNS, connect, TLS): retry.
        return DispatchResult::transient(
            std::string{"curl: "} + err_buf, detail.str());
    }
    if (http_status >= 200 && http_status < 300) {
        return DispatchResult::success(detail.str());
    }
    if (http_status >= 400 && http_status < 500) {
        // 4xx is a client error — retrying won't help unless the operator
        // fixes the config / credential. Mark transient so the retry
        // policy's give_up_after_hours backstop still kicks in, but
        // emit a clear error message so it's obvious in the audit log.
        return DispatchResult::transient(
            "http " + std::to_string(http_status) + " — config/credential issue",
            detail.str());
    }
    // 5xx and unexpected — transient.
    return DispatchResult::transient(
        "http " + std::to_string(http_status), detail.str());
}

}  // namespace nlr
