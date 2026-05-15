// nl_router/crypto/envelope.hpp
//
// C++ counterpart of `nl_router.crypto` (Python). Reads credential
// envelopes written by the management API and decrypts them with the
// same AES-256-GCM scheme so the dispatcher can fetch live credentials
// at send time.
//
// Format (must stay byte-compatible with python/nl_router/crypto.py):
//
//   enc_version = 1
//   nonce       = 12 bytes (NIST recommended for GCM)
//   ciphertext  = encrypt(plaintext) || tag    (16-byte AEAD tag appended)
//   aad         = "nl-router/credentials/v1"
//
// If you change either side, bump CURRENT_ENC_VERSION and add a parallel
// decryption branch on both sides — never alter version 1's bytes-on-disk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nl_router::crypto {

inline constexpr std::int16_t kCurrentEncVersion = 1;
inline constexpr std::size_t  kNonceBytes        = 12;
inline constexpr std::size_t  kTagBytes          = 16;
inline constexpr std::size_t  kKekBytes          = 32;

class CryptoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown when no KEK source is configured. Callers should surface this
// to the operator as a config error, not a runtime failure.
class KEKUnavailableError : public CryptoError {
public:
    using CryptoError::CryptoError;
};

// Thrown when ciphertext fails authentication (tag mismatch). Either the
// KEK rotated without re-encrypting, the row was tampered with, or the
// enc_version is newer than this binary knows about.
class DecryptError : public CryptoError {
public:
    using CryptoError::CryptoError;
};

struct Envelope {
    std::int16_t        enc_version {0};
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ciphertext;     // includes the 16-byte tag suffix
};

// Decrypt an envelope with the provided KEK. Returns the plaintext bytes.
//
// Throws DecryptError on any authentication failure or unsupported
// enc_version. Throws CryptoError on internal OpenSSL errors.
std::vector<std::uint8_t> decrypt(const Envelope& env,
                                   const std::vector<std::uint8_t>& kek);

// Convenience: decrypt and return the plaintext as a string (the credential
// payload is JSON, so callers usually want a string view).
std::string decrypt_to_string(const Envelope& env,
                               const std::vector<std::uint8_t>& kek);

}  // namespace nl_router::crypto
