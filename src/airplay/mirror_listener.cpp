#include "airplay/mirror_listener.h"
#include "airplay/h264_nal.h"
#include "airplay/h264_sps.h"
#include "log.h"

#include <array>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <errno.h>
#endif

namespace ap::airplay {
namespace {

constexpr int kHeaderSize       = 128;
constexpr int kMaxPayloadSize   = 2'000'000;   // generous bound vs RAOP_PACKET_LEN (32 KB)
constexpr auto kRecvTimeoutMs   = 200;         // per-recv timeout; loops check running_

enum : uint16_t {
    kFrameVideoNonIdr   = 0x0000,
    kFrameVideoIdr      = 0x0010,
    kFrameSpsPps        = 0x0100,
    kFrameHeartbeat     = 0x0200,
    kFrameStreamReport  = 0x0500,
};

const char* frame_name(uint16_t t) {
    switch (t) {
        case kFrameVideoNonIdr:  return "VIDEO_NON_IDR";
        case kFrameVideoIdr:     return "VIDEO_IDR";
        case kFrameSpsPps:       return "SPS_PPS";
        case kFrameHeartbeat:    return "HEARTBEAT";
        case kFrameStreamReport: return "STREAM_REPORT";
        default:                 return "UNKNOWN";
    }
}

uint32_t get_u32_le(const unsigned char* b, int off) {
    return  static_cast<uint32_t>(b[off + 0])
         | (static_cast<uint32_t>(b[off + 1]) <<  8)
         | (static_cast<uint32_t>(b[off + 2]) << 16)
         | (static_cast<uint32_t>(b[off + 3]) << 24);
}

// Search the SPS_PPS payload for the fourcc of an inner config box,
// returning the offset of the first byte of the box content (after
// the 8-byte size+type header) or std::string::npos. iOS HEVC mirror
// wraps the hvcC config in an MP4 VisualSampleEntry; this lets the
// logger surface the wrapped form without duplicating the decoder's
// unwrap logic.
std::size_t find_inner_box_offset(const std::vector<unsigned char>& payload,
                                  const char fourcc[4]) {
    for (std::size_t i = 4; i + 4 <= payload.size(); ++i) {
        if (payload[i + 0] == static_cast<uint8_t>(fourcc[0]) &&
            payload[i + 1] == static_cast<uint8_t>(fourcc[1]) &&
            payload[i + 2] == static_cast<uint8_t>(fourcc[2]) &&
            payload[i + 3] == static_cast<uint8_t>(fourcc[3])) {
            return i + 4;
        }
    }
    return std::string::npos;
}

// SPS_PPS packet payload — for H.264 mirror it's the raw AVCC "avcC"
// configuration record:
//
//   [0]    configurationVersion = 1
//   [1]    AVCProfileIndication
//   [2]    profile_compatibility
//   [3]    AVCLevelIndication
//   [4]    reserved(6)=1 + lengthSizeMinusOne(2)
//   [5]    reserved(3)=1 + numOfSequenceParameterSets(5)
//   [6..7] sequenceParameterSetLength (BE uint16)  ← assumes numSPS >= 1
//   [8..]  SPS NAL bytes (incl. 1-byte NAL header)
//   then:  numOfPictureParameterSets, ppsLength(BE16), PPS NAL
//
// For HEVC mirror the payload is an MP4 VisualSampleEntry ("hvc1")
// wrapping an "hvcC" inner box. We log a one-liner with codec +
// payload size in that case — the decoder will unwrap and parse the
// real config separately, so duplicating that work here just for a
// log line isn't worth it.
bool log_sps_from_sps_pps_payload(const std::vector<unsigned char>& payload) {
    if (payload.size() < 8) {
        LOG_WARN << "mirror: SPS/PPS payload too short ("
                 << payload.size() << "B)";
        return false;
    }
    if (payload[0] == 0x01) {
        // Raw avcC (the legacy H.264 mirror path).
        const uint8_t num_sps = payload[5] & 0x1f;
        if (num_sps == 0) {
            LOG_WARN << "mirror: avcC declares 0 SPS";
            return false;
        }
        const uint32_t sps_len =
            (static_cast<uint32_t>(payload[6]) << 8) | payload[7];
        if (sps_len == 0 || sps_len + 8 > payload.size()) {
            LOG_WARN << "mirror: avcC SPS length " << sps_len
                     << " out of range";
            return false;
        }
        const uint8_t* sps = payload.data() + 8;
        ap::airplay::SpsInfo info;
        if (!ap::airplay::parse_h264_sps(sps, sps_len, info)) {
            LOG_WARN << "mirror: SPS present but failed to parse";
            return false;
        }
        LOG_INFO << "mirror video: H.264 "
                 << ap::airplay::profile_name(info.profile_idc)
                 << " level " << (info.level_idc / 10) << '.'
                 << (info.level_idc % 10)
                 << ", " << info.width << 'x' << info.height
                 << (info.interlaced ? " (interlaced)" : "");
        return true;
    }
    // First byte != 0x01: probably an MP4-wrapped sample entry. Look
    // for an inner avcC or hvcC box and surface a corresponding line.
    if (find_inner_box_offset(payload, "hvcC") != std::string::npos) {
        LOG_INFO << "mirror video: HEVC (hvcC config wrapped in "
                 << payload.size() << "B SampleEntry)";
        return true;
    }
    if (find_inner_box_offset(payload, "avcC") != std::string::npos) {
        LOG_INFO << "mirror video: H.264 (avcC config wrapped in "
                 << payload.size() << "B SampleEntry)";
        return true;
    }
    LOG_WARN << "mirror: SPS/PPS — unexpected format, first byte 0x"
             << std::hex << static_cast<int>(payload[0]);
    return false;
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

uint64_t get_u64_le(const unsigned char* b, int off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + i];
    return v;
}

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

bool was_timeout() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Returns len on success, 0 on peer close, -1 on real error.
// Honours running_ flag by checking between recv calls (SO_RCVTIMEO caps each
// individual recv at kRecvTimeoutMs).
template <typename RunningFn>
int recv_exact(socket_t s, unsigned char* buf, int len, RunningFn running) {
    int got = 0;
    while (got < len) {
        int n = ::recv(s, reinterpret_cast<char*>(buf + got), len - got, 0);
        if (n > 0) { got += n; continue; }
        if (n == 0) return 0;                 // peer closed
        if (was_timeout() && running()) continue;
        return -1;                             // real error, or stop requested
    }
    return got;
}

} // namespace

MirrorListener::MirrorListener() = default;
MirrorListener::~MirrorListener() { stop(); }

bool MirrorListener::enable_decrypt(
        const std::vector<unsigned char>& aes_key_audio,
        uint64_t stream_connection_id) {
    decrypt_ = std::make_unique<ap::crypto::MirrorDecrypt>();
    if (!decrypt_->init(aes_key_audio, stream_connection_id)) {
        decrypt_.reset();
        return false;
    }
    decoder_ = std::make_unique<ap::video::H264Decoder>();
    if (!decoder_->init()) {
        LOG_WARN << "H264Decoder init failed — decrypted NALs will be logged "
                    "but not decoded";
        decoder_.reset();
    }
    return true;
}

bool MirrorListener::start(uint16_t& port) {
    // Dual-stack IPv6 listener so iOS reaches the mirror port whether
    // it chose to connect over v4 or v6. This matches what the main
    // TCP server and UDP audio sockets do; the previous IPv4-only
    // bind silently dropped every v6 connect attempt, giving audio
    // only (which uses a separate UDP socket) when the iPhone reached
    // us via IPv6.
    listen_sock_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCK) {
        // Kernel without IPv6: fall back to v4.
        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == INVALID_SOCK) {
            LOG_ERROR << "mirror: TCP socket() failed";
            return false;
        }
        int on = 1;
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = 0;
        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            LOG_ERROR << "mirror: bind(v4) failed";
            ap::net::close_socket(listen_sock_); listen_sock_ = INVALID_SOCK;
            return false;
        }
