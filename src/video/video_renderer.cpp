#include "video/video_renderer.h"
#include "airplay/live_settings.h"
#include "log.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
    #include <dwmapi.h>
    // Duplicate the Win11 backdrop constants locally so we don't require
    // the very latest Windows SDK headers at build time. The DwmApi call
    // is still resolved at runtime against dwmapi.dll, which silently
    // accepts unknown attributes on pre-Win11 hosts.
    #ifndef DWMWA_SYSTEMBACKDROP_TYPE
        #define DWMWA_SYSTEMBACKDROP_TYPE 38
    #endif
    #ifndef DWMSBT_MAINWINDOW
        #define DWMSBT_MAINWINDOW 2
    #endif
#endif

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

// Match the design mockup's 1380x880 reference. The right panel
// collapses on demand so a portrait phone stream still gets a tall
// stage when the user shrinks the window.
constexpr int kDefaultWinWidth  = 1280;
constexpr int kDefaultWinHeight = 880;
constexpr int kSidebarWidth     = 280;
constexpr int kOptionsWidth     = 380;
constexpr int kStatusBarHeight  = 28;
constexpr int kToolbarHeight    = 40;

#if defined(_WIN32)
// Enable the Windows 11 Mica system backdrop on an SDL-owned window.
// No-op on pre-Win11 hosts (DwmSetWindowAttribute silently ignores
// unknown attribute IDs — the function just returns a non-zero HRESULT).
void enable_mica_backdrop(SDL_Window* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return;
    HWND hwnd = info.info.win.window;
    int backdrop = DWMSBT_MAINWINDOW;
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                            &backdrop, sizeof(backdrop));
}
#else
void enable_mica_backdrop(SDL_Window*) {}
#endif

// Minimal "OBS-ish" dark palette for the overlay. We tweak the full
// theme later; this just moves the default ImGui colours into a style
// that reads as part of the receiver window rather than a pale demo.
void apply_overlay_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 8.0f;
    s.FrameRounding   = 4.0f;
    s.GrabRounding    = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding   = ImVec2(12, 10);
    s.FramePadding    = ImVec2(8, 4);
    s.ItemSpacing     = ImVec2(8, 6);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.10f, 0.11f, 0.13f, 0.85f);
    c[ImGuiCol_Border]          = ImVec4(0.22f, 0.23f, 0.26f, 0.60f);
    c[ImGuiCol_Text]            = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.57f, 0.60f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.22f, 0.23f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.26f, 0.28f, 0.33f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.34f, 0.36f, 0.42f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.30f, 0.33f, 0.38f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.30f, 0.32f, 0.36f, 0.55f);
}

void copy_plane(std::vector<unsigned char>& dst,
                const uint8_t* src, int stride,
                int plane_w, int plane_h) {
    dst.resize(static_cast<std::size_t>(plane_w) * plane_h);
    for (int row = 0; row < plane_h; ++row) {
        std::memcpy(dst.data() + static_cast<std::size_t>(row) * plane_w,
                    src + static_cast<std::size_t>(row) * stride,
                    static_cast<std::size_t>(plane_w));
    }
}

} // namespace

namespace {

// Decode a JPEG blob via libavcodec's MJPEG decoder and repack as
// tightly-packed RGB24. Returns width/height through output params.
// `out_rgb` is sized w*h*3 on success.
bool decode_jpeg_to_rgb(const uint8_t* jpeg, std::size_t size,
                        int& w, int& h, std::vector<uint8_t>& out_rgb) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket*       pkt = av_packet_alloc();
    AVFrame*        fr  = av_frame_alloc();
    bool ok = ctx && pkt && fr &&
              avcodec_open2(ctx, codec, nullptr) == 0;
    if (ok) {
        pkt->data = const_cast<uint8_t*>(jpeg);
        pkt->size = static_cast<int>(size);
        ok = avcodec_send_packet(ctx, pkt)  == 0
          && avcodec_receive_frame(ctx, fr) == 0;
    }
    if (ok) {
        w = fr->width;
        h = fr->height;
        out_rgb.assign(static_cast<std::size_t>(w) * h * 3, 0);

        SwsContext* sws = sws_getContext(
            w, h, static_cast<AVPixelFormat>(fr->format),
            w, h, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) {
            uint8_t* dst[1]        = { out_rgb.data() };
            int      dst_stride[1] = { w * 3 };
            sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dst_stride);
            sws_freeContext(sws);
        } else {
            ok = false;
        }
    }
    if (fr)  av_frame_free(&fr);
    if (pkt) av_packet_free(&pkt);
    if (ctx) avcodec_free_context(&ctx);
    return ok;
}

// Open every font in a priority list so we can fall back per codepoint.
// Primary fonts cover Latin / Cyrillic / Greek (Segoe UI, DejaVu Sans…);
// the CJK fonts cover ideographs + kana + hangul (Microsoft YaHei for
// Simplified Chinese, Meiryo for Japanese, Malgun Gothic for Korean).
std::vector<TTF_Font*> open_font_chain(int pt_size) {
    static const std::array<const char*, 12> candidates = {{
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",   // Latin / Cyrillic / Greek
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",      // Chinese Simplified
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",    // Chinese fallback (older)
        "C:\\Windows\\Fonts\\meiryo.ttc",    // Japanese
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",    // Korean
        "C:\\Windows\\Fonts\\seguisym.ttf",  // misc symbols
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    }};
    std::vector<TTF_Font*> fonts;
    for (const char* path : candidates) {
        if (!path) continue;
        TTF_Font* f = TTF_OpenFont(path, pt_size);
        if (f) {
            fonts.push_back(f);
            LOG_INFO << "VideoRenderer loaded font " << path
                     << " @" << pt_size << "pt";
        }
    }
    if (fonts.empty()) {
        LOG_WARN << "VideoRenderer: no system font found — text overlay disabled";
    }
    return fonts;
}

// Tiny UTF-8 decoder — returns codepoint at `i` and advances `i` past
// the sequence. Returns 0xFFFD (replacement) on malformed input.
uint32_t utf8_next(const std::string& s, std::size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { ++i; return c0; }
    uint32_t cp = 0;
    int      n  = 0;
    if      ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; n = 1; }
    else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; n = 2; }
    else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; n = 3; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < n; ++k) {
        if (i >= s.size()) return 0xFFFD;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xc0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (c & 0x3f);
        ++i;
    }
    return cp;
}

// Return the first font in `fonts` that provides the given codepoint,
// or fonts[0] if none of them do (so the missing glyph at least renders
// as .notdef / tofu in a known font).
TTF_Font* pick_font_for(uint32_t cp, const std::vector<TTF_Font*>& fonts) {
    for (TTF_Font* f : fonts) {
        if (TTF_GlyphIsProvided32(f, cp)) return f;
    }
    return fonts.empty() ? nullptr : fonts[0];
}

