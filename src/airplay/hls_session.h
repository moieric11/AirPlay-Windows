#pragma once

#include <condition_variable>
#include <memory>
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
    std::string              session_id;        // X-Apple-Session-ID
    std::string              master_url;        // canonical master.m3u8 URL
    std::string              master_playlist;   // raw bytes (iOS-delivered)
    int                      next_request_id = 2;   // 1 was used for master
    std::vector<std::string> media_uris;        // mlhls://localhost/itag/..
    std::unordered_map<std::string, std::string> media_playlists; // url->content

    // Segment data coordination: the local HTTP server, on a
    // non-.m3u8 GET, sends a FCUP Request for the segment's mlhls://
    // URL then waits here for handle_action to arrive on another
    // thread and fill in the bytes.
    std::mutex                                            seg_mtx;
    std::condition_variable                               seg_cv;
    std::unordered_map<std::string, std::string>          segment_data;
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

    // Fetch a segment (non-.m3u8) by sending a FCUP Request on the
    // reverse socket for the session that last saw a /play, then
    // blocking up to `timeout_ms` ms for iOS to ship bytes back via
    // POST /action. Returns false on timeout or no-active-session.
    bool fetch_segment(const std::string& url,
                       std::string& out_bytes,
                       int timeout_ms = 5000);

    // Store segment bytes iOS delivered and wake any fetch_segment()
    // waiter. Called from handle_action.
    void deliver_segment(const std::string& session_id,
                         const std::string& url,
                         std::string bytes);

    // Child-playlist variant: the local HLS server calls this when
    // the player requests a .m3u8 that isn't in media_playlists yet
    // (iOS only pre-delivers the itags it selected, not all the ones
    // listed in the master). Sends a FCUP for `url` on the session's
    // reverse socket, then blocks on the shared seg_cv until
    // handle_action stores the result in media_playlists.
    bool fetch_playlist(const std::string& url,
                        std::string& out_bytes,
                        int timeout_ms = 5000);

    // Wake fetch_playlist() waiters when handle_action adds a new
    // entry to media_playlists.
    void notify_playlist_arrived(const std::string& session_id,
                                 const std::string& url);

private:
    HlsSessionRegistry() = default;

    mutable std::mutex                                   mtx_;
    std::unordered_map<std::string, std::unique_ptr<HlsSession>> sessions_;
};

// Extract every occurrence of `mlhls://localhost/.../*.m3u8` from a
// master playlist body. Returns the list of URIs in order, duplicates
// removed.
std::vector<std::string> extract_media_uris(const std::string& master);

} // namespace ap::airplay
