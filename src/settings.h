#pragma once

#include <filesystem>
#include <array>
#include <string>
#include <tuple>

namespace ap::airplay { struct LiveSettings; }

namespace ap {

struct UiPreferences {
    bool diagnostics = false;
    bool dark_theme = false;
    std::string language = "en";
    bool window_placement_valid = false;
    int window_x = 0, window_y = 0;
    int window_width = 1220, window_height = 800;
    bool window_maximized = false;
    std::string recording_directory;
    int recording_codec = 0;   // H.264 / H.265
    int recording_encoder = 0; // Auto / GPU / CPU
    int recording_bitrate = 8;
    std::string face_swap_source;
    std::string face_swap_models;
    std::string face_swap_python;
    std::string face_swap_cpu_python;
    int face_swap_quality = 1;
    int face_swap_restoration = 60;
    int face_swap_backend = 0;
    int face_swap_directml_device = 0;
    std::string face_swap_directml_python;
    bool face_swap_gpu_resident = true;
    bool face_swap_tensorrt = true;
    int face_swap_model = 2; // HyperSwap 1A 256.
    bool face_swap_swapper_tensorrt = true;
    int face_swap_animation=0, face_swap_animation_strength=100; // Face swap or LivePortrait reference animation.
    int face_swap_landmarker=1; // HRFFA 68 points, GPU-resident only.
    int face_swap_shape_transfer=0; // Experimental donor morphology intensity, 0..100.
    int face_swap_alignment=1; // FaceFusion-style five-point alignment.
    int face_swap_face_scale=100, face_swap_face_size=100;
    int face_swap_mask_expansion=0; // Final mask dilation, 0..12 percent of aligned crop.
    int face_swap_mask_shape=0, face_swap_mask_blur=18, face_swap_mask_preview=0;
    std::array<int,4> face_swap_mask_padding{}; // Top, right, bottom, left percentages.
    bool face_swap_occlusion = false;
    int face_swap_detector = 0; // SCRFD, YOLOFace, RetinaFace, YuNet.
    int face_swap_parser = 0; // Off, face, preserve eyes, preserve mouth, preserve both.
    int face_swap_swapper_builder = 5; // Shared by all TensorRT models; default 5, optional 3.

    bool operator==(const UiPreferences& other) const {
        return std::tie(diagnostics, dark_theme, language, window_placement_valid, window_x, window_y,
                        window_width, window_height, window_maximized, recording_directory,
                        recording_codec, recording_encoder, recording_bitrate,
                        face_swap_source, face_swap_models, face_swap_python, face_swap_cpu_python,
                        face_swap_quality, face_swap_restoration, face_swap_gpu_resident, face_swap_tensorrt,
                        face_swap_animation, face_swap_animation_strength, face_swap_shape_transfer, face_swap_landmarker, face_swap_alignment, face_swap_face_size, face_swap_face_scale, face_swap_model, face_swap_detector, face_swap_mask_expansion, face_swap_mask_shape, face_swap_mask_blur, face_swap_mask_preview, face_swap_mask_padding, face_swap_parser, face_swap_occlusion, face_swap_swapper_tensorrt, face_swap_swapper_builder, face_swap_backend, face_swap_directml_device, face_swap_directml_python) ==
               std::tie(other.diagnostics, other.dark_theme, other.language, other.window_placement_valid, other.window_x, other.window_y,
                        other.window_width, other.window_height, other.window_maximized, other.recording_directory,
                        other.recording_codec, other.recording_encoder, other.recording_bitrate,
                        other.face_swap_source, other.face_swap_models, other.face_swap_python, other.face_swap_cpu_python,
                        other.face_swap_quality, other.face_swap_restoration, other.face_swap_gpu_resident, other.face_swap_tensorrt,
                        other.face_swap_animation, other.face_swap_animation_strength, other.face_swap_shape_transfer, other.face_swap_landmarker, other.face_swap_alignment, other.face_swap_face_size, other.face_swap_face_scale, other.face_swap_model, other.face_swap_detector, other.face_swap_mask_expansion, other.face_swap_mask_shape, other.face_swap_mask_blur, other.face_swap_mask_preview, other.face_swap_mask_padding, other.face_swap_parser, other.face_swap_occlusion, other.face_swap_swapper_tensorrt, other.face_swap_swapper_builder, other.face_swap_backend, other.face_swap_directml_device, other.face_swap_directml_python);
    }
};

struct PersistentSettings {
    std::string receiver_name = "AirPlay-Windows";
    int  mirror_width   = 0;
    int  mirror_height  = 0;
    bool hevc_enabled   = true;
    bool hls_playback   = false; // Experimental; applies after app restart.
    int  max_fps        = 60;
    int  refresh_rate   = 60;
    bool mirror_hwaccel = true;
    bool vsync_enabled  = true;
    bool fullscreen     = false;
    bool chromeless     = false;
    int video_rotation  = -1; // -1: follow stream; 0..3: clockwise quarter turns
    UiPreferences ui;

    bool operator==(const PersistentSettings& other) const {
        return std::tie(receiver_name, mirror_width, mirror_height, hevc_enabled, hls_playback, max_fps,
                        refresh_rate, mirror_hwaccel, vsync_enabled, fullscreen, chromeless, video_rotation, ui) ==
               std::tie(other.receiver_name, other.mirror_width, other.mirror_height, other.hevc_enabled, other.hls_playback, other.max_fps,
                        other.refresh_rate, other.mirror_hwaccel, other.vsync_enabled, other.fullscreen, other.chromeless, other.video_rotation, other.ui);
    }
};

std::filesystem::path settings_path();
PersistentSettings  load_settings(const std::filesystem::path& path = settings_path());
bool                save_settings(const PersistentSettings& s, const std::filesystem::path& path = settings_path());
PersistentSettings  snapshot(const ap::airplay::LiveSettings& live);

} // namespace ap