// Render an arbitrary UTF-8 string across a font chain: split the string
// into runs of codepoints sharing the same font, render each run with
// TTF_RenderUTF8_Blended, then hconcat the surfaces so the output looks
// as if one super-font rendered the whole thing.
SDL_Surface* render_multi_font(const std::vector<TTF_Font*>& fonts,
                               const std::string& utf8,
                               SDL_Color color) {
    if (fonts.empty() || utf8.empty()) return nullptr;

    // Build runs: [font_for_this_substring, utf8_substring]
    struct Run { TTF_Font* font; std::string text; };
    std::vector<Run> runs;

    TTF_Font*   cur_font = nullptr;
    std::string cur_text;
    for (std::size_t i = 0; i < utf8.size(); ) {
        std::size_t start = i;
        uint32_t    cp    = utf8_next(utf8, i);
        TTF_Font*   f     = pick_font_for(cp, fonts);
        if (f != cur_font) {
            if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});
            cur_text.clear();
            cur_font = f;
        }
        cur_text.append(utf8, start, i - start);
    }
    if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});

    // Fast path: only one run, just use TTF directly.
    if (runs.size() == 1 && runs[0].font) {
        return TTF_RenderUTF8_Blended(runs[0].font, runs[0].text.c_str(), color);
    }

    // Slow path: render each run, then hconcat.
    std::vector<SDL_Surface*> surfs;
    int total_w = 0, max_h = 0;
    for (const auto& r : runs) {
        if (!r.font) { surfs.push_back(nullptr); continue; }
        SDL_Surface* s = TTF_RenderUTF8_Blended(r.font, r.text.c_str(), color);
        surfs.push_back(s);
        if (s) { total_w += s->w; max_h = std::max(max_h, s->h); }
    }
    if (total_w == 0) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }

    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(
        0, total_w, max_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!out) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }
    SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_NONE);

    int x = 0;
    for (SDL_Surface* s : surfs) {
        if (!s) continue;
        SDL_Rect dst{ x, max_h - s->h, s->w, s->h };  // baseline-bottom align
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(s, nullptr, out, &dst);
        x += s->w;
        SDL_FreeSurface(s);
    }
    return out;
}

// Draw a filled rect into a target rect of (win_w, win_h) such that the
// source aspect is preserved (letterbox / pillarbox).
SDL_Rect fit_inside(int src_w, int src_h, int win_w, int win_h) {
    SDL_Rect r{0, 0, win_w, win_h};
    if (src_w <= 0 || src_h <= 0) return r;
    const double src_ar = static_cast<double>(src_w) / src_h;
    const double win_ar = static_cast<double>(win_w) / win_h;
    if (src_ar > win_ar) {
        r.w = win_w;
        r.h = static_cast<int>(win_w / src_ar);
        r.x = 0;
        r.y = (win_h - r.h) / 2;
    } else {
        r.h = win_h;
        r.w = static_cast<int>(win_h * src_ar);
        r.x = (win_w - r.w) / 2;
        r.y = 0;
    }
    return r;
}

} // namespace

VideoRenderer::VideoRenderer()  = default;
VideoRenderer::~VideoRenderer() { stop(); }

bool VideoRenderer::start(const std::string& title) {
    running_ = true;
    thread_  = std::thread(&VideoRenderer::run, this, title);
    return true;
}