#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        ::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (::listen(listen_sock_, 1) != 0) {
            LOG_ERROR << "mirror: listen() failed";
            ap::net::close_socket(listen_sock_); listen_sock_ = INVALID_SOCK;
            return false;
        }
        port     = ntohs(addr.sin_port);
        running_ = true;
        thread_  = std::thread(&MirrorListener::accept_loop, this);
        LOG_INFO << "mirror TCP listener on port " << port << " (v4 only)";
        return true;
    }

    int on = 1;
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    int v6only = 0;
    ::setsockopt(listen_sock_, IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = 0;
    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR << "mirror: bind(v6 dual-stack) failed";
        ap::net::close_socket(listen_sock_); listen_sock_ = INVALID_SOCK;
        return false;
    }
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ap::net::close_socket(listen_sock_); listen_sock_ = INVALID_SOCK;
        return false;
    }
    if (::listen(listen_sock_, 1) != 0) {
        LOG_ERROR << "mirror: listen() failed";
        ap::net::close_socket(listen_sock_); listen_sock_ = INVALID_SOCK;
        return false;
    }

    port     = ntohs(addr.sin6_port);
    running_ = true;
    thread_  = std::thread(&MirrorListener::accept_loop, this);
    LOG_INFO << "mirror TCP listener on port " << port << " (IPv4+IPv6)";
    return true;
}

