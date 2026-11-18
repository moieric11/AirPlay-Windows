#include "settings.h"
#include "ui/i18n.h"
#include "airplay/live_settings.h"
#include "airplay/receiver_name.h"
#include "log.h"

#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <shlobj.h>
#endif

namespace ap {

std::filesystem::path settings_path() {
#if defined(_WIN32)
    PWSTR wide_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wide_path))) {
        CoTaskMemFree(wide_path);
        return {};
    }
    std::filesystem::path base(wide_path);
    CoTaskMemFree(wide_path);
    return base / L"AirPlay-Windows" / L"settings.ini";
#else
    return {};
#endif
}

PersistentSettings load_settings(const std::filesystem::path& path) {
    PersistentSettings s;
    if (path.empty()) return s;

    std::ifstream in(path);
    if (!in.is_open()) return s;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        try {
            if (key == "receiver_name") {
                const auto name = airplay::normalize_receiver_name(val);
                if (!name.empty()) s.receiver_name = name;
            }
            else if (key == "mirror_width") {
                const int n = std::stoi(val);
                if (n >= 320 && n <= 7680) s.mirror_width = n;
            }
            else if (key == "mirror_height") {
                const int n = std::stoi(val);
                if (n >= 240 && n <= 4320) s.mirror_height = n;
            }
            else if (key == "hevc_enabled")   s.hevc_enabled   = std::stoi(val) != 0;
            else if (key == "hls_playback") {
                if (val == "0" || val == "1") s.hls_playback = val == "1";
            }
            else if (key == "max_fps") {
                const int n = std::stoi(val);
                if (n >= 1 && n <= 60) s.max_fps = n;
            }
            else if (key == "refresh_rate") {
                const int n = std::stoi(val);
                if (n >= 1 && n <= 60) s.refresh_rate = n;
            }
            else if (key == "mirror_hwaccel") s.mirror_hwaccel = std::stoi(val) != 0;
            else if (key == "vsync_enabled")  s.vsync_enabled  = std::stoi(val) != 0;
            else if (key == "fullscreen")     s.fullscreen     = std::stoi(val) != 0;
            else if (key == "chromeless")     s.chromeless     = std::stoi(val) != 0;
            else if (key == "video_rotation") {
                const int n = std::stoi(val);
                if (n >= -1 && n <= 3) s.video_rotation = n;
            }
            else if (key == "language") {
                if (ap::i18n::supported(val)) s.ui.language = val;
            }
            else if (key == "dark_theme") {
                if (val == "0" || val == "1") s.ui.dark_theme = val == "1";
            }
            else if (key == "diagnostics") s.ui.diagnostics = std::stoi(val) != 0;
            else if (key == "window_placement_valid") s.ui.window_placement_valid = std::stoi(val) != 0;
            else if (key == "window_maximized") s.ui.window_maximized = std::stoi(val) != 0;
            else if (key == "window_x" || key == "window_y") {
                const int n = std::stoi(val);
                if (n >= -100000 && n <= 100000) {
                    if (key == "window_x") s.ui.window_x = n;
                    else s.ui.window_y = n;
                }
            }
            else if (key == "window_width" || key == "window_height") {
                const int n = std::stoi(val);
                if (n >= 160 && n <= 32768) {
                    if (key == "window_width") s.ui.window_width = n;
                    else s.ui.window_height = n;
                }
            }
            else if (key == "recording_directory") s.ui.recording_directory = val;
            else if (key == "face_swap_source") s.ui.face_swap_source = val;
            else if (key == "face_swap_models") s.ui.face_swap_models = val;
            else if (key == "face_swap_cpu_python") s.ui.face_swap_cpu_python = val;
            else if (key == "face_swap_python") s.ui.face_swap_python = val;
            else if (key == "face_swap_cpu" && (val == "0" || val == "1")) s.ui.face_swap_backend = val == "1" ? 2 : 0;
            else if (key == "face_swap_backend") { int n = std::stoi(val); if (n >= 0 && n <= 2) s.ui.face_swap_backend = n; }
            else if (key == "face_swap_directml_device") { int n = std::stoi(val); if (n >= 0 && n <= 63) s.ui.face_swap_directml_device = n; }
            else if (key == "face_swap_directml_python") s.ui.face_swap_directml_python = val;
            else if (key == "face_swap_gpu_resident" && (val == "0" || val == "1")) s.ui.face_swap_gpu_resident = val == "1";
            else if (key == "face_swap_tensorrt" && (val == "0" || val == "1")) s.ui.face_swap_tensorrt = val == "1";
            else if (key == "face_swap_swapper_tensorrt" && (val == "0" || val == "1")) s.ui.face_swap_swapper_tensorrt = val == "1";
            else if (key == "face_swap_mask_shape") {int n=std::stoi(val);if(n>=0 && n<=1)s.ui.face_swap_mask_shape=n;}
            else if (key == "face_swap_shape_transfer") {int n=std::stoi(val);if(n>=0 && n<=100)s.ui.face_swap_shape_transfer=n;}
            else if (key == "face_swap_animation") {int n=std::stoi(val);if(n>=0 && n<=1)s.ui.face_swap_animation=n;}
            else if (key == "face_swap_animation_strength") {int n=std::stoi(val);if(n>=0 && n<=100)s.ui.face_swap_animation_strength=n;}
            else if (key == "face_swap_landmarker") {int n=std::stoi(val);if(n>=0 && n<=1)s.ui.face_swap_landmarker=n;}
            else if (key == "face_swap_alignment") {int n=std::stoi(val);if(n>=0 && n<=1)s.ui.face_swap_alignment=n;}
            else if (key == "face_swap_face_size") {int n=std::stoi(val);if(n>=80 && n<=120)s.ui.face_swap_face_size=n;}
            else if (key == "face_swap_face_scale") {int n=std::stoi(val);if(n>=80 && n<=120)s.ui.face_swap_face_scale=n;}
            else if (key == "face_swap_mask_expansion") {int n=std::stoi(val);if(n>=0 && n<=12)s.ui.face_swap_mask_expansion=n;}
            else if (key == "face_swap_mask_blur") {int n=std::stoi(val);if(n>=0 && n<=50)s.ui.face_swap_mask_blur=n;}
            else if (key == "face_swap_mask_preview") {int n=std::stoi(val);if(n>=0 && n<=2)s.ui.face_swap_mask_preview=n;}
            else if (key == "face_swap_mask_top" || key == "face_swap_mask_right" || key == "face_swap_mask_bottom" || key == "face_swap_mask_left") {
                int n=std::stoi(val);if(n>=0 && n<=45) {
                    int i=key=="face_swap_mask_top" ? 0 : key=="face_swap_mask_right" ? 1 : key=="face_swap_mask_bottom" ? 2 : 3;
                    s.ui.face_swap_mask_padding[i]=n;
                }
            }
            else if (key == "face_swap_detector") {int n=std::stoi(val);if(n>=0 && n<=3)s.ui.face_swap_detector=n;}
            else if (key == "face_swap_parser") {int n=std::stoi(val);if(n>=0 && n<=4)s.ui.face_swap_parser=n;}
            else if (key == "face_swap_occlusion" && (val=="0" || val=="1")) s.ui.face_swap_occlusion=val=="1";
            else if (key == "face_swap_swapper_builder") { int n=std::stoi(val); if (n==0 || n==3 || n==5) s.ui.face_swap_swapper_builder=n==3 ? 3 : 5; }
            else if (key == "face_swap_model") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 4) s.ui.face_swap_model = n;
            }
            else if (key == "face_swap_quality") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 2) s.ui.face_swap_quality = n;
            }
            else if (key == "face_swap_restoration") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 100) s.ui.face_swap_restoration = n;
            }
            else if (key == "recording_codec") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 1) s.ui.recording_codec = n;
            }
            else if (key == "recording_encoder") {
                const int n = std::stoi(val);
                if (n >= 0 && n <= 2) s.ui.recording_encoder = n;
            }
            else if (key == "recording_bitrate") {
                const int n = std::stoi(val);
                if (n >= 2 && n <= 40) s.ui.recording_bitrate = n;
            }
        } catch (...) {
            LOG_WARN << "settings: malformed value for " << key << "=" << val;
        }
    }
    LOG_INFO << "settings: loaded from " << path.u8string();
    return s;
}

