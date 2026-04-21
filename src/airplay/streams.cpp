#include "airplay/streams.h"
#include "log.h"

#include <cstring>
#include <random>
#include <sstream>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

namespace ap::airplay {
namespace {

// `Transport: RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002`
// We pull out the client-side ports by substring; the full grammar (RFC 2326
// §12.39) has more, but this is what iOS actually sends.
uint16_t pick_port(const std::string& header, const char* key) {
    auto pos = header.find(key);
    if (pos == std::string::npos) return 0;
    pos += std::strlen(key);
    if (pos >= header.size() || header[pos] != '=') return 0;
    ++pos;
    uint16_t v = 0;
    while (pos < header.size() && std::isdigit(static_cast<unsigned char>(header[pos]))) {
        v = static_cast<uint16_t>(v * 10 + (header[pos] - '0'));
        ++pos;
    }
    return v;
}

// Bind a UDP socket on 0.0.0.0:0 (ephemeral) and return both the socket
// and the port the OS picked. Returns (INVALID_SOCK, 0) on failure.
std::pair<socket_t, uint16_t> bind_udp() {
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return {INVALID_SOCK, 0};

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ap::net::close_socket(s);
        return {INVALID_SOCK, 0};
    }

#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ap::net::close_socket(s);
        return {INVALID_SOCK, 0};
    }
    return {s, ntohs(addr.sin_port)};
}

std::string random_session_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(1'000'000, 9'999'999);
    std::ostringstream os;
    os << dist(rng);
    return os.str();
}

} // namespace

StreamSession::StreamSession() : session_id_(random_session_id()) {}

StreamSession::~StreamSession() { teardown(); }

bool StreamSession::setup(const std::string& transport, StreamPorts& allocated) {
    client_ports_.server  = pick_port(transport, "server_port");   // rare from iOS
    client_ports_.control = pick_port(transport, "control_port");
    client_ports_.timing  = pick_port(transport, "timing_port");

    // Data socket: always bind. Control + timing only when iOS mentions them.
    auto [d_sock, d_port] = bind_udp();
    if (d_sock == INVALID_SOCK) {
        LOG_ERROR << "SETUP: cannot bind UDP data socket";
        return false;
    }
    data_sock_       = d_sock;
    allocated.server = d_port;

    if (client_ports_.control) {
        auto [c_sock, c_port] = bind_udp();
        if (c_sock == INVALID_SOCK) {
            LOG_ERROR << "SETUP: cannot bind UDP control socket";
            teardown();
            return false;
        }
        ctrl_sock_        = c_sock;
        allocated.control = c_port;
    }
    if (client_ports_.timing) {
        auto [t_sock, t_port] = bind_udp();
        if (t_sock == INVALID_SOCK) {
            LOG_ERROR << "SETUP: cannot bind UDP timing socket";
            teardown();
            return false;
        }
        timing_sock_     = t_sock;
        allocated.timing = t_port;
    }

    LOG_INFO << "SETUP session=" << session_id_
             << "  client(control=" << client_ports_.control
             << ",timing="           << client_ports_.timing
             << ")  server(data=" << allocated.server
             << ",control="         << allocated.control
             << ",timing="          << allocated.timing << ")";
    return true;
}

void StreamSession::teardown() {
    for (auto* sp : { &data_sock_, &ctrl_sock_, &timing_sock_ }) {
        if (*sp != INVALID_SOCK) {
            ap::net::close_socket(*sp);
            *sp = INVALID_SOCK;
        }
    }
}

} // namespace ap::airplay
