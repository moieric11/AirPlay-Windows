#include "audio/audio_receiver.h"
#include "airplay/ntp_client.h"
#include "audio/rtp_flush.h"
#include "log.h"
#include "video/video_renderer.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iterator>

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

bool AudioReceiver::start(Config cfg, PauseState state) {
    if (cfg.data_sock == INVALID_SOCK || cfg.sample_rate <= 0) return false;
    if (cfg.aes_key.size() != 16 || cfg.aes_iv.size() != 16) {
        LOG_ERROR << "AudioReceiver: aes_key/aes_iv must be 16 B "
                  << "(got " << cfg.aes_key.size() << '/' << cfg.aes_iv.size() << ')';
        return false;
    }

    cfg_ = std::move(cfg);
    if (cfg_.preserve_paused_audio && cfg_.synchronize && state.sample_rate == cfg_.sample_rate) {
        paused_pcm_ = std::move(state.pcm);
        paused_at_ns_ = state.paused_at_ns;
        imported_pause_ = !paused_pcm_.empty();
        output_ = std::move(state.output);
        source_lead_frames_ = state.source_lead_frames;
    }

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

    // SDL-backed audio sink. Goes live only if the OS gave us a device;
    // a headless Linux VM will fall back to silent decode-only mode.
    if (decoder_ && !output_) {
        output_ = std::make_unique<SdlAudioOutput>();
        if (!output_->start(cfg_.sample_rate, 2)) {
            LOG_WARN << "AudioReceiver: SDL audio output unavailable — "
                        "PCM will be decoded but not played";
            output_.reset();
        }
    }

    set_recv_timeout(cfg_.data_sock, kRecvTimeoutMs);

    running_ = true;
    synchronize_ = cfg_.synchronize && cfg_.ntp;
    playout_thread_ = std::thread(&AudioReceiver::playout_fn, this);
    thread_  = std::thread(&AudioReceiver::thread_fn, this);
    LOG_INFO << "AudioReceiver listening (ct=" << cfg_.ct
             << ' ' << ct_name(cfg_.ct)
             << ", sample_rate=" << cfg_.sample_rate << ')';
    return true;
}

void AudioReceiver::set_volume_db(float db) {
    if (output_) output_->set_volume_db(db);
}

