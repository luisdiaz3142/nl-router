// GCP service-account → access-token exchange. Uses OpenSSL for RS256
// signing and libcurl for the HTTP token-endpoint exchange.

#include "gcp_oauth.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "logging.hpp"

namespace nlr {

namespace {

// ---- Base64URL (no padding) ---------------------------------------------
std::string base64url_encode(const std::uint8_t* data, std::size_t len) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (std::size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        bits += 8;
        while (bits >= 0) {
            out.push_back(alpha[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(alpha[((val << 8) >> (bits + 8)) & 0x3F]);
    // Strip padding — JWT spec uses base64url WITHOUT '='.
    return out;
}

std::string base64url_encode(const std::string& s) {
    return base64url_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// ---- OpenSSL helpers ----------------------------------------------------
struct BioDeleter      { void operator()(BIO* b)              const noexcept { if (b) BIO_free(b); } };
struct PkeyDeleter     { void operator()(EVP_PKEY* k)         const noexcept { if (k) EVP_PKEY_free(k); } };
struct MdCtxDeleter    { void operator()(EVP_MD_CTX* c)       const noexcept { if (c) EVP_MD_CTX_free(c); } };

using BioPtr   = std::unique_ptr<BIO,        BioDeleter>;
using PkeyPtr  = std::unique_ptr<EVP_PKEY,   PkeyDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

std::string openssl_error() {
    char buf[256] = {};
    const auto e = ERR_get_error();
    if (e == 0) return "no openssl error";
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string{buf};
}

// Parse a PEM-encoded RSA private key.
PkeyPtr load_private_key_pem(const std::string& pem) {
    BioPtr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) throw GcpOAuthError("BIO_new_mem_buf: " + openssl_error());
    PkeyPtr key{PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)};
    if (!key) {
        throw GcpOAuthError(
            "failed to parse RSA private key from credential payload: " +
            openssl_error());
    }
    return key;
}

// RSA-SHA256 sign `data` with `key`; return raw signature bytes.
std::string rsa_sha256_sign(EVP_PKEY* key, const std::string& data) {
    MdCtxPtr ctx{EVP_MD_CTX_new()};
    if (!ctx) throw GcpOAuthError("EVP_MD_CTX_new: " + openssl_error());

    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        throw GcpOAuthError("EVP_DigestSignInit: " + openssl_error());
    }
    if (EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
        throw GcpOAuthError("EVP_DigestSignUpdate: " + openssl_error());
    }
    std::size_t siglen = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &siglen) != 1) {
        throw GcpOAuthError("EVP_DigestSignFinal (size): " + openssl_error());
    }
    std::string sig(siglen, '\0');
    if (EVP_DigestSignFinal(ctx.get(),
            reinterpret_cast<std::uint8_t*>(sig.data()), &siglen) != 1) {
        throw GcpOAuthError("EVP_DigestSignFinal: " + openssl_error());
    }
    sig.resize(siglen);
    return sig;
}

// SHA-256 hex digest of `data`. Used as the cache key.
std::string sha256_hex(const std::string& data) {
    std::uint8_t out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    static const char hex[] = "0123456789abcdef";
    std::string s(SHA256_DIGEST_LENGTH * 2, '\0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        s[i*2]   = hex[(out[i] >> 4) & 0xF];
        s[i*2+1] = hex[out[i] & 0xF];
    }
    return s;
}

// ---- HTTP POST helper for the token endpoint ----------------------------
std::size_t curl_write_to_string(void* p, std::size_t s, std::size_t n, void* u) {
    static_cast<std::string*>(u)->append(static_cast<char*>(p), s * n);
    return s * n;
}

// URL-encoded form-body POST. Returns (http_status, body).
struct HttpFormPost {
    long        status {0};
    std::string body;
    std::string err;
};

HttpFormPost http_post_form(const std::string& url,
                              const std::string& form_body,
                              long timeout_s = 30)
{
    HttpFormPost r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.err = "curl_easy_init failed"; return r; }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
        "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(form_body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(timeout_s, 10));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);

