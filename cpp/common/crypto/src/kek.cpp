// KEK loader. Mirrors python/nl_router/config.py:load_kek() precedence.

#include "nl_router/crypto/kek.hpp"

#include "nl_router/crypto/envelope.hpp"   // CryptoError types

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nl_router::crypto {

namespace {

constexpr const char* kDefaultKekPath = "/etc/nl-router/kek.key";

// urlsafe-base64 decoder. Accepts input with or without '=' padding.
// Returns empty vector if the input isn't valid base64.
std::vector<std::uint8_t> b64url_decode(const std::string& input) {
    // Map: A-Z=0-25, a-z=26-51, 0-9=52-61, '-'=62, '_'=63
    static int table[256];
    static bool table_init = false;
    if (!table_init) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 26; ++i) {
            table[static_cast<unsigned>('A' + i)] = i;
            table[static_cast<unsigned>('a' + i)] = i + 26;
        }
        for (int i = 0; i < 10; ++i) table[static_cast<unsigned>('0' + i)] = i + 52;
        table[static_cast<unsigned>('-')] = 62;
        table[static_cast<unsigned>('_')] = 63;
        table_init = true;
    }

    // Strip whitespace and padding.
    std::string s;
    s.reserve(input.size());
    for (char c : input) {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        s.push_back(c);
    }
    if (s.empty()) return {};

    std::vector<std::uint8_t> out;
    out.reserve((s.size() * 3) / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : s) {
        const int v = table[static_cast<unsigned char>(c)];
        if (v < 0) return {};                       // bad char
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    return out;
}

// Decode a raw key value. Accepts either:
//   * exactly 32 raw bytes  → returned as-is
//   * a string of base64url characters that decodes to 32 bytes
std::vector<std::uint8_t> decode_key(const std::vector<std::uint8_t>& raw,
                                       const std::string& source) {
    if (raw.size() == kKekBytes) return raw;

    // Treat as base64url string. Build a std::string view of the raw bytes.
    std::string text;
    text.reserve(raw.size());
    for (auto b : raw) {
        if (b == '\n' || b == '\r') continue;       // strip trailing newline
        text.push_back(static_cast<char>(b));
    }
    const auto decoded = b64url_decode(text);
    if (decoded.size() != kKekBytes) {
        throw KEKUnavailableError(
            "KEK from " + source + ": not 32 raw bytes and not valid base64url "
            "(decoded length " + std::to_string(decoded.size()) + ")");
    }
    return decoded;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw KEKUnavailableError("could not open KEK file: " + path.string());
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    // Strip a single trailing CR/LF if present.
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
        bytes.pop_back();
    }
    return bytes;
}

}  // namespace

std::vector<std::uint8_t> load_kek() {
    // 1) File source first (matches the design plan's precedence rule).
    std::filesystem::path path = kDefaultKekPath;
    if (const char* override_path = std::getenv("NL_ROUTER_KEK_FILE")) {
        if (*override_path) path = override_path;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        return decode_key(read_file_bytes(path), "file:" + path.string());
    }

    // 2) Env var fallback.
    if (const char* env_val = std::getenv("NL_ROUTER_KEK")) {
        if (*env_val) {
            std::vector<std::uint8_t> raw(env_val, env_val + std::strlen(env_val));
            return decode_key(raw, "env:NL_ROUTER_KEK");
        }
    }

    throw KEKUnavailableError(
        "No KEK configured. Provide either " + path.string() +
        " (32 raw bytes or base64url) or set NL_ROUTER_KEK.");
}

}  // namespace nl_router::crypto
