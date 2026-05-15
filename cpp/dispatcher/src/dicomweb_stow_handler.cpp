#include "dicomweb_stow_handler.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "credential.hpp"
#include "handler.hpp"
#include "logging.hpp"

namespace nlr {

namespace {

constexpr const char* kCRLF = "\r\n";

// libcurl write callback — accumulate response body.
std::size_t write_to_string(void* ptr, std::size_t size, std::size_t nmemb,
                             void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Generate a unique multipart boundary string. STOW-RS doesn't care about
// its exact form (DICOM PS3.18 §6.6.1.1 says "any token") but it MUST NOT
// appear inside any of the DICOM file payloads. We use a 32-char random
// hex string prefixed by "nlr-stow-" so collisions are astronomically
// unlikely (~10^-77 per study).
std::string make_boundary() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out = "nlr-stow-";
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) out.push_back(hex[dist(rd)]);
    return out;
}

// Recursively collect every .dcm file under `root`.
std::vector<std::filesystem::path>
collect_dicom_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec))
    {
        if (it->is_regular_file(ec) && it->path().extension() == ".dcm") {
            out.push_back(it->path());
        }
    }
    return out;
}

// Read a file completely into a string. Returns nullopt on failure.
std::optional<std::string> read_file_to_string(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Assemble the multipart/related body. Format per DICOM PS3.18 §6.6.1.1
// and RFC 2387 §3:
//
//   --boundary\r\n
//   Content-Type: application/dicom\r\n
//   Content-Length: N\r\n           (optional but well-formed)
//   \r\n
//   <N bytes of DICOM Part-10 file>
//   \r\n
//   --boundary\r\n
//   ...
//   --boundary--\r\n
//
// We build the whole body in memory. That's fine for typical CT/MR
// studies (up to a few GB) on modern hosts; truly enormous studies
// would benefit from streaming via CURLOPT_READFUNCTION — that's a
// hardening pass after we have real load measurements.
std::string build_multipart_body(
    const std::vector<std::pair<std::filesystem::path, std::string>>& files,
    const std::string& boundary)
{
    std::string body;
    // Coarse reserve — exact size is computable but the few-byte savings
    // aren't worth the bookkeeping for the typical "tens to hundreds of
    // instances" case.
    std::size_t reserve = boundary.size() * (files.size() + 1) + 128;
    for (const auto& [_path, data] : files) reserve += data.size() + 128;
    body.reserve(reserve);

    for (const auto& [_path, data] : files) {
        body.append("--").append(boundary).append(kCRLF);
        body.append("Content-Type: application/dicom").append(kCRLF);
        body.append("Content-Length: ").append(std::to_string(data.size())).append(kCRLF);
        body.append(kCRLF);
        body.append(data);
        body.append(kCRLF);
    }
    body.append("--").append(boundary).append("--").append(kCRLF);
    return body;
}

// Parse the DICOMweb status report to learn how many instances stored.
// We accept either application/dicom+json or text/* responses; on parse
// failure we just record the raw size and trust the HTTP status.
struct StowResult {
    int sent      {0};
    int succeeded {0};
    int failed    {0};
    bool partial  {false};
};

StowResult interpret_response(const std::string& body, int total_sent,
                                long http_status, const std::string& content_type)
{
    StowResult r;
    r.sent      = total_sent;
    r.succeeded = total_sent;            // assume success unless we learn otherwise
    r.failed    = 0;
    r.partial   = (http_status == 202);

    if (http_status == 202) {
        // PS3.18 §10.5.1.2: 202 means at least one instance failed.
        // Try to count via the FailedSOPSequence tag (00081198) when the
        // response is JSON. If parsing fails, fall back to "we know there
        // was at least one failure but don't know how many".
        if (content_type.find("application/dicom+json") != std::string::npos
            || content_type.find("application/json") != std::string::npos)
        {
            try {
                const auto j = nlohmann::json::parse(body);
                if (j.is_object()) {
                    if (j.contains("00081198")) {
                        const auto& seq = j["00081198"]["Value"];
                        if (seq.is_array()) {
                            r.failed    = static_cast<int>(seq.size());
                            r.succeeded = std::max(0, r.sent - r.failed);
                        }
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // Parse failure — leave partial=true, exact counts unknown.
                r.failed = -1;
                r.succeeded = -1;
            }
        }
    }
    return r;
}

}  // namespace

DicomwebStowDispatchHandler::DicomwebStowDispatchHandler() {
    // libcurl globals are initialized lazily by HttpDispatchHandler too;
    // calling curl_global_init twice is safe per libcurl's docs.
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
    std::string transfer_syntax;          // optional; advertised in Content-Type
    long timeout_s               = 120;   // STOW-RS uploads can be large
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

    // ---- Collect + read .dcm files ----
    if (a.study_file_root.empty()) {
        return DispatchResult::permanent("no study_file_root recorded on work_queue row");
    }
    const auto paths = collect_dicom_files(a.study_file_root);
    if (paths.empty()) {
        return DispatchResult::permanent(
            "no .dcm files under " + a.study_file_root);
    }
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    files.reserve(paths.size());
    for (const auto& p : paths) {
        auto data = read_file_to_string(p);
        if (!data.has_value()) {
            LOG_WARN("dispatch.stow.read_failed", "path", p.string());
            continue;
        }
        files.emplace_back(p, std::move(*data));
    }
    if (files.empty()) {
        return DispatchResult::permanent(
            "every .dcm file under " + a.study_file_root + " failed to read");
    }

    // ---- Build multipart body ----
    const std::string boundary = make_boundary();
    std::string body = build_multipart_body(files, boundary);

    std::string content_type = "multipart/related; type=\"application/dicom\"; boundary=";
    content_type += boundary;
    if (!transfer_syntax.empty()) {
        // PS3.18 §10.5.1.1: transfer-syntax parameter advertises which
        // DICOM TS the parts use. Optional but well-behaved peers honor it.
        content_type += "; transfer-syntax=" + transfer_syntax;
    }

    // ---- Build curl request ----
    CURL* curl = curl_easy_init();
    if (!curl) return DispatchResult::transient("curl_easy_init failed");

    struct curl_slist* headers = nullptr;
    {
        const std::string ct_hdr = "Content-Type: " + content_type;
        const std::string ac_hdr = "Accept: " + accept;
        headers = curl_slist_append(headers, ct_hdr.c_str());
        headers = curl_slist_append(headers, ac_hdr.c_str());
        // "Expect: 100-continue" can hurt latency to peers that don't
        // implement it correctly; disable explicitly.
        headers = curl_slist_append(headers, "Expect:");
    }
    {
        const bool url_has_query = url.find('?') != std::string::npos;
        auto cred = apply_credential_to_request(a, headers, url_has_query);
        headers = cred.headers;
        if (!cred.append_to_url.empty()) url.append(cred.append_to_url);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(timeout_s, 30));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    char err_buf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_buf);

    // ---- Send ----
    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    char* resp_content_type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &resp_content_type);
    const std::string resp_ct = resp_content_type ? resp_content_type : "";

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // ---- Interpret response ----
    StowResult outcome = interpret_response(response_body,
                                              static_cast<int>(files.size()),
                                              http_status, resp_ct);

    // Build response_detail JSON. Always lossless: http_status, sent,
    // succeeded, failed, partial, body_bytes.
    std::ostringstream detail;
    detail << "{\"http_status\":" << http_status
           << ",\"sent\":"        << outcome.sent
           << ",\"succeeded\":"   << outcome.succeeded
           << ",\"failed\":"      << outcome.failed
           << ",\"partial\":"     << (outcome.partial ? "true" : "false")
           << ",\"body_bytes\":"  << response_body.size()
           << ",\"curl_code\":"   << static_cast<int>(rc)
           << "}";

    if (rc != CURLE_OK) {
        return DispatchResult::transient(
            std::string{"curl: "} + err_buf, detail.str());
    }
    if (http_status == 200) {
        return DispatchResult::success(detail.str());
    }
    if (http_status == 202) {
        // Partial success per STOW-RS. We treat as transient — the retry
        // policy will re-send (re-sending an already-stored instance is
        // safe per DICOM PS3.18 §10.5.1.2.6 — peers either return 200 or
        // ignore the duplicate).
        return DispatchResult::transient(
            "STOW-RS partial: " + std::to_string(outcome.failed) +
            " of " + std::to_string(outcome.sent) + " instances failed",
            detail.str());
    }
    if (http_status >= 400 && http_status < 500) {
        return DispatchResult::transient(
            "STOW-RS http " + std::to_string(http_status) +
            " — config/credential/data issue", detail.str());
    }
    return DispatchResult::transient(
        "STOW-RS http " + std::to_string(http_status), detail.str());
}

}  // namespace nlr
