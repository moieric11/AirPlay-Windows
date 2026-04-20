#include "net/tcp_server.h"
#include "log.h"

#include <cstring>

namespace ap::net {

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(uint16_t port, Handler handler) {
    handler_ = std::move(handler);

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCK) {
        LOG_ERROR << "socket() failed: " << last_error_string();
        return false;
    }

    int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR << "bind(" << port << ") failed: " << last_error_string();
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
    LOG_INFO << "TCP server listening on 0.0.0.0:" << port;
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
        sockaddr_in peer{};
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

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        ClientSocket client;
        client.fd   = fd;
        client.peer = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        LOG_INFO << "accepted " << client.peer;
        workers_.emplace_back([h = handler_, c = std::move(client)]() mutable {
            h(std::move(c));
        });
    }
}

} // namespace ap::net
