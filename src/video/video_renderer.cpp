#include "video/video_renderer.h"
#include "platform/desktop_controls.h"
#include "video/video_recorder.h"
#include "video/gpu_video.h"
#include "video/cover_art.h"
#include "video/receiver_ui.h"
#include "airplay/live_settings.h"
#include "airplay/receiver_name.h"
#include "log.h"
#include "platform/single_instance.h"
#include "platform/firewall.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
    #include <dwmapi.h>
    #include <shobjidl.h>
    // Duplicate the Win11 backdrop constants locally so we don't require
    // the very latest Windows SDK headers at build time. The DwmApi call
    // is still resolved at runtime against dwmapi.dll, which silently
    // accepts unknown attributes on pre-Win11 hosts.
    #ifndef DWMWA_SYSTEMBACKDROP_TYPE
        #define DWMWA_SYSTEMBACKDROP_TYPE 38
    #endif
    #ifndef DWMSBT_MAINWINDOW
        #define DWMSBT_MAINWINDOW 2
    #endif
#endif

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

// Keep the stage spacious; settings occupy a drawer only when requested.
constexpr int kDefaultWinWidth  = 1220;
constexpr int kDefaultWinHeight = 800;
constexpr int kSidebarWidth     = 190;
constexpr int kOptionsWidth     = 380;
constexpr int kStatusLineHeight = 24;
constexpr int kToolbarHeight    = 45;
constexpr const char* kRotationModes[] = {
    "Automatic (device)", "Manual: 0 degrees", "Manual: 90 degrees",
    "Manual: 180 degrees", "Manual: 270 degrees"
};

#if defined(_WIN32)
// Enable the Windows 11 Mica system backdrop on an SDL-owned window.
// No-op on pre-Win11 hosts (DwmSetWindowAttribute silently ignores
// unknown attribute IDs — the function just returns a non-zero HRESULT).
void enable_mica_backdrop(SDL_Window* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return;
    HWND hwnd = info.info.win.window;
    int backdrop = DWMSBT_MAINWINDOW;
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                            &backdrop, sizeof(backdrop));
}

// Native Windows folder picker, parented to the SDL application window.
// Returns false for Cancel as well as dialog creation errors; in both cases
// the current path remains untouched.
bool browse_recording_folder(SDL_Window* w, const std::string& initial,
                             std::string& selected) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return false;

    const HRESULT init_hr = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(init_hr);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr) && dialog) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose the recording folder");

        try {
            const std::wstring initial_w =
                std::filesystem::u8path(initial).wstring();
            IShellItem* initial_item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initial_w.c_str(), nullptr, IID_PPV_ARGS(&initial_item)))) {
                dialog->SetFolder(initial_item);
                initial_item->Release();
            }
        } catch (const std::exception&) {
            // Keep the system default folder if the typed path is malformed.
        }

        hr = dialog->Show(info.info.win.window);
        if (SUCCEEDED(hr)) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(result->GetDisplayName(
                        SIGDN_FILESYSPATH, &path_w)) && path_w) {
                    selected = std::filesystem::path(path_w).u8string();
                    CoTaskMemFree(path_w);
                }
                result->Release();
            }
        }
        dialog->Release();
    }
    if (should_uninitialize) CoUninitialize();
    return !selected.empty();
}
#else
void enable_mica_backdrop(SDL_Window*) {}
#endif

// Calm, light palette shared by the receiver views and settings drawer.
void apply_overlay_theme() {
    ImGui::StyleColorsLight();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.GrabRounding = 8;
    s.WindowPadding = ImVec2(20, 20);
    s.FrameBorderSize = 1;
    s.ScrollbarSize = 10;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 12);
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Text] = ImVec4(.125f, .149f, .161f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.45f, .48f, .50f, 1);
    c[ImGuiCol_Border] = ImVec4(.90f, .91f, .90f, 1);
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_FrameBg] = ImVec4(.988f, .992f, .984f, 1);
    c[ImGuiCol_Button] = ImVec4(.988f, .992f, .984f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.87f, .91f, .97f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.80f, .86f, .96f, 1);
    c[ImGuiCol_Header] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_PlotHistogram] = ImVec4(.14f, .40f, .92f, 1);
}

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

namespace {

// Lifted out of run() so the SDL hit-test callback (a C function
// pointer with no captures) can read `chromeless` via callback_data.
// All fields are touched only on the render thread — no atomic
// needed; the callback runs synchronously on the same thread that
// owns SDL events.
struct UiState {
    bool chromeless = false;
    bool settings_open = false;
    bool diagnostics = false;
    int page = 0;
    bool was_connected = false;
};

// Both layouts use a custom title bar. Keep native resize handles at
// the edges; only the title bar (or the chromeless stage) is draggable.
SDL_HitTestResult SDLCALL hit_test_cb(SDL_Window* w,
                                      const SDL_Point* p,
                                      void* data) {
    auto* ui = reinterpret_cast<UiState*>(data);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(w, &win_w, &win_h);
    const Uint32 flags = SDL_GetWindowFlags(w);
    if (flags & SDL_WINDOW_FULLSCREEN) return SDL_HITTEST_NORMAL;
    const int B = ui && ui->chromeless ? 10 : 6;
    const bool resizable = !(flags & SDL_WINDOW_MAXIMIZED);
    const bool top    = resizable && p->y < B;
    const bool bottom = resizable && p->y >= win_h - B;
    const bool left   = resizable && p->x < B;
    const bool right  = resizable && p->x >= win_w - B;
    if (top    && left)  return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top    && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)             return SDL_HITTEST_RESIZE_TOP;
    if (bottom)          return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)            return SDL_HITTEST_RESIZE_LEFT;
    if (right)           return SDL_HITTEST_RESIZE_RIGHT;
    if (ui && (ui->chromeless || (p->y < kToolbarHeight && p->x < win_w - 144)))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}


// Open every font in a priority list so we can fall back per codepoint.
// Primary fonts cover Latin / Cyrillic / Greek (Segoe UI, DejaVu Sans…);
// the CJK fonts cover ideographs + kana + hangul (Microsoft YaHei for
// Simplified Chinese, Meiryo for Japanese, Malgun Gothic for Korean).
std::vector<TTF_Font*> open_font_chain(int pt_size) {
    static const std::array<const char*, 12> candidates = {{
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",   // Latin / Cyrillic / Greek
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",      // Chinese Simplified
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",    // Chinese fallback (older)
        "C:\\Windows\\Fonts\\meiryo.ttc",    // Japanese
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",    // Korean
        "C:\\Windows\\Fonts\\seguisym.ttf",  // misc symbols
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    }};
    std::vector<TTF_Font*> fonts;
    for (const char* path : candidates) {
        if (!path) continue;
        TTF_Font* f = TTF_OpenFont(path, pt_size);
        if (f) {
            fonts.push_back(f);
            LOG_INFO << "VideoRenderer loaded font " << path
                     << " @" << pt_size << "pt";
        }
    }
    if (fonts.empty()) {
        LOG_WARN << "VideoRenderer: no system font found — text overlay disabled";
    }
    return fonts;
}

// Tiny UTF-8 decoder — returns codepoint at `i` and advances `i` past
// the sequence. Returns 0xFFFD (replacement) on malformed input.
uint32_t utf8_next(const std::string& s, std::size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { ++i; return c0; }
    uint32_t cp = 0;
    int      n  = 0;
    if      ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; n = 1; }
    else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; n = 2; }
    else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; n = 3; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < n; ++k) {
        if (i >= s.size()) return 0xFFFD;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xc0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (c & 0x3f);
        ++i;
    }
    return cp;
}

// Return the first font in `fonts` that provides the given codepoint,
// or fonts[0] if none of them do (so the missing glyph at least renders
// as .notdef / tofu in a known font).
TTF_Font* pick_font_for(uint32_t cp, const std::vector<TTF_Font*>& fonts) {
    for (TTF_Font* f : fonts) {
        if (TTF_GlyphIsProvided32(f, cp)) return f;
    }
    return fonts.empty() ? nullptr : fonts[0];
}

// Render an arbitrary UTF-8 string across a font chain: split the string
// into runs of codepoints sharing the same font, render each run with
// TTF_RenderUTF8_Blended, then hconcat the surfaces so the output looks
// as if one super-font rendered the whole thing.
SDL_Surface* render_multi_font(const std::vector<TTF_Font*>& fonts,
                               const std::string& utf8,
                               SDL_Color color) {
    if (fonts.empty() || utf8.empty()) return nullptr;

    // Build runs: [font_for_this_substring, utf8_substring]
    struct Run { TTF_Font* font; std::string text; };
    std::vector<Run> runs;

    TTF_Font*   cur_font = nullptr;
    std::string cur_text;
    for (std::size_t i = 0; i < utf8.size(); ) {
        std::size_t start = i;
        uint32_t    cp    = utf8_next(utf8, i);
        TTF_Font*   f     = pick_font_for(cp, fonts);
        if (f != cur_font) {
            if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});
            cur_text.clear();
            cur_font = f;
        }
        cur_text.append(utf8, start, i - start);
    }
    if (!cur_text.empty()) runs.push_back({cur_font, std::move(cur_text)});

    // Fast path: only one run, just use TTF directly.
    if (runs.size() == 1 && runs[0].font) {
        return TTF_RenderUTF8_Blended(runs[0].font, runs[0].text.c_str(), color);
    }

    // Slow path: render each run, then hconcat.
    std::vector<SDL_Surface*> surfs;
    int total_w = 0, max_h = 0;
    for (const auto& r : runs) {
        if (!r.font) { surfs.push_back(nullptr); continue; }
        SDL_Surface* s = TTF_RenderUTF8_Blended(r.font, r.text.c_str(), color);
        surfs.push_back(s);
        if (s) { total_w += s->w; max_h = std::max(max_h, s->h); }
    }
    if (total_w == 0) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }

    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(
        0, total_w, max_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!out) {
        for (auto* s : surfs) if (s) SDL_FreeSurface(s);
        return nullptr;
    }
    SDL_SetSurfaceBlendMode(out, SDL_BLENDMODE_NONE);

    int x = 0;
    for (SDL_Surface* s : surfs) {
        if (!s) continue;
        SDL_Rect dst{ x, max_h - s->h, s->w, s->h };  // baseline-bottom align
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(s, nullptr, out, &dst);
        x += s->w;
        SDL_FreeSurface(s);
    }
    return out;
}

// Draw a filled rect into a target rect of (win_w, win_h) such that the
// source aspect is preserved (letterbox / pillarbox).
SDL_Rect fit_inside(int src_w, int src_h, int win_w, int win_h) {
    SDL_Rect r{0, 0, win_w, win_h};
    if (src_w <= 0 || src_h <= 0) return r;
    const double src_ar = static_cast<double>(src_w) / src_h;
    const double win_ar = static_cast<double>(win_w) / win_h;
    if (src_ar > win_ar) {
        r.w = win_w;
        r.h = static_cast<int>(win_w / src_ar);
        r.x = 0;
        r.y = (win_h - r.h) / 2;
    } else {
        r.h = win_h;
        r.w = static_cast<int>(win_h * src_ar);
        r.x = (win_w - r.w) / 2;
        r.y = 0;
    }
    return r;
}

} // namespace

VideoRenderer::VideoRenderer() : recorder_(std::make_unique<VideoRecorder>()) {}
VideoRenderer::~VideoRenderer() {
    stop();
    av_buffer_unref(&gpu_device_);
}

AVBufferRef* VideoRenderer::gpu_device_ref() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait_for(lock, std::chrono::seconds(5), [this] {
        return gpu_ready_ || !running_.load();
    });
    return running_.load() && gpu_device_ ? av_buffer_ref(gpu_device_) : nullptr;
}

bool VideoRenderer::start(const std::string& title, bool start_hidden) {
    running_ = true;
    thread_  = std::thread(&VideoRenderer::run, this, title, start_hidden);
    return true;
}

void VideoRenderer::stop() {
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (recorder_) recorder_->stop();
}