void VideoRenderer::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VideoRenderer::push_cover_art(const uint8_t* jpeg, std::size_t size) {
    if (!jpeg || size == 0) return;
    std::lock_guard<std::mutex> lock(cover_mtx_);
    cover_jpeg_.assign(jpeg, jpeg + size);
    cover_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_progress(uint32_t elapsed_ms, uint32_t total_ms) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    progress_elapsed_ms_.store(elapsed_ms, std::memory_order_relaxed);
    progress_total_ms_.store(total_ms,     std::memory_order_relaxed);
    progress_pushed_at_ns_.store(ns,       std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::push_playback_rate(float rate) {
    const bool new_playing = rate > 0.5f;
    const bool old_playing = playing_.exchange(new_playing, std::memory_order_relaxed);
    if (old_playing != new_playing) {
        LOG_INFO << "VideoRenderer playback state: "
                 << (new_playing ? "PLAYING" : "PAUSED");
    }
    cv_.notify_one();
}

void VideoRenderer::note_flush() {
    constexpr int64_t grace_ms = 500;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    flush_grace_until_ns_.store(now_ns + grace_ms * 1'000'000,
                                 std::memory_order_relaxed);
}

bool VideoRenderer::in_flush_grace() const {
    const int64_t until = flush_grace_until_ns_.load(std::memory_order_relaxed);
    if (until == 0) return false;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now_ns < until;
}

void VideoRenderer::clear_session() {
    LOG_INFO << "VideoRenderer clear_session: reset cover / metadata / progress";
    // Clear metadata on the next render tick by pushing empties (the
    // renderer re-generates textures from empty strings, which produces
    // no visible text).
    {
        std::lock_guard<std::mutex> lock(meta_mtx_);
        meta_title_.clear();
        meta_artist_.clear();
        meta_album_.clear();
        meta_dirty_ = true;
    }
    progress_elapsed_ms_.store(0, std::memory_order_relaxed);
    progress_total_ms_.store(0,   std::memory_order_relaxed);
    progress_pushed_at_ns_.store(0, std::memory_order_relaxed);
    playing_.store(true, std::memory_order_relaxed);
    clear_cover_requested_.store(true, std::memory_order_relaxed);
    // Reset the per-session stats so the status bar starts fresh on
    // the next paired device — "how much did this session use?" is
    // way more useful than "lifetime since app start".
    frames_total_.store(0, std::memory_order_relaxed);
    video_fps_ema_.store(0.0f, std::memory_order_relaxed);
    last_push_ns_.store(0, std::memory_order_relaxed);
    payload_bytes_total_.store(0, std::memory_order_relaxed);
    payload_mbps_ema_.store(0.0f, std::memory_order_relaxed);
    last_payload_ns_.store(0, std::memory_order_relaxed);
    pipeline_latency_ms_ema_.store(0.0f, std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::set_live_settings(ap::airplay::LiveSettings* live) {
    live_settings_ = live;
}

// --- DecodedFrame RAII implementation -----------------------------
DecodedFrame::~DecodedFrame() {
    if (frame) av_frame_free(&frame);
}
DecodedFrame::DecodedFrame(DecodedFrame&& o) noexcept : frame(o.frame) {
    o.frame = nullptr;
}
DecodedFrame& DecodedFrame::operator=(DecodedFrame&& o) noexcept {
    if (this != &o) {
        if (frame) av_frame_free(&frame);
        frame = o.frame;
        o.frame = nullptr;
    }
    return *this;
}

void VideoRenderer::push_avframe(const AVFrame* src, int64_t origin_ns) {
    if (!src) return;
    AVFrame* clone = av_frame_clone(src);
    if (!clone) return;

    // Stats: same FPS / WxH / counter bookkeeping as push_frame so
    // the status bar stays accurate regardless of which entry
    // point fed the slot.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(clone->width,  std::memory_order_relaxed);
    video_h_seen_.store(clone->height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        // Move-assign: drops the previous frame's ref via
        // av_frame_free in DecodedFrame::operator=.
        slot_avframe_   = DecodedFrame{clone};
        frame_w_        = clone->width;
        frame_h_        = clone->height;
        has_frame_      = false;       // legacy plane-buffer slot empty
        frame_is_nv12_  = false;
        has_avframe_    = true;
        frame_origin_ns_= origin_ns;
        cv_.notify_one();
    }
}

void VideoRenderer::record_payload_bytes(std::size_t n) {
    payload_bytes_total_.fetch_add(static_cast<uint64_t>(n),
                                   std::memory_order_relaxed);

    // Instantaneous bytes/sec → Mbps via EMA. Same approach as the
    // FPS EMA in push_frame: cheap, smooth, no buffer.
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_payload_ns_.exchange(now_ns,
                                                      std::memory_order_relaxed);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            // bytes/s × 8 bits/byte ÷ 1e6 → Mbps
            const float inst_mbps =
                static_cast<float>((static_cast<double>(n) * 8.0) /
                                   (dt_s * 1e6));
            const float prev_ema =
                payload_mbps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_mbps
                                  : prev_ema * 0.85f + inst_mbps * 0.15f;
            payload_mbps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
}

void VideoRenderer::set_active_device(DeviceInfo info) {
    std::lock_guard<std::mutex> lock(device_mtx_);
    device_     = std::move(info);
    has_device_ = true;
}

void VideoRenderer::clear_active_device() {
    std::lock_guard<std::mutex> lock(device_mtx_);
    has_device_ = false;
    device_     = {};
}

bool VideoRenderer::active_device_snapshot(DeviceInfo& out) const {
    std::lock_guard<std::mutex> lock(device_mtx_);
    if (!has_device_) return false;
    out = device_;
    return true;
}

void VideoRenderer::set_idle_info(const std::string& name,
                                  const std::string& ip) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    idle_name_  = name;
    idle_ip_    = ip;
    idle_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_metadata(const std::string& title,
                                  const std::string& artist,
                                  const std::string& album) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    if (meta_title_ == title && meta_artist_ == artist && meta_album_ == album) {
        return;   // no change, no need to re-render
    }
    meta_title_  = title;
    meta_artist_ = artist;
    meta_album_  = album;
    meta_dirty_  = true;
    cv_.notify_one();
}

void VideoRenderer::push_frame(const uint8_t* y, int y_stride,
                               const uint8_t* u, int u_stride,
                               const uint8_t* v, int v_stride,
                               int width, int height,
                               int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    const int c_w = width  / 2;
    const int c_h = height / 2;

    // Sample inter-frame interval for the overlay's video-FPS readout.
    // Keep this outside the mtx_ critical section since the producer
    // thread shouldn't have to wait on the mutex just to update stats.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_, y, y_stride, width, height);
    copy_plane(u_buf_, u, u_stride, c_w,   c_h);
    copy_plane(v_buf_, v, v_stride, c_w,   c_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = false;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::push_frame_nv12(const uint8_t* y,  int y_stride,
                                    const uint8_t* uv, int uv_stride,
                                    int width, int height,
                                    int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    const int uv_h = height / 2;

    // Stats: identical book-keeping as push_frame so the FPS EMA /
    // total counter stay accurate regardless of which path filled
    // the slot.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_,  y,  y_stride,  width, height);
    // UV interleaved plane: width bytes wide, height/2 rows tall.
    copy_plane(uv_buf_, uv, uv_stride, width, uv_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = true;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::run(const std::string& title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERROR << "SDL_Init failed: " << SDL_GetError();
        running_ = false;
        return;
    }

    // Bilinear filtering for any texture we scale (video frames and
    // cover art). SDL defaults to nearest-neighbour, which gave the
    // output a hard "blocky" look compared to Apple-certified
    // receivers. Must be set before SDL_CreateRenderer to take effect.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    SDL_Window*   window   = SDL_CreateWindow(
        title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kDefaultWinWidth, kDefaultWinHeight,
        SDL_WINDOW_RESIZABLE);
    // Read the initial vsync preference from LiveSettings (default
    // true). Toggling at runtime is handled by SDL_RenderSetVSync
    // below in the render loop.
    const bool vsync_initial = live_settings_
        ? live_settings_->vsync_enabled.load(std::memory_order_relaxed)
        : true;
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (vsync_initial) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, renderer_flags)
        : nullptr;
    bool vsync_active = vsync_initial;
    if (!window || !renderer) {
        LOG_ERROR << "SDL window/renderer create failed: " << SDL_GetError();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window)   SDL_DestroyWindow(window);
        SDL_Quit();
        running_ = false;
        return;
    }
    LOG_INFO << "VideoRenderer window up (" << kDefaultWinWidth
             << 'x' << kDefaultWinHeight << ", resizable)";

    // Win11 Mica backdrop. No-op on pre-Win11 — caller sees a regular
    // opaque window chrome instead of the subtle acrylic blur.
    enable_mica_backdrop(window);

    // Dear ImGui on top of the SDL renderer. The overlay runs in this
    // same thread and GPU context — zero frame copy, no IPC.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    apply_overlay_theme();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SDL_Texture* video_tex = nullptr;
    int          video_tex_w = 0, video_tex_h = 0;
    Uint32       video_tex_fmt = 0;  // SDL_PIXELFORMAT_IYUV or SDL_PIXELFORMAT_NV12

    SDL_Texture* cover_tex = nullptr;
    int          cover_tex_w = 0, cover_tex_h = 0;

    // Text overlay: load SDL_ttf + a multi-font chain (Latin primary +
    // CJK fallbacks) at two sizes so make_text can cover any iOS track
    // name, including Japanese / Chinese / Korean.
    std::vector<TTF_Font*> fonts_big;
    std::vector<TTF_Font*> fonts_small;
    if (TTF_Init() == 0) {
        fonts_big   = open_font_chain(32);
        fonts_small = open_font_chain(20);
    } else {
        LOG_WARN << "TTF_Init failed: " << TTF_GetError();
    }
    SDL_Texture* title_tex     = nullptr; int title_w     = 0, title_h     = 0;
    SDL_Texture* artist_tex    = nullptr; int artist_w    = 0, artist_h    = 0;
    SDL_Texture* album_tex     = nullptr; int album_w     = 0, album_h     = 0;
    SDL_Texture* progress_tex  = nullptr; int progress_w  = 0, progress_h  = 0;
    SDL_Texture* idle_name_tex = nullptr; int idle_name_w = 0, idle_name_h = 0;
    SDL_Texture* idle_ip_tex   = nullptr; int idle_ip_w   = 0, idle_ip_h   = 0;
    SDL_Texture* idle_msg_tex  = nullptr; int idle_msg_w  = 0, idle_msg_h  = 0;
    std::string  last_progress_text;

    auto make_text = [&](const std::vector<TTF_Font*>& fonts,
                         const std::string& s, SDL_Color c,
                         SDL_Texture*& tex, int& w, int& h) {
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        w = h = 0;
        if (fonts.empty() || s.empty()) return;
        SDL_Surface* surf = render_multi_font(fonts, s, c);
        if (!surf) return;
        tex = SDL_CreateTextureFromSurface(renderer, surf);
        w = surf->w; h = surf->h;
        SDL_FreeSurface(surf);
        // Default texture blend mode is NONE, which ignores alpha and renders
        // the surface as a solid opaque rectangle (every transparent pixel of
        // the glyph background fills the window with white). Force BLEND so
        // only the anti-aliased glyph pixels are drawn.
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    };

    auto last_video_frame_time = std::chrono::steady_clock::now()
                                 - std::chrono::seconds(10);

    // Persistent UI toggles. Survive across frames; mutated from ImGui
    // buttons. Defaults match the design mockup (right panel visible).
    struct UiState {
        bool show_options = true;
        bool show_demo    = false; // F12 toggles ImGui demo, dev only
    } ui;

    bool fullscreen = false;
    auto toggle_fullscreen = [&]() {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(window,
            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
    };

    while (running_.load()) {
        // Honor live VSYNC toggle: SDL 2.0.18+ supports flipping
        // VSYNC at runtime without rebuilding the renderer. The
        // call is cheap enough to issue every frame; we still
        // gate it on a state delta so we don't spam the driver.
        if (live_settings_) {
            const bool want_vsync =
                live_settings_->vsync_enabled.load(std::memory_order_relaxed);
            if (want_vsync != vsync_active) {
                if (SDL_RenderSetVSync(renderer, want_vsync ? 1 : 0) == 0) {
                    LOG_INFO << "VideoRenderer VSYNC "
                             << (want_vsync ? "enabled" : "disabled");
                    vsync_active = want_vsync;
                }
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Always feed ImGui first so focus / hover / capture work.
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                LOG_INFO << "VideoRenderer received SDL_QUIT";
                closed_  = true;
                running_ = false;
                break;
            }
            // When ImGui has keyboard / mouse capture (user interacting with
            // the overlay), don't let the native shortcuts steal the input.
            const ImGuiIO& io_poll = ImGui::GetIO();
            if (e.type == SDL_KEYDOWN && !io_poll.WantCaptureKeyboard) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        if (fullscreen) toggle_fullscreen();
                        else { closed_ = true; running_ = false; }
                        break;
                    case SDLK_f:
                    case SDLK_F11:
                        toggle_fullscreen();
                        break;
                    case SDLK_F12:
                        ui.show_demo = !ui.show_demo;
                        break;
                    default: break;
                }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN &&
                e.button.button == SDL_BUTTON_LEFT &&
                e.button.clicks == 2 &&
                !io_poll.WantCaptureMouse) {
                toggle_fullscreen();
            }
        }
        if (!running_.load()) break;

        // --- Consume pending video frame -----------------------------
        // Two producers feed the slot:
        //   - push_avframe (mirror, refcounted AVFrame, zero-copy)
        //   - push_frame / push_frame_nv12 (HLS, plane buffers)
        // The AVFrame slot wins when present; legacy slot used by
        // HLS players that haven't been migrated.
        bool have_avf   = false;
        bool have_frame = false;
        bool have_nv12  = false;
        std::vector<unsigned char> y, u, v, uv;
        DecodedFrame avf_local;
        int w = 0, h = 0;
        int64_t frame_origin_ns_local = 0;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(16),
                         [this] {
                             return has_avframe_ || has_frame_ ||
                                    cover_dirty_ || !running_.load();
                         });
            if (has_avframe_) {
                avf_local       = std::move(slot_avframe_);
                w               = frame_w_;
                h               = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_avframe_    = false;
                have_avf        = true;
            } else if (has_frame_) {
                have_nv12 = frame_is_nv12_;
                if (have_nv12) {
                    y  = y_buf_;
                    uv = uv_buf_;
                } else {
                    y = y_buf_; u = u_buf_; v = v_buf_;
                }
                w = frame_w_; h = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_frame_ = false;
                have_frame = true;
            }
        }

        // Decide the SDL pixel format to use; depends on which path
        // delivered the frame.
        Uint32 desired_fmt = SDL_PIXELFORMAT_IYUV;
        AVFrame* af = avf_local.frame;
        if (have_avf && af) {
            if (af->format == AV_PIX_FMT_NV12) {
                desired_fmt = SDL_PIXELFORMAT_NV12;
            } else {
                desired_fmt = SDL_PIXELFORMAT_IYUV;
            }
        } else if (have_frame) {
            desired_fmt = have_nv12 ? SDL_PIXELFORMAT_NV12
                                    : SDL_PIXELFORMAT_IYUV;
        }

        if (have_avf || have_frame) {
            if (!video_tex || video_tex_w != w || video_tex_h != h ||
                video_tex_fmt != desired_fmt) {
                if (video_tex) SDL_DestroyTexture(video_tex);
                video_tex = SDL_CreateTexture(renderer,
                    desired_fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
                video_tex_w   = w;
                video_tex_h   = h;
                video_tex_fmt = desired_fmt;
                LOG_INFO << "VideoRenderer texture ("
                         << (desired_fmt == SDL_PIXELFORMAT_NV12 ? "NV12"
                                                                 : "IYUV")
                         << ") created " << w << 'x' << h;
            }
            if (video_tex) {
                if (have_avf && af) {
                    // Direct upload from the refcounted AVFrame —
                    // the planes never crossed our buffers, just
                    // the FFmpeg pool → SDL staging texture.
                    if (af->format == AV_PIX_FMT_NV12) {
                        SDL_UpdateNVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1]);
                    } else {
                        // YUV420P / YUVJ420P (full-range) — SDL
                        // treats both as IYUV, slight color-range
                        // bias on YUVJ but indistinguishable for
                        // mirror.
                        SDL_UpdateYUVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1],
                            af->data[2], af->linesize[2]);
                    }
                } else if (have_nv12) {
                    SDL_UpdateNVTexture(video_tex, nullptr,
                        y.data(), w, uv.data(), w);
                } else {
                    SDL_UpdateYUVTexture(video_tex, nullptr,
                        y.data(), w, u.data(), w / 2, v.data(), w / 2);
                }
            }
            last_video_frame_time = std::chrono::steady_clock::now();
        }
        // avf_local destructor below releases our FFmpeg ref back
        // to the pool the moment SDL's UpdateTexture has issued
        // its CopySubresourceRegion to the GPU.

        // --- Drop the cover if clear_session was called --------------
        if (clear_cover_requested_.exchange(false, std::memory_order_relaxed)) {
            if (cover_tex) {
                SDL_DestroyTexture(cover_tex);
                cover_tex   = nullptr;
                cover_tex_w = cover_tex_h = 0;
            }
            std::lock_guard<std::mutex> lock(cover_mtx_);
            cover_jpeg_.clear();
            cover_dirty_ = false;
        }

        // --- Consume pending cover-art JPEG --------------------------
        std::vector<unsigned char> pending_cover;
        {
            std::lock_guard<std::mutex> lock(cover_mtx_);
            if (cover_dirty_) {
                pending_cover = std::move(cover_jpeg_);
                cover_jpeg_.clear();
                cover_dirty_ = false;
            }
        }
        // --- Consume pending metadata + idle info --------------------
        std::string t_title, t_artist, t_album, t_idle_name, t_idle_ip;
        bool meta_changed = false;
        bool idle_changed = false;
        {
            std::lock_guard<std::mutex> lock(meta_mtx_);
            if (meta_dirty_) {
                t_title      = meta_title_;
                t_artist     = meta_artist_;
                t_album      = meta_album_;
                meta_dirty_  = false;
                meta_changed = true;
            }
            if (idle_dirty_) {
                t_idle_name  = idle_name_;
                t_idle_ip    = idle_ip_;
                idle_dirty_  = false;
                idle_changed = true;
            }
        }
        if (idle_changed) {
            make_text(fonts_big,   t_idle_name,
                      SDL_Color{255, 255, 255, 255},
                      idle_name_tex, idle_name_w, idle_name_h);
            make_text(fonts_small, t_idle_ip,
                      SDL_Color{180, 180, 180, 255},
                      idle_ip_tex,   idle_ip_w,   idle_ip_h);
            make_text(fonts_small, "En attente d'AirPlay",
                      SDL_Color{150, 150, 150, 255},
                      idle_msg_tex,  idle_msg_w,  idle_msg_h);
        }
        if (meta_changed) {
            make_text(fonts_big,   t_title,  SDL_Color{255, 255, 255, 255},
                      title_tex,  title_w,  title_h);
            make_text(fonts_small, t_artist, SDL_Color{220, 220, 220, 255},
                      artist_tex, artist_w, artist_h);
            make_text(fonts_small, t_album,  SDL_Color{180, 180, 180, 255},
                      album_tex,  album_w,  album_h);
            LOG_INFO << "VideoRenderer metadata textures updated";
        }

        if (!pending_cover.empty()) {
            int cw = 0, ch = 0;
            std::vector<uint8_t> rgb;
            if (decode_jpeg_to_rgb(pending_cover.data(), pending_cover.size(),
                                   cw, ch, rgb)) {
                if (!cover_tex || cover_tex_w != cw || cover_tex_h != ch) {
                    if (cover_tex) SDL_DestroyTexture(cover_tex);
                    cover_tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, cw, ch);
                    cover_tex_w = cw; cover_tex_h = ch;
                }
                if (cover_tex) {
                    SDL_UpdateTexture(cover_tex, nullptr, rgb.data(), cw * 3);
                    LOG_INFO << "VideoRenderer cover art updated ("
                             << cw << 'x' << ch << ')';
                }
            } else {
                LOG_WARN << "VideoRenderer: JPEG decode failed ("
                         << pending_cover.size() << "B)";
            }
        }

        // --- Decide which texture to show ----------------------------
        // Video takes priority if we got a recent frame. iOS stops
        // emitting H.264 frames whenever the mirror content is static
        // (menu screens, paused video, idle home screen) — the encoder
        // is too efficient to bother re-sending identical frames. We
        // therefore keep the last video texture on screen as long as
        // a device is paired, regardless of the 500 ms freshness
        // window. Only on TEARDOWN (clear_active_device) does the
        // texture stop showing and we fall back to idle.
        const auto now = std::chrono::steady_clock::now();
        const bool video_fresh =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_video_frame_time).count() < 500;
        const bool playing_now = playing_.load(std::memory_order_relaxed);
        DeviceInfo active_dev;
        const bool has_active_dev = active_device_snapshot(active_dev);
        const bool show_video =
            video_tex && (video_fresh || has_active_dev || !playing_now);

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
        const int full_w = win_w;
        const int full_h = win_h;

        // Layout: top toolbar, sidebar (left), options panel (right,
        // optional), status bar (bottom). The remainder is the video
        // stage. In fullscreen mode the chrome is hidden entirely so
        // the video occupies the whole window — same toggle that flips
        // SDL_WINDOW_FULLSCREEN_DESKTOP also collapses the insets here
        // and skips the ImGui panel block below.
        const int options_w = (ui.show_options && !fullscreen)
                                  ? kOptionsWidth : 0;
        const int chrome_top    = fullscreen ? 0 : kToolbarHeight;
        const int chrome_bottom = fullscreen ? 0 : kStatusBarHeight;
        const int chrome_left   = fullscreen ? 0 : kSidebarWidth;
        const int stage_x = chrome_left;
        const int stage_y = chrome_top;
        const int stage_w = std::max(0, full_w - chrome_left - options_w);
        const int stage_h = std::max(0, full_h - chrome_top - chrome_bottom);

        // Override win_w/win_h locally so the existing video-fit and
        // idle-text centring math operates inside the stage rect
        // rather than the whole window. Restore after the SDL drawing
        // pass, before the ImGui pass references full window size.
        win_w = stage_w;
        win_h = stage_h;

        // Clear the whole window first (background under all panels),
        // then clip subsequent SDL draws to the stage rect via
        // viewport — the ImGui panels will paint over the cleared
        // sides afterwards.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const SDL_Rect stage_rect{stage_x, stage_y, stage_w, stage_h};
        SDL_RenderSetViewport(renderer, &stage_rect);

        auto draw_pause_badge = [&](const SDL_Rect& dst) {
            const int badge = std::min(dst.w, dst.h) / 6;
            const int cx    = dst.x + dst.w / 2;
            const int cy    = dst.y + dst.h / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect bg{cx - badge, cy - badge, badge * 2, badge * 2};
            SDL_RenderFillRect(renderer, &bg);
            const int bw = badge / 3;
            const int bh = badge;
            const int gap = badge / 3;
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 235);
            SDL_Rect b1{cx - gap - bw, cy - bh / 2, bw, bh};
            SDL_Rect b2{cx + gap,      cy - bh / 2, bw, bh};
            SDL_RenderFillRect(renderer, &b1);
            SDL_RenderFillRect(renderer, &b2);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        };

        // Suppress the pause badge entirely on Screen Mirroring
        // sessions: there, "no audio" doesn't mean "playback paused"
        // (it just means whatever is on the iPhone screen has no
        // soundtrack — a menu, the home screen, a static page). The
        // badge made sense in audio-only / HLS playback flows where
        // the user actually paused content. In mirror it just sat
        // there until the user pressed pause+play to clear it.
        const bool show_pause_badge =
            !playing_now &&
            (!has_active_dev || active_dev.kind != "Mirror");

        if (show_video) {
            SDL_Rect dst = fit_inside(video_tex_w, video_tex_h, win_w, win_h);
            SDL_RenderCopy(renderer, video_tex, nullptr, &dst);
            if (show_pause_badge) {
                draw_pause_badge(dst);
            }
        } else if (cover_tex) {
            // Cover art centered in the top 70% of the window; the bottom
            // strip holds the metadata text (title / artist / album).
            const int strip_h = win_h / 4;
            const int top_h   = win_h - strip_h;
            SDL_Rect dst = fit_inside(cover_tex_w, cover_tex_h, win_w, top_h);
            SDL_RenderCopy(renderer, cover_tex, nullptr, &dst);

            // Pause indicator: two white bars on a semi-transparent black
            // square, centered on the cover. Only while iOS reports
            // rate: 0 via text/parameters (or POST /rate?value=0).
            if (show_pause_badge) {
                draw_pause_badge(dst);
            }

            // Text lines stacked left-aligned with a small margin.
            const int pad_x = 20;
            int       y_pos = top_h + 10;
            auto draw_text = [&](SDL_Texture* tex, int tw, int th) {
                if (!tex) return;
                int max_w = win_w - 2 * pad_x;
                int w     = std::min(tw, max_w);
                SDL_Rect r{pad_x, y_pos, w, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
                y_pos += th + 4;
            };
            draw_text(title_tex,  title_w,  title_h);
            draw_text(artist_tex, artist_w, artist_h);
            draw_text(album_tex,  album_w,  album_h);

            // Progress row: "M:SS / M:SS" + thin bar. Only when the sender
            // reported a non-zero track length (otherwise a zero total would
            // hide the bar anyway, and dividing by zero would crash).
            // iOS only sends progress: at transitions, so extrapolate with
            // the wall clock since the last push (unless paused).
            const uint32_t total_ms        = progress_total_ms_.load(std::memory_order_relaxed);
            const uint32_t elapsed_pushed  = progress_elapsed_ms_.load(std::memory_order_relaxed);
            const int64_t  pushed_at_ns    = progress_pushed_at_ns_.load(std::memory_order_relaxed);
            const bool     playing         = playing_now;
            uint32_t elapsed_ms = elapsed_pushed;
            if (total_ms > 0 && playing && pushed_at_ns > 0) {
                const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const int64_t delta_ms = (now_ns - pushed_at_ns) / 1'000'000;
                if (delta_ms > 0) {
                    uint64_t extrapolated = static_cast<uint64_t>(elapsed_pushed) + delta_ms;
                    if (extrapolated > total_ms) extrapolated = total_ms;
                    elapsed_ms = static_cast<uint32_t>(extrapolated);
                }
            }
            if (total_ms > 0) {
                auto fmt = [](uint32_t ms) {
                    const uint32_t s = ms / 1000;
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
                    return std::string(buf);
                };
                std::string txt = fmt(elapsed_ms) + " / " + fmt(total_ms);
                if (txt != last_progress_text || !progress_tex) {
                    make_text(fonts_small, txt, SDL_Color{200, 200, 200, 255},
                              progress_tex, progress_w, progress_h);
                    last_progress_text = std::move(txt);
                }
                draw_text(progress_tex, progress_w, progress_h);

                const int bar_h = 4;
                const int bar_w = win_w - 2 * pad_x;
                SDL_Rect bg{pad_x, y_pos + 2, bar_w, bar_h};
                SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
                SDL_RenderFillRect(renderer, &bg);
                const double frac =
                    std::min(1.0, static_cast<double>(elapsed_ms) / total_ms);
                SDL_Rect fg{pad_x, y_pos + 2, static_cast<int>(bar_w * frac), bar_h};
                SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
                SDL_RenderFillRect(renderer, &fg);
            }
        } else {
            // Idle: no mirror frame, no cover — show device info.
            auto draw_centered = [&](SDL_Texture* tex, int tw, int th, int y) {
                if (!tex) return;
                SDL_Rect r{(win_w - tw) / 2, y, tw, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
            };
            const int total_h = idle_name_h + idle_ip_h + idle_msg_h + 24;
            int y_cursor = (win_h - total_h) / 2;
            draw_centered(idle_name_tex, idle_name_w, idle_name_h, y_cursor);
            y_cursor += idle_name_h + 12;
            draw_centered(idle_ip_tex,   idle_ip_w,   idle_ip_h,   y_cursor);
            y_cursor += idle_ip_h + 12;
            draw_centered(idle_msg_tex,  idle_msg_w,  idle_msg_h,  y_cursor);
        }

        // Restore full-window viewport so ImGui can draw the side panels
        // and status bar across the whole window. win_w / win_h were
        // overridden to stage dims for the SDL drawing pass; bring them
        // back to the full window size for the ImGui pass.
        SDL_RenderSetViewport(renderer, nullptr);
        win_w = full_w;
        win_h = full_h;

        // --- Dear ImGui overlay pass ---------------------------------
        // Runs every frame after the SDL_Render* video pass. The fixed
        // panels (sidebar / options / status) cover everything outside
        // the stage rect; the stage rect itself stays untouched so the
        // video / cover / idle texture is still visible.
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiWindowFlags kPanelFlags =
            ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoMove        |
            ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoTitleBar    |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus  | ImGuiWindowFlags_NoSavedSettings;

        const float status_bar_h = static_cast<float>(kStatusBarHeight);
        const float toolbar_h    = static_cast<float>(kToolbarHeight);
        const float side_bar_w   = static_cast<float>(kSidebarWidth);
        const float opts_bar_w   = static_cast<float>(kOptionsWidth);
        const float fw           = static_cast<float>(full_w);
        const float fh           = static_cast<float>(full_h);

        // Fullscreen mode hides the entire chrome (toolbar / sidebar /
        // options / status bar) so the video occupies the whole screen
        // like a real video player. F11/F/double-click toggles it; ESC
        // exits. The pulsing "Waiting for AirPlay" ring below this block
        // stays drawn — it's part of the stage, not the chrome.
        if (!fullscreen) {
        // ---- Top toolbar (app title + active device + actions) ----
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(fw, toolbar_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
        if (ImGui::Begin("##toolbar", nullptr,
                         kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.92f, 0.93f, 0.95f, 1.0f));
            ImGui::TextUnformatted("AirPlay-Windows");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled(" | ");
            ImGui::SameLine();
            const bool video_recent =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            ImDrawList* dl_tb = ImGui::GetWindowDrawList();
            const ImVec2 dot_p = ImGui::GetCursorScreenPos();
            const ImU32 dot_col = video_recent
                ? IM_COL32(80, 220, 130, 230)     // streaming  : green
                : (has_active_dev
                    ? IM_COL32(220, 200, 90, 230) // paired-quiet: amber
                    : IM_COL32(170, 170, 170, 180)); // idle     : grey
            dl_tb->AddCircleFilled(ImVec2(dot_p.x + 6, dot_p.y + 9),
                                   4.5f, dot_col);
            ImGui::Dummy(ImVec2(16, 18));
            ImGui::SameLine();
            if (has_active_dev) {
                const char* primary_tb =
                    !active_dev.name.empty()    ? active_dev.name.c_str()
                  : !active_dev.peer_ip.empty() ? active_dev.peer_ip.c_str()
                  : "iPhone";
                ImGui::Text("%s - %s", active_dev.kind.c_str(), primary_tb);
            } else {
                ImGui::TextDisabled("Idle - waiting for AirPlay");
            }
            // Right-aligned quick toggles.
            const float btn_options_w = 110.0f;
            const float btn_full_w    = 100.0f;
            const float gap = 8.0f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(fw - btn_options_w - btn_full_w - gap - 12.0f);
            if (ImGui::Button(
                    ui.show_options ? "Hide options" : "Show options",
                    ImVec2(btn_options_w, 0))) {
                ui.show_options = !ui.show_options;
            }
            ImGui::SameLine();
            if (ImGui::Button(fullscreen ? "Exit fullscreen" : "Fullscreen",
                              ImVec2(btn_full_w, 0))) {
                toggle_fullscreen();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // ---- Left sidebar : DEVICES -------------------------------
        ImGui::SetNextWindowPos(ImVec2(0.0f, toolbar_h));
        ImGui::SetNextWindowSize(ImVec2(side_bar_w, fh - toolbar_h - status_bar_h));
        if (ImGui::Begin("##devices_sidebar", nullptr, kPanelFlags)) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.65f, 0.68f, 0.72f, 1.0f));
            ImGui::TextUnformatted("DEVICES");
            ImGui::PopStyleColor();
            ImGui::Separator();
            // Wrap to the panel width so long device names / model
            // strings don't run off the right edge of the sidebar.
            ImGui::PushTextWrapPos(0.0f);
            const bool video_streaming =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            if (has_active_dev) {
                // Pulsing dot in front of the device name when the
                // pipeline is actively producing frames; muted dot
                // otherwise (paired but quiet).
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float t = video_streaming
                    ? 0.55f + 0.45f * std::sin(
                        static_cast<float>(ImGui::GetTime()) * 3.5f)
                    : 0.30f;
                const ImU32 dot = video_streaming
                    ? IM_COL32(80, 220, 130,
                               static_cast<int>(t * 255.0f))
                    : IM_COL32(170, 170, 170, 180);
                dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 9), 4.5f, dot);
                ImGui::Dummy(ImVec2(16, 18));
                ImGui::SameLine();
                // Prefer the human name if iOS sent one in the
                // session-setup plist; fall back to peer IP otherwise.
                const char* primary =
                    !active_dev.name.empty() ? active_dev.name.c_str()
                  : !active_dev.peer_ip.empty() ? active_dev.peer_ip.c_str()
                  : "iPhone";
                ImGui::TextUnformatted(primary);
                ImGui::TextDisabled("  %s", active_dev.kind.c_str());
                if (!active_dev.name.empty() && !active_dev.peer_ip.empty()) {
                    // Show the IP underneath when we have a name — the
                    // name and the IP together pinpoint the device on
                    // the network.
                    ImGui::TextDisabled("  %s", active_dev.peer_ip.c_str());
                }
                if (!active_dev.model.empty()) {
                    ImGui::TextDisabled("  %s", active_dev.model.c_str());
                }
                if (!active_dev.session_id.empty()) {
                    // Truncate the AirPlay session id to a short
                    // identifier — the full string is just noise.
                    const std::string short_id =
                        active_dev.session_id.size() > 8
                          ? active_dev.session_id.substr(0, 8) + "…"
                          : active_dev.session_id;
                    ImGui::TextDisabled("  sid %s", short_id.c_str());
                }
            } else {
                ImGui::TextDisabled("No device connected");
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "Open AirPlay on iOS and pick this "
                    "receiver to start mirroring.");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::End();

        // ---- Right options panel ----------------------------------
        if (ui.show_options) {
            ImGui::SetNextWindowPos(ImVec2(fw - opts_bar_w, toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(opts_bar_w, fh - toolbar_h - status_bar_h));
            if (ImGui::Begin("##options_panel", nullptr, kPanelFlags)) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.65f, 0.68f, 0.72f, 1.0f));
                ImGui::TextUnformatted("OPTIONS");
                ImGui::PopStyleColor();
                ImGui::Separator();
                // Wrap descriptive text to the panel width so long
                // hint strings (VSYNC / GPU decoder explanations, etc.)
                // stay inside the panel instead of running off-screen.
                ImGui::PushTextWrapPos(0.0f);
                if (ImGui::CollapsingHeader("Source",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("iOS-driven (read-only)");
                }
                if (ImGui::CollapsingHeader("Mirror Resolution",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        struct Preset { const char* label; int w; int h; };
                        static const Preset presets[] = {
                            {"1920x1080  (Full HD)",      1920, 1080},
                            {"2560x1440  (QHD, default)", 2560, 1440},
                            {"2868x2868  (iPhone native)",2868, 2868},
                            {"3840x2160  (4K UHD)",       3840, 2160},
                        };
                        int cur_w = live_settings_->mirror_width.load();
                        int cur_h = live_settings_->mirror_height.load();
                        // Find the preset that matches the current
                        // values, or "Custom" if none does.
                        int sel = -1;
                        for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                            if (presets[i].w == cur_w && presets[i].h == cur_h) {
                                sel = i; break;
                            }
                        }
                        const char* preview = sel >= 0
                            ? presets[sel].label : "Custom";
                        if (ImGui::BeginCombo("##mirror_res", preview)) {
                            for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                                const bool selected = (sel == i);
                                if (ImGui::Selectable(presets[i].label, selected)) {
                                    live_settings_->mirror_width.store(presets[i].w);
                                    live_settings_->mirror_height.store(presets[i].h);
                                }
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        // Custom W / H input fields. Each tracks its own
                        // active state — InputInt2's IsItemActive only
                        // covers the last component (height), so editing
                        // the width field would always be flagged "not
                        // active" and clobbered on the next frame.
                        static int  width_buf    = 2560;
                        static int  height_buf   = 1440;
                        static bool width_active = false;
                        static bool height_active= false;
                        if (!width_active)  width_buf  = cur_w;
                        if (!height_active) height_buf = cur_h;

                        ImGui::SetNextItemWidth(80);
                        const bool w_commit = ImGui::InputInt(
                            "Width##custom", &width_buf, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        width_active = ImGui::IsItemActive();
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80);
                        const bool h_commit = ImGui::InputInt(
                            "Height##custom", &height_buf, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        height_active = ImGui::IsItemActive();

                        if (w_commit && width_buf >= 320 &&
                            width_buf <= 7680) {
                            live_settings_->mirror_width.store(width_buf);
                        }
                        if (h_commit && height_buf >= 240 &&
                            height_buf <= 4320) {
                            live_settings_->mirror_height.store(height_buf);
                        }
                        ImGui::TextDisabled("Applies on next iPhone connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (ImGui::CollapsingHeader("Frame rate")) {
                    if (live_settings_) {
                        // iOS effectively takes the min of maxFPS and
                        // refreshRate, so two separate sliders meant
                        // the same thing twice. Drive both atomics
                        // from a single "Frame rate cap" knob and
                        // keep the underlying fields in lockstep.
                        int cap = live_settings_->max_fps.load();
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderInt("Frame rate cap",
                                             &cap, 1, 60, "%d fps")) {
                            live_settings_->max_fps.store(cap);
                            live_settings_->refresh_rate.store(cap);
                        }
                        ImGui::TextDisabled(
                            "iOS caps mirror at 60 fps regardless of hint;"
                            " lower values force a deterministic cadence.");
                        ImGui::TextDisabled("Applies on next iPhone connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (ImGui::CollapsingHeader("Compression",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        bool hevc = live_settings_->hevc_enabled.load();
                        if (ImGui::Checkbox("Allow HEVC (H.265)", &hevc)) {
                            live_settings_->hevc_enabled.store(hevc);
                        }
                        ImGui::TextDisabled(
                            hevc
                              ? "iOS may pick HEVC at high resolution"
                              : "Force H.264 (more CPU, more bandwidth)");
                        ImGui::TextDisabled("Applies on next iPhone connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (ImGui::CollapsingHeader("Mirror Decoder",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        bool hw = live_settings_->mirror_hwaccel.load();
                        if (ImGui::Checkbox("GPU decode (cuvid / D3D11VA)",
                                            &hw)) {
                            live_settings_->mirror_hwaccel.store(hw);
                        }
                        ImGui::TextDisabled(
                            hw
                              ? "GPU pipeline: NVDEC cuvid (NVIDIA) →"
                                " D3D11VA fallback. Higher latency than"
                                " software on single-stream mirror"
                                " (~30 ms vs 12 ms in tests) but offloads"
                                " the CPU."
                              : "Software libavcodec - fastest end-to-end"
                                " latency on single-stream mirror.");
                        ImGui::TextDisabled("Applies on next iPhone connection");

                        ImGui::Separator();

                        bool vsync = live_settings_->vsync_enabled.load();
                        if (ImGui::Checkbox("VSYNC", &vsync)) {
                            live_settings_->vsync_enabled.store(vsync);
                        }
                        ImGui::TextDisabled(
                            vsync
                              ? "Sync presents to monitor refresh"
                                " (clean image, +0..16 ms latency)."
                              : "Present immediately (lower latency,"
                                " visible tearing on fast pans).");
                        ImGui::TextDisabled("Applies live (no reconnect needed)");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (ImGui::CollapsingHeader("Network",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextDisabled("RTSP   :7000");
                    ImGui::TextDisabled("HLS    :7100");
                    ImGui::TextDisabled("mDNS   advertising");
                }
                if (ImGui::CollapsingHeader("Recording")) {
                    ImGui::TextDisabled("(not yet implemented)");
                }
                if (ImGui::CollapsingHeader("Hotkeys")) {
                    ImGui::BulletText("F      Toggle fullscreen");
                    ImGui::BulletText("F11    Toggle fullscreen");
                    ImGui::BulletText("DblClk Toggle fullscreen");
                    ImGui::BulletText("F12    Toggle ImGui demo");
                    ImGui::BulletText("Esc    Exit fullscreen / quit");
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }

        // ---- Bottom status bar ------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0.0f, fh - status_bar_h));
        ImGui::SetNextWindowSize(ImVec2(fw, status_bar_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
        if (ImGui::Begin("##status_bar", nullptr,
                         kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
            const ImGuiIO& io_ui = ImGui::GetIO();
            const bool playing = playing_.load(std::memory_order_relaxed);
            const bool has_video =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            const int      vid_w = video_w_seen_.load(std::memory_order_relaxed);
            const int      vid_h = video_h_seen_.load(std::memory_order_relaxed);
            const float    vid_fps = video_fps_ema_.load(std::memory_order_relaxed);
            const uint64_t total = frames_total_.load(std::memory_order_relaxed);
#if defined(HAVE_GSTREAMER_HLS)
            constexpr const char* kHlsBackend = "GStreamer";
#else
            constexpr const char* kHlsBackend = "libavformat";
#endif
            const auto sep = [&]() {
                ImGui::SameLine();
                ImGui::TextDisabled(" | ");
                ImGui::SameLine();
            };
            // "Streaming" = paired AND frames flowing; "Connected" =
            // paired but iOS quiet (static menu, paused video — common
            // in mirror); "Idle" = nothing connected.
            const char* state_label =
                has_active_dev
                    ? (has_video ? (playing ? "Streaming" : "Paused")
                                 : "Connected")
                    : "Idle";
            ImGui::Text("State: %s", state_label);
            sep();
            if (vid_w > 0 && vid_h > 0) {
                ImGui::Text("Video: %dx%d @ %.1f", vid_w, vid_h,
                            has_video ? vid_fps : 0.0f);
            } else {
                ImGui::TextDisabled("Video: -");
            }
            sep();
            ImGui::Text("Frames: %llu", static_cast<unsigned long long>(total));
            sep();
            ImGui::Text("Stage: %dx%d", stage_w, stage_h);
            sep();
            ImGui::Text("Renderer: %.0f fps", io_ui.Framerate);
            sep();
            const float    mbps  =
                payload_mbps_ema_.load(std::memory_order_relaxed);
            const uint64_t total_b =
                payload_bytes_total_.load(std::memory_order_relaxed);
            // Render the cumulative size in whichever unit reads
            // best for the magnitude (KB up to 10 MB, then MB, then
            // GB) so the eye sees a tidy "82.3 MB" instead of
            // "85786 KB".
            const auto fmt_total = [&](uint64_t bytes) {
                char buf[32];
                if (bytes < (10ull << 20)) {
                    std::snprintf(buf, sizeof(buf), "%.1f KB",
                                  bytes / 1024.0);
                } else if (bytes < (10ull << 30)) {
                    std::snprintf(buf, sizeof(buf), "%.1f MB",
                                  bytes / (1024.0 * 1024.0));
                } else {
                    std::snprintf(buf, sizeof(buf), "%.2f GB",
                                  bytes / (1024.0 * 1024.0 * 1024.0));
                }
                return std::string(buf);
            };
            ImGui::Text("BW: %.1f Mbps", has_video ? mbps : 0.0f);
            sep();
            ImGui::Text("Total: %s", fmt_total(total_b).c_str());
            sep();
            const float lat_ms =
                pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
            // Only show when a recent video frame populated it,
            // otherwise the EMA is stale or zero.
            ImGui::Text("Lat: %.1f ms", has_video ? lat_ms : 0.0f);
            sep();
            ImGui::TextDisabled("HLS backend: %s", kHlsBackend);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        } // !fullscreen

        // Pulsing "Waiting for AirPlay" ring centered in the stage when
        // no fresh video frame is on screen. Drawn to the background
        // list so the side panels still cover it where they overlap.
        {
            const bool video_streaming_idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            if (!video_streaming_idle && !has_active_dev &&
                stage_w > 0 && stage_h > 0) {
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                const float cx = static_cast<float>(stage_x) + stage_w * 0.5f;
                // Sit the rings above the SDL idle text (drawn around
                // 50% of the stage by the existing code) so the two
                // don't fight for the same pixels.
                const float cy = static_cast<float>(stage_y) + stage_h * 0.30f;
                const float t  = static_cast<float>(ImGui::GetTime());
                for (int i = 0; i < 2; ++i) {
                    const float speed  = (i == 0) ? 1.6f : 2.4f;
                    const float phase  = (i == 0) ? 0.0f : 1.05f;
                    const float pulse  =
                        0.5f + 0.5f * std::sin(t * speed + phase);
                    const float r_min  = (i == 0) ? 32.0f : 18.0f;
                    const float r_max  = (i == 0) ? 58.0f : 32.0f;
                    const float radius = r_min + (r_max - r_min) * pulse;
                    const int   alpha  =
                        static_cast<int>((1.0f - pulse) * 110.0f);
                    bg->AddCircle(ImVec2(cx, cy), radius,
                                  IM_COL32(120, 180, 240, alpha),
                                  64, 2.0f);
                }
                bg->AddCircleFilled(ImVec2(cx, cy), 8.0f,
                                    IM_COL32(120, 180, 240, 220));
            }
        }

        if (ui.show_demo) ImGui::ShowDemoWindow(&ui.show_demo);

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(
            ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);

        // Pipeline-latency measurement: from when the encrypted
        // frame body finished arriving on the wire (origin_ns
        // tagged in MirrorListener) to right after present
        // returns. Captures decrypt + decode + render + VSYNC.
        // Ignored when origin_ns is 0 (HLS / non-mirror frames or
        // older callers that didn't tag).
        if ((have_frame || have_avf) && frame_origin_ns_local > 0) {
            const int64_t now_ns2 =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const float lat_ms =
                static_cast<float>((now_ns2 - frame_origin_ns_local) / 1e6);
            const float prev_ema =
                pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? lat_ms
                                  : prev_ema * 0.90f + lat_ms * 0.10f;
            pipeline_latency_ms_ema_.store(ema, std::memory_order_relaxed);
        }
    }

    if (title_tex)     SDL_DestroyTexture(title_tex);
    if (artist_tex)    SDL_DestroyTexture(artist_tex);
    if (album_tex)     SDL_DestroyTexture(album_tex);
    if (progress_tex)  SDL_DestroyTexture(progress_tex);
    if (idle_name_tex) SDL_DestroyTexture(idle_name_tex);
    if (idle_ip_tex)   SDL_DestroyTexture(idle_ip_tex);
    if (idle_msg_tex)  SDL_DestroyTexture(idle_msg_tex);
    if (video_tex)     SDL_DestroyTexture(video_tex);
    if (cover_tex)     SDL_DestroyTexture(cover_tex);
    for (TTF_Font* f : fonts_big)   if (f) TTF_CloseFont(f);
    for (TTF_Font* f : fonts_small) if (f) TTF_CloseFont(f);
    TTF_Quit();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
