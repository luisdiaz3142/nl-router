#include "object_storage_handler.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "handler.hpp"
#include "logging.hpp"
#include "template.hpp"

namespace nlr {

namespace {

// CURLOPT_AWS_SIGV4 is an enum option, available since libcurl 7.75.0.
// The CMake build enforces that minimum via `find_package(CURL 7.75.0)`.
// (A `#ifndef CURLOPT_AWS_SIGV4` belt-and-suspenders check doesn't work:
// enum values aren't preprocessor symbols.)

// libcurl write callback — accumulate response bytes (used only for body
// of error responses; happy-path PUT bodies are empty).
std::size_t write_to_string(void* ptr, std::size_t size, std::size_t nmemb,
                             void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Recursively collect .dcm files under `root`.
struct InstanceFile {
    std::filesystem::path path;
    std::filesystem::path rel;        // relative to root
};

std::vector<InstanceFile> collect_files(const std::filesystem::path& root) {
    std::vector<InstanceFile> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec))
    {
        if (it->is_regular_file(ec) && it->path().extension() == ".dcm") {
            InstanceFile f;
            f.path = it->path();
            f.rel  = std::filesystem::relative(it->path(), root, ec);
            if (ec) f.rel = it->path().filename();
            out.push_back(std::move(f));
        }
    }
    return out;
}

// Build the per-file S3 URL.
//
// In virtual-hosted style (default for AWS):   https://<bucket>.<host>/<key>
// In path style                              :   http://<host>/<bucket>/<key>
// (Path style is necessary for MinIO and most non-AWS S3 implementations
// before they map a bucket to a subdomain; AWS deprecates it but still
// accepts it.)
std::string build_object_url(const std::string& endpoint,
                              const std::string& bucket,
                              const std::string& key,
                              bool force_path_style) {
    // Split endpoint into scheme://host[:port][/path]
    const auto scheme_end = endpoint.find("://");
    if (scheme_end == std::string::npos) {
        // Fallback: treat the whole thing as host with http://
        return "http://" + endpoint + "/" + bucket + "/" + key;
    }
    const std::string scheme = endpoint.substr(0, scheme_end);
    const std::string host_etc = endpoint.substr(scheme_end + 3);

    if (force_path_style) {
        std::string base = endpoint;
        if (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/" + bucket + "/" + key;
    }
    // virtual-hosted
    return scheme + "://" + bucket + "." + host_etc + "/" + key;
}

// Sanitize a path segment so it survives as an S3 key character. S3 keys
// permit nearly anything but a few control characters; we just strip the
// truly disruptive ones (newline, NUL, CR) — operators see the rest
// preserved as-is in their bucket listing.
std::string sanitize_key_segment(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\0' || c == '\r' || c == '\n') continue;
        out.push_back(c);
    }
    return out;
}

// PUT one file. Returns (http_status, curl_code, error).
struct PutResult {
    long        http_status {0};
    int         curl_code   {0};
    std::string error;
    std::uint64_t bytes_sent {0};
};

PutResult put_object(const std::string& url,
                       const std::filesystem::path& file_path,
                       const std::string& access_key_id,
                       const std::string& secret_access_key,
                       const std::string& session_token,
                       const std::string& region,
                       const std::string& storage_class,
                       long timeout_s)
{
    PutResult r;

    // Open file for streaming upload — avoids loading multi-GB files
    // into memory.
    FILE* fp = std::fopen(file_path.c_str(), "rb");
    if (!fp) {
        r.error = "fopen failed: " + file_path.string();
        return r;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(file_path, ec);
    if (ec) { std::fclose(fp); r.error = "stat failed: " + ec.message(); return r; }
    r.bytes_sent = static_cast<std::uint64_t>(sz);

    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(fp); r.error = "curl_easy_init failed"; return r; }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Expect:");
    if (!storage_class.empty()) {
        const std::string hdr = "x-amz-storage-class: " + storage_class;
        headers = curl_slist_append(headers, hdr.c_str());
    }
    if (!session_token.empty()) {
        const std::string hdr = "x-amz-security-token: " + session_token;
        headers = curl_slist_append(headers, hdr.c_str());
    }

    // libcurl's AWS SigV4 implementation:
    //   provider:scheme:region:service
    //   "aws:amz:<region>:s3"  → s3 service in the given region
    const std::string sigv4 = "aws:amz:" + region + ":s3";
    const std::string userpwd = access_key_id + ":" + secret_access_key;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(sz));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(timeout_s, 30));
    curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, sigv4.c_str());
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    char err_buf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_buf);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.curl_code = static_cast<int>(rc);
        r.error = err_buf;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_status);
        if (r.http_status < 200 || r.http_status >= 300) {
            // S3 returns XML error bodies. Keep them short for the audit
            // log but truncate to avoid blowing out logs on a chatty server.
            r.error = "http " + std::to_string(r.http_status);
            if (!response_body.empty()) {
                const auto preview = response_body.size() > 200
                    ? response_body.substr(0, 200) + "..."
                    : response_body;
                r.error += " body=" + preview;
            }
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(fp);
    return r;
}

}  // namespace