void VideoRenderer::push_cover_art(const uint8_t* jpeg, std::size_t size) {
    if (!jpeg || size == 0) return;
    std::lock_guard<std::mutex> lock(cover_mtx_);
    cover_jpeg_.assign(jpeg, jpeg + size);
    cover_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_progress(uint32_t elapsed_ms, uint32_t total_ms) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    progress_elapsed_ms_.store(elapsed_ms, std::memory_order_relaxed);
    progress_total_ms_.store(total_ms,     std::memory_order_relaxed);
    progress_pushed_at_ns_.store(ns,       std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::push_playback_rate(float rate) {
    const bool new_playing = rate > 0.5f;
    const bool old_playing = playing_.exchange(new_playing, std::memory_order_relaxed);
    if (old_playing != new_playing) {
        LOG_INFO << "VideoRenderer playback state: "
                 << (new_playing ? "PLAYING" : "PAUSED");
    }
    cv_.notify_one();
}

void VideoRenderer::note_flush() {
    constexpr int64_t grace_ms = 500;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    flush_grace_until_ns_.store(now_ns + grace_ms * 1'000'000,
                                 std::memory_order_relaxed);
}

bool VideoRenderer::in_flush_grace() const {
    const int64_t until = flush_grace_until_ns_.load(std::memory_order_relaxed);
    if (until == 0) return false;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now_ns < until;
}

void VideoRenderer::clear_session() {
    LOG_INFO << "VideoRenderer clear_session: reset cover / metadata / progress / video frame";
    audio_control_available_.store(false);
    audio_command_pending_.store(false);
    audio_command_failed_.store(false);
    // Drop the last decoded frame so the stage falls back to the
    // idle "Waiting for AirPlay" screen instead of freezing on the
    // last picture from the disconnected device. Both producer slots
    // (plane-buffer and refcounted AVFrame) are cleared.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        has_frame_      = false;
        frame_is_nv12_  = false;
        frame_w_        = 0;
        frame_h_        = 0;
        slot_avframe_   = DecodedFrame();   // releases the AVFrame ref
        has_avframe_    = false;
    }
    // Clear metadata on the next render tick by pushing empties (the
    // renderer re-generates textures from empty strings, which produces
    // no visible text).
    {
        std::lock_guard<std::mutex> lock(meta_mtx_);
        meta_title_.clear();
        meta_artist_.clear();
        meta_album_.clear();
        meta_dirty_ = true;
    }
    progress_elapsed_ms_.store(0, std::memory_order_relaxed);
    progress_total_ms_.store(0,   std::memory_order_relaxed);
    progress_pushed_at_ns_.store(0, std::memory_order_relaxed);
    playing_.store(true, std::memory_order_relaxed);
    clear_cover_requested_.store(true, std::memory_order_relaxed);
    // Reset the per-session stats so the status bar starts fresh on
    // the next paired device — "how much did this session use?" is
    // way more useful than "lifetime since app start".
    frames_total_.store(0, std::memory_order_relaxed);
    video_fps_ema_.store(0.0f, std::memory_order_relaxed);
    last_push_ns_.store(0, std::memory_order_relaxed);
    payload_bytes_total_.store(0, std::memory_order_relaxed);
    payload_mbps_ema_.store(0.0f, std::memory_order_relaxed);
    last_payload_ns_.store(0, std::memory_order_relaxed);
    pipeline_latency_ms_ema_.store(0.0f, std::memory_order_relaxed);
    cv_.notify_one();
}

void VideoRenderer::set_live_settings(ap::airplay::LiveSettings* live) {
    live_settings_ = live;
}

// --- DecodedFrame RAII implementation -----------------------------
DecodedFrame::~DecodedFrame() {
    if (frame) av_frame_free(&frame);
}
DecodedFrame::DecodedFrame(DecodedFrame&& o) noexcept : frame(o.frame) {
    o.frame = nullptr;
}
DecodedFrame& DecodedFrame::operator=(DecodedFrame&& o) noexcept {
    if (this != &o) {
        if (frame) av_frame_free(&frame);
        frame = o.frame;
        o.frame = nullptr;
    }
    return *this;
}

void VideoRenderer::push_avframe(const AVFrame* src, int64_t origin_ns) {
    if (!src) return;
    if (recorder_) recorder_->submit(src);
    AVFrame* clone = av_frame_clone(src);
    if (!clone) return;

    // Stats: same FPS / WxH / counter bookkeeping as push_frame so
    // the status bar stays accurate regardless of which entry
    // point fed the slot.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(clone->width,  std::memory_order_relaxed);
    video_h_seen_.store(clone->height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        // Move-assign: drops the previous frame's ref via
        // av_frame_free in DecodedFrame::operator=.
        slot_avframe_   = DecodedFrame{clone};
        frame_w_        = clone->width;
        frame_h_        = clone->height;
        has_frame_      = false;       // legacy plane-buffer slot empty
        frame_is_nv12_  = false;
        has_avframe_    = true;
        frame_origin_ns_= origin_ns;
        cv_.notify_one();
    }
}

void VideoRenderer::push_audio_pcm(const int16_t* samples, int sample_count,
                                   int sample_rate, int channels) {
    if (recorder_)
        recorder_->submit_audio(samples, sample_count, sample_rate, channels);
}

void VideoRenderer::record_payload_bytes(std::size_t n) {
    payload_bytes_total_.fetch_add(static_cast<uint64_t>(n),
                                   std::memory_order_relaxed);

    // Instantaneous bytes/sec → Mbps via EMA. Same approach as the
    // FPS EMA in push_frame: cheap, smooth, no buffer.
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_payload_ns_.exchange(now_ns,
                                                      std::memory_order_relaxed);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            // bytes/s × 8 bits/byte ÷ 1e6 → Mbps
            const float inst_mbps =
                static_cast<float>((static_cast<double>(n) * 8.0) /
                                   (dt_s * 1e6));
            const float prev_ema =
                payload_mbps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_mbps
                                  : prev_ema * 0.85f + inst_mbps * 0.15f;
            payload_mbps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
}

void VideoRenderer::set_active_device(DeviceInfo info) {
    std::lock_guard<std::mutex> lock(device_mtx_);
    device_     = std::move(info);
    has_device_ = true;
}

void VideoRenderer::clear_active_device() {
    std::lock_guard<std::mutex> lock(device_mtx_);
    has_device_ = false;
    device_     = {};
}

bool VideoRenderer::clear_session_if_active(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(device_mtx_);
    if (!has_device_ || device_.session_id != session_id) return false;
    clear_session();
    has_device_ = false;
    device_ = {};
    LOG_INFO << "Connection ended: cleared active session=" << session_id;
    return true;
}

void VideoRenderer::set_audio_control_handler(
        std::function<void(ap::airplay::MediaCommand, uint32_t)> handler) {
    audio_control_handler_ = std::move(handler);
}

void VideoRenderer::set_disconnect_handler(std::function<void()> h) {
    disconnect_handler_ = std::move(h);
}

bool VideoRenderer::active_device_snapshot(DeviceInfo& out) const {
    std::lock_guard<std::mutex> lock(device_mtx_);
    if (!has_device_) return false;
    out = device_;
    return true;
}

void VideoRenderer::set_idle_info(const std::string& name,
                                  const std::string& ip) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    idle_name_  = name;
    idle_ip_    = ip;
    idle_dirty_ = true;
    cv_.notify_one();
}

void VideoRenderer::push_metadata(const std::string& title,
                                  const std::string& artist,
                                  const std::string& album) {
    std::lock_guard<std::mutex> lock(meta_mtx_);
    if (meta_title_ == title && meta_artist_ == artist && meta_album_ == album) {
        return;   // no change, no need to re-render
    }
    meta_title_  = title;
    meta_artist_ = artist;
    meta_album_  = album;
    meta_dirty_  = true;
    cv_.notify_one();
}

void VideoRenderer::push_frame(const uint8_t* y, int y_stride,
                               const uint8_t* u, int u_stride,
                               const uint8_t* v, int v_stride,
                               int width, int height,
                               int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    if (recorder_) recorder_->submit_i420(y, y_stride, u, u_stride,
                                          v, v_stride, width, height);
    const int c_w = width  / 2;
    const int c_h = height / 2;

    // Sample inter-frame interval for the overlay's video-FPS readout.
    // Keep this outside the mtx_ critical section since the producer
    // thread shouldn't have to wait on the mutex just to update stats.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_, y, y_stride, width, height);
    copy_plane(u_buf_, u, u_stride, c_w,   c_h);
    copy_plane(v_buf_, v, v_stride, c_w,   c_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = false;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::push_frame_nv12(const uint8_t* y,  int y_stride,
                                    const uint8_t* uv, int uv_stride,
                                    int width, int height,
                                    int64_t origin_ns) {
    if (width <= 0 || height <= 0) return;
    if (recorder_) recorder_->submit_nv12(y, y_stride, uv, uv_stride,
                                          width, height);
    const int uv_h = height / 2;

    // Stats: identical book-keeping as push_frame so the FPS EMA /
    // total counter stay accurate regardless of which path filled
    // the slot.
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t prev_ns = last_push_ns_.exchange(now_ns);
    if (prev_ns > 0) {
        const double dt_s = (now_ns - prev_ns) / 1e9;
        if (dt_s > 0.001 && dt_s < 1.0) {
            const float inst_fps = static_cast<float>(1.0 / dt_s);
            const float prev_ema = video_fps_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? inst_fps
                                  : prev_ema * 0.85f + inst_fps * 0.15f;
            video_fps_ema_.store(ema, std::memory_order_relaxed);
        }
    }
    video_w_seen_.store(width,  std::memory_order_relaxed);
    video_h_seen_.store(height, std::memory_order_relaxed);
    frames_total_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mtx_);
    copy_plane(y_buf_,  y,  y_stride,  width, height);
    // UV interleaved plane: width bytes wide, height/2 rows tall.
    copy_plane(uv_buf_, uv, uv_stride, width, uv_h);
    frame_w_         = width;
    frame_h_         = height;
    has_frame_       = true;
    frame_is_nv12_   = true;
    frame_origin_ns_ = origin_ns;
    cv_.notify_one();
}

void VideoRenderer::run(const std::string& title, bool start_hidden) {
    ap::platform::DesktopControls desktop;
    bool tray_available = false;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERROR << "SDL_Init failed: " << SDL_GetError();
        running_ = false;
        return;
    }

    // Bilinear filtering for any texture we scale (video frames and
    // cover art). SDL defaults to nearest-neighbour, which gave the
    // output a hard "blocky" look compared to Apple-certified
    // receivers. Must be set before SDL_CreateRenderer to take effect.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
#if defined(_WIN32)
    // SDL otherwise omits WS_THICKFRAME for borderless windows, which
    // prevents Windows from resizing them despite the hit-test handles.
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
#endif

    auto preferences = live_settings_ ? live_settings_->ui_preferences() : ap::UiPreferences{};
    int initial_x = SDL_WINDOWPOS_CENTERED, initial_y = SDL_WINDOWPOS_CENTERED;
    int initial_w = kDefaultWinWidth, initial_h = kDefaultWinHeight;
    if (preferences.window_placement_valid) {
        initial_w = preferences.window_width;
        initial_h = preferences.window_height;
        SDL_Rect available{};
        bool found = false;
        for (int display = 0; display < SDL_GetNumVideoDisplays(); ++display) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayUsableBounds(display, &bounds) != 0) continue;
            // Require a reachable title bar, including on displays with negative coordinates.
            if (preferences.window_y >= bounds.y && preferences.window_y + kToolbarHeight <= bounds.y + bounds.h &&
                preferences.window_x + 100 <= bounds.x + bounds.w && preferences.window_x + initial_w - 100 >= bounds.x) {
                available = bounds; found = true; break;
            }
        }
        if (found || SDL_GetDisplayUsableBounds(0, &available) == 0) {
            initial_w = std::min(initial_w, available.w);
            initial_h = std::min(initial_h, available.h);
            initial_x = found ? std::clamp(preferences.window_x, available.x, available.x + available.w - initial_w)
                              : available.x + (available.w - initial_w) / 2;
            initial_y = found ? std::clamp(preferences.window_y, available.y, available.y + available.h - initial_h)
                              : available.y + (available.h - initial_h) / 2;
        }
    }
    SDL_Window*   window   = SDL_CreateWindow(
        title.c_str(), initial_x, initial_y, initial_w, initial_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS
            | (start_hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
    // Read the initial vsync preference from LiveSettings (default
    // true). Toggling at runtime is handled by SDL_RenderSetVSync
    // below in the render loop.
    const bool vsync_initial = live_settings_
        ? live_settings_->vsync_enabled.load(std::memory_order_relaxed)
        : true;
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (vsync_initial) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
#if defined(_WIN32)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
#endif
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, renderer_flags)
        : nullptr;
    bool vsync_active = vsync_initial;
    if (!window || !renderer) {
        LOG_ERROR << "SDL window/renderer create failed: " << SDL_GetError();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window)   SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        running_ = false;
        return;
    }
    LOG_INFO << "VideoRenderer window up (" << kDefaultWinWidth
             << 'x' << kDefaultWinHeight << ", resizable)";
    SDL_SetWindowMinimumSize(window, 960, 720);

    bool presentation_pending = start_hidden;
    auto reveal_window = [&]() {
        if (presentation_pending) {
            if (live_settings_ && live_settings_->fullscreen.load())
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            else if ((!live_settings_ || !live_settings_->chromeless.load()) && preferences.window_maximized)
                SDL_MaximizeWindow(window);
            presentation_pending = false;
        }
        SDL_ShowWindow(window);
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window);
        SDL_RaiseWindow(window);
    };
    GpuVideo gpu_video;
#if defined(_WIN32)
    SDL_SysWMinfo instance_window_info{};
    SDL_VERSION(&instance_window_info.version);
    if (SDL_GetWindowWMInfo(window, &instance_window_info)) {
        SetPropW(instance_window_info.info.win.window,
                 ap::platform::kInstanceWindowProperty, reinterpret_cast<HANDLE>(1));
        tray_available = desktop.start(instance_window_info.info.win.window,
            [&](bool show) {
                if (show) reveal_window();
                else SDL_HideWindow(window);
            }, [this] { closed_ = true; running_ = false; },
            [this](ap::airplay::MediaCommand command) {
                if (audio_control_handler_) audio_control_handler_(command, 0);
            });
    }
#endif
    gpu_video.init(renderer);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        av_buffer_unref(&gpu_device_);
        gpu_device_ = gpu_video.device_ref();
        gpu_ready_ = true;
        cv_.notify_all();
    }

    // Win11 Mica backdrop. No-op on pre-Win11 — caller sees a regular
    // opaque window chrome instead of the subtle acrylic blur.
    enable_mica_backdrop(window);

    // Dear ImGui on top of the SDL renderer. The overlay runs in this
    // same thread and GPU context — zero frame copy, no IPC.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    apply_overlay_theme();
    ImFont* heading_font = nullptr;
    ImFont* bold_font = nullptr;
    ImFont* small_font = nullptr;
    ImFont* page_font = nullptr;
    ImFont* receiver_font = nullptr;
#if defined(_WIN32)
    if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 18.0f);
        heading_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", 42.0f);
        bold_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", 18.0f);
        page_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", 28.0f);
        small_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f);
        receiver_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", 22.0f);
    }
