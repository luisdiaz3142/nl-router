// nl-dispatch/gcp_oauth.hpp
//
// OAuth2 service-account flow for GCP Healthcare API.
//
// Given a `gcp_service_account` credential payload (the JSON file
// downloaded from GCP, with at minimum client_email + private_key), we:
//   1. Build a signed JWT (RS256, OpenSSL EVP_DigestSign*).
//   2. POST it to https://oauth2.googleapis.com/token (configurable via
//      payload.token_uri) as `grant_type=urn:ietf:params:oauth:grant-type:
//      jwt-bearer&assertion=<JWT>`.
//   3. Cache the resulting access token until ~60s before expiry.
//
// Caching is in-memory per handler instance, keyed by the credential
// payload bytes (sha256). Rotating the credential invalidates the cache
// automatically — the next dispatch decrypts a different payload, the
// hash changes, we miss and re-exchange.

#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace nlr {

class GcpOAuthError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GcpAccessToken {
    std::string                                 token;
    std::chrono::system_clock::time_point       expires_at;

    bool valid_for(std::chrono::seconds safety_margin = std::chrono::seconds{60}) const {
        return !token.empty() &&
               std::chrono::system_clock::now() + safety_margin < expires_at;
    }
};

// Exchange a service-account JSON payload for an OAuth2 access token.
// `service_account_json` is the raw JSON string from the credential
// payload. `scope` is the OAuth2 scope (e.g.
// "https://www.googleapis.com/auth/cloud-platform").
//
// Throws GcpOAuthError on JSON parse failure, missing required fields,
// signing failure, HTTP failure, or non-2xx response from the token
// endpoint.
GcpAccessToken exchange_service_account_for_token(
    const std::string& service_account_json,
    const std::string& scope);

// Cache wrapper. A single instance lives inside the handler; thread-safe
// because only one Worker (== one thread) uses it.
class GcpTokenCache {
public:
    // Return a valid access token for the given payload+scope. Re-uses
    // the cached value if it's still valid; otherwise exchanges a fresh
    // one. Throws GcpOAuthError on exchange failure.
    const std::string& get(const std::string& service_account_json,
                             const std::string& scope);

private:
    // Cached entry. Keyed by hash(payload + "\0" + scope) so rotating the
    // credential or switching scopes invalidates the cache automatically.
    std::string  cache_key_;
    GcpAccessToken cache_;
};

}  // namespace nlr