    char err_buf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_buf);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.err = err_buf;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

// URL-encode a form parameter value (RFC 3986 unreserved + percent-encode).
std::string form_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) out.push_back(static_cast<char>(c));
        else            { out.push_back('%'); out.push_back(hex[(c>>4)&0xF]); out.push_back(hex[c&0xF]); }
    }
    return out;
}

}  // namespace

GcpAccessToken exchange_service_account_for_token(
    const std::string& service_account_json,
    const std::string& scope)
{
    // ---- 1) Parse the service account JSON ----
    std::string client_email, private_key_pem, private_key_id, token_uri;
    try {
        const auto j = nlohmann::json::parse(service_account_json);
        if (j.value("type", "") != "service_account") {
            throw GcpOAuthError("credential payload is not a service_account");
        }
        client_email = j.value("client_email",  "");
        private_key_pem = j.value("private_key", "");
        private_key_id  = j.value("private_key_id", "");
        token_uri       = j.value("token_uri", "https://oauth2.googleapis.com/token");
        if (client_email.empty() || private_key_pem.empty()) {
            throw GcpOAuthError("service_account missing client_email or private_key");
        }
    } catch (const nlohmann::json::exception& e) {
        throw GcpOAuthError(std::string{"service_account JSON invalid: "} + e.what());
    }

    // ---- 2) Build the JWT header + claims ----
    const auto now = std::chrono::system_clock::now();
    const auto iat = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    const auto exp = iat + 3600;   // 1-hour token

    nlohmann::json header = {{"alg","RS256"},{"typ","JWT"}};
    if (!private_key_id.empty()) header["kid"] = private_key_id;
    nlohmann::json claims = {
        {"iss",   client_email},
        {"scope", scope},
        {"aud",   token_uri},
        {"iat",   iat},
        {"exp",   exp},
    };

    const std::string header_b64 = base64url_encode(header.dump());
    const std::string claims_b64 = base64url_encode(claims.dump());
    const std::string signing_input = header_b64 + "." + claims_b64;

    // ---- 3) Sign ----
    PkeyPtr key = load_private_key_pem(private_key_pem);
    const std::string sig = rsa_sha256_sign(key.get(), signing_input);
    const std::string sig_b64 = base64url_encode(sig);
    const std::string jwt = signing_input + "." + sig_b64;

    // ---- 4) Exchange ----
    const std::string body =
        "grant_type=" + form_encode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
        "&assertion=" + form_encode(jwt);
    const auto resp = http_post_form(token_uri, body);
    if (!resp.err.empty()) {
        throw GcpOAuthError("token endpoint: " + resp.err);
    }
    if (resp.status < 200 || resp.status >= 300) {
        throw GcpOAuthError("token endpoint returned HTTP " +
                             std::to_string(resp.status) + ": " + resp.body);
    }

    // ---- 5) Parse access_token + expires_in ----
    nlohmann::json out_j;
    try {
        out_j = nlohmann::json::parse(resp.body);
    } catch (const nlohmann::json::exception& e) {
        throw GcpOAuthError(std::string{"token response invalid JSON: "} + e.what());
    }
    GcpAccessToken tok;
    tok.token = out_j.value("access_token", "");
    if (tok.token.empty()) {
        throw GcpOAuthError("token response missing access_token: " + resp.body);
    }
    const int expires_in = out_j.value("expires_in", 3600);
    tok.expires_at = std::chrono::system_clock::now() +
                      std::chrono::seconds(expires_in);
    return tok;
}

const std::string& GcpTokenCache::get(const std::string& sa_json,
                                        const std::string& scope) {
    const std::string key = sha256_hex(sa_json + "\0" + scope);
    if (key == cache_key_ && cache_.valid_for()) {
        return cache_.token;
    }
    cache_ = exchange_service_account_for_token(sa_json, scope);
    cache_key_ = key;
    LOG_INFO("gcp_oauth.refreshed",
        "key_hash", cache_key_.substr(0, 12));
    return cache_.token;
}

}  // namespace nlr
