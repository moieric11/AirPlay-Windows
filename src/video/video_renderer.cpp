#include "video/video_renderer.h"
#include "log.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

constexpr int kDefaultWinWidth  = 600;
constexpr int kDefaultWinHeight = 1080;

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
                               int width, int height) {
    if (width <= 0 || height <= 0) return;
    const int c_w = width  / 2;
    const int c_h = height / 2;

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_, y, y_stride, width, height);
    copy_plane(u_buf_, u, u_stride, c_w,   c_h);
    copy_plane(v_buf_, v, v_stride, c_w,   c_h);
    frame_w_   = width;
    frame_h_   = height;
    has_frame_ = true;
    cv_.notify_one();
}

void VideoRenderer::run(const std::string& title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERROR << "SDL_Init failed: " << SDL_GetError();
        running_ = false;
        return;
    }

    SDL_Window*   window   = SDL_CreateWindow(
        title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kDefaultWinWidth, kDefaultWinHeight,
        SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
        : nullptr;
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

    SDL_Texture* video_tex = nullptr;
    int          video_tex_w = 0, video_tex_h = 0;

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
    SDL_Texture* title_tex    = nullptr; int title_w    = 0, title_h    = 0;
    SDL_Texture* artist_tex   = nullptr; int artist_w   = 0, artist_h   = 0;
    SDL_Texture* album_tex    = nullptr; int album_w    = 0, album_h    = 0;
    SDL_Texture* progress_tex = nullptr; int progress_w = 0, progress_h = 0;
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

    while (running_.load()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                closed_  = true;
                running_ = false;
                break;
            }
        }
        if (!running_.load()) break;

        // --- Consume pending video frame -----------------------------
        bool have_frame = false;
        std::vector<unsigned char> y, u, v;
        int w = 0, h = 0;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(16),
                         [this] {
                             return has_frame_ || cover_dirty_ || !running_.load();
                         });
            if (has_frame_) {
                y = y_buf_; u = u_buf_; v = v_buf_;
                w = frame_w_; h = frame_h_;
                has_frame_ = false;
                have_frame = true;
            }
        }

        if (have_frame) {
            if (!video_tex || video_tex_w != w || video_tex_h != h) {
                if (video_tex) SDL_DestroyTexture(video_tex);
                video_tex = SDL_CreateTexture(renderer,
                    SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
                video_tex_w = w; video_tex_h = h;
                LOG_INFO << "VideoRenderer texture (IYUV) created "
                         << w << 'x' << h;
            }
            if (video_tex) {
                SDL_UpdateYUVTexture(video_tex, nullptr,
                    y.data(), w, u.data(), w / 2, v.data(), w / 2);
            }
            last_video_frame_time = std::chrono::steady_clock::now();
        }

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
        // --- Consume pending metadata --------------------------------
        std::string t_title, t_artist, t_album;
        bool meta_changed = false;
        {
            std::lock_guard<std::mutex> lock(meta_mtx_);
            if (meta_dirty_) {
                t_title      = meta_title_;
                t_artist     = meta_artist_;
                t_album      = meta_album_;
                meta_dirty_  = false;
                meta_changed = true;
            }
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
        // Video takes priority if we got a frame within the last 500 ms.
        // Otherwise fall back to the cover texture; if neither, black.
        const auto now = std::chrono::steady_clock::now();
        const bool video_fresh =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_video_frame_time).count() < 500;

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (video_fresh && video_tex) {
            SDL_Rect dst = fit_inside(video_tex_w, video_tex_h, win_w, win_h);
            SDL_RenderCopy(renderer, video_tex, nullptr, &dst);
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
            if (!playing_.load(std::memory_order_relaxed)) {
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
            }

            // Text lines stacked left-aligned with a small margin.
            const int pad_x = 20;
            int       y     = top_h + 10;
            auto draw_text = [&](SDL_Texture* tex, int tw, int th) {
                if (!tex) return;
                int max_w = win_w - 2 * pad_x;
                int w     = std::min(tw, max_w);
                SDL_Rect r{pad_x, y, w, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
                y += th + 4;
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
            const bool     playing         = playing_.load(std::memory_order_relaxed);
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
                SDL_Rect bg{pad_x, y + 2, bar_w, bar_h};
                SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
                SDL_RenderFillRect(renderer, &bg);
                const double frac =
                    std::min(1.0, static_cast<double>(elapsed_ms) / total_ms);
                SDL_Rect fg{pad_x, y + 2, static_cast<int>(bar_w * frac), bar_h};
                SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
                SDL_RenderFillRect(renderer, &fg);
            }
        }
        SDL_RenderPresent(renderer);
    }

    if (title_tex)    SDL_DestroyTexture(title_tex);
    if (artist_tex)   SDL_DestroyTexture(artist_tex);
    if (album_tex)    SDL_DestroyTexture(album_tex);
    if (progress_tex) SDL_DestroyTexture(progress_tex);
    if (video_tex)  SDL_DestroyTexture(video_tex);
    if (cover_tex)  SDL_DestroyTexture(cover_tex);
    for (TTF_Font* f : fonts_big)   if (f) TTF_CloseFont(f);
    for (TTF_Font* f : fonts_small) if (f) TTF_CloseFont(f);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
