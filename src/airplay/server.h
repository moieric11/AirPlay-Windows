#pragma once

#include "airplay/routes.h"
#include "net/tcp_server.h"

namespace ap::airplay {

// High-level server glueing the TCP listener to the RTSP parser + dispatcher.
class Server {
public:
    bool start(const DeviceContext& ctx, uint16_t port = 7000);
    void stop();
    uint16_t port() const { return tcp_.port(); }

    // Drop every currently-connected AirPlay client. Mirror, audio,
    // RTSP — all unwound by the natural EOF cascade once the RTSP
    // TCP closes. Listener stays up so iOS can re-pair / re-mirror.
    void disconnect_clients() { tcp_.close_all_clients(); }

private:
    void handle_client(ap::net::ClientSocket client);

    DeviceContext ctx_;
    ap::net::TcpServer tcp_;
};

} // namespace ap::airplay
