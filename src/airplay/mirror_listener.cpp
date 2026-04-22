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

// SPS_PPS packet payload is the AVCC "avcC" decoder configuration record
// (ISO 14496-15 §5.2.4.1.1), not a raw length-prefixed NAL pair:
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
// We only parse the first SPS — plenty for a human-readable video line.
bool log_sps_from_sps_pps_payload(const std::vector<unsigned char>& payload) {
    if (payload.size() < 8 || payload[0] != 0x01) {
        LOG_WARN << "mirror: SPS/PPS — unexpected format, first byte 0x"
                 << std::hex << static_cast<int>(payload.empty() ? 0 : payload[0]);
        return false;
    }

    uint8_t num_sps = payload[5] & 0x1f;
    if (num_sps == 0) {
        LOG_WARN << "mirror: avcC declares 0 SPS";
        return false;
    }
    uint32_t sps_len = (static_cast<uint32_t>(payload[6]) << 8) | payload[7];
    if (sps_len == 0 || sps_len + 8 > payload.size()) {
        LOG_WARN << "mirror: avcC SPS length " << sps_len << " out of range";
        return false;
    }

    const uint8_t* sps = payload.data() + 8;
    ap::airplay::SpsInfo info;
    if (!ap::airplay::parse_h264_sps(sps, sps_len, info)) {
        LOG_WARN << "mirror: SPS present but failed to parse";
        return false;
    }

    LOG_INFO << "mirror video: H.264 " << ap::airplay::profile_name(info.profile_idc)
             << " level " << (info.level_idc / 10) << '.' << (info.level_idc % 10)
             << ", " << info.width << 'x' << info.height
             << (info.interlaced ? " (interlaced)" : "");
    return true;
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
    return true;
}

bool MirrorListener::start(uint16_t& port) {
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
        LOG_ERROR << "mirror: bind() failed";
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

    port     = ntohs(addr.sin_port);
    running_ = true;
    thread_  = std::thread(&MirrorListener::accept_loop, this);
    LOG_INFO << "mirror TCP listener on port " << port;
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

        sockaddr_in peer{};
#if defined(_WIN32)
        int plen = sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        socket_t client = ::accept(listen_sock_,
                                   reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client == INVALID_SOCK) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        LOG_INFO << "mirror: iOS connected from " << ip << ':' << ntohs(peer.sin_port);

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

        // Only once, on the first SPS/PPS frame, log the raw bytes and try
        // to decode the SPS into a human-readable video description.
        if (ftype == kFrameSpsPps && counts[kFrameSpsPps] == 1 && !payload.empty()) {
            LOG_INFO << "mirror SPS/PPS hex: " << hex_dump(payload.data(), payload.size());
            log_sps_from_sps_pps_payload(payload);
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
                std::vector<NalUnit> nals;
                const bool ok = parse_nals_avcc_to_annexb(
                    payload.data(), payload_size, nals);

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
                            LOG_INFO << "  NAL " << nal_type_name(n.type)
                                     << "(type=" << n.type
                                     << ", ref_idc=" << n.ref_idc
                                     << ", size=" << n.size << "B)";
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
        LOG_INFO << "NAL units decoded (after decrypt + AVCC→Annex-B):";
        for (const auto& [t, c] : nal_counts) {
            LOG_INFO << "  " << nal_type_name(t) << " (type=" << t << "): "
                     << c << " NAL(s)";
        }
    }
}

} // namespace ap::airplay
