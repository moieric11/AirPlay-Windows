#include "video/video_renderer.h"
#include "log.h"

#include <SDL.h>

#include <chrono>
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
            SDL_Rect dst = fit_inside(cover_tex_w, cover_tex_h, win_w, win_h);
            SDL_RenderCopy(renderer, cover_tex, nullptr, &dst);
        }
        SDL_RenderPresent(renderer);
    }

    if (video_tex) SDL_DestroyTexture(video_tex);
    if (cover_tex) SDL_DestroyTexture(cover_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
