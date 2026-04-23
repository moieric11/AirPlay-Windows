#include "net/tcp_server.h"
#include "log.h"

#include <cstring>

namespace ap::net {

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(uint16_t port, Handler handler) {
    handler_ = std::move(handler);

    // Dual-stack listener: an IPv6 socket with IPV6_V6ONLY=0 accepts
    // both v4 and v6 clients (v4 peers arrive as ::ffff:A.B.C.D).
    // iOS 14+ sometimes reaches receivers over link-local IPv6 first,
    // and an IPv4-only listener dropped those connections silently.
    listener_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCK) {
        LOG_WARN << "socket(AF_INET6) failed — falling back to IPv4-only: "
                 << last_error_string();
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCK) {
            LOG_ERROR << "socket() failed: " << last_error_string();
            return false;
        }
    }

    int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));

    // Disable IPV6_V6ONLY so the socket accepts v4 too. On Windows this
    // defaults to ON, on Linux it varies; set it explicitly either way.
    int v6only = 0;
    ::setsockopt(listener_, IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr   = in6addr_any;
    addr6.sin6_port   = htons(port);

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) != 0) {
        LOG_ERROR << "bind([::]:" << port << ") failed: " << last_error_string();
        close_socket(listener_);
        listener_ = INVALID_SOCK;
        return false;
    }

    if (::listen(listener_, 8) != 0) {
        LOG_ERROR << "listen() failed: " << last_error_string();
        close_socket(listener_);
        listener_ = INVALID_SOCK;
        return false;
    }

    port_    = port;
    running_ = true;
    accept_thread_ = std::thread(&TcpServer::accept_loop, this);
    LOG_INFO << "TCP server listening on [::]:" << port
             << " (IPv4+IPv6)";
    return true;
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;

    // Closing the listener unblocks accept().
    close_socket(listener_);
    listener_ = INVALID_SOCK;

    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}

void TcpServer::accept_loop() {
    while (running_) {
        sockaddr_storage peer{};
#if defined(_WIN32)
        int len = sizeof(peer);
#else
        socklen_t len = sizeof(peer);
#endif
        socket_t fd = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd == INVALID_SOCK) {
            if (running_) {
                LOG_WARN << "accept() failed: " << last_error_string();
            }
            return;
        }

        // Format the peer as a human-readable "ip:port". For v4-mapped
        // v6 addresses (::ffff:A.B.C.D), strip the prefix so iOS clients
        // connecting over v4 still show up as plain dotted-quad.
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

        ClientSocket client;
        client.fd   = fd;
        client.peer = std::string(ip) + ":" + std::to_string(peer_port);

        LOG_INFO << "accepted " << client.peer;
        workers_.emplace_back([h = handler_, c = std::move(client)]() mutable {
            h(std::move(c));
        });
    }
}

} // namespace ap::net
