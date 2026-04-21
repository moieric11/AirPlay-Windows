#include "airplay/mirror_listener.h"
#include "log.h"

#include <array>
#include <cstring>
#include <map>
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
}

} // namespace ap::airplay