bool save_settings(const PersistentSettings& s, const std::filesystem::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_WARN << "settings: could not create directory: " << ec.message();
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN << "settings: could not write " << path.u8string();
        return false;
    }

    out << "receiver_name=" << s.receiver_name << '\n'
        << "mirror_width="   << s.mirror_width   << '\n'
        << "mirror_height="  << s.mirror_height  << '\n'
        << "hevc_enabled="   << s.hevc_enabled   << '\n'
        << "hls_playback="   << s.hls_playback   << '\n'
        << "max_fps="        << s.max_fps        << '\n'
        << "refresh_rate="   << s.refresh_rate   << '\n'
        << "mirror_hwaccel=" << s.mirror_hwaccel << '\n'
        << "vsync_enabled="  << s.vsync_enabled  << '\n'
        << "fullscreen="     << s.fullscreen     << '\n'
        << "chromeless="     << s.chromeless     << '\n'
        << "video_rotation=" << s.video_rotation << '\n'
        << "language=" << (ap::i18n::supported(s.ui.language) ? s.ui.language : "en") << '\n'
        << "dark_theme=" << s.ui.dark_theme << '\n'
        << "diagnostics=" << s.ui.diagnostics << '\n'
        << "window_placement_valid=" << s.ui.window_placement_valid << '\n'
        << "window_x=" << s.ui.window_x << '\n'
        << "window_y=" << s.ui.window_y << '\n'
        << "window_width=" << s.ui.window_width << '\n'
        << "window_height=" << s.ui.window_height << '\n'
        << "window_maximized=" << s.ui.window_maximized << '\n'
        << "recording_directory=" << s.ui.recording_directory << '\n'
        << "recording_codec=" << s.ui.recording_codec << '\n'
        << "recording_encoder=" << s.ui.recording_encoder << '\n'
        << "recording_bitrate=" << s.ui.recording_bitrate << '\n'
        << "face_swap_source=" << s.ui.face_swap_source << '\n'
        << "face_swap_models=" << s.ui.face_swap_models << '\n'
        << "face_swap_cpu_python=" << s.ui.face_swap_cpu_python << '\n'
        << "face_swap_python=" << s.ui.face_swap_python << '\n'
        << "face_swap_quality=" << s.ui.face_swap_quality << '\n'
        << "face_swap_restoration=" << s.ui.face_swap_restoration << '\n'
        << "face_swap_backend=" << s.ui.face_swap_backend << '\n'
        << "face_swap_directml_device=" << s.ui.face_swap_directml_device << '\n'
        << "face_swap_directml_python=" << s.ui.face_swap_directml_python << '\n'
        << "face_swap_gpu_resident=" << s.ui.face_swap_gpu_resident << '\n'
        << "face_swap_tensorrt=" << s.ui.face_swap_tensorrt << '\n'
        << "face_swap_model=" << s.ui.face_swap_model << '\n'
        << "face_swap_swapper_tensorrt=" << s.ui.face_swap_swapper_tensorrt << '\n'
        << "face_swap_mask_shape=" << s.ui.face_swap_mask_shape << '\n'
        << "face_swap_shape_transfer=" << s.ui.face_swap_shape_transfer << '\n'
        << "face_swap_animation=" << s.ui.face_swap_animation << '\n'
        << "face_swap_animation_strength=" << s.ui.face_swap_animation_strength << '\n'
        << "face_swap_landmarker=" << s.ui.face_swap_landmarker << '\n'
        << "face_swap_alignment=" << s.ui.face_swap_alignment << '\n'
        << "face_swap_face_size=" << s.ui.face_swap_face_size << '\n'
        << "face_swap_face_scale=" << s.ui.face_swap_face_scale << '\n'
        << "face_swap_mask_expansion=" << s.ui.face_swap_mask_expansion << '\n'
        << "face_swap_mask_blur=" << s.ui.face_swap_mask_blur << '\n'
        << "face_swap_mask_preview=" << s.ui.face_swap_mask_preview << '\n'
        << "face_swap_mask_top=" << s.ui.face_swap_mask_padding[0] << '\n'
        << "face_swap_mask_right=" << s.ui.face_swap_mask_padding[1] << '\n'
        << "face_swap_mask_bottom=" << s.ui.face_swap_mask_padding[2] << '\n'
        << "face_swap_mask_left=" << s.ui.face_swap_mask_padding[3] << '\n'
        << "face_swap_detector=" << s.ui.face_swap_detector << '\n'
        << "face_swap_parser=" << s.ui.face_swap_parser << '\n'
        << "face_swap_occlusion=" << s.ui.face_swap_occlusion << '\n'
        << "face_swap_swapper_builder=" << s.ui.face_swap_swapper_builder << '\n';
    out.close();
    if (!out) {
        LOG_WARN << "settings: could not finish writing " << temporary.u8string();
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_WARN << "settings: could not replace file, Windows error " << GetLastError();
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) return false;
#endif

    LOG_INFO << "settings: saved to " << path.u8string();
    return true;
}

PersistentSettings snapshot(const ap::airplay::LiveSettings& live) {
    PersistentSettings s;
    s.receiver_name = live.receiver_name();
    s.mirror_width   = live.mirror_width.load(std::memory_order_relaxed);
    s.mirror_height  = live.mirror_height.load(std::memory_order_relaxed);
    s.hevc_enabled   = live.hevc_enabled.load(std::memory_order_relaxed);
    s.hls_playback   = live.hls_playback.load(std::memory_order_relaxed);
    s.max_fps        = live.max_fps.load(std::memory_order_relaxed);
    s.refresh_rate   = live.refresh_rate.load(std::memory_order_relaxed);
    s.mirror_hwaccel = live.mirror_hwaccel.load(std::memory_order_relaxed);
    s.vsync_enabled  = live.vsync_enabled.load(std::memory_order_relaxed);
    s.fullscreen     = live.fullscreen.load(std::memory_order_relaxed);
    s.chromeless     = live.chromeless.load(std::memory_order_relaxed);
    s.video_rotation = live.video_rotation.load(std::memory_order_relaxed);
    s.ui = live.ui_preferences();
    return s;
}

} // namespace ap
