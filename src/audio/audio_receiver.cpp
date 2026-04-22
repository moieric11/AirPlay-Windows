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

    // AAC-ELD decoder — ct==4 or ct==8 per observed iOS streams. We try
    // to init unconditionally; if the codec is something else (PCM, ALAC)
    // the packets will fail to decode and we'll log but not crash.
    decoder_ = std::make_unique<AacDecoder>();
    AacDecoder::Config dc;
    dc.ct          = cfg_.ct;
    dc.sample_rate = cfg_.sample_rate;
    dc.channels    = 2;
    dc.spf         = 480;   // observed frame cadence in logs; 512 also seen
    if (!decoder_->init(dc)) {
        LOG_WARN << "AudioReceiver: AAC decoder init failed — running in "
                    "decrypt-only mode";
        decoder_.reset();
    }

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
    uint64_t pkts            = 0;
    uint64_t dedup_dropped   = 0;
    uint64_t total_bytes     = 0;
    uint64_t hist_0_16 = 0, hist_17_64 = 0, hist_65_256 = 0,
             hist_257_512 = 0, hist_513_plus = 0;
    int big_pkts_logged = 0;

    // RAOP retransmits lost packets; iOS typically sends each seq 2-3 times
    // for resilience. We keep a bitset of recently-seen seqs so the decoder
    // only sees each audio frame once. seq is 16-bit — we use a 65 536-bit
    // bitset via a boolean vector (8 KB).
    std::vector<bool> seen_seq(65536, false);
    uint32_t seq_wrap_base = 0;   // if we cross the 16-bit boundary we bump this

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

        if (payload_len <=  16)      ++hist_0_16;
        else if (payload_len <=  64) ++hist_17_64;
        else if (payload_len <= 256) ++hist_65_256;
        else if (payload_len <= 512) ++hist_257_512;
        else                         ++hist_513_plus;

        // Dedup only the "big enough to be real audio" packets — the dwarf
        // sync packets (4 B) are not audio frames, they come with their own
        // overlapping seq values we don't want to blacklist.
        const bool real_audio = (payload_len >= 100);
        bool is_duplicate     = false;
        if (real_audio) {
            if (seen_seq[seq]) {
                is_duplicate = true;
                ++dedup_dropped;
            } else {
                seen_seq[seq] = true;
                // Clear a sliding 8k-wide window behind the new seq so the
                // bitset doesn't stay full forever once we wrap past 65k.
                const uint16_t erase_from = static_cast<uint16_t>(seq - 8192);
                // erase a small contiguous region every 1024 packets to cap
                // the work per packet.
                if ((pkts & 0x3ff) == 0) {
                    for (int k = 0; k < 1024; ++k) {
                        seen_seq[static_cast<uint16_t>(erase_from + k)] = false;
                    }
                }
            }
        }

        // Verbose log for the first couple of everything, AND for the first
        // 5 non-duplicate "big enough to be real audio" packets. That's
        // where the codec signature lives.
        const bool first_any = (pkts <= 2);
        const bool first_big = (real_audio && !is_duplicate && big_pkts_logged < 5);
        if (first_any || first_big) {
            LOG_INFO << "audio pkt#" << pkts << ": pt=" << static_cast<int>(pt)
                     << " seq=" << seq << " ts=" << ts
                     << " paylen=" << payload_len
                     << " (enc=" << encrypted_len << " tail=" << tail_len
                     << (is_duplicate ? " DUP" : "") << ')';
            LOG_INFO << "  clear: " << hex_dump(cleartext,
                                                static_cast<std::size_t>(outlen));
            if (first_big) ++big_pkts_logged;
        }

        // Feed the real-audio (non-duplicate) frames to the AAC decoder.
        // PCM accumulates inside the decoder; once WASAPI is wired up
        // we'll drain it via pull_pcm_s16 from the audio output thread.
        if (real_audio && !is_duplicate && decoder_) {
            int got = decoder_->decode(cleartext, outlen);
            if (got > 0 && decoder_->frames_decoded() <= 3) {
                LOG_INFO << "  decoded " << got << " samples (" << got / 2
                         << " per channel) — total PCM frames out: "
                         << decoder_->frames_decoded();
            }
        }

        if ((pkts % 500) == 0) {
            LOG_INFO << "audio: " << pkts << " pkts, "
                     << (total_bytes / 1024) << " KB received";
        }
    }

    LOG_INFO << "AudioReceiver stopped (" << pkts << " pkts, "
             << total_bytes << " payload bytes, "
             << dedup_dropped << " duplicate audio frames dropped, "
             << (decoder_ ? decoder_->frames_decoded() : 0)
             << " PCM frames decoded)";
    LOG_INFO << "  payload size histogram: "
             << "[0..16]="      << hist_0_16
             << "  [17..64]="   << hist_17_64
             << "  [65..256]="  << hist_65_256
             << "  [257..512]=" << hist_257_512
             << "  [513+]="     << hist_513_plus;
}

} // namespace ap::audio
