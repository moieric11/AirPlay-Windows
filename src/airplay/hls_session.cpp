#include "airplay/hls_session.h"
#include "airplay/fcup.h"
#include "airplay/reverse_channel.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ap::airplay {

HlsSessionRegistry& HlsSessionRegistry::instance() {
    static HlsSessionRegistry inst;
    return inst;
}

HlsSession* HlsSessionRegistry::get_or_create(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& slot = sessions_[session_id];
    if (!slot) {
        slot = std::make_unique<HlsSession>();
        slot->session_id = session_id;
    }
    return slot.get();
}

HlsSession* HlsSessionRegistry::find(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

void HlsSessionRegistry::remove(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    sessions_.erase(session_id);
}

bool HlsSessionRegistry::lookup_playlist(const std::string& url,
                                         std::string& out_bytes,
                                         bool& out_is_master) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [sid, s] : sessions_) {
        if (!s) continue;
        if (s->master_url == url) {
            out_bytes     = s->master_playlist;
            out_is_master = true;
            return !out_bytes.empty();
        }
        const auto it = s->media_playlists.find(url);
        if (it != s->media_playlists.end()) {
            out_bytes     = it->second;
            out_is_master = false;
            return !out_bytes.empty();
        }
    }
    return false;
}

bool HlsSessionRegistry::fetch_segment(const std::string& url,
                                       std::string& out_bytes,
                                       int timeout_ms) {
    // Pick the most recently created session with a reverse socket and
    // send FCUP on it. The caller is the local HTTP server which has no
    // direct session handle — for a single iPhone, there's only one
    // HlsSession live at a time.
    HlsSession* s = nullptr;
    std::string session_id;
    int         reverse_fd = -1;
    int         request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [sid, sess] : sessions_) {
            if (!sess) continue;
            const int fd =
                ReverseChannelRegistry::instance().socket_for(sid);
            if (fd < 0) continue;
            s           = sess.get();
            session_id  = sid;
            reverse_fd  = fd;
            request_id  = s->next_request_id++;
            break;
        }
    }
    if (!s) {
        LOG_WARN << "fetch_segment: no active HLS session with a reverse channel";
        return false;
    }

    // Was it already delivered (iOS sometimes races a prior FCUP)?
    {
        std::lock_guard<std::mutex> lock(s->seg_mtx);
        const auto it = s->segment_data.find(url);
        if (it != s->segment_data.end()) {
            out_bytes = std::move(it->second);
            s->segment_data.erase(it);
            return true;
        }
    }

    if (!send_fcup_request(reverse_fd, url, session_id, request_id)) {
        LOG_WARN << "fetch_segment: FCUP send failed for " << url;
        return false;
    }

    std::unique_lock<std::mutex> lock(s->seg_mtx);
    const bool arrived = s->seg_cv.wait_for(lock,
        std::chrono::milliseconds(timeout_ms),
        [&] { return s->segment_data.count(url) != 0; });
    if (!arrived) {
        LOG_WARN << "fetch_segment: timeout after " << timeout_ms
                 << "ms for " << url;
        return false;
    }
    out_bytes = std::move(s->segment_data[url]);
    s->segment_data.erase(url);
    return true;
}

void HlsSessionRegistry::deliver_segment(const std::string& session_id,
                                         const std::string& url,
                                         std::string bytes) {
    HlsSession* s = find(session_id);
    if (!s) {
        LOG_WARN << "deliver_segment: no session " << session_id;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s->seg_mtx);
        s->segment_data[url] = std::move(bytes);
    }
    s->seg_cv.notify_all();
}

bool HlsSessionRegistry::fetch_playlist(const std::string& url,
                                        std::string& out_bytes,
                                        int timeout_ms) {
    // Pick the most recent session with a reverse channel (single
    // iPhone = single session) and FCUP the requested .m3u8 URL,
    // then block on seg_cv until handle_action populates it in
    // media_playlists.
    HlsSession* s = nullptr;
    std::string session_id;
    int         reverse_fd = -1;
    int         request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [sid, sess] : sessions_) {
            if (!sess) continue;
            const int fd =
                ReverseChannelRegistry::instance().socket_for(sid);
            if (fd < 0) continue;
            s          = sess.get();
            session_id = sid;
            reverse_fd = fd;
            request_id = s->next_request_id++;
            break;
        }
    }
    if (!s) return false;

    // Maybe it was already delivered while we were initialising.
    {
        std::lock_guard<std::mutex> lock(s->seg_mtx);
        const auto it = s->media_playlists.find(url);
        if (it != s->media_playlists.end()) {
            out_bytes = it->second;
            return true;
        }
    }

    if (!send_fcup_request(reverse_fd, url, session_id, request_id)) {
        LOG_WARN << "fetch_playlist: FCUP send failed for " << url;
        return false;
    }

    std::unique_lock<std::mutex> lock(s->seg_mtx);
    const bool arrived = s->seg_cv.wait_for(lock,
        std::chrono::milliseconds(timeout_ms),
        [&] { return s->media_playlists.count(url) != 0; });
    if (!arrived) {
        LOG_WARN << "fetch_playlist: timeout after " << timeout_ms
                 << "ms for " << url;
        return false;
    }
    out_bytes = s->media_playlists[url];
    return true;
}

void HlsSessionRegistry::notify_playlist_arrived(const std::string& session_id,
                                                 const std::string& /*url*/) {
    HlsSession* s = find(session_id);
    if (!s) return;
    // The playlist was already stored in media_playlists by the
    // caller; we just need to wake waiters so their predicate check
    // succeeds. notify_all because multiple waiters might be blocked
    // on different URLs on the same seg_cv.
    s->seg_cv.notify_all();
}

std::vector<std::string> extract_media_uris(const std::string& master) {
    constexpr const char* kPrefix = "mlhls://localhost/";
    constexpr const char* kSuffix = ".m3u8";
    std::vector<std::string> out;
    std::size_t pos = 0;
    while ((pos = master.find(kPrefix, pos)) != std::string::npos) {
        const std::size_t end = master.find(kSuffix, pos);
        if (end == std::string::npos) break;
        const std::size_t stop = end + std::strlen(kSuffix);
        std::string uri = master.substr(pos, stop - pos);
        if (std::find(out.begin(), out.end(), uri) == out.end()) {
            out.push_back(std::move(uri));
        }
        pos = stop;
    }
    return out;
}

} // namespace ap::airplay
