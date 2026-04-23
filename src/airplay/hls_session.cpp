#include "airplay/hls_session.h"
#include "airplay/fcup.h"
#include "airplay/reverse_channel.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <atomic>
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

std::string HlsSessionRegistry::resolve_segment_path(
        const std::string& local_path) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [sid, s] : sessions_) {
        if (!s) continue;
        std::lock_guard<std::mutex> sl(s->seg_url_mtx);
        const auto it = s->seg_url_map.find(local_path);
        if (it != s->seg_url_map.end()) return it->second;
    }
    return {};
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
                                       std::string& out_redirect,
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
        if (auto it = s->segment_redirect.find(url);
            it != s->segment_redirect.end()) {
            out_redirect = std::move(it->second);
            s->segment_redirect.erase(it);
            return true;
        }
        if (auto it = s->segment_data.find(url);
            it != s->segment_data.end()) {
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
        [&] { return s->segment_data.count(url)     != 0 ||
                     s->segment_redirect.count(url) != 0; });
    if (!arrived) {
        LOG_WARN << "fetch_segment: timeout after " << timeout_ms
                 << "ms for " << url;
        return false;
    }
    if (auto it = s->segment_redirect.find(url);
        it != s->segment_redirect.end()) {
        out_redirect = std::move(it->second);
        s->segment_redirect.erase(it);
    } else {
        out_bytes = std::move(s->segment_data[url]);
        s->segment_data.erase(url);
    }
    return true;
}

