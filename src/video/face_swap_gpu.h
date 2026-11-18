#pragma once
#include <cstdint>
#include <memory>
#include <string>
extern "C" { struct AVFrame; }
namespace ap::video {
struct GpuTextureTicket {
    uint64_t input = 0, output = 0, adapter = 0;
    int width = 0, height = 0;
};
class FaceSwapGpu {
public:
    FaceSwapGpu();
    ~FaceSwapGpu();
    bool prepare(const AVFrame* frame, int max_side, GpuTextureTicket& ticket);
    AVFrame* complete();
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
