#include "video/video_renderer.h"
#include "log.h"

#include <SDL.h>

#include <chrono>
#include <cstring>

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

    SDL_Texture* texture      = nullptr;
    int          texture_w    = 0;
    int          texture_h    = 0;

    while (running_.load()) {
        // Pump events (quit, resize, ...). Avoid blocking the render loop.
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                closed_  = true;
                running_ = false;
                break;
            }
        }
        if (!running_.load()) break;

        // Wait briefly for a new frame; if none, clear and present anyway
        // to keep the window responsive.
        bool have_frame = false;
        std::vector<unsigned char> y, u, v;
        int w = 0, h = 0;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(16),
                         [this] { return has_frame_ || !running_.load(); });
            if (has_frame_) {
                y = y_buf_;
                u = u_buf_;
                v = v_buf_;
                w = frame_w_;
                h = frame_h_;
                has_frame_ = false;
                have_frame = true;
            }
        }

        if (have_frame) {
            if (!texture || texture_w != w || texture_h != h) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer,
                    SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
                texture_w = w;
                texture_h = h;
                LOG_INFO << "VideoRenderer texture (IYUV) created " << w << 'x' << h;
            }
            if (texture) {
                SDL_UpdateYUVTexture(texture, nullptr,
                    y.data(), w,
                    u.data(), w / 2,
                    v.data(), w / 2);
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (texture) {
            // Letter/pillar-box the source into the window while preserving aspect.
            int win_w = 0, win_h = 0;
            SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
            const double src_ar = static_cast<double>(texture_w) / texture_h;
            const double win_ar = static_cast<double>(win_w)     / win_h;
            SDL_Rect dst{};
            if (src_ar > win_ar) {
                dst.w = win_w;
                dst.h = static_cast<int>(win_w / src_ar);
                dst.x = 0;
                dst.y = (win_h - dst.h) / 2;
            } else {
                dst.h = win_h;
                dst.w = static_cast<int>(win_h * src_ar);
                dst.x = (win_w - dst.w) / 2;
                dst.y = 0;
            }
            SDL_RenderCopy(renderer, texture, nullptr, &dst);
        }
        SDL_RenderPresent(renderer);
    }

    if (texture)  SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
