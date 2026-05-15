#include "stow_upload.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "logging.hpp"

namespace nlr {

namespace {

constexpr const char* kCRLF = "\r\n";

std::size_t write_to_string(void* ptr, std::size_t size, std::size_t nmemb,
                             void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Generate a unique multipart boundary. Random 32-char hex; collision
// against embedded DICOM bytes is ~10^-77 per upload.
std::string make_boundary() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out = "nlr-stow-";
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) out.push_back(hex[dist(rd)]);
    return out;
}

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

std::optional<std::string> read_file_to_string(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Build the multipart/related body. See PS3.18 §6.6.1.1 / RFC 2387 §3.
std::string build_multipart_body(
    const std::vector<std::pair<std::filesystem::path, std::string>>& files,
    const std::string& boundary)
{
    std::string body;
    std::size_t reserve = boundary.size() * (files.size() + 1) + 128;
    for (const auto& [_p, data] : files) reserve += data.size() + 128;
    body.reserve(reserve);

    for (const auto& [_p, data] : files) {
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

// On 202, try to count failed instances via FailedSOPSequence (00081198).
// Returns (-1, -1) if we can't parse — caller logs "partial, count unknown".
struct Counts { int succeeded; int failed; };

Counts count_failures(int total_sent, const std::string& body, const std::string& ct) {
    if (ct.find("application/dicom+json") == std::string::npos
        && ct.find("application/json")     == std::string::npos) {
        return {-1, -1};
    }
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.is_object() && j.contains("00081198")) {
            const auto& seq = j["00081198"]["Value"];
            if (seq.is_array()) {
                const int failed = static_cast<int>(seq.size());
                return {std::max(0, total_sent - failed), failed};
            }
        }
    } catch (const nlohmann::json::exception&) {
        // fall through
    }
    return {-1, -1};
}

}  // namespace

std::string StowUploadResult::to_response_detail_json() const {
    std::ostringstream out;
    out << "{\"http_status\":" << http_status
        << ",\"sent\":"        << sent
        << ",\"succeeded\":"   << succeeded
        << ",\"failed\":"      << failed
        << ",\"partial\":"     << (partial ? "true" : "false")
        << ",\"body_bytes\":"  << body_bytes
        << ",\"curl_code\":"   << curl_code
        << "}";
    return out.str();
}

StowUploadResult run_stow_upload(const StowUploadParams& p) {
    StowUploadResult r;

    if (p.study_file_root.empty()) {
        r.kind = StowUploadResult::Kind::NoFiles;
        r.error_message = "no study_file_root recorded on work_queue row";
        return r;
    }

    const auto paths = collect_dicom_files(p.study_file_root);
    if (paths.empty()) {
        r.kind = StowUploadResult::Kind::NoFiles;
        r.error_message = "no .dcm files under " + p.study_file_root;
        return r;
    }
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        auto data = read_file_to_string(path);
        if (!data.has_value()) {
            LOG_WARN("dispatch.stow.read_failed", "path", path.string());
            continue;
        }
        files.emplace_back(path, std::move(*data));
    }
    if (files.empty()) {
        r.kind = StowUploadResult::Kind::ReadAllFailed;
        r.error_message = "every .dcm file under " + p.study_file_root + " failed to read";
        return r;
    }

    const std::string boundary = make_boundary();
    std::string body = build_multipart_body(files, boundary);

    std::string content_type = "multipart/related; type=\"application/dicom\"; boundary=";
    content_type += boundary;
    if (!p.transfer_syntax.empty()) {
        content_type += "; transfer-syntax=" + p.transfer_syntax;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        r.kind = StowUploadResult::Kind::CurlError;
        r.error_message = "curl_easy_init failed";
        return r;
    }

    // Build a local headers list that points BACK to the caller's
    // auth_headers chain. curl_slist_append on the new list extends our
    // local entries; we MUST free only ours, not the caller's chain.
    struct curl_slist* local_headers = nullptr;
    const std::string ct_hdr = "Content-Type: " + content_type;
    const std::string ac_hdr = "Accept: " + p.accept;
    local_headers = curl_slist_append(local_headers, ct_hdr.c_str());
    local_headers = curl_slist_append(local_headers, ac_hdr.c_str());
    local_headers = curl_slist_append(local_headers, "Expect:");
    for (const auto& h : p.extra_headers) {
        local_headers = curl_slist_append(local_headers, h.c_str());
    }
    // Concat auth_headers AT THE END so we can free local_headers
    // distinctly. libcurl traverses the chain forward, so we walk to the
    // tail of local_headers and link in auth_headers there. Caller still
    // owns auth_headers and frees it.
    if (p.auth_headers) {
        struct curl_slist* tail = local_headers;
        while (tail && tail->next) tail = tail->next;
        if (tail) tail->next = p.auth_headers;
        else      local_headers = p.auth_headers;  // can't happen given the appends above
    }

    curl_easy_setopt(curl, CURLOPT_URL, p.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, local_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, p.timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(p.timeout_s, 30));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    char err_buf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_buf);

    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    char* resp_ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &resp_ct);

    // Detach the caller's auth_headers from our local chain before freeing.
    // Walk local_headers; when the next pointer equals p.auth_headers,
    // null it out.
    if (p.auth_headers && local_headers) {
        struct curl_slist* prev = local_headers;
        while (prev && prev->next && prev->next != p.auth_headers) prev = prev->next;
        if (prev && prev->next == p.auth_headers) prev->next = nullptr;
    }
    curl_slist_free_all(local_headers);
    curl_easy_cleanup(curl);

    r.http_status = http_status;
    r.curl_code   = static_cast<int>(rc);
    r.sent        = static_cast<int>(files.size());
    r.body_bytes  = response_body.size();
    r.response_content_type = resp_ct ? resp_ct : "";

    if (rc != CURLE_OK) {
        r.kind = StowUploadResult::Kind::CurlError;
        r.curl_error = err_buf;
        r.error_message = std::string{"curl: "} + err_buf;
        return r;
    }
    if (http_status == 200) {
        r.kind = StowUploadResult::Kind::Success;
        r.succeeded = r.sent;
        return r;
    }
    if (http_status == 202) {
        r.kind    = StowUploadResult::Kind::Partial;
        r.partial = true;
        const auto c = count_failures(r.sent, response_body, r.response_content_type);
        r.succeeded = c.succeeded;
        r.failed    = c.failed;
        r.error_message = "STOW-RS partial success";
        return r;
    }
    if (http_status >= 400 && http_status < 500) {
        r.kind = StowUploadResult::Kind::ClientError;
        r.error_message = "STOW-RS http " + std::to_string(http_status) +
                           " — config/credential/data issue";
        return r;
    }
    r.kind = StowUploadResult::Kind::ServerError;
    r.error_message = "STOW-RS http " + std::to_string(http_status);
    return r;
}

}  // namespace nlr
