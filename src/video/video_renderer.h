#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ap::video {

// SDL2-backed real-time renderer for the decoded iPhone mirror stream.
//
// Runs its own thread that owns an SDL_Window + SDL_Renderer + SDL_Texture
// (YUV420P / IYUV planar, uploaded via SDL_UpdateYUVTexture so the YUV →
// RGB conversion happens on the GPU — no libswscale overhead here).
//
// Producers call push_frame() from the MirrorListener thread with the Y,
// U, V planes of a freshly decoded AVFrame. Only the most recent frame is
// kept: if the renderer hasn't consumed the pending one yet, push_frame
// silently overwrites it. This bounds memory usage and keeps latency
// predictable when the decoder outpaces the display.
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&)            = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    // Initialise SDL, create the window at a default size, start the
    // rendering thread. Returns false on SDL failure.
    bool start(const std::string& title = "AirPlay-Windows");

    // Tell the thread to exit, join it, tear down SDL.
    void stop();

    // Producer entry point. Copies each YUV plane into internal buffers
    // (skipping AVFrame padding via per-plane strides). Thread-safe.
    void push_frame(const uint8_t* y, int y_stride,
                    const uint8_t* u, int u_stride,
                    const uint8_t* v, int v_stride,
                    int width, int height);

    // Set true when the user closes the window (SDL_QUIT). main() polls
    // this to fold the window close into its Ctrl-C stop path.
    bool user_closed() const { return closed_.load(); }

private:
    void run(const std::string& title);

    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::thread       thread_;

    // Single-slot frame buffer (producer overwrites, consumer reads).
    std::mutex                 mtx_;
    std::condition_variable    cv_;
    std::vector<unsigned char> y_buf_;
    std::vector<unsigned char> u_buf_;
    std::vector<unsigned char> v_buf_;
    int                        frame_w_{0};
    int                        frame_h_{0};
    bool                       has_frame_{false};
};

} // namespace ap::video
