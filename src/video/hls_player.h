#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace ap::video {

class VideoRenderer;

// Internal media player for the AirPlay Streaming HLS path. Opens
// `http://localhost:7100/master.m3u8` (served by HlsLocalServer),
// demuxes with libavformat, decodes H.264 video with libavcodec, and
// pushes YUV420P frames into the VideoRenderer.
//
// A separate thread runs the read/decode loop. start() returns as
// soon as the thread is spawned; the URL is fetched asynchronously.
// stop() signals the thread to exit and joins. The player is
// single-shot — create a new instance per iPhone /play call.
class HlsPlayer {
public:
    HlsPlayer() = default;
    ~HlsPlayer() { stop(); }

    HlsPlayer(const HlsPlayer&)            = delete;
    HlsPlayer& operator=(const HlsPlayer&) = delete;

    // Non-blocking. Returns false if the thread couldn't be spawned.
    // `renderer` is non-owning and must outlive the player.
    bool start(const std::string& url, VideoRenderer* renderer);
    void stop();

private:
    void run(std::string url);

    std::atomic<bool> running_{false};
    std::thread       thread_;
    VideoRenderer*    renderer_{nullptr};
};

} // namespace ap::video
