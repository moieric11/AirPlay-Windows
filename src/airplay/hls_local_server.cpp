#include "airplay/hls_local_server.h"
#include "airplay/hls_session.h"
#include "log.h"
#include "net/socket.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include <atomic>
#include <chrono>

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

    // URL-decode the path: FFmpeg percent-encodes reserved characters
    // like "=" (-> "%3D") when fetching from the manifest, but iOS's
    // original URL in the playlist contained the literal "=". We need
    // the decoded form to reconstruct the canonical mlhls:// URL that
    // the FCUP lookup/fetch keys on.
    {
        std::string decoded;
        decoded.reserve(path.size());
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size()) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return -1;
                };
                const int hi = hex(path[i + 1]);
                const int lo = hex(path[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    decoded.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(path[i]);
        }
        path = std::move(decoded);
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

    // Route "/seg/<n>" to the CDN-URL proxy. Rewritten media playlists
    // serve these local paths; we resolve id -> googlevideo.com URL
    // and stream bytes back via libavio (HTTPS natively, with Range
    // passthrough so HLS clients using byte-range requests get a
    // proper 206 Partial Content).
    if (path.size() > 5 && path.compare(0, 5, "/seg/") == 0) {
        const std::string cdn_url =
            HlsSessionRegistry::instance().resolve_segment_path(path);
        if (cdn_url.empty()) {
            LOG_WARN << "HLS GET " << path << " -> 404 (seg id unknown)";
            send_response(404, "Not Found", "", "text/plain");
            ap::net::close_socket(client.fd);
            return;
        }

        // Parse Range: bytes=<start>-<end> from the incoming headers.
        // Empty end means "to EOF". Negative start means "last N bytes"
        // (suffix range) — uncommon in HLS, not supported here.
        int64_t range_start = -1, range_end = -1;
        {
            // Lower-case copy of headers for case-insensitive search.
            std::string lc(headers.size(), ' ');
            for (std::size_t i = 0; i < headers.size(); ++i) {
                lc[i] = (headers[i] >= 'A' && headers[i] <= 'Z')
                            ? headers[i] + 32 : headers[i];
            }
            const auto rp = lc.find("\r\nrange:");
            if (rp != std::string::npos) {
                const auto eq = headers.find('=', rp);
                const auto eol = headers.find("\r\n", rp + 2);
                if (eq != std::string::npos && eol != std::string::npos &&
                    eq < eol) {
                    const std::string val = headers.substr(eq + 1, eol - eq - 1);
                    const auto dash = val.find('-');
                    try {
                        if (dash != std::string::npos) {
                            if (dash > 0) range_start = std::stoll(val.substr(0, dash));
                            if (dash + 1 < val.size())
                                range_end = std::stoll(val.substr(dash + 1));
                        }
                    } catch (...) {}
                }
            }
        }

        LOG_INFO << "HLS GET " << path
                 << (range_start >= 0
                         ? " [Range=" + std::to_string(range_start) + "-" +
                           (range_end >= 0 ? std::to_string(range_end) : "") + "]"
                         : std::string{})
                 << " -> proxy "
                 << cdn_url.substr(0, std::min<std::size_t>(120, cdn_url.size()))
                 << (cdn_url.size() > 120 ? "..." : "");

        // Concurrency gauge — high concurrency means FFmpeg is still
        // probing too many itags or sending many range requests.
        static std::atomic<int> kInFlight{0};
        const int in_flight = kInFlight.fetch_add(1) + 1;
        const auto t_start = std::chrono::steady_clock::now();

        AVDictionary* opts = nullptr;
        if (range_start >= 0) {
            av_dict_set_int(&opts, "offset", range_start, 0);
            if (range_end >= 0) {
                av_dict_set_int(&opts, "end_offset", range_end + 1, 0);
            }
        }
        AVIOContext* avio = nullptr;
        const int rc = avio_open2(&avio, cdn_url.c_str(),
                                  AVIO_FLAG_READ, nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0) {
            kInFlight.fetch_sub(1);
            char err[128]{};
            av_strerror(rc, err, sizeof(err));
            LOG_WARN << "seg proxy: avio_open2 failed: " << err;
            send_response(502, "Bad Gateway", "", "text/plain");
            ap::net::close_socket(client.fd);
            return;
        }
        const int64_t size = avio_size(avio);
        const auto t_opened = std::chrono::steady_clock::now();
        const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t_opened - t_start).count();

        std::string hdr;
        if (range_start >= 0) {
            // 206 Partial Content — we don't know the full resource
            // size (libavio doesn't expose the upstream Content-Range
            // total), so use "*" per RFC 7233 §4.2.
            const int64_t effective_end =
                (range_end >= 0) ? range_end
                                 : (size > 0 ? range_start + size - 1 : -1);
            hdr.append("HTTP/1.1 206 Partial Content\r\n");
            hdr.append("Content-Type: application/octet-stream\r\n");
            if (effective_end >= 0) {
                char crange[96];
                std::snprintf(crange, sizeof(crange),
                              "Content-Range: bytes %lld-%lld/*\r\n",
                              static_cast<long long>(range_start),
                              static_cast<long long>(effective_end));
                hdr.append(crange);
            }
            hdr.append("Accept-Ranges: bytes\r\n");
        } else {
            hdr.append("HTTP/1.1 200 OK\r\n");
            hdr.append("Content-Type: application/octet-stream\r\n");
            hdr.append("Accept-Ranges: bytes\r\n");
        }
        if (size > 0) {
            hdr.append("Content-Length: ").append(std::to_string(size))
               .append("\r\n");
        } else {
            hdr.append("Transfer-Encoding: chunked\r\n");
        }
        hdr.append("Connection: close\r\n\r\n");
        ap::net::send_all(client.fd, hdr.data(), static_cast<int>(hdr.size()));

        unsigned char buf[16 * 1024];
        std::int64_t total = 0;
        std::chrono::steady_clock::time_point t_first_byte{};
        bool got_first_byte = false;
        while (true) {
            const int n = avio_read(avio, buf, sizeof(buf));
            if (n <= 0) break;
            if (!got_first_byte) {
                t_first_byte = std::chrono::steady_clock::now();
                got_first_byte = true;
            }
            total += n;
            if (size > 0) {
                if (ap::net::send_all(client.fd,
                        reinterpret_cast<const char*>(buf), n) < 0) break;
            } else {
                char chunk_hdr[16];
                const int hlen = std::snprintf(chunk_hdr, sizeof(chunk_hdr),
                                               "%x\r\n", n);
                ap::net::send_all(client.fd, chunk_hdr, hlen);
                ap::net::send_all(client.fd,
                    reinterpret_cast<const char*>(buf), n);
                ap::net::send_all(client.fd, "\r\n", 2);
            }
        }
        if (size <= 0) {
            ap::net::send_all(client.fd, "0\r\n\r\n", 5);
        }
        avio_close(avio);
        const auto t_done = std::chrono::steady_clock::now();
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t_done - t_start).count();
        const auto ttfb_ms = got_first_byte
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                t_first_byte - t_start).count()
            : total_ms;
        const double mbps = total_ms > 0
            ? (static_cast<double>(total) * 8.0 / 1000.0 / total_ms)
            : 0.0;
        LOG_INFO << "HLS GET " << path << " proxy done ("
                 << (range_start >= 0 ? "206" : "200") << " "
                 << total << "B, open=" << open_ms << "ms"
                 << " ttfb=" << ttfb_ms << "ms"
                 << " total=" << total_ms << "ms"
                 << " " << mbps << "Mbps"
                 << " concurrent=" << in_flight
                 << (range_start >= 0 ? " range=yes" : " range=no")
                 << ')';
        kInFlight.fetch_sub(1);
        ap::net::close_socket(client.fd);
        return;
    }

    // Playlist vs segment: only .m3u8 paths come from the stored static
    // map. Everything else is a segment the player is pulling — those
    // aren't in media_playlists, we fetch them on demand from iOS via
    // FCUP and block until /action arrives.
    const bool is_playlist = (path.size() >= 5 &&
        path.compare(path.size() - 5, 5, ".m3u8") == 0);

    if (is_playlist) {
        std::string body;
        bool is_master = false;
        bool cached = HlsSessionRegistry::instance().lookup_playlist(
            mlhls_url, body, is_master);
        if (!cached) {
            if (!HlsSessionRegistry::instance().fetch_playlist(
                    mlhls_url, body)) {
                LOG_WARN << "HLS GET " << path << " -> 504 (playlist fetch failed)";
                send_response(504, "Gateway Timeout", "", "text/plain");
                ap::net::close_socket(client.fd);
                return;
            }
        } else if (!is_master &&
                   body.find("#EXT-X-ENDLIST") == std::string::npos) {
            std::string refreshed;
            if (HlsSessionRegistry::instance().fetch_playlist(
                    mlhls_url, refreshed, true)) {
                body = std::move(refreshed);
                cached = false;
                LOG_INFO << "HLS GET " << path
                         << " (media) refreshed non-final playlist";
            } else {
                LOG_WARN << "HLS GET " << path
                         << " refresh failed; serving cached non-final playlist";
            }
        }
        body = rewrite_urls(body);
        // Master playlist: strip all-but-first variant so FFmpeg picks
        // one rendition and stops probing 16 itags. Media playlists
        // already hold the localised /seg/ URLs — serve as-is.
        const std::size_t before = body.size();
        if (is_master) {
            body = filter_master_to_single_variant(body);
        }
        LOG_INFO << "HLS GET " << path
                 << (is_master ? " (master" : " (media")
                 << (cached ? "" : ", fetched")
                 << ") -> " << body.size() << 'B'
                 << (is_master && body.size() != before
                         ? " (filtered from " + std::to_string(before) + "B)"
                         : std::string{});
        send_response(200, "OK", body,
                      "application/vnd.apple.mpegurl");
        ap::net::close_socket(client.fd);
        return;
    }

    // Segment: FCUP to iOS and wait for POST /action to return either
    // the bytes inline (rare) or a 302 Location URL to the CDN (the
    // common YouTube path). If redirect, forward the 302 to FFmpeg
    // which follows it over HTTPS straight to googlevideo.com.
    std::string seg;
    std::string redirect;
    if (!HlsSessionRegistry::instance().fetch_segment(mlhls_url, seg, redirect)) {
        LOG_WARN << "HLS GET " << path << " -> 504 (segment fetch failed)";
        send_response(504, "Gateway Timeout", "", "text/plain");
        ap::net::close_socket(client.fd);
        return;
    }
    if (!redirect.empty()) {
        LOG_INFO << "HLS GET " << path << " (segment) -> 302 ("
                 << redirect.size() << "B Location)";
        // Inline 302 + Location + empty body.
        std::string wire;
        wire.append("HTTP/1.1 302 Found\r\n");
        wire.append("Location: ").append(redirect).append("\r\n");
        wire.append("Content-Length: 0\r\n");
        wire.append("Connection: close\r\n\r\n");
        ap::net::send_all(client.fd, wire.data(),
                          static_cast<int>(wire.size()));
        ap::net::close_socket(client.fd);
        return;
    }
    LOG_INFO << "HLS GET " << path << " (segment) -> " << seg.size() << 'B';
    send_response(200, "OK", seg, "application/octet-stream");
    ap::net::close_socket(client.fd);
}

} // namespace ap::airplay
