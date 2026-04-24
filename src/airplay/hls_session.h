#pragma once

#include <condition_variable>
#include <atomic>
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
    // thread and fill in either the bytes (rare) or a 302 redirect
    // Location URL (the common YouTube case — iOS mints a signed
    // googlevideo.com URL and expects us to follow). The local
    // server forwards the 302 to FFmpeg which fetches the segment
    // directly from the CDN over HTTPS.
    std::mutex                                            seg_mtx;
    std::condition_variable                               seg_cv;
    std::unordered_map<std::string, std::string>          segment_data;       // raw bytes (empty string on 302)
    std::unordered_map<std::string, std::string>          segment_redirect;   // Location URL on 302

    // Local /seg/<id> -> real CDN URL mapping. Populated when the
    // expanded media playlist is rewritten so every segment URI
    // points at http://localhost:7100/seg/<id>. HlsLocalServer
    // reads this when the media player actually asks for a segment
    // and proxies the HTTPS fetch from our side via libavio.
    std::mutex                                            seg_url_mtx;
    std::unordered_map<std::string, std::string>          seg_url_map;
    uint64_t                                              seg_url_counter = 0;

    // Lightweight playback state surfaced via GET /playback-info.
    std::atomic<bool>                                     playback_started{false};
    std::atomic<bool>                                     media_playlist_ready{false};

    // Best-effort cache of POST /setProperty payloads so matching
    // /getProperty calls can echo the latest known value.
    std::mutex                                            props_mtx;
    std::unordered_map<std::string, std::vector<unsigned char>> props;
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

    // Resolve a local "/seg/<n>" path populated by
    // rewrite_segments_to_local back to the CDN URL iOS gave us.
    // Returns empty if the path isn't registered on any live session.
    std::string resolve_segment_path(const std::string& local_path) const;

    // Fetch a segment (non-.m3u8) by sending a FCUP Request on the
    // reverse socket for the session that last saw a /play, then
    // blocking up to `timeout_ms` ms for iOS to ship bytes back via
    // POST /action. Returns false on timeout or no-active-session.
    // On success, either `out_bytes` holds the segment bytes OR
    // `out_redirect` holds the signed CDN URL iOS wants us to fetch
    // from — the local HLS server forwards the redirect to FFmpeg.
    bool fetch_segment(const std::string& url,
                       std::string& out_bytes,
                       std::string& out_redirect,
                       int timeout_ms = 5000);

    // Store segment bytes iOS delivered and wake any fetch_segment()
    // waiter. Called from handle_action.
    void deliver_segment(const std::string& session_id,
                         const std::string& url,
                         std::string bytes);

    // Store a 302 redirect Location URL iOS handed us for a segment.
    void deliver_segment_redirect(const std::string& session_id,
                                  const std::string& url,
                                  std::string redirect);

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

// Reduce a YouTube-style master playlist to a single video variant
// and one audio rendition per TYPE so FFmpeg doesn't probe every
// itag. Keeps all header tags, the first #EXT-X-MEDIA per TYPE, and
// the first #EXT-X-STREAM-INF (plus its URI line). Drops every
// later variant. Returns `master` unchanged if it doesn't match the
// expected shape.
std::string filter_master_to_single_variant(const std::string& master);

// Rewrite every segment URL in `expanded_playlist` to a local path
// "/seg/<n>" where <n> is a fresh counter, and record the mapping
// n -> original URL on `session`. Comments (#…) and blank lines are
// preserved verbatim. HlsLocalServer uses the recorded URLs when
// FFmpeg eventually fetches the local path.
//
// Counts how many URL lines were rewritten; the caller logs this
// and can assert googlevideo URLs no longer appear in the output.
std::string rewrite_segments_to_local(HlsSession& session,
                                      uint16_t local_port,
                                      const std::string& expanded,
                                      std::size_t& out_rewritten_count);

// Look up a local /seg/<id> path on any active HLS session; returns
// the original CDN URL or empty string if not registered.
std::string lookup_segment_url(const std::string& local_path);

// Port of UxPlay's adjust_yt_condensed_playlist. YouTube-delivered
// media playlists carry a "#YT-EXT-CONDENSED-URL" header with three
// attributes — BASE-URI (signed googlevideo.com URL), PARAMS (slash-
// separated fields like "s,slices") and PREFIX (the mlhls:// stub the
// condensed chunks start with). Each EXTINF chunk body is a condensed
// path with "/" separators that must be un-condensed by inserting the
// PARAMS values. After expansion the segment URIs point directly at
// the CDN over HTTPS so FFmpeg fetches them without a FCUP round trip.
// Returns `playlist` unchanged when the header isn't present.
std::string expand_yt_condensed_playlist(const std::string& playlist);

} // namespace ap::airplay
