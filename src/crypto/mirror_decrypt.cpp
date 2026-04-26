#include "crypto/mirror_decrypt.h"
#include "log.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <openssl/evp.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace ap::crypto {
namespace {

// One-shot probe so we know whether AES-CTR is going through AES-NI on
// this host. Logged at the first MirrorDecrypt::init() of the process.
//
//   1. CPUID.1:ECX[25] — does the CPU expose the AES-NI ISA extension.
//   2. Encrypt 8 MiB of zeros with EVP_aes_128_ctr() and report MB/s.
//      AES-NI typically lands at 3-5 GB/s; pure-software OpenSSL AES
//      sits around 200-400 MB/s on the same hardware. The ~10× gap
//      makes the path obvious without parsing OpenSSL internals.
void log_aesni_status_once() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        bool cpu_aesni = false;
#if defined(_MSC_VER)
        int cpuid_info[4] = {0, 0, 0, 0};
        __cpuid(cpuid_info, 1);
        cpu_aesni = (cpuid_info[2] & (1 << 25)) != 0;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            cpu_aesni = (ecx & (1u << 25)) != 0;
        }
#endif
        LOG_INFO << "AES-NI CPU support: "
                 << (cpu_aesni ? "yes" : "NO");

        constexpr std::size_t bench_bytes = 8 * 1024 * 1024;
        std::vector<unsigned char> buf(bench_bytes, 0);
        unsigned char key[16] = {0};
        unsigned char iv[16]  = {0};
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        int out_len = 0;
        EVP_EncryptUpdate(ctx, buf.data(), &out_len,
                          buf.data(), static_cast<int>(buf.size()));
        const auto t1 = std::chrono::steady_clock::now();
        EVP_CIPHER_CTX_free(ctx);

        const double sec  = std::chrono::duration<double>(t1 - t0).count();
        const double mbps = (bench_bytes / (1024.0 * 1024.0)) / sec;
        LOG_INFO << "AES-128-CTR bench: " << static_cast<int>(mbps)
                 << " MB/s ("
                 << (mbps > 1500.0 ? "AES-NI active"
                                   : "software path — AES-NI not engaged")
                 << ')';
    });
}

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
    log_aesni_status_once();
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
