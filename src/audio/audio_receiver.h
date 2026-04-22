#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Forward-declare OpenSSL type to keep this header self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::audio {

// UDP audio receiver for the AirPlay RAOP stream (type 96).
//
// Ported from UxPlay's raop_buffer.c (GPL-3.0). Packets arrive on the
// UDP data port negotiated during SETUP. Each packet is:
//
//   [12 bytes RTP header][AES-128-CBC encrypted payload]
//
// The AES key/IV come from SETUP (the same `aes_key` post-hashed against
// the ECDH secret, and `aes_iv` as-is). For every packet, the AES context
// is re-initialised with the SAME iv (UxPlay calls this "aes_cbc_reset"
// after each decrypt) so packets stay independent — trailing bytes that
// don't fit into a full 16-byte block are copied unchanged.
//
// The decrypted payload is a codec frame (ALAC / AAC-ELD / AAC-LC
// depending on the `ct` negotiated in SETUP). We don't decode yet: the
// receiver just logs a hex dump of the first packet so the codec can be
// identified by its signature, then counts packets for telemetry. The
// full decode + playback path (FFmpeg → WASAPI) comes next.
class AudioReceiver {
public:
    struct Config {
        socket_t                    data_sock = INVALID_SOCK; // ownership transferred
        std::vector<unsigned char>  aes_key;                   // 16 B
        std::vector<unsigned char>  aes_iv;                    // 16 B
        int                         ct          = 0;           // compression type
        int                         sample_rate = 44100;
    };

    AudioReceiver();
    ~AudioReceiver();

    AudioReceiver(const AudioReceiver&)            = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    bool start(Config cfg);
    void stop();

private:
    void thread_fn();

    Config            cfg_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
    EVP_CIPHER_CTX*   aes_ctx_{nullptr};
};

// Human-readable label for a RAOP "ct" (compression type) value.
const char* ct_name(int ct);

} // namespace ap::audio
