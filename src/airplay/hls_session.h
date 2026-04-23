#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ap::airplay {

// Per-X-Apple-Session-ID state for an active AirPlay Streaming / HLS
// playback. Holds the master playlist and each media sub-playlist
// (keyed by its mlhls:// URL), populated as POST /action responses
// from iOS arrive via the FCUP round trip.
//
// Accessed from two threads: the RTSP server thread stores playlists
// on /action, and the (future) local HLS HTTP server thread reads
// them to serve the player. Mutex-protected.
struct HlsSession {
    std::string              master_url;        // canonical master.m3u8 URL
    std::string              master_playlist;   // raw bytes (iOS-delivered)
    int                      next_request_id = 2;   // 1 was used for master
    std::vector<std::string> media_uris;        // mlhls://localhost/itag/..
    std::unordered_map<std::string, std::string> media_playlists; // url->content
};

class HlsSessionRegistry {
public:
    static HlsSessionRegistry& instance();

    // Create or look up the session for the given X-Apple-Session-ID.
    // Never returns null; the entry persists until remove() is called.
    HlsSession* get_or_create(const std::string& session_id);

    // Best-effort lookup (no allocation).
    HlsSession* find(const std::string& session_id);

    // Remove + free. Called on TEARDOWN / session end.
    void remove(const std::string& session_id);

    // Scan every active session for a stored playlist matching `url`
    // (mlhls://localhost/...). Returns a copy of the bytes and whether
    // it's the master (vs. a media sub-playlist) of that session. Used
    // by the local HTTP server to serve the internal media player.
    bool lookup_playlist(const std::string& url,
                         std::string& out_bytes,
                         bool& out_is_master) const;

private:
    HlsSessionRegistry() = default;

    mutable std::mutex                                   mtx_;
    std::unordered_map<std::string, HlsSession>          sessions_;
};

// Extract every occurrence of `mlhls://localhost/.../*.m3u8` from a
// master playlist body. Returns the list of URIs in order, duplicates
// removed.
std::vector<std::string> extract_media_uris(const std::string& master);

} // namespace ap::airplay
