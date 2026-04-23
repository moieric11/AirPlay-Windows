#pragma once

#include <memory>
#include <string>

namespace ap::video {

class VideoRenderer;

// Internal media player for the AirPlay Streaming HLS path. Opens
// `http://localhost:7100/master.m3u8` served by HlsLocalServer and
// pushes decoded YUV420P frames into the VideoRenderer.
//
// Two interchangeable backends, selected at CMake time:
//   - hls_player.cpp            (libavformat, default)
//   - hls_player_gstreamer.cpp  (USE_GSTREAMER_HLS=ON)
// Both expose the same class interface below.
//
// start() is non-blocking; the player runs its own thread(s) and
// pushes frames as they become available. stop() tears everything
// down and joins. The player is single-shot — create a new instance
// per iPhone /play call.
class HlsPlayer {
public:
    HlsPlayer();
    ~HlsPlayer();

    HlsPlayer(const HlsPlayer&)            = delete;
    HlsPlayer& operator=(const HlsPlayer&) = delete;

    bool start(const std::string& url, VideoRenderer* renderer);
    void stop();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace ap::video
