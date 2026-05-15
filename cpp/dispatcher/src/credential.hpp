// nl-dispatch/credential.hpp
//
// Shared credential → HTTP headers helper used by every HTTP-shaped
// destination handler (http, dicomweb_stow, gcp_dicomweb later).
//
// The credential payload (decrypted by the worker upstream) lives on the
// Assignment as a JSON string. This module knows how to turn each
// supported `kind` into an Authorization header or a URL query-string
// suffix, mirroring the Python `nl_router.api.kinds` schemas.
//
// Unsupported kinds (e.g. mTLS for which a header isn't the right
// answer) log a warning and leave the headers list untouched; the
// caller can still detect "no auth applied" by sniffing for an
// Authorization header in the result.

#pragma once

#include <string>

#include <curl/curl.h>

#include "handler.hpp"

namespace nlr {

struct CredentialApplyResult {
    // libcurl slist of headers (caller takes ownership; free with curl_slist_free_all).
    struct curl_slist* headers {nullptr};

    // For api_key with query_param shape, the credential value is encoded
    // into the URL rather than a header. If non-empty, the caller should
    // append `?<append_to_url>` or `&<append_to_url>` to its target URL.
    std::string append_to_url;
};

// Apply the assignment's decrypted credential to a libcurl headers list.
//
// `existing_headers` is the operator-supplied static header list (e.g.
// custom X-* headers from destination.config.headers). We extend it rather
// than rebuild — caller stays in charge of header order.
//
// Returns a list that should replace `existing_headers` in the caller
// (it may add new entries via curl_slist_append; the same pointer is
// returned in the simple case but may be the head of a longer chain).
//
// `current_url_has_query` controls whether append_to_url should start
// with '?' (no query yet) or '&' (already has one). The caller passes
// `true` if its URL already contains '?'.
CredentialApplyResult apply_credential_to_request(
    const Assignment&     a,
    struct curl_slist*    existing_headers,
    bool                  current_url_has_query);

}  // namespace nlr
