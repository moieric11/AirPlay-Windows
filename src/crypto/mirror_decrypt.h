#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declare OpenSSL type so this header stays self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::crypto {

// Per-stream AES-128-CTR decryption context for the AirPlay 2 mirror video
// stream. Key derivation copied from UxPlay's mirror_buffer_init_aes
// (GPL-3.0):
//
//   aes_key_video = SHA-512("AirPlayStreamKey<streamConnectionID>" || aes_key_audio)[0..16]
//   aes_iv_video  = SHA-512("AirPlayStreamIV<streamConnectionID>"  || aes_key_audio)[0..16]
//
// where aes_key_audio is the 16-byte output of fairplay_decrypt() over the
// 72-byte ekey iOS sends in SETUP, and streamConnectionID is the uint64
// field iOS sends inside `streams[]` of the per-stream SETUP.
//
// The resulting AES-CTR context is used as a CONTINUOUS stream across all
// encrypted frames of the session — we never re-init between frames, the
// counter advances naturally. UxPlay's mirror_buffer_decrypt manages partial
// blocks by hand; OpenSSL's EVP AES-CTR does it automatically, so our
// `decrypt(data, len)` is simply EVP_EncryptUpdate.
class MirrorDecrypt {
public:
    MirrorDecrypt();
    ~MirrorDecrypt();

    MirrorDecrypt(const MirrorDecrypt&)            = delete;
    MirrorDecrypt& operator=(const MirrorDecrypt&) = delete;

    // Derive the video key/IV and initialise the AES-CTR context. Returns
    // false on any OpenSSL or length error.
    bool init(const std::vector<unsigned char>& aes_key_audio,
              uint64_t stream_connection_id);

    // In-place decrypt `len` bytes of encrypted NAL payload. The EVP context
    // carries the CTR counter between calls.
    bool decrypt(unsigned char* data, int len);

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    EVP_CIPHER_CTX* ctx_{nullptr};
};

} // namespace ap::crypto
