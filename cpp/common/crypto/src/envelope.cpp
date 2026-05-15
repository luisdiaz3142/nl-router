// Envelope decryption via OpenSSL EVP. Mirrors the Python writer
// (nl_router.crypto.encrypt) byte-for-byte.

#include "nl_router/crypto/envelope.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nl_router::crypto {

namespace {

// Domain-separating AAD. Must match python/nl_router/crypto.py exactly.
const std::uint8_t kAadV1[] = {
    'n','l','-','r','o','u','t','e','r','/','c','r','e','d','e','n',
    't','i','a','l','s','/','v','1'
};

// RAII wrapper for EVP_CIPHER_CTX so error paths don't leak.
struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const noexcept { if (c) EVP_CIPHER_CTX_free(c); }
};
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

std::string openssl_last_error() {
    char buf[256] = {};
    const auto e = ERR_get_error();
    if (e == 0) return "no openssl error";
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string{buf};
}

}  // namespace

std::vector<std::uint8_t> decrypt(const Envelope& env,
                                   const std::vector<std::uint8_t>& kek) {
    if (env.enc_version != kCurrentEncVersion) {
        throw DecryptError(
            "unsupported enc_version=" + std::to_string(env.enc_version) +
            "; this binary supports " + std::to_string(kCurrentEncVersion));
    }
    if (env.nonce.size() != kNonceBytes) {
        throw DecryptError(
            "nonce length " + std::to_string(env.nonce.size()) +
            " != " + std::to_string(kNonceBytes) + "; row corrupt");
    }
    if (kek.size() != kKekBytes) {
        throw CryptoError(
            "KEK length " + std::to_string(kek.size()) +
            " != " + std::to_string(kKekBytes));
    }
    if (env.ciphertext.size() < kTagBytes) {
        throw DecryptError("ciphertext too short to hold the AEAD tag");
    }

    // Layout matches Python's AESGCM.encrypt() return:
    //   [ enciphered_plaintext (N bytes) || tag (16 bytes) ]
    const std::size_t cipher_len = env.ciphertext.size() - kTagBytes;
    const std::uint8_t* cipher_ptr = env.ciphertext.data();
    const std::uint8_t* tag_ptr    = env.ciphertext.data() + cipher_len;

    CipherCtxPtr ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) throw CryptoError("EVP_CIPHER_CTX_new: " + openssl_last_error());

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                            nullptr, nullptr) != 1) {
        throw CryptoError("EVP_DecryptInit_ex: " + openssl_last_error());
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                             static_cast<int>(env.nonce.size()), nullptr) != 1) {
        throw CryptoError("EVP_CIPHER_CTX_ctrl SET_IVLEN: " + openssl_last_error());
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                            kek.data(), env.nonce.data()) != 1) {
        throw CryptoError("EVP_DecryptInit_ex (key): " + openssl_last_error());
    }

    // AAD must be supplied before any plaintext output. Pass the entire
    // associated-data blob via DecryptUpdate(..., NULL, &out, ...).
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &out_len,
                           kAadV1, static_cast<int>(sizeof(kAadV1))) != 1) {
        throw CryptoError("EVP_DecryptUpdate (AAD): " + openssl_last_error());
    }

    std::vector<std::uint8_t> plaintext(cipher_len);
    if (cipher_len > 0) {
        if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len,
                               cipher_ptr, static_cast<int>(cipher_len)) != 1) {
            throw DecryptError("EVP_DecryptUpdate failed: " + openssl_last_error());
        }
    }

    // Provide the AEAD tag before Final.
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                             static_cast<int>(kTagBytes),
                             const_cast<std::uint8_t*>(tag_ptr)) != 1) {
        throw CryptoError("EVP_CIPHER_CTX_ctrl SET_TAG: " + openssl_last_error());
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + cipher_len, &final_len) != 1) {
        // Final_ex returning 0 with GCM means the tag failed authentication.
        throw DecryptError(
            "ciphertext failed authentication — KEK mismatch or row tampered");
    }
    plaintext.resize(static_cast<std::size_t>(out_len + final_len));
    return plaintext;
}

std::string decrypt_to_string(const Envelope& env,
                               const std::vector<std::uint8_t>& kek) {
    const auto bytes = decrypt(env, kek);
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace nl_router::crypto