#endif
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();
    if (!bold_font) bold_font = io.FontDefault;
    if (!small_font) small_font = io.FontDefault;
    if (!heading_font) heading_font = io.FontDefault;
    if (!page_font) page_font = bold_font;
    if (!receiver_font) receiver_font = bold_font;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SDL_Texture* video_tex = nullptr;
    int          video_tex_w = 0, video_tex_h = 0;
    Uint32       video_tex_fmt = 0;  // SDL_PIXELFORMAT_IYUV or SDL_PIXELFORMAT_NV12

    SDL_Texture* cover_tex = nullptr;
    int          cover_tex_w = 0, cover_tex_h = 0;

    // Text overlay: load SDL_ttf + a multi-font chain (Latin primary +
    // CJK fallbacks) at two sizes so make_text can cover any iOS track
    // name, including Japanese / Chinese / Korean.
    std::vector<TTF_Font*> fonts_big;
    std::vector<TTF_Font*> fonts_small;
    if (TTF_Init() == 0) {
        fonts_big   = open_font_chain(32);
        fonts_small = open_font_chain(20);
        if (TTF_Font* semibold = TTF_OpenFont("C:/Windows/Fonts/seguisb.ttf",32)) {
            if (!fonts_big.empty()) { TTF_CloseFont(fonts_big.front()); fonts_big.front()=semibold; }
            else fonts_big.push_back(semibold);
        }
    } else {
        LOG_WARN << "TTF_Init failed: " << TTF_GetError();
    }
    SDL_Texture* title_tex     = nullptr; int title_w     = 0, title_h     = 0;
    SDL_Texture* artist_tex    = nullptr; int artist_w    = 0, artist_h    = 0;
    SDL_Texture* album_tex     = nullptr; int album_w     = 0, album_h     = 0;
    SDL_Texture* idle_name_tex = nullptr; int idle_name_w = 0, idle_name_h = 0;
    SDL_Texture* idle_ip_tex   = nullptr; int idle_ip_w   = 0, idle_ip_h   = 0;
    SDL_Texture* idle_msg_tex  = nullptr; int idle_msg_w  = 0, idle_msg_h  = 0;

    auto make_text = [&](const std::vector<TTF_Font*>& fonts,
                         const std::string& s, SDL_Color c,
                         SDL_Texture*& tex, int& w, int& h) {
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        w = h = 0;
        if (fonts.empty() || s.empty()) return;
        SDL_Surface* surf = render_multi_font(fonts, s, c);
        if (!surf) return;
        tex = SDL_CreateTextureFromSurface(renderer, surf);
        w = surf->w; h = surf->h;
        SDL_FreeSurface(surf);
        // Default texture blend mode is NONE, which ignores alpha and renders
        // the surface as a solid opaque rectangle (every transparent pixel of
        // the glyph background fills the window with white). Force BLEND so
        // only the anti-aliased glyph pixels are drawn.
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    };

    auto last_video_frame_time = std::chrono::steady_clock::now()
                                 - std::chrono::seconds(10);

    // Persistent UI toggles (struct lifted to file scope above so
    // the SDL hit-test callback can access it). Defaults match the
    // design mockup (chrome visible). Chromeless mode hides toolbar
    // + sidebar + options + status bar so the AirPlay video fills
    // the whole window; unlike fullscreen this stays a regular
    // resizable window. F11 / double-click toggle real fullscreen,
    // H toggles chromeless, ESC peels back layers.
    UiState ui;
    ui.diagnostics = preferences.diagnostics;
    if (live_settings_)
        ui.chromeless = live_settings_->chromeless.load(std::memory_order_relaxed);
    if (SDL_SetWindowHitTest(window, hit_test_cb, &ui) != 0)
        LOG_WARN << "Window resize hit-test unavailable: " << SDL_GetError();
    if (ui.chromeless) SDL_SetWindowMinimumSize(window, 160, 120);

    std::array<char, 1024> recording_dir{};
    std::array<char, 51> receiver_name_buf{};
    if (live_settings_)
        std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s",
                      live_settings_->receiver_name().c_str());
    VideoRecorder::Options recording_options;
    recording_options.codec = static_cast<VideoRecorder::VideoCodec>(preferences.recording_codec);
    recording_options.encoder = static_cast<VideoRecorder::EncoderMode>(preferences.recording_encoder);
    recording_options.bitrate_mbps = preferences.recording_bitrate;
    const std::string default_recording_dir = VideoRecorder::default_directory();
    std::snprintf(recording_dir.data(), recording_dir.size(), "%s",
                  preferences.recording_directory.empty() ? default_recording_dir.c_str() : preferences.recording_directory.c_str());
    auto toggle_recording = [&]() {
        if (!recorder_) return;
        if (recorder_->status().recording) recorder_->stop();
        else recorder_->start(recording_dir.data(), recording_options);
    };

    bool fullscreen = live_settings_
        ? live_settings_->fullscreen.load(std::memory_order_relaxed) : false;
    auto toggle_fullscreen = [&]() {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(window,
            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
        if (live_settings_)
            live_settings_->fullscreen.store(fullscreen, std::memory_order_relaxed);
    };
    if (fullscreen && !start_hidden) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(SDL_DISABLE);
    } else if (!start_hidden && !ui.chromeless && preferences.window_maximized) {
        SDL_MaximizeWindow(window);
    }

    auto publish_preferences = [&]() {
        auto current = preferences;
        current.diagnostics = ui.diagnostics;
        current.recording_directory = recording_dir.data();
        current.recording_codec = static_cast<int>(recording_options.codec);
        current.recording_encoder = static_cast<int>(recording_options.encoder);
        current.recording_bitrate = recording_options.bitrate_mbps;
        const Uint32 flags = SDL_GetWindowFlags(window);
        if (!presentation_pending && !fullscreen && !ui.chromeless && !(flags & SDL_WINDOW_MINIMIZED)) {
            current.window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            if (!current.window_maximized) {
                SDL_GetWindowPosition(window, &current.window_x, &current.window_y);
                SDL_GetWindowSize(window, &current.window_width, &current.window_height);
                current.window_placement_valid = true;
            }
        }
        if (!(current == preferences)) {
            preferences = current;
            if (live_settings_) live_settings_->set_ui_preferences(current);
        }
    };

    // Chromeless aspect-lock state. Triggers a re-snap on:
    //   - chromeless toggled on
    //   - video frame dimensions change (iPhone rotation, e.g.
    //     portrait <-> landscape mid-mirror)
    //   - user resize via the hit-test edges
    // saved_w/h capture the chrome-mode window size before entering chromeless so we can
    // restore it on exit (otherwise the menus get crushed against a
    // tall narrow video-aspect window).
    bool prev_chromeless    = false;
    int  prev_video_w       = 0;
    int  prev_video_h       = 0;
    int  saved_window_w     = 0;
    int  saved_window_h     = 0;
    bool saved_window_maximized = false;
    bool custom_resolution = false;
    double settings_reset_notice_until = 0;

    auto reset_all_settings = [&]() {
        if (!live_settings_) return;
        const ap::PersistentSettings defaults;
        int mirror_w = 2560, mirror_h = 1440;
        SDL_DisplayMode display{};
        if (SDL_GetCurrentDisplayMode(0, &display) == 0 &&
            display.w >= 320 && display.h >= 240 && display.w <= 7680 && display.h <= 4320) {
            mirror_w = display.w;
            mirror_h = display.h;
        }
        live_settings_->set_receiver_name(defaults.receiver_name);
        live_settings_->mirror_width.store(mirror_w);
        live_settings_->mirror_height.store(mirror_h);
        live_settings_->hevc_enabled.store(defaults.hevc_enabled);
        live_settings_->max_fps.store(defaults.max_fps);
        live_settings_->refresh_rate.store(defaults.refresh_rate);
        live_settings_->mirror_hwaccel.store(defaults.mirror_hwaccel);
        live_settings_->vsync_enabled.store(defaults.vsync_enabled);
        live_settings_->video_rotation.store(defaults.video_rotation);
        live_settings_->fullscreen.store(defaults.fullscreen);
        live_settings_->chromeless.store(defaults.chromeless);
        preferences = defaults.ui;
        live_settings_->set_ui_preferences(preferences);
        std::snprintf(receiver_name_buf.data(),receiver_name_buf.size(),"%s",defaults.receiver_name.c_str());
        std::snprintf(recording_dir.data(),recording_dir.size(),"%s",default_recording_dir.c_str());
        recording_options = VideoRecorder::Options{};
        ui.diagnostics = defaults.ui.diagnostics;
        ui.chromeless = false;
        custom_resolution = false;
        prev_chromeless = false;
        saved_window_w = saved_window_h = 0;
        fullscreen = false;
        SDL_SetWindowFullscreen(window,0);
        SDL_ShowCursor(SDL_ENABLE);
        SDL_RestoreWindow(window);
        SDL_SetWindowMinimumSize(window,960,720);
        saved_window_maximized = false;
        SDL_SetWindowSize(window,defaults.ui.window_width,defaults.ui.window_height);
        SDL_SetWindowPosition(window,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
        publish_preferences();
        settings_reset_notice_until = ImGui::GetTime()+4;
    };

    // iOS sends upright frames with updated dimensions when it rotates.
    // Auto must not apply another 90 degrees to an already upright image.
    auto rotation_degrees = [&]() {
        const int mode = live_settings_ ? live_settings_->video_rotation.load() : -1;
        return mode >= 0 && mode <= 3 ? mode * 90 : 0;
    };
    auto display_width = [&]() {
        return rotation_degrees() % 180 ? video_tex_h : video_tex_w;
    };
    auto display_height = [&]() {
        return rotation_degrees() % 180 ? video_tex_w : video_tex_h;
    };

    // "Shrink to fit" snap: collapse the dimension that exceeds the
    // target aspect, leave the other one alone. Used on chromeless
    // entry so the new window fits inside the previous chrome window's
    // bounds. Manual resizing stays free; fit_inside preserves video aspect.
    auto snap_to_video_aspect = [&]() {
        if (!ui.chromeless || fullscreen) return;
        if (video_tex_w <= 0 || video_tex_h <= 0) return;
        int wnow = 0, hnow = 0;
        SDL_GetWindowSize(window, &wnow, &hnow);
        if (wnow <= 0 || hnow <= 0) return;
        const float vid_a = float(display_width()) / float(display_height());
        const float win_a = float(wnow) / float(hnow);
        int snap_w = wnow, snap_h = hnow;
        if (win_a > vid_a) {
            snap_w = static_cast<int>(hnow * vid_a + 0.5f);
        } else {
            snap_h = static_cast<int>(wnow / vid_a + 0.5f);
        }
        if (std::abs(snap_w - wnow) > 1 || std::abs(snap_h - hnow) > 1) {
            SDL_SetWindowSize(window, snap_w, snap_h);
        }
    };

    // "Preserve area" snap: keep the longer current dimension as the
    // new long axis of the target aspect. Used on iPhone rotation —
    // the naive shrink-to-fit halves the visible area each rotation
    // because portrait/landscape inverts the aspect, and the dim
    // that was just snapped becomes the one to shrink again. Long-
    // axis preservation effectively swaps width/height across the
    // rotation so screen real estate stays roughly constant.
    auto snap_preserving_area = [&]() {
        if (!ui.chromeless || fullscreen) return;
        if (video_tex_w <= 0 || video_tex_h <= 0) return;
        int wnow = 0, hnow = 0;
        SDL_GetWindowSize(window, &wnow, &hnow);
        if (wnow <= 0 || hnow <= 0) return;
        const float vid_a = float(display_width()) / float(display_height());
        const int long_dim = std::max(wnow, hnow);
        int new_w, new_h;
        if (vid_a >= 1.0f) {        // landscape video
            new_w = long_dim;
            new_h = static_cast<int>(long_dim / vid_a + 0.5f);
        } else {                     // portrait video
            new_h = long_dim;
            new_w = static_cast<int>(long_dim * vid_a + 0.5f);
        }
        if (std::abs(new_w - wnow) > 1 || std::abs(new_h - hnow) > 1) {
            SDL_SetWindowSize(window, new_w, new_h);
        }
    };

    if (start_hidden) {
        if (tray_available) SDL_HideWindow(window);
        else reveal_window();
    }
    ap::platform::FirewallMonitor firewall_monitor;
    while (running_.load()) {
        if (show_requested_.exchange(false)) {
            reveal_window();
#if defined(_WIN32)
            if (instance_window_info.info.win.window)
                SetForegroundWindow(instance_window_info.info.win.window);
#endif
        }
        // Re-snap the window aspect on either of:
        //   - chromeless flipping on (snap once on entry)
        //   - video dimensions changing while chromeless (iPhone
        //     rotation between portrait/landscape, resolution change
        //     on next mirror connect, etc.)
        // On the way OUT of chromeless, restore the chrome-mode size
        // we captured on entry — otherwise the panels render into a
        // window the size of the video, which crushes the layout.
        const bool video_changed =
            ui.chromeless &&
            (display_width() != prev_video_w || display_height() != prev_video_h);
        if (ui.chromeless != prev_chromeless) {
            SDL_SetWindowMinimumSize(window, ui.chromeless ? 160 : 960, ui.chromeless ? 120 : 720);
            if (ui.chromeless && !fullscreen) {
                saved_window_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
                if (saved_window_maximized) SDL_RestoreWindow(window);
                SDL_GetWindowSize(window,
                                  &saved_window_w, &saved_window_h);
                snap_to_video_aspect();
            } else if (!ui.chromeless &&
                       saved_window_w > 0 && saved_window_h > 0) {
                SDL_SetWindowSize(window,
                                  saved_window_w, saved_window_h);
                if (saved_window_maximized) SDL_MaximizeWindow(window);
            }
            prev_chromeless = ui.chromeless;
            if (live_settings_)
                live_settings_->chromeless.store(ui.chromeless, std::memory_order_relaxed);
            prev_video_w    = display_width();
            prev_video_h    = display_height();
        } else if (video_changed) {
            // iPhone rotation flips portrait <-> landscape: use the
            // area-preserving snap so the window swaps dims rather
            // than shrinking by half each rotation.
            snap_preserving_area();
            prev_video_w = display_width();
            prev_video_h = display_height();
        }

        // Honor live VSYNC toggle: SDL 2.0.18+ supports flipping
        // VSYNC at runtime without rebuilding the renderer. The
        // call is cheap enough to issue every frame; we still
        // gate it on a state delta so we don't spam the driver.
        if (live_settings_) {
            const bool want_vsync =
                live_settings_->vsync_enabled.load(std::memory_order_relaxed);
            if (want_vsync != vsync_active) {
                if (SDL_RenderSetVSync(renderer, want_vsync ? 1 : 0) == 0) {
                    LOG_INFO << "VideoRenderer VSYNC "
                             << (want_vsync ? "enabled" : "disabled");
                    vsync_active = want_vsync;
                }
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Always feed ImGui first so focus / hover / capture work.
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                if (tray_available) { SDL_HideWindow(window); continue; }
                LOG_INFO << "VideoRenderer received SDL_QUIT";
                closed_  = true;
                running_ = false;
                break;
            }
            const ImGuiIO& io_poll = ImGui::GetIO();
            // A focused panel captures navigation keys even when no field is
            // being edited. Keep receiver shortcuts global in that case;
            // text fields retain letters and modified key combinations.
            const bool receiver_shortcut = e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_f || e.key.keysym.sym == SDLK_F11 ||
                 e.key.keysym.sym == SDLK_h || e.key.keysym.sym == SDLK_r);
            const bool allow_keyboard = receiver_shortcut
                ? !io_poll.WantTextInput &&
                  !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
                : !io_poll.WantCaptureKeyboard;
            if (e.type == SDL_KEYDOWN && !e.key.repeat && allow_keyboard) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        // ESC peels back layers in order:
                        // fullscreen first, then chromeless, then quit.
                        if (fullscreen)         toggle_fullscreen();
                        else if (ui.chromeless) ui.chromeless = false;
                        else if (ui.settings_open) ui.settings_open = false;
                        else if (tray_available) SDL_HideWindow(window);
                        else { closed_ = true; running_ = false; }
                        break;
                    case SDLK_f:
                    case SDLK_F11:
                        toggle_fullscreen();
                        break;
                    case SDLK_h:
                        ui.chromeless = !ui.chromeless;
                        break;
                    case SDLK_r:
                        toggle_recording();
                        break;
                    default: break;
                }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN &&
                e.button.button == SDL_BUTTON_LEFT &&
                e.button.clicks == 2 &&
                !io_poll.WantCaptureMouse) {
                toggle_fullscreen();
            }
        }
        if (!running_.load()) break;

        // --- Consume pending video frame -----------------------------
        // Two producers feed the slot:
        //   - push_avframe (mirror, refcounted AVFrame, zero-copy)
        //   - push_frame / push_frame_nv12 (HLS, plane buffers)
        // The AVFrame slot wins when present; legacy slot used by
        // HLS players that haven't been migrated.
        bool have_avf   = false;
        bool have_frame = false;
        bool have_nv12  = false;
        std::vector<unsigned char> y, u, v, uv;
        DecodedFrame avf_local;
        int w = 0, h = 0;
        int64_t frame_origin_ns_local = 0;
        DeviceInfo pacing_device;
        const bool audio_only = active_device_snapshot(pacing_device) && pacing_device.kind == "Audio";
        // Audio controls/progress need fewer redraws than mirrored video.
        // A newly queued video frame still wakes this wait immediately.
        const auto refresh_wait = std::chrono::milliseconds(audio_only ? 33 : 16);
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, refresh_wait,
                         [this] {
                             return has_avframe_ || has_frame_ ||
                                    cover_dirty_ || !running_.load();
                         });
            if (has_avframe_) {
                avf_local       = std::move(slot_avframe_);
                w               = frame_w_;
                h               = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_avframe_    = false;
                have_avf        = true;
            } else if (has_frame_) {
                have_nv12 = frame_is_nv12_;
                if (have_nv12) {
                    y  = y_buf_;
                    uv = uv_buf_;
                } else {
                    y = y_buf_; u = u_buf_; v = v_buf_;
                }
                w = frame_w_; h = frame_h_;
                frame_origin_ns_local = frame_origin_ns_;
                has_frame_ = false;
                have_frame = true;
            }
        }

        // Decide the SDL pixel format to use; depends on which path
        // delivered the frame.
        Uint32 desired_fmt = SDL_PIXELFORMAT_IYUV;
        AVFrame* af = avf_local.frame;
        if (have_avf && af) {
            if (af->format == AV_PIX_FMT_D3D11) {
                desired_fmt = SDL_PIXELFORMAT_ARGB8888;
            } else if (af->format == AV_PIX_FMT_NV12) {
                desired_fmt = SDL_PIXELFORMAT_NV12;
            } else {
                desired_fmt = SDL_PIXELFORMAT_IYUV;
            }
        } else if (have_frame) {
            desired_fmt = have_nv12 ? SDL_PIXELFORMAT_NV12
                                    : SDL_PIXELFORMAT_IYUV;
        }

        if (have_avf || have_frame) {
            if (!video_tex || video_tex_w != w || video_tex_h != h ||
                video_tex_fmt != desired_fmt) {
                gpu_video.reset_target();
                if (video_tex) SDL_DestroyTexture(video_tex);
                video_tex = SDL_CreateTexture(renderer,
                    desired_fmt, desired_fmt == SDL_PIXELFORMAT_ARGB8888
                        ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING, w, h);
                video_tex_w   = w;
                video_tex_h   = h;
                video_tex_fmt = desired_fmt;
                LOG_INFO << "VideoRenderer texture ("
                         << (desired_fmt == SDL_PIXELFORMAT_NV12 ? "NV12"
                             : desired_fmt == SDL_PIXELFORMAT_ARGB8888 ? "GPU BGRA"
                                                                     : "IYUV")
                         << ") created " << w << 'x' << h;
            }
            if (video_tex) {
                if (have_avf && af) {
                    // Direct upload from the refcounted AVFrame —
                    // the planes never crossed our buffers, just
                    // the FFmpeg pool → SDL staging texture.
                    if (af->format == AV_PIX_FMT_D3D11) {
                        if (!gpu_video.convert(renderer, video_tex, af)) {
                            LOG_ERROR << "D3D11 surface conversion failed";
                            gpu_video.reset_target();
                            SDL_DestroyTexture(video_tex);
                            video_tex = nullptr;
                        }
                    } else if (af->format == AV_PIX_FMT_NV12) {
                        SDL_UpdateNVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1]);
                    } else {
                        // YUV420P / YUVJ420P (full-range) — SDL
                        // treats both as IYUV, slight color-range
                        // bias on YUVJ but indistinguishable for
                        // mirror.
                        SDL_UpdateYUVTexture(video_tex, nullptr,
                            af->data[0], af->linesize[0],
                            af->data[1], af->linesize[1],
                            af->data[2], af->linesize[2]);
                    }
                } else if (have_nv12) {
                    SDL_UpdateNVTexture(video_tex, nullptr,
                        y.data(), w, uv.data(), w);
                } else {
                    SDL_UpdateYUVTexture(video_tex, nullptr,
                        y.data(), w, u.data(), w / 2, v.data(), w / 2);
                }
            }
            last_video_frame_time = std::chrono::steady_clock::now();
        }
        // avf_local destructor below releases our FFmpeg ref back
        // to the pool the moment SDL's UpdateTexture has issued
        // its CopySubresourceRegion to the GPU.

        // --- Drop the cover if clear_session was called --------------
        if (clear_cover_requested_.exchange(false, std::memory_order_relaxed)) {
            desktop.artwork(nullptr, 0);
            if (cover_tex) {
                SDL_DestroyTexture(cover_tex);
                cover_tex   = nullptr;
                cover_tex_w = cover_tex_h = 0;
            }
            std::lock_guard<std::mutex> lock(cover_mtx_);
            cover_jpeg_.clear();
            cover_dirty_ = false;
        }

        // --- Consume pending cover-art JPEG --------------------------
        std::vector<unsigned char> pending_cover;
        {
            std::lock_guard<std::mutex> lock(cover_mtx_);
            if (cover_dirty_) {
                pending_cover = std::move(cover_jpeg_);
                cover_jpeg_.clear();
                cover_dirty_ = false;
            }
        }
        // --- Consume pending metadata + idle info --------------------
        std::string t_title, t_artist, t_album, t_idle_name, t_idle_ip;
        bool meta_changed = false;
        bool idle_changed = false;
        {
            std::lock_guard<std::mutex> lock(meta_mtx_);
            if (meta_dirty_) {
                t_title      = meta_title_;
                t_artist     = meta_artist_;
                t_album      = meta_album_;
                meta_dirty_  = false;
                meta_changed = true;
            }
            if (idle_dirty_) {
                t_idle_name  = idle_name_;
                t_idle_ip    = idle_ip_;
                idle_dirty_  = false;
                idle_changed = true;
            }
        }
        if (!pending_cover.empty()) desktop.artwork(pending_cover.data(), pending_cover.size());
        if (meta_changed) desktop.metadata(t_title, t_artist, t_album);
        if (idle_changed) {
            make_text(fonts_big,   t_idle_name,
                      SDL_Color{255, 255, 255, 255},
                      idle_name_tex, idle_name_w, idle_name_h);
            make_text(fonts_small, t_idle_ip,
                      SDL_Color{180, 180, 180, 255},
                      idle_ip_tex,   idle_ip_w,   idle_ip_h);
            make_text(fonts_small, "Waiting for AirPlay",
                      SDL_Color{150, 150, 150, 255},
                      idle_msg_tex,  idle_msg_w,  idle_msg_h);
        }
        if (meta_changed) {
            make_text(fonts_big,   t_title,  SDL_Color{255, 255, 255, 255},
                      title_tex,  title_w,  title_h);
            make_text(fonts_small, t_artist, SDL_Color{220, 220, 220, 255},
                      artist_tex, artist_w, artist_h);
            make_text(fonts_small, t_album,  SDL_Color{180, 180, 180, 255},
                      album_tex,  album_w,  album_h);
            LOG_INFO << "VideoRenderer metadata textures updated";
        }

        if (!pending_cover.empty()) {
            int cw = 0, ch = 0;
            std::vector<uint8_t> rgb;
            if (decode_cover_art(pending_cover.data(), pending_cover.size(),
                                   cw, ch, rgb)) {
                if (!cover_tex || cover_tex_w != cw || cover_tex_h != ch) {
                    if (cover_tex) SDL_DestroyTexture(cover_tex);
                    cover_tex = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, cw, ch);
                    cover_tex_w = cw; cover_tex_h = ch;
                }
                if (cover_tex) {
                    SDL_UpdateTexture(cover_tex, nullptr, rgb.data(), cw * 3);
                    LOG_INFO << "VideoRenderer cover art updated ("
                             << cw << 'x' << ch << ')';
                }
            } else {
                LOG_WARN << "VideoRenderer: JPEG decode failed ("
                         << pending_cover.size() << "B)";
            }
        }

        // --- Decide which texture to show ----------------------------
        // Video takes priority if we got a recent frame. iOS stops
        // emitting H.264 frames whenever the mirror content is static
        // (menu screens, paused video, idle home screen) — the encoder
        // is too efficient to bother re-sending identical frames. We
        // therefore keep the last video texture on screen as long as
        // a device is paired, regardless of the 500 ms freshness
        // window. Only on TEARDOWN (clear_active_device) does the
        // texture stop showing and we fall back to idle.
        const auto now = std::chrono::steady_clock::now();
        const bool video_fresh =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_video_frame_time).count() < 500;
        const bool playing_now = playing_.load(std::memory_order_relaxed);
        DeviceInfo active_dev;
        const bool has_active_dev = active_device_snapshot(active_dev);
        const uint32_t media_total = progress_total_ms_.load();
        uint64_t media_elapsed = progress_elapsed_ms_.load();
        const int64_t media_pushed = progress_pushed_at_ns_.load();
        const auto media_now = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        if (playing_now && media_pushed > 0 && media_now > media_pushed)
            media_elapsed += (media_now - media_pushed) / 1000000;
        desktop.publish(has_active_dev, playing_now, audio_control_available_.load(),
            static_cast<uint32_t>(std::min<uint64_t>(media_elapsed, media_total)), media_total);
        const bool show_video =
            video_tex && active_dev.kind != "Audio" && (video_fresh || has_active_dev || !playing_now);

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
        const int full_w = win_w;
        const int full_h = win_h;

        // Layout: top toolbar, sidebar (left), options panel (right),
        // status bar (bottom). Two ways to drop the chrome:
        //   - fullscreen: SDL_WINDOW_FULLSCREEN_DESKTOP, no chrome
        //   - chromeless: regular resizable window, no chrome
        // Both collapse all insets to 0 so the video stage covers the
        // whole window, and both skip the ImGui panel block below.
        const bool no_chrome = fullscreen || ui.chromeless;
        const auto firewall=firewall_monitor.status();
        const bool firewall_notice=firewall.state!=ap::platform::FirewallState::Checking &&
            firewall.state!=ap::platform::FirewallState::Allowed;
        const int firewall_offset=firewall_notice ? (firewall.message.empty()?124:150) : 0;
        const int options_w     = 0; // Settings overlay the content, as in the approved design.
        const int chrome_top    = no_chrome ? 0 : kToolbarHeight;
        const int diagnostics_columns = std::clamp((full_w - 20) / 300, 1, 4);
        const int diagnostics_rows = (9 + diagnostics_columns - 1) / diagnostics_columns;
        const int chrome_bottom = no_chrome || !ui.diagnostics
            ? 0 : diagnostics_rows * kStatusLineHeight + 8;
        const int chrome_left   = no_chrome ? 0 : kSidebarWidth;
        const int stage_x = chrome_left;
        const int stage_y = chrome_top;
        const int stage_w = std::max(0, full_w - chrome_left - options_w);
        const int stage_h = std::max(0, full_h - chrome_top - chrome_bottom);

        // Override win_w/win_h locally so the existing video-fit and
        // idle-text centring math operates inside the stage rect
        // rather than the whole window. Restore after the SDL drawing
        // pass, before the ImGui pass references full window size.
        win_w = stage_w;
        win_h = stage_h;

        // Clear the whole window first (background under all panels),
        // then clip subsequent SDL draws to the stage rect via
        // viewport — the ImGui panels will paint over the cleared
        // sides afterwards.
        SDL_SetRenderDrawColor(renderer, no_chrome ? 0 : 245, no_chrome ? 0 : 246, no_chrome ? 0 : 244, 255);
        SDL_RenderClear(renderer);
        const bool framed_video = !no_chrome && show_video;
        const SDL_Rect stage_rect{stage_x + (framed_video ? 42 : 0), stage_y + (framed_video ? 96 + firewall_offset : 0),
                                  std::max(1, stage_w - (framed_video ? 84 : 0)), std::max(1, stage_h - (framed_video ? 156 + firewall_offset : 0))};
        win_w = stage_rect.w;
        win_h = stage_rect.h;
        SDL_RenderSetViewport(renderer, &stage_rect);

        auto draw_pause_badge = [&](const SDL_Rect& dst) {
            const int badge = std::min(dst.w, dst.h) / 6;
            const int cx    = dst.x + dst.w / 2;
            const int cy    = dst.y + dst.h / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
            SDL_Rect bg{cx - badge, cy - badge, badge * 2, badge * 2};
            SDL_RenderFillRect(renderer, &bg);
            const int bw = badge / 3;
            const int bh = badge;
            const int gap = badge / 3;
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 235);
            SDL_Rect b1{cx - gap - bw, cy - bh / 2, bw, bh};
            SDL_Rect b2{cx + gap,      cy - bh / 2, bw, bh};
            SDL_RenderFillRect(renderer, &b1);
            SDL_RenderFillRect(renderer, &b2);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        };

        // Suppress the pause badge entirely on Screen Mirroring
        // sessions: there, "no audio" doesn't mean "playback paused"
        // (it just means whatever is on the iPhone screen has no
        // soundtrack — a menu, the home screen, a static page). The
        // badge made sense in audio-only / HLS playback flows where
        // the user actually paused content. In mirror it just sat
        // there until the user pressed pause+play to clear it.
        const bool show_pause_badge =
            !playing_now &&
            (!has_active_dev || active_dev.kind != "Mirror");

        if (show_video) {
            SDL_SetRenderDrawColor(renderer, 23, 30, 34, 255);
            SDL_RenderFillRect(renderer, nullptr);
            const int angle = rotation_degrees();
            SDL_Rect dst = fit_inside(display_width(), display_height(), win_w, win_h);
            // SDL rotates the destination rectangle around its centre. Swap
            // its unrotated extents so the final bounds fit the stage exactly.
            SDL_Rect copy_dst = dst;
            if (angle % 180) {
                copy_dst.w = dst.h;
                copy_dst.h = dst.w;
                copy_dst.x = dst.x + (dst.w - copy_dst.w) / 2;
                copy_dst.y = dst.y + (dst.h - copy_dst.h) / 2;
            }
            SDL_RenderCopyEx(renderer, video_tex, nullptr, &copy_dst,
                             angle, nullptr, SDL_FLIP_NONE);
            if (show_pause_badge) {
                draw_pause_badge(dst);
            }
        } else if (cover_tex && no_chrome) {
            // Cover art centered in the top 70% of the window; the bottom
            // strip holds the metadata text (title / artist / album).
            const int strip_h = std::min(win_h, std::max(win_h / 4, 230));
            const int top_h   = win_h - strip_h;
            SDL_Rect dst = fit_inside(cover_tex_w, cover_tex_h, win_w, top_h);
            SDL_RenderCopy(renderer, cover_tex, nullptr, &dst);

            // Pause indicator: two white bars on a semi-transparent black
            // square, centered on the cover. Only while iOS reports
            // rate: 0 via text/parameters (or POST /rate?value=0).
            if (show_pause_badge) {
                draw_pause_badge(dst);
            }

            // Text lines stacked left-aligned with a small margin.
            const int pad_x = 20;
            int       y_pos = top_h + 10;
            auto draw_text = [&](SDL_Texture* tex, int tw, int th) {
                if (!tex) return;
                int max_w = win_w - 2 * pad_x;
                int w     = std::min(tw, max_w);
                SDL_Rect r{pad_x, y_pos, w, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
                y_pos += th + 4;
            };
            draw_text(title_tex,  title_w,  title_h);
            draw_text(artist_tex, artist_w, artist_h);
            draw_text(album_tex,  album_w,  album_h);


        } else if (no_chrome) {
            // Idle: no mirror frame, no cover — show device info.
            auto draw_centered = [&](SDL_Texture* tex, int tw, int th, int y) {
                if (!tex) return;
                SDL_Rect r{(win_w - tw) / 2, y, tw, th};
                SDL_RenderCopy(renderer, tex, nullptr, &r);
            };
            const int total_h = idle_name_h + idle_ip_h + idle_msg_h + 24;
            int y_cursor = (win_h - total_h) / 2;
            draw_centered(idle_name_tex, idle_name_w, idle_name_h, y_cursor);
            y_cursor += idle_name_h + 12;
            draw_centered(idle_ip_tex,   idle_ip_w,   idle_ip_h,   y_cursor);
            y_cursor += idle_ip_h + 12;
            draw_centered(idle_msg_tex,  idle_msg_w,  idle_msg_h,  y_cursor);
        }

        // Restore full-window viewport so ImGui can draw the side panels
        // and status bar across the whole window. win_w / win_h were
        // overridden to stage dims for the SDL drawing pass; bring them
        // back to the full window size for the ImGui pass.
        SDL_RenderSetViewport(renderer, nullptr);
        win_w = full_w;
        win_h = full_h;

        // --- Dear ImGui overlay pass ---------------------------------
        // Runs every frame after the SDL_Render* video pass. The fixed
        // panels (sidebar / options / status) cover everything outside
        // the stage rect; the stage rect itself stays untouched so the
        // video / cover / idle texture is still visible.
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiWindowFlags kPanelFlags =
            ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoMove        |
            ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoTitleBar    |

            ImGuiWindowFlags_NoNavFocus  | ImGuiWindowFlags_NoSavedSettings;

        const float status_bar_h = static_cast<float>(chrome_bottom);
        const float toolbar_h    = static_cast<float>(kToolbarHeight);
        const float side_bar_w   = static_cast<float>(kSidebarWidth);
        const float opts_bar_w   = static_cast<float>(std::min(kOptionsWidth, full_w / 2));
        const float fw           = static_cast<float>(full_w);
        const float fh           = static_cast<float>(full_h);

        namespace ui_draw = receiver_ui;
        auto draw_audio_controls = [&]() {
            const bool ready = audio_control_available_.load();
            const bool pending = audio_command_pending_.load();
            const uint32_t total = progress_total_ms_.load();
            uint64_t elapsed = progress_elapsed_ms_.load();
            const int64_t pushed = progress_pushed_at_ns_.load();
            if (playing_now && pushed > 0) {
                const auto clock_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                if (clock_ns > pushed) elapsed += (clock_ns - pushed) / 1000000;
            }
            elapsed = total > 0 ? std::min<uint64_t>(elapsed, total) : 0;
            const float progress = total > 0 ? static_cast<float>(elapsed) / total : 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(p,ImVec2(p.x+width,p.y+3),IM_COL32(225,229,223,255),2);
            if (progress > 0) draw->AddRectFilled(p,ImVec2(p.x+width*progress,p.y+3),IM_COL32(87,106,89,255),2);
            ImGui::Dummy(ImVec2(width,5));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback position (read-only)");
            ImGui::PushFont(small_font);
            ImGui::TextDisabled("%u:%02u",static_cast<unsigned>(elapsed/60000),static_cast<unsigned>((elapsed/1000)%60));
            char duration[32]; std::snprintf(duration,sizeof(duration),"%u:%02u",total/60000,(total/1000)%60);
            ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x-ImGui::CalcTextSize(duration).x);
            ImGui::TextDisabled("%s",duration);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0,5));
            auto command = [&](ap::airplay::MediaCommand action) {
                audio_command_failed_.store(false); audio_command_pending_.store(true); audio_control_handler_(action,0);
            };
            const ImVec2 row = ImGui::GetCursorPos();
            ImGui::BeginDisabled(!ready || pending);
            ImGui::SetCursorPos(ImVec2(row.x,row.y+10.5f));
            if (ui_draw::icon_button("##previous",ui_draw::Icon::Previous,"Previous track",32)) command(ap::airplay::MediaCommand::Previous);
            ImGui::SetCursorPos(ImVec2(row.x+51,row.y));
            if (ui_draw::icon_button("##play",playing_now ? ui_draw::Icon::Pause : ui_draw::Icon::Play,playing_now ? "Pause" : "Play",53,true)) command(ap::airplay::MediaCommand::PlayPause);
            ImGui::SetCursorPos(ImVec2(row.x+123,row.y+10.5f));
            if (ui_draw::icon_button("##next",ui_draw::Icon::Next,"Next track",32)) command(ap::airplay::MediaCommand::Next);
            ImGui::EndDisabled();
            ImGui::SetCursorPos(ImVec2(row.x,row.y+65));
            ImGui::PushFont(small_font);
            if (pending) ImGui::TextDisabled("Sending command...");
            else if (!ready) ImGui::TextDisabled("Remote control unavailable");
            else if (audio_command_failed_.load()) ImGui::TextDisabled("Command failed or unsupported");
            ImGui::PopFont();
        };

        // Fullscreen and chromeless both hide the entire chrome
        // (toolbar / sidebar / options / status bar). Fullscreen also
        // takes the whole screen via SDL_WINDOW_FULLSCREEN_DESKTOP;
        // chromeless keeps a regular resizable window with just the
        // video. F11/F/double-click toggles fullscreen, H toggles
        // chromeless, ESC peels back layers. The pulsing "Waiting
        // for AirPlay" ring stays drawn — it's part of the stage.
        if (!no_chrome) {
        if (has_active_dev && !ui.was_connected) ui.page = active_dev.kind == "Audio" ? 1 : 2;
        if (!has_active_dev && ui.was_connected) ui.page = 0;
        ui.was_connected = has_active_dev;
        const char* receiver = receiver_name_buf.data();
        const ImU32 ink = ui_draw::ink, muted = ui_draw::muted, line = ui_draw::line;
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(fw,toolbar_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20,0));
        ImGui::Begin("##titlebar",nullptr,kPanelFlags|ImGuiWindowFlags_NoScrollbar);
        auto* title_draw = ImGui::GetWindowDrawList();
        ui_draw::icon(title_draw,ui_draw::Icon::Cast,ImVec2(20,12),20,ui_draw::blue);
        title_draw->AddText(bold_font,16,ImVec2(50,12),ink,"AirPlay Windows");
        title_draw->AddLine(ImVec2(0,44),ImVec2(fw,44),line);
        ImGui::SetCursorPos(ImVec2(fw-144,0));
        auto window_control = [&](const char* id, ui_draw::Icon kind) {
            const ImVec2 pos=ImGui::GetCursorScreenPos();
            const bool click=ui_draw::surface(id,ImVec2(48,44));
            if (ImGui::IsItemHovered()) title_draw->AddRectFilled(pos,ImVec2(pos.x+48,pos.y+44),kind==ui_draw::Icon::Close ? IM_COL32(247,220,220,255) : IM_COL32(239,242,239,255));
            ui_draw::icon(title_draw,kind,ImVec2(pos.x+14,pos.y+12),20,muted);
            return click;
        };
        if (window_control("##minimize",ui_draw::Icon::Minimize)) SDL_MinimizeWindow(window);
        ImGui::SameLine(0,0);
        if (window_control("##maximize",ui_draw::Icon::Maximize)) {
            if (SDL_GetWindowFlags(window)&SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window); else SDL_MaximizeWindow(window);
        }
        ImGui::SameLine(0,0);
        if (window_control("##close",ui_draw::Icon::Close)) { SDL_Event event{}; event.type=SDL_QUIT; SDL_PushEvent(&event); }
        ImGui::End(); ImGui::PopStyleVar();

        ImGui::SetNextWindowPos(ImVec2(0,toolbar_h));
        ImGui::SetNextWindowSize(ImVec2(side_bar_w,fh-toolbar_h-status_bar_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,29));
        ImGui::Begin("##navigation",nullptr,kPanelFlags|ImGuiWindowFlags_NoScrollbar);
        auto* rail=ImGui::GetWindowDrawList();
        rail->AddLine(ImVec2(side_bar_w-1,toolbar_h),ImVec2(side_bar_w-1,fh-status_bar_h),line);
        ImGui::PushFont(small_font); ImGui::SetCursorPosX(26); ImGui::TextDisabled("YOUR SPACE"); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0,5));
        if (ui_draw::nav("##home","Home",ui_draw::Icon::Home,ui.page==0,162,ui.page==0?bold_font:io.FontDefault)) ui.page=0;
        if (ui_draw::nav("##music","Music",ui_draw::Icon::Music,ui.page==1,162,ui.page==1?bold_font:io.FontDefault)) ui.page=1;
        if (ui_draw::nav("##mirror","Screen mirroring",ui_draw::Icon::Screen,ui.page==2,162,ui.page==2?bold_font:io.FontDefault)) ui.page=2;
        ImGui::SetCursorPosY(fh-toolbar_h-status_bar_h-150);
        if (ui_draw::nav("##settings","Settings",ui_draw::Icon::Settings,ui.settings_open,162,io.FontDefault)) ui.settings_open=!ui.settings_open;
        ImGui::Separator(); ImGui::Dummy(ImVec2(0,3));
        ImGui::PushFont(small_font);
        ImVec2 p=ImGui::GetCursorScreenPos(); rail->AddCircleFilled(ImVec2(p.x+11,p.y+8),3.5f,IM_COL32(44,161,116,255));
        ImGui::SetCursorPosX(36); ImGui::TextDisabled(has_active_dev ? "Device connected" : "Receiver available");
        ImGui::SetCursorPosX(25); ImGui::PushTextWrapPos(side_bar_w-15); ImGui::TextUnformatted(receiver); ImGui::PopTextWrapPos();
        ImGui::PopFont(); ImGui::End(); ImGui::PopStyleVar();

        const bool music_view=ui.page==1 && has_active_dev && active_dev.kind=="Audio";
        const bool video_view=ui.page==2 && show_video;
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(stage_x),toolbar_h));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(stage_w)-14,static_cast<float>(stage_h)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(42,0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(245/255.f,246/255.f,244/255.f,1));
        ImGui::Begin("##content",nullptr,kPanelFlags|(video_view ? ImGuiWindowFlags_NoBackground : 0));
        auto* draw=ImGui::GetWindowDrawList();
        ImVec2 origin=ImGui::GetWindowPos();
        origin.y-=ImGui::GetScrollY();
        const float width=stage_w-84.f;
        auto text_at=[&](ImFont* font,float x,float y,ImU32 color,const char* text) { draw->AddText(font,font->FontSize,ImVec2(origin.x+x,origin.y+y),color,text); };
        auto center_text=[&](ImFont* font,float y,ImU32 color,const char* text) {
            const float tw=font->CalcTextSizeA(font->FontSize,10000,0,text).x;
            text_at(font,(stage_w-tw)/2,y,color,text);
        };
        auto decoding_note = [&](float bottom) {
            const bool gpu=live_settings_ && live_settings_->mirror_hwaccel.load();
            const char* note=gpu ? "GPU acceleration enabled.\nPerformance issues? Try CPU decoding in Settings."
                                 : "CPU decoding selected.\nGPU acceleration is available in Settings.";
            const float wrap=std::max(180.f,width-(video_view?506.f:0.f));
            const ImVec2 size=small_font->CalcTextSizeA(14,10000,wrap,note);
            draw->AddText(small_font,14,ImVec2(origin.x+stage_w-42-size.x,origin.y+bottom-size.y),
                          IM_COL32(145,154,147,255),note,nullptr,wrap);
        };
        if (music_view || video_view) {
            draw->AddRectFilled(ImVec2(origin.x+42,origin.y+30),ImVec2(origin.x+81,origin.y+69),IM_COL32(255,255,255,255),9);
            draw->AddRect(ImVec2(origin.x+42,origin.y+30),ImVec2(origin.x+81,origin.y+69),line,9);
            ui_draw::icon(draw,ui_draw::Icon::Phone,ImVec2(origin.x+51,origin.y+39),21,muted);
            text_at(bold_font,93,31,ink,active_dev.name.empty()?"Connected device":active_dev.name.c_str());
            draw->AddCircleFilled(ImVec2(origin.x+97,origin.y+60),3,IM_COL32(44,161,116,255));
            text_at(small_font,107,51,muted,music_view ? "Connected · Audio" : "Connected · Screen mirroring");
            if (firewall_notice) {
                const float badge_x=stage_w-300.f;
                draw->AddRectFilled(ImVec2(origin.x+badge_x,origin.y+36),ImVec2(origin.x+badge_x+125,origin.y+66),IM_COL32(255,247,227,255),15);
                draw->AddRect(ImVec2(origin.x+badge_x,origin.y+36),ImVec2(origin.x+badge_x+125,origin.y+66),IM_COL32(235,216,167,255),15);
                draw->AddCircleFilled(ImVec2(origin.x+badge_x+14,origin.y+51),3.5f,IM_COL32(183,135,41,255));
                text_at(small_font,badge_x+26,43,IM_COL32(123,87,30,255),"Check firewall");
            }
            ImGui::SetCursorPos(ImVec2(stage_w-150.f,32));
            if (ui_draw::text_button("Disconnect",ImVec2(108,34)) && disconnect_handler_) disconnect_handler_();
        } else {
            text_at(page_font,42,36,ink,ui.page==0 ? "Welcome home." : ui.page==1 ? "Your music on this PC." : "Your screen, live.");
            const float badge_x=stage_w-167.f;
            const bool firewall_attention=firewall_notice && !has_active_dev;
            draw->AddRectFilled(ImVec2(origin.x+badge_x,origin.y+36),ImVec2(origin.x+stage_w-42,origin.y+66),firewall_attention ? IM_COL32(255,247,227,255) : IM_COL32(233,243,236,255),15);
            draw->AddRect(ImVec2(origin.x+badge_x,origin.y+36),ImVec2(origin.x+stage_w-42,origin.y+66),firewall_attention ? IM_COL32(235,216,167,255) : IM_COL32(217,234,221,255),15);
            draw->AddCircleFilled(ImVec2(origin.x+badge_x+14,origin.y+51),3.5f,firewall_attention ? IM_COL32(183,135,41,255) : IM_COL32(44,161,116,255));
            text_at(small_font,badge_x+26,43,firewall_attention ? IM_COL32(123,87,30,255) : IM_COL32(55,117,87,255),has_active_dev ? "Connected" : firewall_attention ? "Check firewall" : "Ready to receive");
        }
            if (firewall_notice) {
                const bool can_allow=firewall.state==ap::platform::FirewallState::NeedsRule;
                const float banner_y=86, banner_h=firewall.message.empty()?106.f:132.f;
                const ImVec2 a(origin.x+42,origin.y+banner_y), b(origin.x+stage_w-42,origin.y+banner_y+banner_h);
                draw->AddRectFilled(a,b,IM_COL32(255,247,227,255),10);
                draw->AddRect(a,b,IM_COL32(235,216,167,255),10);
                const char* banner_title=can_allow ? "Allow AirPlay through Windows Firewall" :
                    firewall.state==ap::platform::FirewallState::Managed ? "Windows Firewall blocks incoming connections" : "Firewall check unavailable";
                text_at(bold_font,58,banner_y+14,IM_COL32(123,87,30,255),banner_title);
                const char* detail=can_allow ? "Allow this app to receive connections from devices on your local network." : firewall.message.c_str();
                draw->AddText(small_font,16,ImVec2(a.x+16,a.y+44),IM_COL32(131,105,62,255),detail,nullptr,width-210);
                if (can_allow && !firewall.message.empty())
                    draw->AddText(small_font,16,ImVec2(a.x+16,a.y+82),IM_COL32(131,105,62,255),firewall.message.c_str(),nullptr,width-210);
                ImGui::SetCursorPos(ImVec2(stage_w-222.f,banner_y+41));
                ImGui::BeginDisabled(firewall.busy);
                if (can_allow && ui_draw::text_button(firewall.busy ? "Waiting for Windows..." : "Allow on local network",ImVec2(164,34)))
                    firewall_monitor.allow_local_network();
                ImGui::EndDisabled();
            }

        if (music_view) {
            const bool wide=stage_w>=780;
            const float cover_size=wide?std::min(330.f,width*.45f):230.f;
            const float cover_x=wide?42.f:(stage_w-cover_size)/2;
            const float cover_y=(wide?std::max(120.f,(stage_h-cover_size)*.45f):100.f)+firewall_offset;
            const ImVec2 tl(origin.x+cover_x,origin.y+cover_y),br(tl.x+cover_size,tl.y+cover_size);
            draw->AddRectFilled(ImVec2(tl.x,tl.y+9),ImVec2(br.x,br.y+9),IM_COL32(48,63,51,12),12);
            if (cover_tex) draw->AddImageRounded((ImTextureID)(intptr_t)cover_tex,tl,br,ImVec2(0,0),ImVec2(1,1),IM_COL32_WHITE,12);
            else {
                draw->AddRectFilled(tl,br,IM_COL32(231,235,229,255),12);
                ui_draw::icon(draw,ui_draw::Icon::Music,ImVec2(tl.x+cover_size/2-25,tl.y+cover_size/2-25),50,IM_COL32(145,158,147,255));
            }
            const float tx=wide?cover_x+cover_size+43:42;
            float ty=wide?cover_y+20:cover_y+cover_size+22;
            text_at(small_font,tx,ty,muted,playing_now ? "NOW PLAYING" : "PAUSED"); ty+=30;
            auto track_line=[&](SDL_Texture* texture,int w,int h,float scale,ImU32 tint) {
                if (!texture || w<=0 || h<=0) return;
                const float fit=std::min(scale,(stage_w-tx-42)/w);
                draw->AddImage((ImTextureID)(intptr_t)texture,ImVec2(origin.x+tx,origin.y+ty),ImVec2(origin.x+tx+w*fit,origin.y+ty+h*fit),ImVec2(0,0),ImVec2(1,1),tint);
                ty+=h*fit+9;
            };
            track_line(title_tex,title_w,title_h,1,ink);
            track_line(artist_tex,artist_w,artist_h,.8f,IM_COL32(134,146,137,255));
            track_line(album_tex,album_w,album_h,.65f,IM_COL32(205,215,202,255));
            ImGui::SetCursorPos(ImVec2(tx,ty+23));
            ImGui::BeginChild("##music_transport",ImVec2(stage_w-tx-42,165),false);
            if (audio_control_handler_) draw_audio_controls();
            ImGui::EndChild();
            if (wide) {
                const float footer_y=std::max(stage_h-78.f,std::max(cover_y+cover_size,ty+188)+35);
                draw->AddLine(ImVec2(origin.x+42,origin.y+footer_y),ImVec2(origin.x+stage_w-42,origin.y+footer_y),line);
                ui_draw::icon(draw,ui_draw::Icon::Cast,ImVec2(origin.x+42,origin.y+footer_y+19),16,IM_COL32(114,145,123,255));
                text_at(small_font,67,footer_y+19,muted,"Playing from your device on this PC");
                ImGui::SetCursorPosY(footer_y+45); ImGui::Dummy(ImVec2(1,1));
            }
        } else if (video_view) {
            ImGui::SetCursorPos(ImVec2(42,stage_h-47.f));
            const bool recording=recorder_ && recorder_->status().recording;
            if (ui_draw::text_button(recording ? "Stop recording" : "Record",ImVec2(125,34))) toggle_recording();
            ImGui::SameLine(); if (ui_draw::text_button("Fullscreen",ImVec2(110,34))) toggle_fullscreen();
            ImGui::SameLine(); if (ui_draw::text_button("Hide interface",ImVec2(120,34))) ui.chromeless=true;
            ImGui::SameLine();
            ImGui::BeginDisabled(!live_settings_);
            if (ui_draw::text_button("Rotation",ImVec2(98,34))) ImGui::OpenPopup("##quick_rotation");
            const ImVec2 rotation_button=ImGui::GetItemRectMin();
            draw->AddTriangleFilled(ImVec2(rotation_button.x+82,rotation_button.y+15),
                                    ImVec2(rotation_button.x+90,rotation_button.y+15),
                                    ImVec2(rotation_button.x+86,rotation_button.y+20),muted);
            ImGui::EndDisabled();
            ImGui::SetNextWindowPos(ImVec2(rotation_button.x,rotation_button.y-6),ImGuiCond_Appearing,ImVec2(0,1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,10));
            if (ImGui::BeginPopup("##quick_rotation")) {
                const int selected=live_settings_->video_rotation.load()+1;
                for (int i=0;i<IM_ARRAYSIZE(kRotationModes);++i) {
                    if (ImGui::MenuItem(kRotationModes[i],nullptr,selected==i))
                        live_settings_->video_rotation.store(i-1);
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            decoding_note(stage_h-12.f);
        } else {
            const float cx=stage_w*.5f;
            const float hero_y=(stage_h < 720 ? 90.f : std::max(105.f,(stage_h-690.f)*.3f+120.f))+firewall_offset;
            const ImVec2 tile(origin.x+cx-38,origin.y+hero_y);
            draw->AddRectFilled(tile,ImVec2(tile.x+76,tile.y+76),IM_COL32(233,239,253,255),22);
            draw->AddRect(tile,ImVec2(tile.x+76,tile.y+76),IM_COL32(219,229,252,255),22);
            ui_draw::icon(draw,ui_draw::Icon::Cast,ImVec2(tile.x+19,tile.y+19),38,ui_draw::blue);
            center_text(heading_font,hero_y+99,ink,"Your device.");
            center_text(heading_font,hero_y+143,ink,"A little bigger.");
            center_text(io.FontDefault,hero_y+194,muted,"Your music and your screen, right here on this PC.");
            center_text(io.FontDefault,hero_y+219,muted,"From your iPhone or iPad.");
            const float card_w=std::min(440.f,width),card_x=(stage_w-card_w)/2,card_y=hero_y+(stage_h<720?248:268);
            const ImVec2 cp(origin.x+card_x,origin.y+card_y);
            draw->AddRectFilled(cp,ImVec2(cp.x+card_w,cp.y+78),IM_COL32_WHITE,12);
            draw->AddRect(cp,ImVec2(cp.x+card_w,cp.y+78),line,12);
            text_at(small_font,card_x+20,card_y+15,muted,"SELECT ON YOUR DEVICE");
            draw->PushClipRect(cp,ImVec2(cp.x+card_w-56,cp.y+78),true);
            text_at(receiver_font,card_x+20,card_y+38,ink,receiver);
            draw->PopClipRect();
            ImGui::SetCursorPos(ImVec2(card_x+card_w-54,card_y+21));
            if (ui_draw::icon_button("##rename",ui_draw::Icon::Settings,"Receiver settings")) ui.settings_open=true;
            const float steps_y=card_y+(stage_h<720?105:121);
            draw->AddLine(ImVec2(origin.x+42,origin.y+steps_y),ImVec2(origin.x+stage_w-42,origin.y+steps_y),line);
            const char* titles[]={"Same Wi-Fi network","Choose your mode","Select your PC"};
            const char* descriptions[]={"Connect your device and this PC to the same network.","Use AirPlay for music, or Screen Mirroring from Control Center.","Choose this receiver. The connection starts automatically."};
            for (int i=0;i<3;++i) {
                const float x=42+i*(width+22)/3;
                const ImVec2 circle(origin.x+x+12,origin.y+steps_y+40);
                draw->AddCircleFilled(circle,12,IM_COL32_WHITE,24); draw->AddCircle(circle,12,line,24);
                char n[2]={static_cast<char>('1'+i),0};
                text_at(small_font,x+9,steps_y+32,muted,n);
                text_at(bold_font,x,steps_y+67,ink,titles[i]);
                draw->AddText(small_font,16,ImVec2(origin.x+x,origin.y+steps_y+93),muted,descriptions[i],nullptr,(width-44)/3);
            }
            const char* help="My PC isn't showing up";
            const float help_w=small_font->CalcTextSizeA(16,10000,0,help).x;
            ImGui::SetCursorPos(ImVec2((stage_w-help_w)/2,steps_y+157));
            if (ui_draw::surface("##connection_help",ImVec2(help_w,22))) ImGui::OpenPopup("Connection help");
            const ImVec2 hp=ImGui::GetItemRectMin();
            draw->AddText(small_font,16,hp,muted,help); draw->AddLine(ImVec2(hp.x,hp.y+17),ImVec2(hp.x+help_w,hp.y+17),muted);
            const float help_card_width=std::min(440.f,stage_w-64.f);
            ImGui::SetNextWindowSize(ImVec2(help_card_width,0));
            ImGui::SetNextWindowPos(ImVec2(origin.x+stage_w*.5f,std::min(hp.y-10,fh-20)),ImGuiCond_Appearing,ImVec2(.5f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24,22));
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,12);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize,1);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10,8));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(230/255.f,232/255.f,231/255.f,1));
            if (ImGui::BeginPopup("Connection help",ImGuiWindowFlags_NoMove)) {
                ImGui::PushFont(page_font);
                ImGui::TextUnformatted("Can't find your PC?");
                ImGui::PopFont();
                ImGui::SameLine(help_card_width-59);
                if (ui_draw::icon_button("##close_help",ui_draw::Icon::Close,"Close help")) ImGui::CloseCurrentPopup();
                ImGui::Spacing();
                ImGui::PushTextWrapPos(help_card_width-24);
                auto help_step=[&](const char* title,const char* description) {
                    ImGui::PushFont(bold_font); ImGui::TextUnformatted(title); ImGui::PopFont();
                    ImGui::PushFont(small_font); ImGui::TextDisabled("%s",description); ImGui::PopFont();
                    ImGui::Dummy(ImVec2(0,8));
                };
                help_step("1. Use the same network", "Connect your iPhone or iPad and this PC to the same Wi-Fi network.");
                help_step("2. Check your VPN", "A VPN can hide local devices. Check that it allows access to your local network.");
                help_step("3. Check Windows Firewall", "If a firewall warning is shown, choose Allow on local network.");
                ImGui::Separator();
                ImGui::PushFont(small_font); ImGui::TextDisabled("Then reopen AirPlay on your device."); ImGui::PopFont();
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(4);
            const float footer_y=std::max(steps_y+(stage_h<720?185:203),stage_h-34.f);
            text_at(small_font,42,footer_y,IM_COL32(145,154,147,255),"Windows · Local network");
            if (ui.page==2) decoding_note(footer_y+16);
            ImGui::SetCursorPosY(footer_y+16); ImGui::Dummy(ImVec2(1,1));
        }
        ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar();

        // Settings are a drawer above the current view, not a permanent sidebar.
        if (ui.settings_open) {
            ImGui::SetNextWindowPos(ImVec2(0,toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(fw-opts_bar_w,fh-toolbar_h-status_bar_h));
            ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.11f,.17f,.20f,.18f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
            ImGui::Begin("##settings_shade",nullptr,kPanelFlags|ImGuiWindowFlags_NoScrollbar);
            if (ui_draw::surface("##dismiss_settings",ImGui::GetContentRegionAvail())) ui.settings_open=false;
            ImGui::End(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
            ImGui::SetNextWindowPos(ImVec2(fw - opts_bar_w, toolbar_h));
            ImGui::SetNextWindowSize(ImVec2(opts_bar_w, fh - toolbar_h - status_bar_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
            ImGui::Begin("##settings_drawer",nullptr,kPanelFlags|ImGuiWindowFlags_NoScrollbar);
            // Keep the entire scrollbar inside the client area, clear of
            // the six-pixel native resize handles on the outer edge.
            ImGui::SetCursorPos(ImVec2(0,8));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(27,27));
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,14);
            if (ImGui::BeginChild("##options_panel",ImVec2(opts_bar_w-14,fh-toolbar_h-status_bar_h-16),false,
                                  kPanelFlags|ImGuiWindowFlags_AlwaysUseWindowPadding)) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(.125f,.149f,.161f,1));
                ImGui::PushFont(page_font);
                ImGui::TextUnformatted("Settings");
                ImGui::PopFont();
                ImGui::SameLine(opts_bar_w-65);
                if (ui_draw::icon_button("##close_settings",ui_draw::Icon::Close,"Close settings")) ui.settings_open = false;
                ImGui::PopStyleColor();
                ImGui::Separator();
                // Wrap descriptive text to the panel width so long
                // hint strings (VSYNC / GPU decoder explanations, etc.)
                // stay inside the panel instead of running off-screen.
                ImGui::PushTextWrapPos(0.0f);
                auto settings_section = [&](const char* label, ImGuiTreeNodeFlags flags = 0) {
                    ImGui::PushFont(bold_font);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
                    const bool open = ImGui::CollapsingHeader(label,flags);
                    ImGui::PopStyleVar(); ImGui::PopFont();
                    return open;
                };
                if (live_settings_ && settings_section("Receiver name",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool enter = ImGui::InputText("##receiver_name",
                        receiver_name_buf.data(), receiver_name_buf.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    const auto name = ap::airplay::normalize_receiver_name(receiver_name_buf.data());
                    const bool changed = name != live_settings_->receiver_name();
                    ImGui::BeginDisabled(name.empty() || !changed);
                    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.14f,.40f,.92f,1));
                    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
                    const bool apply = ImGui::Button("Apply name");
                    ImGui::PopStyleColor(2);
                    ImGui::EndDisabled();
                    if ((apply || enter) && !name.empty() && changed) {
                        live_settings_->set_receiver_name(name);
                        std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s", name.c_str());
                    }
                    if (name.empty())
                        ImGui::TextDisabled("Use a name without dots or backslashes (max. 50 UTF-8 bytes).");
                    else
                        ImGui::TextDisabled("Shown in AirPlay on your devices. Saved automatically.");
                }
                if (settings_section("Mirror resolution",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        struct Preset { const char* label; int w; int h; };
                        static const Preset presets[] = {
                            {"1920x1080  (Full HD)",      1920, 1080},
                            {"2560x1440  (QHD, default)", 2560, 1440},
                            {"1920x1920  (square limit)",1920, 1920},
                            {"2560x2560  (square limit)",2560, 2560},
                            {"2868x2868  (square limit)",2868, 2868},
                            {"3840x3840  (square limit)",3840, 3840},
                            {"3840x2160  (4K UHD)",       3840, 2160},
                        };
                        int cur_w = live_settings_->mirror_width.load();
                        int cur_h = live_settings_->mirror_height.load();
                        int sel = -1;   // -1 = none of the presets match
                        for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                            if (presets[i].w == cur_w && presets[i].h == cur_h) {
                                sel = i; break;
                            }
                        }
                        // Custom mode is sticky once chosen so the
                        // inputs don't disappear the moment the user
                        // pauses on a partial value that happens to
                        // match a preset (eg typing 1920 then 1080).
                        // Reset to false whenever a preset is chosen
                        // explicitly, or auto-set when current values
                        // don't match any preset (eg --mirror-res CLI).
                        if (sel < 0) custom_resolution = true;

                        char preview_buf[48];
                        const char* preview;
                        if (custom_resolution) {
                            std::snprintf(preview_buf, sizeof(preview_buf),
                                          "Custom: %dx%d", cur_w, cur_h);
                            preview = preview_buf;
                        } else {
                            preview = presets[sel].label;
                        }

                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##mirror_res", preview)) {
                            for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                                const bool is_sel =
                                    (!custom_resolution && sel == i);
                                if (ImGui::Selectable(presets[i].label,
                                                      is_sel)) {
                                    custom_resolution = false;
                                    live_settings_->mirror_width.store(presets[i].w);
                                    live_settings_->mirror_height.store(presets[i].h);
                                }
                                if (is_sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::Separator();
                            if (ImGui::Selectable("Custom...", custom_resolution)) {
                                custom_resolution = true;
                            }
                            ImGui::EndCombo();
                        }

                        if (custom_resolution) {
                            // Width / height inputs that commit live
                            // (no Enter) as soon as the value falls in
                            // the accepted range. While the user is
                            // typing a partial number outside the
                            // range nothing is written — so 2 / 25 /
                            // 256 don't get applied on the way to 2560.
                            static int  width_buf    = 0;
                            static int  height_buf   = 0;
                            static bool width_active = false;
                            static bool height_active= false;
                            if (!width_active)  width_buf  = cur_w;
                            if (!height_active) height_buf = cur_h;

                            ImGui::SetNextItemWidth(80);
                            ImGui::InputInt("Width##custom",
                                            &width_buf, 0, 0);
                            width_active = ImGui::IsItemActive();
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(80);
                            ImGui::InputInt("Height##custom",
                                            &height_buf, 0, 0);
                            height_active = ImGui::IsItemActive();

                            if (width_buf  >= 320 && width_buf  <= 7680 &&
                                width_buf  != cur_w) {
                                live_settings_->mirror_width.store(width_buf);
                            }
                            if (height_buf >= 240 && height_buf <= 4320 &&
                                height_buf != cur_h) {
                                live_settings_->mirror_height.store(height_buf);
                            }
                            ImGui::TextDisabled(
                                "Range: 320-7680 x 240-4320");
                        }
                        ImGui::TextDisabled(
                            "Maximum video size. Your device chooses the actual stream resolution.");
                        if (video_tex_w > 0 && video_tex_h > 0)
                            ImGui::TextDisabled("Received: %d x %d", video_tex_w, video_tex_h);
                        ImGui::TextDisabled("Applies on next device connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (settings_section("Video decoder",
                        ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (live_settings_) {
                        bool hw = live_settings_->mirror_hwaccel.load();
                        if (ImGui::Checkbox("Use GPU acceleration",
                                            &hw)) {
                            live_settings_->mirror_hwaccel.store(hw);
                        }
                        ImGui::TextDisabled(
                            hw
                              ? "Uses your graphics card when available."
                              : "Uses your processor to decode video.");
                        ImGui::TextDisabled("Applies on next device connection");

                        ImGui::Separator();

                        bool vsync = live_settings_->vsync_enabled.load();
                        if (ImGui::Checkbox("Synchronize with display", &vsync)) {
                            live_settings_->vsync_enabled.store(vsync);
                        }
                        ImGui::TextDisabled(
                            vsync
                              ? "Sync presents to monitor refresh."
                              : "Present immediately (lower latency,"
                                " visible tearing on fast pans).");
                        ImGui::TextDisabled("Applies live (no reconnect needed)");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (settings_section("Recording")) {
                    const auto rec = recorder_->status();
                    ImGui::TextUnformatted("Folder");
#if defined(_WIN32)
                    ImGui::SetNextItemWidth(-92.0f);
#endif
                    ImGui::InputText("##recording_folder", recording_dir.data(),
                                     recording_dir.size());
#if defined(_WIN32)
                    ImGui::SameLine();
                    if (ImGui::Button("Browse...", ImVec2(-1.0f, 0.0f))) {
                        std::string selected_folder;
                        if (browse_recording_folder(
                                window, recording_dir.data(), selected_folder)) {
                            std::snprintf(recording_dir.data(),
                                          recording_dir.size(), "%s",
                                          selected_folder.c_str());
                        }
                    }
#endif
                    const char* encoder_modes[] = {
                        "Auto (GPU preferred)", "GPU only", "CPU only"
                    };
                    const char* video_codecs[] = {
                        "H.264 (compatible)", "H.265 / HEVC (smaller)"
                    };
                    int video_codec =
                        static_cast<int>(recording_options.codec);
                    if (ImGui::Combo("Video codec", &video_codec,
                                     video_codecs, 2)) {
                        recording_options.codec =
                            static_cast<VideoRecorder::VideoCodec>(video_codec);
                    }
                    int encoder_mode =
                        static_cast<int>(recording_options.encoder);
                    if (ImGui::Combo("Encoder", &encoder_mode,
                                     encoder_modes, 3)) {
                        recording_options.encoder =
                            static_cast<VideoRecorder::EncoderMode>(encoder_mode);
                    }
                    ImGui::SliderInt("Bitrate (Mbps)",
                                     &recording_options.bitrate_mbps, 2, 40);
                    ImGui::TextDisabled(
                        "Lower bitrate = smaller file / more compression");
                    if (rec.recording)
                        ImGui::TextDisabled("Settings apply to the next recording");
                    if (rec.recording) {
                        const int64_t seconds = rec.elapsed_ms / 1000;
                        ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.32f, 1.0f),
                                           "REC  %02lld:%02lld",
                                           static_cast<long long>(seconds / 60),
                                           static_cast<long long>(seconds % 60));
                        ImGui::Text("Frames: %llu",
                            static_cast<unsigned long long>(rec.frames));
                        ImGui::Text("Audio: %s",
                            rec.audio_samples > 0 ? "AAC stereo" : "waiting for PCM");
                        if (rec.dropped > 0) {
                            ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                                "Queue drops: %llu",
                                static_cast<unsigned long long>(rec.dropped));
                        }
                        if (!rec.codec.empty())
                            ImGui::TextWrapped("Codec: %s", rec.codec.c_str());
                        if (ImGui::Button("Stop recording", ImVec2(-1, 0)))
                            toggle_recording();
                    } else {
                        ImGui::TextDisabled("MP4 video (decoded AirPlay stream)");
                        if (ImGui::Button("Start recording", ImVec2(-1, 0)))
                            toggle_recording();
                    }
                    if (!rec.output_path.empty())
                        ImGui::TextWrapped("File: %s", rec.output_path.c_str());
                    if (!rec.error.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                           "%s", rec.error.c_str());
                }
                ImGui::Separator();
                if (settings_section("Advanced")) {
                    ImGui::Checkbox("Show diagnostics", &ui.diagnostics);
                if (settings_section("Rotation")) {
                    if (live_settings_) {
                        int selected = live_settings_->video_rotation.load() + 1;
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::Combo("##video_rotation", &selected, kRotationModes, IM_ARRAYSIZE(kRotationModes)))
                            live_settings_->video_rotation.store(selected - 1);
                        ImGui::TextDisabled(
                            "Auto follows the orientation sent by the device."
                            " Manual angles rotate clockwise from the received image.");
                        if (video_tex_w > 0 && video_tex_h > 0)
                            ImGui::TextDisabled("Received: %s (%d x %d)",
                                video_tex_w > video_tex_h ? "Landscape" :
                                video_tex_w < video_tex_h ? "Portrait" : "Square",
                                video_tex_w, video_tex_h);
                        ImGui::TextDisabled("Applies live to display only; recording is unchanged.");
                    }
                }
                if (settings_section("Frame rate")) {
                    if (live_settings_) {
                        // iOS effectively takes the min of maxFPS and
                        // refreshRate, so two separate sliders meant
                        // the same thing twice. Drive both atomics
                        // from a single "Frame rate cap" knob and
                        // keep the underlying fields in lockstep.
                        int cap = live_settings_->max_fps.load();
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderInt("Frame rate cap",
                                             &cap, 1, 60, "%d fps")) {
                            live_settings_->max_fps.store(cap);
                            live_settings_->refresh_rate.store(cap);
                        }
                        ImGui::TextDisabled(
                            "iOS caps mirror at 60 fps regardless of hint;"
                            " lower values force a deterministic cadence.");
                        ImGui::TextDisabled("Applies on next device connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (settings_section("Compression")) {
                    if (live_settings_) {
                        bool hevc = live_settings_->hevc_enabled.load();
                        if (ImGui::Checkbox("Allow HEVC (H.265)", &hevc)) {
                            live_settings_->hevc_enabled.store(hevc);
                        }
                        ImGui::TextDisabled(
                            hevc
                              ? "The device may pick HEVC at high resolution"
                              : "Force H.264 (more CPU, more bandwidth)");
                        ImGui::TextDisabled("Applies on next device connection");
                    } else {
                        ImGui::TextDisabled("(LiveSettings not wired)");
                    }
                }
                if (settings_section("Network")) {
                    ImGui::TextDisabled("RTSP   :7000");
                    ImGui::TextDisabled("HLS    :7100");
                    ImGui::TextDisabled("mDNS   advertising");
                }
                if (settings_section("Keyboard shortcuts")) {
                    ImGui::BulletText("F / F11 / double-click  Toggle fullscreen");
                    ImGui::BulletText("H       Hide / show the interface");
                    ImGui::BulletText("R       Start / stop recording");
                    ImGui::BulletText("Esc     Exit fullscreen / show UI / quit");
                }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Restore all app preferences to their defaults.");
                ImGui::BeginDisabled(!live_settings_);
                if (ui_draw::text_button("Reset all settings",ImVec2(-1,36))) reset_all_settings();
                ImGui::EndDisabled();
                if (ImGui::GetTime() < settings_reset_notice_until)
                    ImGui::TextDisabled("Default settings restored.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (ui.diagnostics) {
        // ---- Bottom status bar ------------------------------------
        ImGui::SetNextWindowPos(ImVec2(0.0f, fh - status_bar_h));
        ImGui::SetNextWindowSize(ImVec2(fw, status_bar_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
        if (ImGui::Begin("##status_bar", nullptr,
                         kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
            const ImGuiIO& io_ui = ImGui::GetIO();
            const bool playing = playing_.load(std::memory_order_relaxed);
            const bool has_video =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            const int      vid_w = video_w_seen_.load(std::memory_order_relaxed);
            const int      vid_h = video_h_seen_.load(std::memory_order_relaxed);
            const float    vid_fps = video_fps_ema_.load(std::memory_order_relaxed);
            const uint64_t total = frames_total_.load(std::memory_order_relaxed);
#if defined(HAVE_GSTREAMER_HLS)
            constexpr const char* kHlsBackend = "GStreamer";
#else
            constexpr const char* kHlsBackend = "libavformat";
#endif
            // Wrap complete metrics into equal-width columns instead of
            // clipping a single long line at the edge of the window.
            if (ImGui::BeginTable("##diagnostics_metrics", diagnostics_columns,
                                  ImGuiTableFlags_SizingStretchSame)) {
            const auto sep = [&]() {
                ImGui::TableNextColumn();
            };
            ImGui::TableNextColumn();
            // "Streaming" = paired AND frames flowing; "Connected" =
            // paired but iOS quiet (static menu, paused video — common
            // in mirror); "Idle" = nothing connected.
            const char* state_label =
                has_active_dev
                    ? (has_video ? (playing ? "Streaming" : "Paused")
                                 : "Connected")
                    : "Idle";
            ImGui::Text("State: %s", state_label);
            sep();
            if (vid_w > 0 && vid_h > 0) {
                ImGui::Text("Video: %dx%d @ %.1f", vid_w, vid_h,
                            has_video ? vid_fps : 0.0f);
            } else {
                ImGui::TextDisabled("Video: -");
            }
            sep();
            ImGui::Text("Frames: %llu", static_cast<unsigned long long>(total));
            sep();
            ImGui::Text("Stage: %dx%d", stage_w, stage_h);
            sep();
            ImGui::Text("Renderer: %.0f fps", io_ui.Framerate);
            sep();
            const float    mbps  =
                payload_mbps_ema_.load(std::memory_order_relaxed);
            const uint64_t total_b =
                payload_bytes_total_.load(std::memory_order_relaxed);
            // Render the cumulative size in whichever unit reads
            // best for the magnitude (KB up to 10 MB, then MB, then
            // GB) so the eye sees a tidy "82.3 MB" instead of
            // "85786 KB".
            const auto fmt_total = [&](uint64_t bytes) {
                char buf[32];
                if (bytes < (10ull << 20)) {
                    std::snprintf(buf, sizeof(buf), "%.1f KB",
                                  bytes / 1024.0);
                } else if (bytes < (10ull << 30)) {
                    std::snprintf(buf, sizeof(buf), "%.1f MB",
                                  bytes / (1024.0 * 1024.0));
                } else {
                    std::snprintf(buf, sizeof(buf), "%.2f GB",
                                  bytes / (1024.0 * 1024.0 * 1024.0));
                }
                return std::string(buf);
            };
            ImGui::Text("BW: %.1f Mbps", has_video ? mbps : 0.0f);
            sep();
            ImGui::Text("Total: %s", fmt_total(total_b).c_str());
            sep();
            const float lat_ms =
                pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
            // Only show when a recent video frame populated it,
            // otherwise the EMA is stale or zero.
            ImGui::Text("Lat: %.1f ms", has_video ? lat_ms : 0.0f);
            sep();
            ImGui::TextDisabled("HLS backend: %s", kHlsBackend);
            ImGui::EndTable();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        } // diagnostics
        } // !fullscreen

        // Audio controls stay on the render thread; network work is queued
        // to main(). Only received sender state drives play/time indicators.
        if (has_active_dev && active_dev.kind == "Audio" && no_chrome && audio_control_handler_ && stage_w > 120 && stage_h > 100) {
            constexpr float controls_h = 150.0f;
            const bool wide = !no_chrome && stage_w >= 780;
            const float controls_x = wide ? 400.0f : 20.0f;
            const float controls_y = wide ? std::max(90.0f, (stage_h - std::min(300.0f, stage_h * .40f)) * .42f) + 180 : stage_h - controls_h;
            ImGui::SetNextWindowPos(ImVec2(stage_x + controls_x, stage_y + controls_y));
            ImGui::SetNextWindowSize(ImVec2(stage_w - controls_x - 20, controls_h));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.961f,.965f,.957f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 8));
            if (ImGui::Begin("##audio_controls", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar)) {
                draw_audio_controls();
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        // Pulsing "Waiting for AirPlay" ring centered in the stage when
        // no fresh video frame is on screen. Drawn to the background
        // list so the side panels still cover it where they overlap.
        {
            const bool video_streaming_idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_video_frame_time).count() < 1500;
            if (no_chrome && !video_streaming_idle && !has_active_dev &&
                stage_w > 0 && stage_h > 0) {
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                const float cx = static_cast<float>(stage_x) + stage_w * 0.5f;
                // Sit the rings above the SDL idle text (drawn around
                // 50% of the stage by the existing code) so the two
                // don't fight for the same pixels.
                const float cy = static_cast<float>(stage_y) + stage_h * 0.30f;
                const float t  = static_cast<float>(ImGui::GetTime());
                for (int i = 0; i < 2; ++i) {
                    const float speed  = (i == 0) ? 1.6f : 2.4f;
                    const float phase  = (i == 0) ? 0.0f : 1.05f;
                    const float pulse  =
                        0.5f + 0.5f * std::sin(t * speed + phase);
                    const float r_min  = (i == 0) ? 32.0f : 18.0f;
                    const float r_max  = (i == 0) ? 58.0f : 32.0f;
                    const float radius = r_min + (r_max - r_min) * pulse;
                    const int   alpha  =
                        static_cast<int>((1.0f - pulse) * 110.0f);
                    bg->AddCircle(ImVec2(cx, cy), radius,
                                  IM_COL32(120, 180, 240, alpha),
                                  64, 2.0f);
                }
                bg->AddCircleFilled(ImVec2(cx, cy), 8.0f,
                                    IM_COL32(120, 180, 240, 220));
            }
        }

        publish_preferences();
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(
            ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);

        // Pipeline-latency measurement: from when the encrypted
        // frame body finished arriving on the wire (origin_ns
        // tagged in MirrorListener) to right after present
        // returns. Captures decrypt + decode + render + VSYNC.
        // Ignored when origin_ns is 0 (HLS / non-mirror frames or
        // older callers that didn't tag).
        if ((have_frame || have_avf) && frame_origin_ns_local > 0) {
            const int64_t now_ns2 =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const float lat_ms =
                static_cast<float>((now_ns2 - frame_origin_ns_local) / 1e6);
            const float prev_ema =
                pipeline_latency_ms_ema_.load(std::memory_order_relaxed);
            const float ema = (prev_ema <= 0.0f)
                                  ? lat_ms
                                  : prev_ema * 0.90f + lat_ms * 0.10f;
            pipeline_latency_ms_ema_.store(ema, std::memory_order_relaxed);
        }
    }

    if (recorder_) recorder_->stop();
    publish_preferences();
    if (title_tex)     SDL_DestroyTexture(title_tex);
    if (artist_tex)    SDL_DestroyTexture(artist_tex);
    if (album_tex)     SDL_DestroyTexture(album_tex);
    if (idle_name_tex) SDL_DestroyTexture(idle_name_tex);
    if (idle_ip_tex)   SDL_DestroyTexture(idle_ip_tex);
    if (idle_msg_tex)  SDL_DestroyTexture(idle_msg_tex);
    gpu_video.reset_target();
    if (video_tex)     SDL_DestroyTexture(video_tex);
    if (cover_tex)     SDL_DestroyTexture(cover_tex);
    for (TTF_Font* f : fonts_big)   if (f) TTF_CloseFont(f);
    for (TTF_Font* f : fonts_small) if (f) TTF_CloseFont(f);
    TTF_Quit();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    desktop.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LOG_INFO << "VideoRenderer stopped";
}

} // namespace ap::video