void MirrorListener::stop() {
    if (!running_.exchange(false)) return;
    if (listen_sock_ != INVALID_SOCK) {
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
    }
    if (thread_.joinable()) thread_.join();
}

void MirrorListener::accept_loop() {
    while (running_) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_sock_, &rfds);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;
        int ret = ::select(static_cast<int>(listen_sock_) + 1, &rfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;
        if (listen_sock_ == INVALID_SOCK) break;

        // sockaddr_storage covers both AF_INET and AF_INET6 without a
        // preliminary branch. Format the peer address after accept.
        sockaddr_storage peer{};
#if defined(_WIN32)
        int plen = sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        socket_t client = ::accept(listen_sock_,
                                   reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client == INVALID_SOCK) continue;

        char ip[INET6_ADDRSTRLEN] = {0};
        uint16_t peer_port = 0;
        if (peer.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&peer);
            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                const uint32_t v4 = reinterpret_cast<uint32_t*>(&in6->sin6_addr)[3];
                in_addr a{}; a.s_addr = v4;
                inet_ntop(AF_INET, &a, ip, sizeof(ip));
            } else {
                inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
            }
            peer_port = ntohs(in6->sin6_port);
        } else if (peer.ss_family == AF_INET) {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&peer);
            inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
            peer_port = ntohs(in4->sin_port);
        }
        LOG_INFO << "mirror: iOS connected from " << ip << ':' << peer_port;

        set_recv_timeout(client, kRecvTimeoutMs);

        reader_loop(client);

        ap::net::close_socket(client);
    }
}

