#include "airplay/mirror_listener.h"
#include "log.h"

#include <cstring>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

namespace ap::airplay {

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
        LOG_ERROR << "mirror: TCP bind() failed";
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        LOG_ERROR << "mirror: getsockname() failed";
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }
    if (::listen(listen_sock_, 1) != 0) {
        LOG_ERROR << "mirror: listen() failed";
        ap::net::close_socket(listen_sock_);
        listen_sock_ = INVALID_SOCK;
        return false;
    }

    port     = ntohs(addr.sin_port);
    running_ = true;
    thread_  = std::thread(&MirrorListener::run, this);
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

void MirrorListener::run() {
    socket_t client      = INVALID_SOCK;
    uint64_t total_bytes = 0;
    uint64_t chunks      = 0;

    while (running_) {
        // 100ms select tick so we can honour stop() without closing sockets
        // from another thread. Matches UxPlay's pattern (5ms there, but 100ms
        // is plenty for a drain-only implementation).
        fd_set rfds;
        FD_ZERO(&rfds);
        socket_t watched = (client == INVALID_SOCK) ? listen_sock_ : client;
        if (watched == INVALID_SOCK) break;
        FD_SET(watched, &rfds);

        timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;
        int ret = ::select(static_cast<int>(watched) + 1, &rfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        if (client == INVALID_SOCK) {
            sockaddr_in peer{};
#if defined(_WIN32)
            int plen = sizeof(peer);
#else
            socklen_t plen = sizeof(peer);
#endif
            client = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (client == INVALID_SOCK) continue;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            LOG_INFO << "mirror: iOS connected from " << ip
                     << ':' << ntohs(peer.sin_port);
        } else {
            unsigned char buf[4096];
            int n = ::recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) {
                LOG_INFO << "mirror: iOS disconnected (received "
                         << chunks << " chunks, "
                         << total_bytes << " bytes)";
                ap::net::close_socket(client);
                client       = INVALID_SOCK;
                total_bytes  = 0;
                chunks       = 0;
                continue;
            }
            total_bytes += static_cast<uint64_t>(n);
            ++chunks;
            if ((chunks % 100) == 0) {
                LOG_INFO << "mirror stream: " << chunks
                         << " chunks, " << total_bytes << " bytes so far";
            }
        }
    }

    if (client != INVALID_SOCK) {
        ap::net::close_socket(client);
    }
}

} // namespace ap::airplay
