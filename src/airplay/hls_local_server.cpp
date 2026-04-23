#include "airplay/hls_local_server.h"
#include "airplay/hls_session.h"
#include "log.h"
#include "net/socket.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace ap::airplay {

bool HlsLocalServer::start(uint16_t port) {
    const bool ok = server_.start(port,
        [this](ap::net::ClientSocket c) { handle_client(std::move(c)); });
    if (ok) {
        LOG_INFO << "HLS local server listening on http://localhost:"
                 << server_.port() << '/';
    }
    return ok;
}

void HlsLocalServer::stop() {
    server_.stop();
}

std::string HlsLocalServer::rewrite_urls(const std::string& content) const {
    constexpr const char* kFrom = "mlhls://localhost/";
    const std::string to = "http://localhost:" +
        std::to_string(server_.port()) + "/";
    std::string out;
    out.reserve(content.size() + 64);
    const std::size_t flen = std::strlen(kFrom);
    std::size_t pos = 0;
    while (pos < content.size()) {
        const auto hit = content.find(kFrom, pos);
        if (hit == std::string::npos) {
            out.append(content, pos, std::string::npos);
            break;
        }
        out.append(content, pos, hit - pos);
        out.append(to);
        pos = hit + flen;
    }
    return out;
}

void HlsLocalServer::handle_client(ap::net::ClientSocket client) {
    // Byte-at-a-time read until we see "\r\n\r\n" or the socket closes.
    // Media-player GETs are small (<500 B) so 8 KB is plenty.
    std::string headers;
    {
        char tmp[1024];
        while (headers.size() < 8192) {
            const int n = ap::net::recv_all(client.fd, tmp, 1);
            if (n <= 0) break;
            headers.append(tmp, 1);
            if (headers.size() >= 4 &&
                headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
                break;
            }
        }
    }

    // Parse just the request-line: "GET <uri> HTTP/1.1".
    std::string path;
    {
        const auto eol = headers.find("\r\n");
        if (eol != std::string::npos) {
            std::istringstream line(headers.substr(0, eol));
            std::string method, uri, version;
            if ((line >> method >> uri >> version) && method == "GET") {
                path = uri;
            }
        }
    }

    auto send_response = [&](int status, const char* status_text,
                             const std::string& body,
                             const char* content_type) {
        std::string wire;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP/1.1 %d ", status);
        wire.append(buf).append(status_text).append("\r\n");
        wire.append("Content-Type: ").append(content_type).append("\r\n");
        std::snprintf(buf, sizeof(buf), "Content-Length: %zu\r\n",
                      body.size());
        wire.append(buf);
        wire.append("Connection: close\r\n\r\n");
        wire.append(body);
        ap::net::send_all(client.fd, wire.data(),
                          static_cast<int>(wire.size()));
    };

    if (path.empty()) {
        send_response(400, "Bad Request", "", "text/plain");
        ap::net::close_socket(client.fd);
        return;
    }

    // The player GETs `/<path>`. Rebuild the canonical mlhls:// URL iOS
    // used so the registry lookup hits.
    const std::string mlhls_url = "mlhls://localhost/" +
        (path.empty() || path[0] != '/' ? path : path.substr(1));

    std::string body;
    bool is_master = false;
    if (!HlsSessionRegistry::instance().lookup_playlist(
            mlhls_url, body, is_master)) {
        LOG_WARN << "HLS GET " << path << " -> 404 (no session owns "
                 << mlhls_url << ")";
        send_response(404, "Not Found", "", "text/plain");
        ap::net::close_socket(client.fd);
        return;
    }

    // Rewrite inner "mlhls://localhost/" so the player stays on us for
    // every fetch (master -> media playlists -> segments).
    body = rewrite_urls(body);
    LOG_INFO << "HLS GET " << path
             << (is_master ? " (master" : " (media")
             << ") -> " << body.size() << 'B';
    send_response(200, "OK", body, "application/vnd.apple.mpegurl");
    ap::net::close_socket(client.fd);
}

} // namespace ap::airplay
