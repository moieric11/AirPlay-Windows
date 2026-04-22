#include "audio/audio_receiver.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include <openssl/evp.h>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <errno.h>
#endif

namespace ap::audio {
namespace {

constexpr int kRecvTimeoutMs = 200;

void set_recv_timeout(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

std::string hex_dump(const unsigned char* b, std::size_t n, std::size_t max = 32) {
    std::ostringstream os;
    for (std::size_t i = 0; i < n && i < max; ++i) {
        char tmp[4];
        std::snprintf(tmp, sizeof(tmp), "%02x ", b[i]);
        os << tmp;
    }
    if (n > max) os << "...(" << n << "B)";
    return os.str();
}

} // namespace

// RAOP compression-type values, from UxPlay/global.h and observed sessions.
const char* ct_name(int ct) {
    switch (ct) {
        case 0: return "unspecified";
        case 1: return "PCM";
        case 2: return "ALAC";
        case 3: return "AAC-LC";
        case 4: return "AAC-ELD";      // common on AirPlay 2 (Apple Music)
        case 8: return "AAC-ELD 44.1k";
        default: return "unknown";
    }
}

AudioReceiver::AudioReceiver()  = default;
AudioReceiver::~AudioReceiver() { stop(); }

bool AudioReceiver::start(Config cfg) {
    if (cfg.data_sock == INVALID_SOCK) return false;
    if (cfg.aes_key.size() != 16 || cfg.aes_iv.size() != 16) {
        LOG_ERROR << "AudioReceiver: aes_key/aes_iv must be 16 B "
                  << "(got " << cfg.aes_key.size() << '/' << cfg.aes_iv.size() << ')';
        return false;
    }

    cfg_ = std::move(cfg);

    aes_ctx_ = EVP_CIPHER_CTX_new();
    if (!aes_ctx_ ||
        EVP_DecryptInit_ex(aes_ctx_, EVP_aes_128_cbc(), nullptr,
                           cfg_.aes_key.data(), cfg_.aes_iv.data()) != 1) {
        LOG_ERROR << "AudioReceiver: EVP_DecryptInit_ex(aes-128-cbc) failed";
        if (aes_ctx_) { EVP_CIPHER_CTX_free(aes_ctx_); aes_ctx_ = nullptr; }
        return false;
    }
    EVP_CIPHER_CTX_set_padding(aes_ctx_, 0);

    set_recv_timeout(cfg_.data_sock, kRecvTimeoutMs);

    running_ = true;
    thread_  = std::thread(&AudioReceiver::thread_fn, this);
    LOG_INFO << "AudioReceiver listening (ct=" << cfg_.ct
             << ' ' << ct_name(cfg_.ct)
             << ", sample_rate=" << cfg_.sample_rate << ')';
    return true;
}

void AudioReceiver::stop() {
    if (!running_.exchange(false)) return;

    if (cfg_.data_sock != INVALID_SOCK) {
        ap::net::close_socket(cfg_.data_sock);
        cfg_.data_sock = INVALID_SOCK;
    }
    if (thread_.joinable()) thread_.join();

    if (aes_ctx_) {
        EVP_CIPHER_CTX_free(aes_ctx_);
        aes_ctx_ = nullptr;
    }
}

void AudioReceiver::thread_fn() {
    unsigned char buf[4096];
    unsigned char cleartext[4096];
    uint64_t pkts         = 0;
    uint64_t total_bytes  = 0;

    while (running_.load()) {
        int n = ::recvfrom(cfg_.data_sock,
                           reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           nullptr, nullptr);
        if (n < 0) {
            // Timeout or shutdown; loop back to check running_.
            continue;
        }
        if (n < 12) continue;   // RTP header minimum

        // Parse RTP header (RFC 3550, 12-byte fixed part).
        const uint8_t  pt  =  buf[1] & 0x7f;
        const uint16_t seq = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
        const uint32_t ts  = (static_cast<uint32_t>(buf[4]) << 24)
                           | (static_cast<uint32_t>(buf[5]) << 16)
                           | (static_cast<uint32_t>(buf[6]) <<  8)
                           |  static_cast<uint32_t>(buf[7]);

        const int payload_len   = n - 12;
        const int encrypted_len = (payload_len / 16) * 16;
        const int tail_len      = payload_len - encrypted_len;

        // Reset IV before decrypting each packet — UxPlay does the same
        // via aes_cbc_reset(). The aes_iv from SETUP is therefore used
        // as the IV of every packet (packets are independent).
        EVP_DecryptInit_ex(aes_ctx_, nullptr, nullptr, nullptr,
                           cfg_.aes_iv.data());
        int outlen = 0;
        if (encrypted_len > 0) {
            EVP_DecryptUpdate(aes_ctx_, cleartext, &outlen,
                              buf + 12, encrypted_len);
        }
        if (tail_len > 0) {
            std::memcpy(cleartext + outlen, buf + 12 + encrypted_len,
                        static_cast<std::size_t>(tail_len));
            outlen += tail_len;
        }

        ++pkts;
        total_bytes += static_cast<uint64_t>(payload_len);

        if (pkts <= 3) {
            LOG_INFO << "audio pkt#" << pkts << ": pt=" << static_cast<int>(pt)
                     << " seq=" << seq << " ts=" << ts
                     << " paylen=" << payload_len;
            LOG_INFO << "  clear: " << hex_dump(cleartext, static_cast<std::size_t>(outlen));
        } else if ((pkts % 500) == 0) {
            LOG_INFO << "audio: " << pkts << " pkts, "
                     << (total_bytes / 1024) << " KB received";
        }
    }

    LOG_INFO << "AudioReceiver stopped (" << pkts << " pkts, "
             << total_bytes << " payload bytes)";
}

} // namespace ap::audio