void MirrorListener::reader_loop(socket_t client) {
    std::array<unsigned char, kHeaderSize> header{};
    std::map<uint16_t, uint64_t> counts;
    std::map<uint16_t, uint64_t> bytes_by_type;
    std::map<int, uint64_t>      nal_counts;
    uint64_t frames       = 0;
    uint64_t total_header = 0;
    uint64_t total_body   = 0;

    auto running_fn = [this]() -> bool { return running_.load(); };

    while (running_) {
        int n = recv_exact(client, header.data(), kHeaderSize, running_fn);
        if (n == 0) { LOG_INFO << "mirror: iOS closed stream"; break; }
        if (n < 0)  { LOG_WARN << "mirror: header recv error";  break; }

        uint32_t payload_size = get_u32_le(header.data(), 0);
        uint16_t ftype        = (static_cast<uint16_t>(header[4]) << 8) | header[5];
        uint16_t fopt         = (static_cast<uint16_t>(header[6]) << 8) | header[7];
        uint64_t ts           = get_u64_le(header.data(), 8);

        if (payload_size > static_cast<uint32_t>(kMaxPayloadSize)) {
            LOG_ERROR << "mirror: insane payload_size " << payload_size
                      << " — likely desync, dropping connection";
            break;
        }

        std::vector<unsigned char> payload;
        if (payload_size > 0) {
            payload.resize(payload_size);
            n = recv_exact(client, payload.data(),
                           static_cast<int>(payload_size), running_fn);
            if (n == 0) { LOG_INFO << "mirror: iOS closed mid-payload"; break; }
            if (n < 0)  { LOG_WARN << "mirror: payload recv error";     break; }
        }

        ++frames;
        total_header += kHeaderSize;
        total_body   += payload_size;
        counts[ftype]++;
        bytes_by_type[ftype] += payload_size;

        // Surface the encrypted-frame body byte count to the renderer
        // so the status bar can report a live mirror bandwidth without
        // the renderer needing to peek into us.
        if (renderer_) {
            renderer_->record_payload_bytes(
                kHeaderSize + static_cast<std::size_t>(payload_size));
        }

        // Verbose: first 5 frames regardless of type, plus every IDR/SPS_PPS.
        const bool verbose = (frames <= 5)
                          || (ftype == kFrameVideoIdr)
                          || (ftype == kFrameSpsPps);
        if (verbose) {
            LOG_INFO << "mirror frame[" << frames << "]: "
                     << frame_name(ftype)
                     << "  size=" << payload_size
                     << "  opt=0x" << std::hex << fopt << std::dec
                     << "  ts=" << ts;
        }

        // On the first SPS/PPS frame, log the raw bytes + human-readable
        // video characteristics. Every SPS/PPS (initial and resolution
        // changes) is forwarded to the decoder.
        if (ftype == kFrameSpsPps && !payload.empty()) {
            if (counts[kFrameSpsPps] == 1) {
                LOG_INFO << "mirror SPS/PPS hex: "
                         << hex_dump(payload.data(), payload.size());
                log_sps_from_sps_pps_payload(payload);
            }
            if (decoder_) {
                decoder_->set_parameter_sets_from_avcc(payload.data(),
                                                       payload.size());
            }
        }

        // Encrypted frames (IDR + non-IDR): AES-CTR-decrypt in place, then
        // split the resulting cleartext AVCC stream into individual NAL
        // units while converting to Annex-B (start-code-prefixed).
        const bool encrypted = (ftype == kFrameVideoIdr ||
                                ftype == kFrameVideoNonIdr);
        if (encrypted && decrypt_ && payload_size >= 5) {
            if (!decrypt_->decrypt(payload.data(), static_cast<int>(payload_size))) {
                LOG_WARN << "mirror: AES-CTR decrypt failed on frame " << frames;
            } else {
                // Pick the right NAL-header layout based on which codec
                // the SPS/PPS frame configured. is_hevc() flips after
                // set_parameter_sets_from_avcc parses an hvcC blob.
                const NalCodec nal_codec =
                    (decoder_ && decoder_->is_hevc()) ? NalCodec::HEVC
                                                      : NalCodec::H264;
                std::vector<NalUnit> nals;
                const bool ok = parse_nals_avcc_to_annexb(
                    payload.data(), payload_size, nals, nal_codec);

                if (!ok) {
                    LOG_WARN << "mirror frame[" << frames << "]: malformed NAL "
                                "stream after decrypt ("
                             << nals.size() << " NAL(s) parsed before stopping)";
                } else {
                    for (const auto& n : nals) nal_counts[n.type]++;
                    // Verbose: first IDR, first non-IDR, and any multi-NAL frame.
                    const bool first_of_kind = (counts[ftype] == 1);
                    const bool multi_nal     = (nals.size() > 1);
                    if (first_of_kind || multi_nal) {
                        LOG_INFO << "mirror frame[" << frames << "] "
                                 << frame_name(ftype) << " → "
                                 << nals.size() << " NAL(s):";
                        for (const auto& n : nals) {
                            LOG_INFO << "  NAL "
                                     << nal_type_name(n.type, nal_codec)
                                     << "(type=" << n.type
                                     << ", ref_idc=" << n.ref_idc
                                     << ", size=" << n.size << "B)";
                        }
                    }

                    // Feed the Annex-B NAL bytes to libavcodec. The payload is
                    // already in Annex-B form after parse_nals_avcc_to_annexb.
                    if (decoder_) {
                        const bool is_idr = (ftype == kFrameVideoIdr);
                        bool got_frame = false;
                        int  dw = 0, dh = 0;
                        decoder_->decode(payload.data(), payload_size, is_idr,
                                         got_frame, dw, dh);
                        if (got_frame) {
                            if (decoder_->frames_decoded() == 1) {
                                LOG_INFO << "decoded first frame: "
                                         << dw << 'x' << dh;
                                decoder_->dump_last_frame_ppm("first_frame.ppm");
                            } else if (decoder_->frames_decoded() % 100 == 0) {
                                LOG_INFO << "decoded " << decoder_->frames_decoded()
                                         << " frames (" << dw << 'x' << dh << ')';
                            }

                            // Push the decoded YUV420P planes to the
                            // renderer (if any) so it shows up on screen.
                            if (renderer_) {
                                const uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
                                int ys = 0, us = 0, vs = 0, fw = 0, fh = 0;
                                if (decoder_->last_frame_yuv(y, ys, u, us, v, vs, fw, fh)) {
                                    renderer_->push_frame(y, ys, u, us, v, vs, fw, fh);
                                }
                            }
                        }
                    }
                }
            }
        }

        if ((frames % 500) == 0) {
            LOG_INFO << "mirror progress: " << frames << " frames, "
                     << (total_header + total_body) / 1024 << " KB total";
        }
    }

    LOG_INFO << "mirror stream ended: " << frames << " frames"
             << ", " << total_body << " payload bytes, per-type:";
    for (const auto& [t, c] : counts) {
        LOG_INFO << "  " << frame_name(t) << " (0x" << std::hex << t << std::dec
                 << "): " << c << " frames, " << bytes_by_type[t] << " B";
    }
    if (!nal_counts.empty()) {
        const NalCodec final_codec =
            (decoder_ && decoder_->is_hevc()) ? NalCodec::HEVC
                                              : NalCodec::H264;
        LOG_INFO << "NAL units decoded (after decrypt + AVCC→Annex-B):";
        for (const auto& [t, c] : nal_counts) {
            LOG_INFO << "  " << nal_type_name(t, final_codec)
                     << " (type=" << t << "): "
                     << c << " NAL(s)";
        }
    }
    if (decoder_) {
        LOG_INFO << "H264Decoder: " << decoder_->frames_decoded()
                 << " frame(s) decoded this session";
    }
}

} // namespace ap::airplay