void HlsSessionRegistry::deliver_segment_redirect(const std::string& session_id,
                                                  const std::string& url,
                                                  std::string redirect) {
    HlsSession* s = find(session_id);
    if (!s) return;
    {
        std::lock_guard<std::mutex> lock(s->seg_mtx);
        s->segment_redirect[url] = std::move(redirect);
    }
    s->seg_cv.notify_all();
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

// Helper: extract the quoted value following `key=`, e.g.
//   input "...BASE-URI=\"https://x\",PARAMS=\"a,b\"..."
//   key   "BASE-URI="
//   -> "https://x"
// Returns empty string if the key isn't present or isn't quoted.
static std::string extract_quoted_attr(const std::string& src,
                                       const std::string& key) {
    const auto kp = src.find(key);
    if (kp == std::string::npos) return {};
    const auto open = src.find('"', kp + key.size());
    if (open == std::string::npos) return {};
    const auto close = src.find('"', open + 1);
    if (close == std::string::npos) return {};
    return src.substr(open + 1, close - open - 1);
}

// Port of UxPlay's adjust_yt_condensed_playlist (airplay_video.c).
// Given a media playlist:
//   #EXTM3U
//   #YT-EXT-CONDENSED-URL:BASE-URI="https://cdn/.../",PARAMS="s,slices",PREFIX="mlhls://localhost/..."
//   #EXTINF:5.0,
//   /0/5600/0-62329/0
//   ...
// Expand each condensed chunk line by replacing PREFIX with BASE-URI
// and interleaving the PARAMS tokens between the "/"-separated fields
// of the condensed path. Result: full https://cdn/.../s/0/5600/slices=0-62329/0
std::string expand_yt_condensed_playlist(const std::string& playlist) {
    const char* kHdr = "#YT-EXT-CONDENSED-URL";
    const auto hdr_pos = playlist.find(kHdr);
    if (hdr_pos == std::string::npos) return playlist;

    // Header runs until the next newline.
    const auto hdr_eol = playlist.find('\n', hdr_pos);
    if (hdr_eol == std::string::npos) return playlist;
    const std::string hdr_line = playlist.substr(hdr_pos, hdr_eol - hdr_pos);

    const std::string base_uri = extract_quoted_attr(hdr_line, "BASE-URI=");
    const std::string params   = extract_quoted_attr(hdr_line, "PARAMS=");
    const std::string prefix   = extract_quoted_attr(hdr_line, "PREFIX=");
    if (base_uri.empty() || params.empty() || prefix.empty()) return playlist;

    // Split params on ','
    std::vector<std::string> param_tokens;
    {
        std::size_t p = 0;
        while (p < params.size()) {
            const auto comma = params.find(',', p);
            if (comma == std::string::npos) {
                param_tokens.push_back(params.substr(p));
                break;
            }
            param_tokens.push_back(params.substr(p, comma - p));
            p = comma + 1;
        }
    }

    // Walk each chunk: find the line starting with `prefix` after an
    // #EXTINF tag, rebuild it as base_uri + interleaved fields.
    std::string out;
    out.reserve(playlist.size() * 2);
    std::size_t pos = 0;
    while (pos < playlist.size()) {
        const auto extinf = playlist.find("#EXTINF", pos);
        if (extinf == std::string::npos) {
            out.append(playlist, pos, std::string::npos);
            break;
        }
        const auto chunk_line = playlist.find(prefix, extinf);
        if (chunk_line == std::string::npos) {
            out.append(playlist, pos, std::string::npos);
            break;
        }
        // Copy everything up to the condensed URL.
        out.append(playlist, pos, chunk_line - pos);
        // Find end of the condensed URL line (next '\n' or end).
        auto url_end = playlist.find('\n', chunk_line);
        if (url_end == std::string::npos) url_end = playlist.size();
        const std::string condensed = playlist.substr(
            chunk_line + prefix.size(), url_end - chunk_line - prefix.size());

        // Split condensed on '/' and drop empty tokens — robust to
        // prefixes that do or do not end with '/' (iOS builds vary).
        std::vector<std::string> parts;
        {
            std::size_t p = 0;
            while (p < condensed.size()) {
                const auto slash = condensed.find('/', p);
                const auto end =
                    (slash == std::string::npos) ? condensed.size() : slash;
                if (end > p) {
                    parts.push_back(condensed.substr(p, end - p));
                }
                if (slash == std::string::npos) break;
                p = slash + 1;
            }
        }

        // Detect which shape iOS used:
        //   Model A (UxPlay / older iOS): condensed = "val0/val1/…/valN"
        //     — pure values. We interleave the PARAMS keys between them.
        //   Model B (newer iOS, seen on iOS 18): condensed already is
        //     "path0/path1/key0/val0/key1/val1/…" — path prefix plus
        //     in-line key/value pairs, possibly ending with a bare key
        //     (observed trailing "…/gosq/" with no value).
        // Discriminator: look for PARAMS[0] (e.g. "begin") anywhere in
        // the tokens. Any earlier tokens are path-prefix and stay.
        const std::string& first_key = param_tokens.front();
        const auto kv_it = std::find(parts.begin(), parts.end(), first_key);
        const bool model_b = (kv_it != parts.end());

        // BASE-URI ends with ".m3u8" (no trailing slash) so a single
        // '/' before whatever follows avoids a "//" artifact.
        out.append(base_uri);
        if (model_b) {
            // Model B: path_prefix + kv pairs, possibly with a bare
            // terminal key (iOS 18 ends chunks with ".../gosq/" — the
            // segment number is encoded into the trailing slash).
            //
            // Emit every token verbatim; preserve a trailing "/" from
            // the source so the CDN sees the exact URL iOS intended.
            // Only warn when an odd kv tail ends in a token we don't
            // recognise as a legitimate bare-terminal key — everything
            // else (even-length kv lists, or odd lists ending in an
            // allowed terminal) is emitted silently.
            static const std::array<const char*, 1> kBareTerminalKeys{"gosq"};
            const std::size_t kv_len = std::distance(kv_it, parts.end());
            if ((kv_len & 1U) != 0U) {
                const std::string& last = parts.back();
                const bool allowed = std::any_of(
                    kBareTerminalKeys.begin(), kBareTerminalKeys.end(),
                    [&](const char* k) { return last == k; });
                if (!allowed) {
                    LOG_WARN << "expand_yt_condensed_playlist: unexpected "
                                "odd kv tail in condensed \"" << condensed
                             << "\" — emitting verbatim";
                }
            }
            for (const auto& token : parts) {
                out.push_back('/');
                out.append(token);
            }
            // Preserve a trailing "/" from the condensed source (a bare
            // terminal key like "/gosq/"), but don't double it up — in
            // Model A the loop above already emits a "/" after every
            // key regardless of whether the value exists, so the
            // output may already end with "/".
            if (!condensed.empty() && condensed.back() == '/' &&
                (out.empty() || out.back() != '/')) {
                out.push_back('/');
            }
        } else {
            // Model A: emit "/param/val" for each param_tokens slot.
            // If iOS under-delivered values (fewer than param_tokens),
            // the tail emits empty values — combined with the trailing
            // "/" preservation below this keeps a "…/gosq/" shape
            // byte-identical to Model B.
            for (std::size_t i = 0; i < param_tokens.size(); ++i) {
                out.push_back('/');
                out.append(param_tokens[i]);
                out.push_back('/');
                if (i < parts.size()) out.append(parts[i]);
            }
            // Preserve a trailing "/" from the condensed source (a bare
            // terminal key like "/gosq/"), but don't double it up — in
            // Model A the loop above already emits a "/" after every
            // key regardless of whether the value exists, so the
            // output may already end with "/".
            if (!condensed.empty() && condensed.back() == '/' &&
                (out.empty() || out.back() != '/')) {
                out.push_back('/');
            }
        }
        out.push_back('\n');
        pos = url_end + 1;
    }
    return out;
}

std::string rewrite_segments_to_local(HlsSession& session,
                                      uint16_t local_port,
                                      const std::string& expanded,
                                      std::size_t& out_rewritten_count) {
    out_rewritten_count = 0;
    std::string out;
    out.reserve(expanded.size());
    std::size_t pos = 0;
    while (pos < expanded.size()) {
        const auto eol = expanded.find('\n', pos);
        const auto line_end = (eol == std::string::npos) ? expanded.size() : eol;
        const std::string line = expanded.substr(pos, line_end - pos);
        pos = (eol == std::string::npos) ? expanded.size() : eol + 1;

        // Blank / comment / header lines (HLS tags) — pass through
        // verbatim. A URI line starts with a non-'#' non-whitespace.
        const bool is_uri =
            !line.empty() && line[0] != '#' &&
            line[0] != ' ' && line[0] != '\t' && line[0] != '\r';
        if (!is_uri) {
            out.append(line);
            if (eol != std::string::npos) out.push_back('\n');
            continue;
        }

        // Strip \r on Windows line endings.
        std::string url_line = line;
        if (!url_line.empty() && url_line.back() == '\r') url_line.pop_back();

        // Register a fresh local path and remember the real URL.
        std::string local_path;
        {
            std::lock_guard<std::mutex> lock(session.seg_url_mtx);
            local_path = "/seg/" + std::to_string(session.seg_url_counter++);
            session.seg_url_map[local_path] = url_line;
        }
        out.append("http://localhost:");
        out.append(std::to_string(local_port));
        out.append(local_path);
        if (eol != std::string::npos) out.push_back('\n');
        ++out_rewritten_count;
    }
    return out;
}

std::string lookup_segment_url(const std::string& local_path) {
    return HlsSessionRegistry::instance().resolve_segment_path(local_path);
}

std::string filter_master_to_single_variant(const std::string& master) {
    // Two-pass. First pass picks the STREAM-INF we'll keep (first
    // seen) and records its AUDIO="…" group id if any. Second pass
    // rebuilds the playlist keeping only compatible renditions so
    // FFmpeg doesn't probe every itag.
    auto quoted = [](const std::string& line,
                     const char* key) -> std::string {
        const std::string k = key;
        const auto p = line.find(k);
        if (p == std::string::npos) return {};
        const auto open = line.find('"', p);
        if (open == std::string::npos) return {};
        const auto close = line.find('"', open + 1);
        if (close == std::string::npos) return {};
        return line.substr(open + 1, close - open - 1);
    };
    auto bare_attr = [](const std::string& line,
                        const char* key) -> std::string {
        const std::string k = key;
        const auto p = line.find(k);
        if (p == std::string::npos) return {};
        const auto start = p + k.size();
        const auto end = line.find_first_of(",\r\n ", start);
        return line.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
    };
    auto split_lines = [](const std::string& s) {
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos < s.size()) {
            const auto eol = s.find('\n', pos);
            const auto end = (eol == std::string::npos) ? s.size() : eol;
            std::string line = s.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        return lines;
    };

    const auto lines = split_lines(master);

    // --- Pass 1: pick the STREAM-INF and remember its AUDIO group.
    std::string audio_group;
    std::size_t stream_inf_idx = std::string::npos;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].compare(0, 18, "#EXT-X-STREAM-INF:") == 0) {
            stream_inf_idx = i;
            audio_group = quoted(lines[i], "AUDIO=");
            break;
        }
    }

    // --- Pass 2: rebuild.
    //   - Keep all header tags not of interest as-is.
    //   - For #EXT-X-MEDIA:TYPE=AUDIO: keep only entries whose
    //     GROUP-ID matches audio_group. Within that group, prefer
    //     the one with DEFAULT=YES, else AUTOSELECT=YES, else the
    //     first one seen.
    //   - For #EXT-X-MEDIA:TYPE!=AUDIO (e.g. SUBTITLES, CLOSED-
    //     CAPTIONS): keep the first per TYPE so captions still work.
    //   - For #EXT-X-STREAM-INF: keep only the first one + its URI.
    std::string out;
    out.reserve(master.size());

    // Preselect the single audio rendition.
    std::size_t chosen_audio = std::string::npos;
    {
        std::size_t default_idx = std::string::npos;
        std::size_t autoselect_idx = std::string::npos;
        std::size_t first_idx = std::string::npos;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& l = lines[i];
            if (l.compare(0, 13, "#EXT-X-MEDIA:") != 0) continue;
            if (bare_attr(l, "TYPE=") != "AUDIO") continue;
            if (!audio_group.empty() &&
                quoted(l, "GROUP-ID=") != audio_group) continue;
            if (first_idx == std::string::npos) first_idx = i;
            if (quoted(l, "DEFAULT=") == "YES" &&
                default_idx == std::string::npos) default_idx = i;
            if (quoted(l, "AUTOSELECT=") == "YES" &&
                autoselect_idx == std::string::npos) autoselect_idx = i;
        }
        if      (default_idx    != std::string::npos) chosen_audio = default_idx;
        else if (autoselect_idx != std::string::npos) chosen_audio = autoselect_idx;
        else                                          chosen_audio = first_idx;
    }

    // Preselect a single non-AUDIO MEDIA entry per TYPE (first seen).
    std::vector<std::string> kept_nonaudio_types;

    bool emitted_stream_inf = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];

        if (l.compare(0, 13, "#EXT-X-MEDIA:") == 0) {
            const std::string type = bare_attr(l, "TYPE=");
            if (type == "AUDIO") {
                if (i == chosen_audio) { out.append(l); out.push_back('\n'); }
                continue;
            }
            if (std::find(kept_nonaudio_types.begin(),
                          kept_nonaudio_types.end(), type)
                == kept_nonaudio_types.end()) {
                kept_nonaudio_types.push_back(type);
                out.append(l); out.push_back('\n');
            }
            continue;
        }
        if (l.compare(0, 18, "#EXT-X-STREAM-INF:") == 0) {
            if (emitted_stream_inf) {
                // skip this stream-inf + its URI
                if (i + 1 < lines.size()) ++i;
                continue;
            }
            emitted_stream_inf = true;
            out.append(l); out.push_back('\n');
            if (i + 1 < lines.size()) {
                out.append(lines[i + 1]);
                out.push_back('\n');
                ++i;
            }
            continue;
        }
        // All other tags / blank lines / URI lines: emit as-is.
        out.append(l);
        if (i + 1 < lines.size()) out.push_back('\n');
    }
    (void)stream_inf_idx;
    return out;
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
