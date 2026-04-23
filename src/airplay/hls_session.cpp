#include "airplay/hls_session.h"

#include <algorithm>
#include <cstring>

namespace ap::airplay {

HlsSessionRegistry& HlsSessionRegistry::instance() {
    static HlsSessionRegistry inst;
    return inst;
}

HlsSession* HlsSessionRegistry::get_or_create(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    return &sessions_[session_id];
}

HlsSession* HlsSessionRegistry::find(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : &it->second;
}

void HlsSessionRegistry::remove(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    sessions_.erase(session_id);
}

std::vector<std::string> extract_media_uris(const std::string& master) {
    // Scan for substrings of the form mlhls://localhost/<path>.m3u8
    // (both #EXT-X-MEDIA:URI="..." and #EXT-X-STREAM-INF: children
    // on their own line). UxPlay's create_media_uri_table does the
    // same linear scan; keep it minimal — every iOS-delivered HLS
    // master we've seen puts every child URI between the mlhls:// and
    // the literal .m3u8 with no nested quotes.
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
