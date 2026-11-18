#include "settings.h"
#include "ui/i18n.h"
#include "airplay/live_settings.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::current_path() / ("settings-test-" + std::to_string(stamp) + ".ini");
    try {
        ap::PersistentSettings wanted;
        require(wanted.mirror_hwaccel, "GPU acceleration must be the default");
        require(!wanted.hls_playback, "experimental HLS must default to disabled");
        const auto fresh = ap::load_settings(path);
        require(!fresh.hls_playback, "new installation must not enable HLS");
        require(fresh.ui.face_swap_model == 2 && fresh.ui.face_swap_detector == 0 &&
                fresh.ui.face_swap_landmarker == 1 && fresh.ui.face_swap_alignment == 1,
                "fresh profile must select HyperSwap 1A, SCRFD, HRFFA and FaceFusion alignment");
        require(fresh.ui.face_swap_parser == 0 && !fresh.ui.face_swap_occlusion &&
                fresh.ui.face_swap_swapper_tensorrt && fresh.ui.face_swap_swapper_builder == 5,
                "fresh profile must use TensorRT level 5 with BiSeNet and XSeg off");
        wanted.hls_playback = true;
        wanted.receiver_name = "Living room";
        wanted.mirror_width = 3840; wanted.mirror_height = 2160;
        wanted.mirror_hwaccel = true;
        wanted.ui.diagnostics = true;
        wanted.ui.dark_theme = true;
        wanted.ui.language = "zh-CN";
        wanted.ui.window_placement_valid = true;
        wanted.ui.window_x = -1400; wanted.ui.window_y = 100;
        wanted.ui.window_width = 1100; wanted.ui.window_height = 760;
        wanted.ui.window_maximized = true;
        wanted.ui.recording_directory = "C:/Videos/Clips = \xc3\xa9t\xc3\xa9";
        wanted.ui.recording_codec = 1;
        wanted.ui.recording_encoder = 2;
        wanted.ui.recording_bitrate = 24;
        wanted.ui.face_swap_source = "//nas/Photos/Portrait = \xc3\xa9t\xc3\xa9.jpg";
        wanted.ui.face_swap_models = "D:/Models/NVIDIA";
        wanted.ui.face_swap_cpu_python = "D:/Apps/AirPlay/face_swap/python-cpu/python.exe";
        wanted.ui.face_swap_python = "D:/Apps/AirPlay/.venv-face-swap/Scripts/python.exe";
        wanted.ui.face_swap_quality = 2;
        wanted.ui.face_swap_restoration = 75;
        wanted.ui.face_swap_backend = 1;
        wanted.ui.face_swap_directml_device = 1;
        wanted.ui.face_swap_directml_python = "D:/Runtime DirectML/python.exe";
        wanted.ui.face_swap_gpu_resident = false;
        wanted.ui.face_swap_tensorrt = false;
        wanted.ui.face_swap_model = 4;
        wanted.ui.face_swap_swapper_tensorrt = false;
        wanted.ui.face_swap_swapper_builder = 3;
        wanted.ui.face_swap_occlusion = true;
        wanted.ui.face_swap_parser = 4;
        wanted.ui.face_swap_detector = 3;
        wanted.ui.face_swap_shape_transfer = 55;
        wanted.ui.face_swap_animation = 1;
        wanted.ui.face_swap_animation_strength = 80;
        wanted.ui.face_swap_landmarker = 0;
        wanted.ui.face_swap_alignment = 0;
        wanted.ui.face_swap_face_size = 110;
        wanted.ui.face_swap_face_scale = 105;
        wanted.ui.face_swap_mask_expansion = 4;
        wanted.ui.face_swap_mask_shape = 1;
        wanted.ui.face_swap_mask_blur = 32;
        wanted.ui.face_swap_mask_preview = 2;
        wanted.ui.face_swap_mask_padding = {7,11,13,17};
        require(ap::save_settings(wanted, path), "initial save failed");
        require(ap::load_settings(path) == wanted, "preferences lost on reload");
        wanted.ui.diagnostics = false;
        wanted.ui.dark_theme = false;
        wanted.ui.language = "fr";
        wanted.mirror_hwaccel = false;
        wanted.hls_playback = false;
        wanted.ui.recording_bitrate = 40;
        wanted.ui.face_swap_swapper_builder = 5;
        require(ap::save_settings(wanted, path), "replacement save failed");
        require(ap::load_settings(path) == wanted, "replacement did not preserve preferences");
        ap::airplay::LiveSettings live;
        require(!live.hls_playback.load(), "live HLS preference must default to disabled");
        live.hls_playback.store(true);
        require(ap::snapshot(live).hls_playback, "HLS missing from live snapshot");
        live.set_ui_preferences(wanted.ui);
        require(ap::snapshot(live).ui == wanted.ui, "UI preferences missing from live snapshot");
        {
            std::ofstream old(path);
            old << "receiver_name=Legacy receiver\nmirror_width=1920\nmirror_height=1080\nmirror_hwaccel=1\n";
        }
        const auto legacy = ap::load_settings(path);
        require(legacy.receiver_name == "Legacy receiver" && legacy.mirror_hwaccel, "legacy preferences broken");
        require(legacy.ui == ap::UiPreferences{}, "missing fields do not use defaults");
        require(!legacy.hls_playback, "legacy settings must not opt into experimental HLS");
        {
            std::ofstream old_cpu(path); old_cpu << "face_swap_cpu=1\n";
        }
        require(ap::load_settings(path).ui.face_swap_backend==2,"legacy CPU mode not migrated");

        {
            std::ofstream invalid(path);
            invalid << "language=unsupported\ndark_theme=2\nwindow_width=-5\nwindow_height=9999999\nrecording_codec=99\n"
                       "recording_encoder=-1\nrecording_bitrate=0\ndiagnostics=invalid\nhls_playback=2\n"
                       "face_swap_animation=2\nface_swap_animation_strength=101\nface_swap_quality=99\nface_swap_restoration=-1\nface_swap_backend=9\nface_swap_directml_device=-1\nface_swap_swapper_builder=4\nface_swap_mask_expansion=13\nface_swap_mask_shape=9\nface_swap_mask_blur=99\nface_swap_mask_preview=3\nface_swap_shape_transfer=101\nface_swap_landmarker=2\nface_swap_alignment=2\nface_swap_face_size=79\nface_swap_face_scale=121\nface_swap_mask_top=-1\nface_swap_mask_right=46\n";
        }
        require(ap::load_settings(path).ui == ap::UiPreferences{}, "invalid preferences accepted");
        require(!ap::load_settings(path).hls_playback, "invalid value must not enable experimental HLS");
        {
            std::ofstream automatic(path); automatic << "face_swap_swapper_builder=0\n";
        }
        require(ap::load_settings(path).ui.face_swap_swapper_builder==5,"legacy automatic must migrate to level 5");
        require(!ap::save_settings(wanted, {}), "empty path accepted");
        for (const auto* code : ap::i18n::codes) {
            wanted.ui.language=code;
            require(ap::save_settings(wanted,path),"language save failed");
            require(ap::load_settings(path)==wanted,"language preference lost on reload");
            live.set_ui_preferences(wanted.ui);
            require(ap::snapshot(live).ui.language==code,"language missing from live snapshot");
        }
        wanted.ui.language = "unknown";
        require(ap::save_settings(wanted, path), "invalid-language fallback save failed");
        require(ap::load_settings(path).ui.language == "en", "invalid saved language must fall back to English");
        std::filesystem::remove(path);
        std::cout << "Settings round-trip, replacement, legacy and invalid-value checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
