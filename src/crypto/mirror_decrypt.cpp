#include "crypto/mirror_decrypt.h"
#include "log.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <openssl/evp.h>

namespace ap::crypto {
namespace {

// SHA-512(salt || secret) → first 16 bytes of digest.
bool sha512_first_16(const char*          salt, std::size_t salt_len,
                     const unsigned char* secret, std::size_t secret_len,
                     unsigned char        out[16]) {
    unsigned char digest[64];
    unsigned int  dlen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx &&
              EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, salt, salt_len)         == 1 &&
              EVP_DigestUpdate(ctx, secret, secret_len)     == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &dlen)        == 1 &&
              dlen == 64;
    if (ctx) EVP_MD_CTX_free(ctx);
    if (!ok) return false;

    std::memcpy(out, digest, 16);
    return true;
}

} // namespace

MirrorDecrypt::MirrorDecrypt() = default;

MirrorDecrypt::~MirrorDecrypt() {
    if (ctx_) EVP_CIPHER_CTX_free(ctx_);
}

bool MirrorDecrypt::init(const std::vector<unsigned char>& aes_key_audio,
                         uint64_t stream_connection_id) {
    if (aes_key_audio.size() != 16) {
        LOG_ERROR << "MirrorDecrypt::init: aes_key_audio size "
                  << aes_key_audio.size() << " (want 16)";
        return false;
    }
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }

    // The strings are formatted with the stream ID as PRIu64 to match UxPlay.
    char key_salt[64];
    char iv_salt[64];
    std::snprintf(key_salt, sizeof(key_salt),
                  "AirPlayStreamKey%" PRIu64, stream_connection_id);
    std::snprintf(iv_salt, sizeof(iv_salt),
                  "AirPlayStreamIV%"  PRIu64, stream_connection_id);

    unsigned char video_key[16], video_iv[16];
    if (!sha512_first_16(key_salt, std::strlen(key_salt),
                         aes_key_audio.data(), aes_key_audio.size(),
                         video_key)) return false;
    if (!sha512_first_16(iv_salt,  std::strlen(iv_salt),
                         aes_key_audio.data(), aes_key_audio.size(),
                         video_iv))  return false;

    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_) return false;
    if (EVP_EncryptInit_ex(ctx_, EVP_aes_128_ctr(), nullptr,
                           video_key, video_iv) != 1) {
        LOG_ERROR << "MirrorDecrypt: EVP_EncryptInit_ex(aes-128-ctr) failed";
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }
    LOG_INFO << "MirrorDecrypt initialised (sid=" << stream_connection_id << ')';
    return true;
}

bool MirrorDecrypt::decrypt(unsigned char* data, int len) {
    if (!ctx_ || len <= 0) return false;
    int out_len = 0;
    // AES-CTR is symmetric; EVP_EncryptUpdate does the XOR in-place.
    if (EVP_EncryptUpdate(ctx_, data, &out_len, data, len) != 1) return false;
    return out_len == len;
}

} // namespace ap::crypto
