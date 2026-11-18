#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::video {
struct FaceSwapRuntime {
    std::string python;
    std::string directml_python;
    std::string cpu_python;
    std::string worker;
    std::string models;
    std::string logs;
    std::string gpu_bridge;
    bool cpu = false;
    bool directml = false;
    int directml_device = 0;
    bool gpu_resident = true;
    bool tensorrt = true; // Optional TensorRT FP32 for GPEN 256/512, CUDA fallback.
    int swapper_model = 0; // InSwapper, AlphaFace, HyperSwap A/B/C.
    int swapper_builder = 5; // Optimized level 5 / standard level 3.
    bool occlusion = false;
    int detector = 0;
    int parser = 0;
    int animation=0, animation_strength=100; // Face swap or LivePortrait reference animation.
    int landmarker=0; // Off or HRFFA 68 points, GPU-resident only.
    int shape_transfer=0; // Experimental donor morphology intensity, 0..100.
    int shape_reset=0; // Session-only neutral calibration generation.
    int alignment=0; // Legacy or FaceFusion-style five-point alignment.
    int face_scale=100, face_size=100;
    int mask_expansion=0; // Final mask dilation, 0..12 percent of aligned crop.
    int mask_shape=0, mask_blur=18, mask_preview=0;
    std::array<int,4> mask_padding{};
    bool swapper_tensorrt = true; // Experimental native 256 models only.
    int quality = 1; // 0: fast, 1: GPEN 256, 2: GPEN 512
    int restoration = 60; // percent; blend restored detail with swapped face
    // Explicit external mode is used by protocol tests and diagnostic tools.
    bool launch = true;
    uint16_t port = 7011;
    static FaceSwapRuntime discover();
    static std::vector<std::string> directml_adapters();
};

// Owns only the process it creates. The Windows job kills the worker and its
// children on normal shutdown or if the receiver crashes.
class FaceSwapProcess {
public:
    FaceSwapProcess();
    ~FaceSwapProcess();
    bool start(const FaceSwapRuntime& runtime, const std::atomic<bool>& cancelled,
               uint16_t& port, std::string& error);
    std::string log_path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct FaceSwapDownloadStatus {
    bool active=false, failed=false;
    uint64_t received=0, total=0;
    std::string phase, message;
};
class FaceSwapDownload {
public:
    static constexpr int scrfd_model = 12;
    static constexpr int arcface_model = 13;
    FaceSwapDownload();
    ~FaceSwapDownload();
    void start(FaceSwapRuntime runtime, int model);
    void cancel();
    FaceSwapDownloadStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct FaceSwapPythonStatus {
    bool active=false, failed=false, complete=false;
    uint64_t received=0, total=0;
    std::string phase, message, python, log;
};
class FaceSwapPythonSetup {
public:
    FaceSwapPythonSetup();
    ~FaceSwapPythonSetup();
    void start(FaceSwapRuntime runtime, int backend);
    void cancel();
    FaceSwapPythonStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
