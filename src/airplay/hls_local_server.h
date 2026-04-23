#pragma once

#include "net/tcp_server.h"

#include <cstdint>
#include <string>

namespace ap::airplay {

// Local HTTP server that fronts the HlsSessionRegistry so an internal
// media player (libavformat, coming next) can fetch HLS playlists
// over a plain `http://localhost:<port>/...` URL.
//
// The server rewrites the iOS-supplied "mlhls://localhost/" scheme
// (which no HTTP client can resolve) to its own "http://localhost:
// <port>/" prefix on the fly, so the player sees a normal HLS tree.
// For every GET <path>, the server reconstructs the original
// "mlhls://localhost/<path>" URL and looks it up across all active
// HlsSessions.
class HlsLocalServer {
public:
    HlsLocalServer() = default;
    ~HlsLocalServer() { stop(); }

    HlsLocalServer(const HlsLocalServer&)            = delete;
    HlsLocalServer& operator=(const HlsLocalServer&) = delete;

    // Starts the listening socket on `port` (0 = OS-assigned). Returns
    // false on bind/listen failure.
    bool start(uint16_t port = 7100);
    void stop();

    uint16_t port() const { return server_.port(); }

private:
    void handle_client(ap::net::ClientSocket client);

    // Return a copy of `content` with "mlhls://localhost/" replaced by
    // "http://localhost:<port>/" so the player can fetch through us.
    std::string rewrite_urls(const std::string& content) const;

    ap::net::TcpServer server_;
};

} // namespace ap::airplay
