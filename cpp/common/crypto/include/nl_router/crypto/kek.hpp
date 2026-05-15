// nl_router/crypto/kek.hpp
//
// KEK (master key) loader for native processes. Matches the precedence
// rule from the design plan and the Python loader: file beats env when
// both are set. Accepts the key as either raw 32 bytes or as a base64url
// string (with or without padding).
//
// Sources (highest precedence first):
//   1. /etc/nl-router/kek.key       (default file path)
//      override via NL_ROUTER_KEK_FILE env var
//   2. NL_ROUTER_KEK env var        (string contents)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nl_router::crypto {

// Load the KEK from configured sources. Result is exactly kKekBytes (32) bytes.
//
// Throws KEKUnavailableError if no source is set or the contents are
// malformed. Callers should only invoke this once at startup; cache the
// returned vector for subsequent decrypts.
std::vector<std::uint8_t> load_kek();

}  // namespace nl_router::crypto
