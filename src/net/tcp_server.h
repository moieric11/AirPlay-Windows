#pragma once

#include "net/socket.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ap::net {

struct ClientSocket {
    socket_t fd = INVALID_SOCK;
    std::string peer;   // "ip:port"
};

// Minimal blocking TCP server. Each accepted client runs on its own thread
// calling `handler(client)`. The handler owns the socket and must close it.
// Stop() joins all client threads and closes the listener.
class TcpServer {
public:
    using Handler = std::function<void(ClientSocket)>;

    TcpServer() = default;
    ~TcpServer();

    bool start(uint16_t port, Handler handler);
    void stop();

    // Forcibly shutdown every currently-connected client socket. The
    // handler thread sees recv() return 0 / error and tears down its
    // session naturally. Listener stays up — new connections still
    // accepted. Used by the "Disconnect" button to drop an active
    // AirPlay session on demand.
    void close_all_clients();

    uint16_t port() const { return port_; }

private:
    void accept_loop();

    socket_t listener_{INVALID_SOCK};
    uint16_t port_{0};
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    Handler handler_;

    // Tracked here so we can close() them on demand from another
    // thread. Erased by the worker on exit.
    std::mutex          clients_mtx_;
    std::set<socket_t>  active_clients_;
};

} // namespace ap::net