ObjectStorageDispatchHandler::ObjectStorageDispatchHandler() {
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

ObjectStorageDispatchHandler::~ObjectStorageDispatchHandler() = default;

DispatchResult ObjectStorageDispatchHandler::dispatch(
    const Assignment& a, const Destination& d)
{
    // ---- 1) Parse config ----
    std::string endpoint, bucket, key_template, storage_class;
    std::string region            = "us-east-1";
    bool        force_path_style  = false;
    long        timeout_s         = 120;
    try {
        const auto j = nlohmann::json::parse(d.config_json);
        endpoint     = j.value("endpoint",     "");
        bucket       = j.value("bucket",       "");
        key_template = j.value("key_template", "");
        if (j.contains("region")        && j["region"].is_string())  region = j["region"].get<std::string>();
        if (j.contains("storage_class") && j["storage_class"].is_string())
            storage_class = j["storage_class"].get<std::string>();
        if (j.contains("force_path_style") && j["force_path_style"].is_boolean())
            force_path_style = j["force_path_style"].get<bool>();
        if (j.contains("timeout_s") && j["timeout_s"].is_number())
            timeout_s = j["timeout_s"].get<long>();
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"object_storage destination config invalid: "} + e.what());
    }
    if (endpoint.empty() || bucket.empty() || key_template.empty()) {
        return DispatchResult::permanent(
            "object_storage destination requires endpoint, bucket, key_template");
    }

    // ---- 2) Parse credential ----
    if (a.credential_kind != "aws_keys" || a.credential_payload.empty()) {
        return DispatchResult::permanent(
            "object_storage requires a credential of kind 'aws_keys'");
    }
    std::string access_key_id, secret_access_key, session_token;
    try {
        const auto j = nlohmann::json::parse(a.credential_payload);
        // value() throws on present-but-null. The Pydantic schema for
        // aws_keys serializes optional fields as JSON null, so we have to
        // explicitly check is_string() before reading.
        auto read_opt_string = [&](const char* key) -> std::string {
            if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
            return {};
        };
        access_key_id     = read_opt_string("access_key_id");
        secret_access_key = read_opt_string("secret_access_key");
        session_token     = read_opt_string("session_token");
    } catch (const nlohmann::json::exception& e) {
        return DispatchResult::permanent(
            std::string{"aws_keys credential payload invalid: "} + e.what());
    }
    if (access_key_id.empty() || secret_access_key.empty()) {
        return DispatchResult::permanent(
            "aws_keys credential missing access_key_id or secret_access_key");
    }

    // ---- 3) Expand the key_template prefix ----
    std::string key_prefix = expand_template(key_template, a.tags_json);
    key_prefix = sanitize_key_segment(key_prefix);
    if (key_prefix.empty()) {
        return DispatchResult::permanent(
            "key_template expanded to empty (all referenced tags missing?)");
    }
    // Ensure exactly one trailing slash so we can append the per-file
    // suffix without inserting "//".
    while (key_prefix.size() > 1 && key_prefix.back() == '/') key_prefix.pop_back();
    key_prefix.push_back('/');

    // ---- 4) Walk files + PUT each ----
    if (a.study_file_root.empty()) {
        return DispatchResult::permanent("no study_file_root recorded on work_queue row");
    }
    const auto files = collect_files(a.study_file_root);
    if (files.empty()) {
        return DispatchResult::permanent("no .dcm files under " + a.study_file_root);
    }

    int sent = 0, succeeded = 0, failed = 0;
    std::uint64_t bytes_total = 0;
    std::string first_err;
    for (const auto& f : files) {
        const std::string rel_key = sanitize_key_segment(f.rel.generic_string());
        const std::string full_key = key_prefix + rel_key;
        const std::string url = build_object_url(endpoint, bucket, full_key,
                                                  force_path_style);

        const auto pr = put_object(url, f.path, access_key_id, secret_access_key,
                                     session_token, region, storage_class, timeout_s);
        ++sent;
        if (pr.http_status >= 200 && pr.http_status < 300 && pr.curl_code == 0) {
            ++succeeded;
            bytes_total += pr.bytes_sent;
        } else {
            ++failed;
            if (first_err.empty()) {
                first_err = pr.error.empty()
                    ? ("curl_code=" + std::to_string(pr.curl_code))
                    : pr.error;
            }
            LOG_WARN("dispatch.object_storage.put_failed",
                "key",          full_key,
                "http_status",  std::to_string(pr.http_status),
                "curl_code",    std::to_string(pr.curl_code),
                "error",        pr.error);
        }
    }

    std::ostringstream detail;
    detail << "{\"sent\":"      << sent
           << ",\"succeeded\":" << succeeded
           << ",\"failed\":"    << failed
           << ",\"bytes\":"     << bytes_total
           << ",\"bucket\":\""  << bucket << "\""
           << ",\"key_prefix\":\"" << key_prefix << "\""
           << "}";

    if (failed == 0) {
        return DispatchResult::success(detail.str());
    }
    // Any failed PUT → transient (S3 outages, throttling, transient
    // network blips). The retry-policy give_up_after_hours bounds runaway
    // retries on permanent misconfig.
    return DispatchResult::transient(
        std::to_string(failed) + " of " + std::to_string(sent) +
        " puts failed: " + first_err, detail.str());
}

}  // namespace nlr
