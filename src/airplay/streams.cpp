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

bool StreamSession::setup_legacy(const std::string& transport, StreamPorts& allocated) {
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

bool StreamSession::setup_session(uint16_t& event_port, uint16_t& timing_port) {
    auto [e_sock, e_port] = bind_udp();
    if (e_sock == INVALID_SOCK) {
        LOG_ERROR << "SETUP (airplay2 session): cannot bind event socket";
        return false;
    }
    auto [t_sock, t_port] = bind_udp();
    if (t_sock == INVALID_SOCK) {
        LOG_ERROR << "SETUP (airplay2 session): cannot bind timing socket";
        ap::net::close_socket(e_sock);
        return false;
    }
    event_sock_      = e_sock;
    ap2_timing_sock  = t_sock;
    event_port       = e_port;
    timing_port      = t_port;
    LOG_INFO << "SETUP session=" << session_id_
             << " airplay2 event=" << e_port << " timing=" << t_port;
    return true;
}

bool StreamSession::setup_stream(int type,
                                 uint16_t& data_port,
                                 uint16_t& control_port,
                                 const std::vector<unsigned char>& aes_key_audio,
                                 uint64_t stream_connection_id) {
    control_port = 0;

    // Type 110 (mirror video) uses a TCP byte stream, not UDP RTP. iOS will
    // open a TCP connection to the returned dataPort right after SETUP and
    // push H.264 NAL units over it. UxPlay implements this in raop_rtp_mirror.c.
    if (type == 110) {
        mirror_ = std::make_unique<MirrorListener>();

        // Wire AES-CTR decryption before starting the accept loop. iOS might
        // already be opening the TCP connection by the time listen() returns,
        // so decrypt must be ready first.
        if (!aes_key_audio.empty() && stream_connection_id != 0) {
            if (!mirror_->enable_decrypt(aes_key_audio, stream_connection_id)) {
                LOG_WARN << "SETUP stream 110: could not init AES-CTR decrypt "
                            "(stream will be logged encrypted)";
            }
        }
        if (renderer_) mirror_->attach_renderer(renderer_);

        uint16_t p = 0;
        if (!mirror_->start(p)) {
            mirror_.reset();
            return false;
        }
        data_port = p;

        StreamChannel ch;
        ch.type         = type;
        ch.data_port    = p;
        ch.data_sock    = INVALID_SOCK; // owned by MirrorListener
        channels_.push_back(ch);

        LOG_INFO << "SETUP session=" << session_id_
                 << " stream type=110 (TCP) data=" << p;
        return true;
    }

    // Type 96 (audio) and any other UDP-based stream.
    auto [d_sock, d_port] = bind_udp();
    if (d_sock == INVALID_SOCK) return false;
    auto [c_sock, c_port] = bind_udp();
    if (c_sock == INVALID_SOCK) { ap::net::close_socket(d_sock); return false; }

    StreamChannel ch;
    ch.type         = type;
    ch.data_port    = d_port;
    ch.control_port = c_port;
    ch.data_sock    = d_sock;
    ch.control_sock = c_sock;
    channels_.push_back(ch);

    data_port    = d_port;
    control_port = c_port;
    LOG_INFO << "SETUP session=" << session_id_ << " stream type=" << type
             << " (UDP) data=" << d_port << " control=" << c_port;
    return true;
}

void StreamSession::stop_stream(int type) {
    LOG_INFO << "stop_stream type=" << type << " session=" << session_id_;
    if (type == 110 && mirror_) {
        mirror_->stop();
        mirror_.reset();
    }
    // Remove any audio channels matching the type.
    for (auto it = channels_.begin(); it != channels_.end(); ) {
        if (it->type == type) {
            if (it->data_sock    != INVALID_SOCK) ap::net::close_socket(it->data_sock);
            if (it->control_sock != INVALID_SOCK) ap::net::close_socket(it->control_sock);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

bool StreamSession::start_ntp(const std::string& remote_ip, uint16_t remote_port) {
    if (ap2_timing_sock == INVALID_SOCK) {
        LOG_ERROR << "start_ntp: timing socket not bound (call setup_session first)";
        return false;
    }
    ntp_ = std::make_unique<NtpClient>();
    return ntp_->start(ap2_timing_sock, remote_ip, remote_port);
}

void StreamSession::teardown() {
    if (ntp_)    { ntp_->stop();    ntp_.reset(); }
    if (mirror_) { mirror_->stop(); mirror_.reset(); }

    for (auto* sp : { &data_sock_, &ctrl_sock_, &timing_sock_,
                       &event_sock_, &ap2_timing_sock }) {
        if (*sp != INVALID_SOCK) {
            ap::net::close_socket(*sp);
            *sp = INVALID_SOCK;
        }
    }
    for (auto& ch : channels_) {
        if (ch.data_sock    != INVALID_SOCK) ap::net::close_socket(ch.data_sock);
        if (ch.control_sock != INVALID_SOCK) ap::net::close_socket(ch.control_sock);
    }
    channels_.clear();
}

} // namespace ap::airplay
