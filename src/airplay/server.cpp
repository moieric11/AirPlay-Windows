#include "airplay/server.h"
#include "airplay/client_session.h"
#include "airplay/reverse_channel.h"
#include "log.h"

namespace ap::airplay {

bool Server::start(const DeviceContext& ctx, uint16_t port) {
    ctx_ = ctx;
    return tcp_.start(port, [this](ap::net::ClientSocket c) {
        handle_client(std::move(c));
    });
}

void Server::stop() {
    tcp_.stop();
}

void Server::handle_client(ap::net::ClientSocket client) {
    RequestReader reader;
    Request req;

    // Peer is "ip:port"; split off the IP for the NTP client destination.
    std::string peer_ip = client.peer;
    auto colon = peer_ip.rfind(':');
    if (colon != std::string::npos) peer_ip.resize(colon);

    ClientSession session(*ctx_.identity, peer_ip,
                          static_cast<int>(client.fd), ctx_.renderer);

    while (reader.read(static_cast<int>(client.fd), req)) {
        Response res = dispatch(ctx_, session, req);
        if (res.silent) continue;
        std::string wire = res.serialize();
        if (ap::net::send_all(client.fd, wire.data(),
                              static_cast<int>(wire.size())) < 0) {
            LOG_WARN << client.peer << ": send failed";
            break;
        }
    }
    if (!session.reverse_session_id.empty()) {
        ReverseChannelRegistry::instance().unregister_socket(
            session.reverse_session_id);
    }
    LOG_INFO << "closing " << client.peer;
    ap::net::close_socket(client.fd);
}

} // namespace ap::airplay
