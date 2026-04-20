#include "airplay/server.h"
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
    while (reader.read(static_cast<int>(client.fd), req)) {
        Response res = dispatch(ctx_, req);
        std::string wire = res.serialize();
        if (ap::net::send_all(client.fd, wire.data(),
                              static_cast<int>(wire.size())) < 0) {
            LOG_WARN << client.peer << ": send failed";
            break;
        }
    }
    LOG_INFO << "closing " << client.peer;
    ap::net::close_socket(client.fd);
}

} // namespace ap::airplay
