#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <functional>
#include "video/face_swap_process.h"

extern "C" { struct AVFrame; }

namespace ap::video {
struct FaceSwapPreparation {
    bool active=false;
    std::string stage;
    double elapsed_seconds=0;
};
// Optional loopback CUDA worker. One pending frame and one completed frame;
// all downloads, conversion and inference run away from the display thread.
class FaceSwap {
public:
    FaceSwap();
    ~FaceSwap();
    void start(const std::string& source, FaceSwapRuntime runtime = FaceSwapRuntime::discover());
    void stop();
    // Disconnect video without interrupting model preparation or destroying caches.
    void detach_video();
    void set_mask(int shape, int blur, int preview, const std::array<int,4>& padding, int face_scale=100, int face_size=100, int alignment=0, int mask_expansion=0, int shape_transfer=0, int shape_reset=0, int animation_strength=100);
    void submit(const AVFrame* frame, int64_t origin_ns);
    AVFrame* take(int width, int height, int64_t& origin_ns);
    bool has_result() const;
    void set_result_callback(std::function<void()> callback);
    std::string status() const;
    bool failed() const;
    std::string log_path() const;
    FaceSwapPreparation preparation() const;
    void record_cadence(uint64_t decoded, uint64_t replaced, bool input, bool presented, bool held,
                        double wait_ms, double present_ms, double origin_age_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
