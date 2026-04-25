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

    // Peer is formatted by tcp_server as "ip:port" for v4 and v4-mapped,
    // "[ip]:port" for real IPv6. Strip brackets first when present, then
    // cut at the final ':'. The bracketed form is unambiguous; the
    // unbracketed-v4 form has exactly one ':'.
    std::string peer_ip = client.peer;
    if (!peer_ip.empty() && peer_ip.front() == '[') {
        const auto close_br = peer_ip.find(']');
        peer_ip = (close_br == std::string::npos)
                    ? peer_ip.substr(1)
                    : peer_ip.substr(1, close_br - 1);
    } else {
        const auto colon = peer_ip.rfind(':');
        if (colon != std::string::npos) peer_ip.resize(colon);
    }

    ClientSession session(*ctx_.identity, peer_ip,
                          static_cast<int>(client.fd), ctx_.renderer,
                          ctx_.mirror_hwaccel);

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