void AudioReceiver::flush(std::optional<uint32_t> boundary) {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    if (!boundary) imported_pause_ = false;
    const auto before = queued_samples_;
    if (!imported_pause_) {
        paused_pcm_.clear();
        paused_at_ns_ = 0;
    }
    if (!imported_pause_ && cfg_.preserve_paused_audio && synchronize_ && boundary && !pcm_queue_.empty()) {
        paused_pcm_ = pcm_queue_;
        paused_at_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    flush_boundary_ = boundary;
    if (!boundary) {
        pcm_queue_.clear();
        queued_samples_ = 0;
    } else {
        for (auto it = pcm_queue_.begin(); it != pcm_queue_.end();) {
            queued_samples_ -= trim_before(it->rtp, it->samples, *boundary);
            if (it->samples.empty()) it = pcm_queue_.erase(it);
            else ++it;
        }
    }
    clear_output_ = true;
    LOG_INFO << "Audio FLUSH: boundary=" << (boundary ? std::to_string(*boundary) : "all")
             << " removed_frames=" << (before - queued_samples_) / 2
             << " retained_frames=" << queued_samples_ / 2;
    pcm_cv_.notify_all();
}

void AudioReceiver::resume() {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    // A recreated stream has an unrelated RTP epoch. Its first real packet,
    // rather than wall-clock elapsed time, provides the new boundary.
    if (imported_pause_) return;
    if (paused_pcm_.empty()) return;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto elapsed = now - paused_at_ns_;
    // Immediate FLUSH/audioMode pairs are buffer resets, not a user pause.
    if (elapsed < 200'000'000LL || elapsed > 120'000'000'000LL || !synchronize_) {
        paused_pcm_.clear();
        return;
    }
    const auto shift = static_cast<uint32_t>(elapsed * cfg_.sample_rate / 1'000'000'000LL);
    for (auto& frame : paused_pcm_) {
        frame.rtp += shift;
        frame.local_target_ns = 0;
    }
    const auto end = paused_pcm_.back().rtp + static_cast<uint32_t>(paused_pcm_.back().samples.size() / 2);
    // The saved tail precedes newly transmitted audio. Suppress any overlap.
    for (auto it = pcm_queue_.begin(); it != pcm_queue_.end();) {
        trim_before(it->rtp, it->samples, end);
        if (it->samples.empty()) it = pcm_queue_.erase(it); else ++it;
    }
    pcm_queue_.insert(pcm_queue_.begin(),
        std::make_move_iterator(paused_pcm_.begin()), std::make_move_iterator(paused_pcm_.end()));
    paused_pcm_.clear();
    queued_samples_ = 0;
    for (const auto& frame : pcm_queue_) queued_samples_ += frame.samples.size();
    flush_boundary_ = end;
    LOG_INFO << "Audio pause resume: pause_ms=" << elapsed / 1'000'000
             << " restored_frames=" << queued_samples_ / 2 << " end_rtp=" << end;
    pcm_cv_.notify_all();
}

void AudioReceiver::discard_pause() {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    paused_pcm_.clear();
    imported_pause_ = false;
}

AudioReceiver::PauseState AudioReceiver::suspend() {
    // Both workers access the sink. Join before moving its ownership.
    if (running_.exchange(false)) {
        pcm_cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (playout_thread_.joinable()) playout_thread_.join();
    }
    PauseState state;
    if (cfg_.preserve_paused_audio && synchronize_ && !paused_pcm_.empty()) {
        state.pcm = std::move(paused_pcm_);
        state.output = std::move(output_);
        if (state.output) state.output->clear();
        state.sample_rate = cfg_.sample_rate;
        state.paused_at_ns = paused_at_ns_;
        state.source_lead_frames = source_lead_frames_;
        LOG_INFO << "Audio pause suspended: blocks=" << state.pcm.size()
                 << " device_preserved=" << (state.output ? 1 : 0);
    }
    release_resources();
    return state;
}

void AudioReceiver::set_synchronized(bool enabled) {
    if (synchronize_.exchange(enabled && cfg_.ntp) != (enabled && cfg_.ntp)) flush();
}

void AudioReceiver::stop() {
    if (!running_.exchange(false)) return;
    pcm_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (playout_thread_.joinable()) playout_thread_.join();
    release_resources();
}

void AudioReceiver::release_resources() {
    if (cfg_.data_sock != INVALID_SOCK) ap::net::close_socket(cfg_.data_sock);
    cfg_.data_sock = INVALID_SOCK;

    if (output_)  { output_->stop();  output_.reset();  }
    if (decoder_) {                    decoder_.reset(); }

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

    // RAOP retransmits lost packets; iOS typically sends each seq 2-3 times
    // for resilience. We keep a bitset of recently-seen seqs so the decoder
    // only sees each audio frame once. seq is 16-bit — we use a 65 536-bit
    // bitset via a boolean vector (8 KB).
    std::vector<bool> seen_seq(65536, false);
    std::optional<uint16_t> newest_sequence;

    // Silence watchdog: any valid RTP packet pushes the renderer to
    // "playing"; going idle for more than kSilencePauseMs pushes it to
    // "paused". The rate=1 push is UNCONDITIONAL on each packet — other
    // paths (FLUSH, POST /rate, text/parameters rate:) may have set
    // playing=false without telling us, and push_playback_rate is a
    // cheap atomic exchange that no-ops when the state already matches.
    // Without the unconditional push, a tentative FLUSH that arrives
    // during active playback would leave the overlay stuck until the
    // user manually pauses+resumes.
    constexpr int kSilencePauseMs = 500;
    auto last_packet = std::chrono::steady_clock::now();
    // Arm the silence watchdog only after we've seen at least one real
    // audio packet. Otherwise, a fresh mirror session that starts with
    // iOS's keepalive-only dribble (sub-100B packets) would flip the
    // renderer to rate=0 within 500ms and paint a pause badge over the
    // first video frame, even though the iPhone is actively playing.
    bool audio_ever_seen = false;
    uint64_t sync_packets = 0;

    while (running_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(cfg_.data_sock, &readable);
        socket_t highest = cfg_.data_sock;
        if (cfg_.control_sock != INVALID_SOCK) {
            FD_SET(cfg_.control_sock, &readable);
            if (cfg_.control_sock > highest) highest = cfg_.control_sock;
        }
        timeval timeout{0, kRecvTimeoutMs * 1000};
        const int ready = ::select(static_cast<int>(highest + 1), &readable, nullptr, nullptr, &timeout);
        if (!running_) break;
        if (ready > 0 && cfg_.control_sock != INVALID_SOCK &&
            FD_ISSET(cfg_.control_sock, &readable)) {
            unsigned char control[2048];
            const int size = ::recvfrom(cfg_.control_sock, reinterpret_cast<char*>(control), sizeof(control), 0, nullptr, nullptr);
            if (size >= 20 && (control[1] & 0x7f) == 0x54) {
                const auto be32 = [&](int offset) {
                    return (static_cast<uint32_t>(control[offset]) << 24)
                         | (static_cast<uint32_t>(control[offset + 1]) << 16)
                         | (static_cast<uint32_t>(control[offset + 2]) << 8)
                         | control[offset + 3];
                };
                const uint64_t ntp = (static_cast<uint64_t>(be32(8)) << 32) | be32(12);
                ++sync_packets;
                {
                    std::lock_guard<std::mutex> lock(pcm_mutex_);
                    sync_rtp_ = be32(4);
                    sync_ntp_ = ntp;
                    have_anchor_ = ntp != 0;
                    const uint32_t lead = be32(16) - be32(4);
                    if (lead <= static_cast<uint32_t>(cfg_.sample_rate * 5))
                        source_lead_frames_ = lead;
                }
                pcm_cv_.notify_all();
                if (sync_packets <= 3 || sync_packets % 10 == 0)
                    LOG_INFO << "Audio timing anchor: rtp=" << be32(4)
                             << " ntp_raw=" << ntp << " next_rtp=" << be32(16);
            }
        }
        if (ready <= 0 || !FD_ISSET(cfg_.data_sock, &readable)) {
            if (cfg_.renderer && audio_ever_seen &&
                std::chrono::steady_clock::now() - last_packet >= std::chrono::milliseconds(kSilencePauseMs))
                cfg_.renderer->push_playback_rate(0.0f);
            continue;
        }
        int n = ::recvfrom(cfg_.data_sock,
                           reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           nullptr, nullptr);
        if (n < 0) {
            if (cfg_.renderer && audio_ever_seen) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_packet).count();
                if (idle >= kSilencePauseMs) {
                    cfg_.renderer->push_playback_rate(0.0f);
                }
            }
            continue;
        }
        if (n < 12) continue;   // RTP header minimum
        if (!have_first_packet_) {
            std::lock_guard<std::mutex> lock(pcm_mutex_);
            first_packet_rtp_ = (static_cast<uint32_t>(buf[4]) << 24)
                | (static_cast<uint32_t>(buf[5]) << 16)
                | (static_cast<uint32_t>(buf[6]) << 8) | buf[7];
            first_packet_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            have_first_packet_ = true;
        }

        // Only count substantial packets (real audio) as proof of
        // active playback. During pause iOS keeps dribbling small
        // keepalive / silence-frame packets (observed around 32 B
        // payload) at the normal packet rate — they used to keep
        // resetting the silence watchdog and flipping rate back to 1.
        // Real AAC-ELD / ALAC audio frames are 400+ bytes once any
        // musical content is present, so a 100-byte threshold separates
        // the two cleanly.
        constexpr int kAudioPacketMinBytes = 100;
        if (n >= kAudioPacketMinBytes) {
            last_packet = std::chrono::steady_clock::now();
            audio_ever_seen = true;
            // Suppress rate=1 during the post-FLUSH grace window: iOS
            // still drains its buffer with real audio for a few hundred
            // ms after a real pause FLUSH.
            if (cfg_.renderer && !cfg_.renderer->in_flush_grace()) {
                cfg_.renderer->push_playback_rate(1.0f);
            }
        }

        // Parse RTP header (RFC 3550, 12-byte fixed part).
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
            // Age history by sequence progress, including across 16-bit wrap.
            // Packet-count cleanup can be skipped by duplicate arrivals.
            constexpr uint16_t history = 8192;
            if (newest_sequence) {
                const uint16_t distance = static_cast<uint16_t>(seq - *newest_sequence);
                if (distance && distance < 32768) {
                    if (distance >= history) std::fill(seen_seq.begin(), seen_seq.end(), false);
                    else for (uint16_t step = 1; step <= distance; ++step)
                        seen_seq[static_cast<uint16_t>(*newest_sequence + step - history)] = false;
                    newest_sequence = seq;
                } else if (static_cast<uint16_t>(*newest_sequence - seq) >= history) {
                    is_duplicate = true;
                }
            } else newest_sequence = seq;
            is_duplicate = is_duplicate || seen_seq[seq];
            if (is_duplicate) ++dedup_dropped;
            else seen_seq[seq] = true;
        }

        // Feed the real-audio (non-duplicate) frames to the AAC decoder,
        // then immediately drain any PCM it produced into the SDL sink.
        if (real_audio && !is_duplicate && decoder_) {
            int got = decoder_->decode(cleartext, outlen);
            if (got > 0 && decoder_->frames_decoded() <= 3) {
                LOG_INFO << "  decoded " << got << " samples (" << got / 2
                         << " per channel) — total PCM frames out: "
                         << decoder_->frames_decoded();
            }

            if (got > 0) {
                // Drain every available PCM sample from the decoder's
                // queue into SDL. We do it in one pull — buffer is large
                // enough for the ~1920 stereo samples of a single AAC-ELD
                // frame at 480 spf.
                int16_t pcm[8192];
                int     have = 0;
                uint32_t pcm_rtp = ts;
                while ((have = decoder_->pull_pcm_s16(
                            pcm, static_cast<int>(sizeof(pcm) / sizeof(pcm[0])))) > 0) {
                    if (cfg_.renderer) {
                        cfg_.renderer->push_audio_pcm(
                            pcm, have, cfg_.sample_rate, 2);
                    }
                    {
                        std::lock_guard<std::mutex> lock(pcm_mutex_);
                        // Bound decoded PCM to five seconds, including when
                        // sender timing is unavailable or malformed.
                        const auto limit = static_cast<std::size_t>(cfg_.sample_rate) * 2 * 5;
                        while (!pcm_queue_.empty() && queued_samples_ + have > limit) {
                            queued_samples_ -= pcm_queue_.front().samples.size();
                            pcm_queue_.pop_front();
                        }
                        PcmFrame frame{pcm_rtp, std::vector<int16_t>(pcm, pcm + have)};
                        // FLUSH can precede the last already-sent musical packets.
                        // Retain their pre-boundary suffix rather than silently
                        // losing a few milliseconds at the saved/live junction.
                        if (!imported_pause_ && flush_boundary_ && !paused_pcm_.empty()) {
                            const auto end = paused_pcm_.back().rtp +
                                static_cast<uint32_t>(paused_pcm_.back().samples.size() / 2);
                            auto drain = frame;
                            trim_before(drain.rtp, drain.samples, end);
                            const uint32_t remaining = *flush_boundary_ - drain.rtp;
                            if (drain.rtp == end && remaining <= static_cast<uint32_t>(cfg_.sample_rate * 5)
                                && !drain.samples.empty()) {
                                if (drain.samples.size() / 2 > remaining)
                                    drain.samples.resize(static_cast<std::size_t>(remaining) * 2);
                                if (!drain.samples.empty()) paused_pcm_.push_back(std::move(drain));
                            }
                        }
                        if (imported_pause_) {
                            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (now - paused_at_ns_ < 120'000'000'000LL && !paused_pcm_.empty()) {
                                const uint32_t old_end = paused_pcm_.back().rtp +
                                    static_cast<uint32_t>(paused_pcm_.back().samples.size() / 2);
                                const uint32_t shift = frame.rtp - old_end;
                                std::size_t retained_frames = 0;
                                for (const auto& saved : paused_pcm_) retained_frames += saved.samples.size() / 2;
                                // The old sender anchor reports its transmission lead.
                                // Align the saved tail's end with the estimated live
                                // deadline, allowing one device buffer for network
                                // jitter. This keeps the splice fed while the new
                                // stream's first RTCP anchor is still outstanding.
                                const int64_t packet_delta = static_cast<int32_t>(frame.rtp - first_packet_rtp_);
                                const int64_t fresh_estimate = first_packet_ns_
                                    + (packet_delta + source_lead_frames_) * 1'000'000'000LL / cfg_.sample_rate;
                                const int64_t tail_duration = static_cast<int64_t>(retained_frames)
                                    * 1'000'000'000LL / cfg_.sample_rate;
                                const int64_t device_margin = output_ ? output_->pending_ns() : 0;
                                int64_t retained_target = std::max(now, fresh_estimate - tail_duration + device_margin);
                                for (auto& saved : paused_pcm_) {
                                    saved.rtp += shift;
                                    saved.local_target_ns = retained_target;
                                    retained_target += static_cast<int64_t>(saved.samples.size() / 2)
                                        * 1'000'000'000LL / cfg_.sample_rate;
                                    queued_samples_ += saved.samples.size();
                                    pcm_queue_.push_back(std::move(saved));
                                }
                                LOG_INFO << "Audio pause restored across stream: frames=" << queued_samples_ / 2
                                         << " new_rtp=" << frame.rtp;
                            }
                            paused_pcm_.clear();
                            imported_pause_ = false;
                        }
                        if (flush_boundary_) trim_before(frame.rtp, frame.samples, *flush_boundary_);
                        if (!frame.samples.empty()) {
                            // Boundary reached; normal duplicate/late handling
                            // takes over instead of keeping a fence for hours.
                            flush_boundary_.reset();
                            queued_samples_ += frame.samples.size();
                            pcm_queue_.push_back(std::move(frame));
                        }
                    }
                    pcm_rtp += static_cast<uint32_t>(have / 2);
                    pcm_cv_.notify_one();
                }
            }
        }

        if ((pkts % 500) == 0) {
            const uint64_t queue_ms = output_
                ? static_cast<uint64_t>(output_->queued_bytes()) * 1000 / (cfg_.sample_rate * 2 * sizeof(int16_t))
                : 0;
            LOG_INFO << "audio: " << pkts << " pkts, "
                     << (total_bytes / 1024) << " KB received, rtp=" << ts
                     << " sink_queue=" << queue_ms << " ms, sync_packets=" << sync_packets;
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

void AudioReceiver::playout_fn() {
    using Clock = std::chrono::steady_clock;
    const auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
    };
    // Clock acquisition is a startup grace period, not a new delay to insert
    // every time a FLUSH or pause empties the PCM queue.
    const auto clock_deadline = Clock::now() + std::chrono::seconds(2);
    bool fallback_logged = false;
    bool scheduled_logged = false;
    uint64_t played = 0, late_dropped = 0;
    std::unique_lock<std::mutex> lock(pcm_mutex_);
    while (running_) {
        if (clear_output_) {
            if (output_) output_->clear();
            clear_output_ = false;
        }
        if (pcm_queue_.empty()) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }

        std::optional<int64_t> target;
        if (pcm_queue_.front().local_target_ns) {
            target = pcm_queue_.front().local_target_ns;
        } else if (synchronize_ && cfg_.ntp && have_anchor_) {
            target = cfg_.ntp->local_time_ns(sync_ntp_);
            if (target) {
                const uint32_t ticks = pcm_queue_.front().rtp - sync_rtp_;
                const int64_t delta = ticks <= 0x7fffffffU
                    ? ticks : static_cast<int64_t>(ticks) - 0x100000000LL;
                *target += delta * 1'000'000'000LL / cfg_.sample_rate;
            }
        }
        if (synchronize_ && !target && cfg_.ntp && cfg_.ntp->running() && Clock::now() < clock_deadline) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        if (synchronize_ && !target && !fallback_logged) {
            LOG_WARN << "Audio sync unavailable; playing without timestamp scheduling (no repeated startup wait)";
            fallback_logged = true;
        }
        const int64_t pending = output_ ? output_->pending_ns() : 0;
        // Keep future PCM under our control: SDL can only flush its entire queue.
        if (synchronize_ && pending > 60'000'000LL) {
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }
        const int64_t advance = target ? *target - now_ns() - pending : 0;
        if (target && !scheduled_logged) {
            LOG_INFO << "Audio synchronized playout: initial_wait_ms=" << advance / 1'000'000;
            scheduled_logged = true;
        }
        // Ignore broken anchors rather than buffering forever.
        if (target && advance > 5'000'000'000LL) {
            target.reset();
        } else if (target && advance > 1'000'000LL) {
            pcm_cv_.wait_for(lock, std::chrono::nanoseconds(std::min<int64_t>(advance, 10'000'000LL)));
            continue;
        } else if (target && advance < -250'000'000LL) {
            queued_samples_ -= pcm_queue_.front().samples.size();
            pcm_queue_.pop_front();
            ++late_dropped;
            if (late_dropped == 1 || late_dropped % 500 == 0)
                LOG_WARN << "Audio late frames dropped=" << late_dropped
                         << " behind_ms=" << -advance / 1'000'000;
            continue;
        }

        auto frame = std::move(pcm_queue_.front());
        pcm_queue_.pop_front();
        queued_samples_ -= frame.samples.size();
        // Keep flush and enqueue serialized: after FLUSH is observed no
        // already-dequeued PCM can be reinserted into SDL by this thread.
        if (output_) output_->push(frame.samples.data(), static_cast<int>(frame.samples.size()));
        if (++played <= 3 || played % 500 == 0)
            LOG_INFO << "Audio playout: synchronized=" << (target ? 1 : 0)
                     << " remaining=" << queued_samples_ / 2 << " samples, sink_ms="
                     << pending / 1'000'000 << " late_dropped=" << late_dropped;
    }
    pcm_queue_.clear();
    queued_samples_ = 0;
}

} // namespace ap::audio
