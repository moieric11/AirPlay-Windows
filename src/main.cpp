// AirPlay-Windows — first-skeleton entry point.
//
// Responsibilities of main():
//   1. Initialise Winsock (net::global_init).
//   2. Build a DeviceContext (identifiers echoed in both mDNS TXT and /info).
//   3. Start the AirPlay RTSP-like server on TCP 7000.
//   4. Start the native mDNS advertisement (Windows DnsService API).
//   5. Block until Ctrl-C, then tear everything down cleanly.

#include "airplay/hls_local_server.h"
#include "airplay/live_settings.h"
#include "airplay/server.h"
#include "video/hls_player.h"
#include "crypto/identity.h"
#include "log.h"
#include "mdns/mdns_service.h"
#include "net/socket.h"
#include "settings.h"
#include "platform/single_instance.h"
#include "platform/firewall.h"
#include "usb/usb_supervisor.h"
#include "video/video_renderer.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <mutex>
#include <csignal>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) { g_stop = true; }
#endif

// Pick sensible defaults. The PI and feature bitmask are copied from RPiPlay's
// defaults — known to pass the initial iOS detection phase.
//
// `hls_playback` gates the AirPlay Streaming HLS proxy path (bits 0+4 of
// features low word). When it's off, iOS routes YouTube / Photos to the
// audio fallback — the mirror + RAOP-audio pipeline that actually behaves
// like AirPlay mirror (low-latency, native). When on, iOS hands us an
// mlhls:// URL and we play back via libavformat — works but acts like a
// VOD player (probing, buffering, occasional stutter), best treated as
// an opt-in experimental path.
ap::airplay::DeviceContext build_device_context(bool hls_playback) {
    ap::airplay::DeviceContext ctx;
    ctx.name     = "AirPlay-Windows";
    ctx.deviceid = ap::net::primary_mac();
    ctx.model    = "AppleTV3,2";
    ctx.pi       = "b08f5a79-db29-4384-b456-a4784d9e6055";
    // Features:
    //   0x5A7FFEE6 - UxPlay default (mirror + audio, no HLS/video push).
    //   + bit 0    - AirPlay video supported  (enables AirPlay Streaming)
    //   + bit 4    - HTTP live streaming (HLS) supported
    //   High word 0x400: bit 42 "Screen Multi Codec" = HEVC mirror.
    ctx.features = hls_playback ? "0x5A7FFEF7,0x400"   // video+HLS+HEVC
                                : "0x5A7FFEE6,0x400";  // HEVC mirror only
    ctx.srcvers  = "220.68";
    return ctx;
}

} // namespace

int main(int argc, char** argv) {
    bool start_hidden = false;
    for (int i = 1; i < argc; ++i) start_hidden |= std::string(argv[i]) == "--start-hidden";
#if defined(_WIN32)
    for (int i=1; i<argc; ++i)
        if (std::string(argv[i])=="--configure-firewall") return ap::platform::configure_firewall();
    ap::platform::SingleInstance instance;
    // Help is read-only and should remain usable while the receiver is open.
    bool help_only = false;
    for (int i = 1; i < argc; ++i)
        help_only |= std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h";
    if (!help_only) {
        const auto result = instance.acquire(!start_hidden);
        if (result == ap::platform::SingleInstance::Result::Secondary) return 0;
        if (result == ap::platform::SingleInstance::Result::Error) {
            MessageBoxW(nullptr, L"Unable to initialize the single application instance.",
                        L"AirPlay-Windows", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
#endif
    // Logs are off by default in the published binary. Scan for the
    // toggle BEFORE anything else so even the very first init message
    // honours the flag.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--log" ||
            std::string(argv[i]) == "--verbose") {
            ap::set_log_enabled(true);
            break;
        }
    }

#if defined(_WIN32)
    // We're a /SUBSYSTEM:WINDOWS binary so no console pops up by
    // default. With --log, attach to the parent terminal if we were
    // launched from one (cmd, powershell, terminal); otherwise spawn
    // a fresh console window so the user sees the stderr stream.
    const DWORD log_handle_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    const bool log_redirected = log_handle_type == FILE_TYPE_DISK || log_handle_type == FILE_TYPE_PIPE;
    if (ap::is_log_enabled() && !log_redirected) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
#endif

    auto saved = ap::load_settings();
    if (saved.mirror_width == 0 || saved.mirror_height == 0) {
        saved.mirror_width = 2560;
        saved.mirror_height = 1440;
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            SDL_DisplayMode dm{};
            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 &&
                dm.w >= 320 && dm.h >= 240 &&
                dm.w <= 7680 && dm.h <= 4320) {
                saved.mirror_width = dm.w;
                saved.mirror_height = dm.h;
            }
            SDL_Quit();
        }
    }

    if (!ap::net::global_init()) {
        return 1;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // Default mode: native AirPlay (mirror for video, RAOP for audio).
    // This matches UxPlay's "no --hls" behaviour — low-latency, reliable,
    // but YouTube/Photos play audio-only with thumbnail. Opt-in HLS
    // proxy for the rare cases where you want the video stream delivered
    // via the signed-CDN path we built.
    bool hls_playback = false;
    bool mirror_hwaccel = saved.mirror_hwaccel;
    int  mirror_w = saved.mirror_width;
    int  mirror_h = saved.mirror_height;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hls-proxy-playback" || arg == "--hls") {
            hls_playback = true;
        } else if (arg == "--mirror-hwaccel") {
            mirror_hwaccel = true;
        } else if ((arg == "--mirror-res" || arg == "-r") && i + 1 < argc) {
            // Accept "WxH" or "WxHi" with either separator, e.g. 1920x1080,
            // 2560x1440, 3840x2160. Anything malformed falls back silently
            // to the default and logs a warning.
            const std::string val = argv[++i];
            const auto x = val.find_first_of("xX*");
            if (x != std::string::npos) {
                try {
                    const int w = std::stoi(val.substr(0, x));
                    const int h = std::stoi(val.substr(x + 1));
                    if (w >= 320 && h >= 240 &&
                        w <= 7680 && h <= 4320) {
                        mirror_w = w;
                        mirror_h = h;
                    } else {
                        LOG_WARN << "--mirror-res out of range (" << val
                                 << "), keeping default";
                    }
                } catch (...) {
                    LOG_WARN << "--mirror-res not parseable (" << val
                             << "), keeping default";
                }
            }
        } else if (arg == "--start-hidden") {
            // Parsed before single-instance activation.
        } else if (arg == "--log" || arg == "--verbose") {
            // Already handled in the pre-init scan above; consume so
            // future unknown-arg paths don't trip on it.
        } else if (arg == "--help" || arg == "-h") {
            std::puts(
                "AirPlay-Windows [--log] [--hls-proxy-playback]\n"
                "                [--mirror-res WxH] [--mirror-hwaccel] [--start-hidden]\n"
                "  --start-hidden        start in the Windows notification area\n"
                "  --log / --verbose     print init/runtime logs to stderr\n"
                "                        (off by default in the released\n"
                "                        binary)\n"
                "  --hls-proxy-playback  advertise HLS capability so iOS\n"
                "                        hands video URLs to our proxy\n"
                "                        (experimental, VOD-style latency)\n"
                "  --mirror-res WxH      advertise the given display\n"
                "                        resolution to iOS (default: saved or\n"
                "                        detected display, else 2560x1440;\n"
                "                        higher = sharper portrait at the cost\n"
                "                        of bandwidth, common: 1920x1080,\n"
                "                        2560x1440, 3840x2160)\n"
                "  --mirror-hwaccel      decode mirror H.264/HEVC on the GPU\n"
                "                        (D3D11VA, GPU-resident presentation).\n"
                "                        No CPU pixel readback for display.\n"
                "                        Software fallback if unavailable.");
            ap::net::global_shutdown();
            return 0;
        }
    }

    auto ctx = build_device_context(hls_playback);
    ctx.name = saved.receiver_name;
    ctx.mirror_width   = mirror_w;
    ctx.mirror_height  = mirror_h;
    ctx.mirror_hwaccel = mirror_hwaccel;
    LOG_INFO << "Mirror display advertised: " << mirror_w << 'x' << mirror_h;
    LOG_INFO << "Mirror decoder default: "
             << (mirror_hwaccel
                   ? "GPU hwaccel (--mirror-hwaccel; D3D11VA resident surfaces)"
                   : "software libavcodec (toggle GPU in OPTIONS panel)");

    // Live-mutable settings exposed to the overlay UI. Seed from the
    // CLI defaults; the UI thread can change values at runtime, and
    // /info reads them on every iPhone handshake (so changes take
    // effect on the next reconnect).
    ap::airplay::LiveSettings live_settings;
    live_settings.set_receiver_name(saved.receiver_name);
    live_settings.mirror_width.store(mirror_w);
    live_settings.mirror_height.store(mirror_h);
    live_settings.hevc_enabled.store(saved.hevc_enabled);
    live_settings.max_fps.store(saved.max_fps);
    live_settings.refresh_rate.store(saved.refresh_rate);
    live_settings.mirror_hwaccel.store(mirror_hwaccel);
    live_settings.vsync_enabled.store(saved.vsync_enabled);
    live_settings.fullscreen.store(saved.fullscreen);
    live_settings.chromeless.store(saved.chromeless);
    live_settings.video_rotation.store(saved.video_rotation);
    live_settings.set_ui_preferences(saved.ui);
    ctx.live = &live_settings;
    LOG_INFO << "AirPlay Streaming HLS path: "
             << (hls_playback ? "ENABLED (--hls-proxy-playback)"
                              : "disabled (mirror + RAOP audio only)");

    // Load (or create) the persistent Ed25519 identity. iOS caches receivers
    // by pk — keeping a stable keypair avoids re-pairing on every launch.
    auto identity = ap::crypto::Identity::load_or_create("identity.key");
    if (!identity) {
        LOG_ERROR << "failed to initialise identity — aborting";
        ap::net::global_shutdown();
        return 1;
    }
    ctx.public_key = identity->public_key();
    ctx.identity   = identity.get();

    LOG_INFO << "=== AirPlay-Windows skeleton ===";
    LOG_INFO << "name="     << ctx.name;
    LOG_INFO << "deviceid=" << ctx.deviceid;
    const std::string local_ip   = ap::net::primary_ipv4();
    const std::string local_ipv6 = ap::net::primary_ipv6();
    LOG_INFO << "ip="       << local_ip;
    if (!local_ipv6.empty()) {
        LOG_INFO << "ipv6="  << local_ipv6;
    }
    LOG_INFO << "pk(Ed25519)=" << ctx.public_key.size() << " bytes";

    // Start the SDL2 renderer window upfront. Streams will push decoded
    // frames once an iPhone connects.
    ap::video::VideoRenderer renderer;
    renderer.set_live_settings(&live_settings);
    if (!renderer.start("AirPlay-Windows", start_hidden)) {
        LOG_WARN << "VideoRenderer could not start — running headless";
    }
    // Prefer the v4 address on the idle screen (shorter, more familiar),
    // but fall back to v6 if we're on a v6-only network. Without this,
    // an v6-only host would display "0.0.0.0" to the user.
    const std::string display_ip =
        (local_ip.empty() || local_ip == "0.0.0.0") && !local_ipv6.empty()
            ? local_ipv6 : local_ip;
    renderer.set_idle_info(ctx.name, display_ip);
    ctx.renderer = &renderer;

    ap::video::HlsPlayer hls_player;
    if (hls_playback) ctx.hls_player = &hls_player;

    ap::airplay::Server server;
    if (!server.start(ctx, 7000)) {
        LOG_ERROR << "Failed to start AirPlay server";
        renderer.stop();
        ap::net::global_shutdown();
        return 1;
    }
    // Keep the render thread available while workers finish GPU/audio teardown.
    std::atomic<bool> disconnect_requested{false};
    renderer.set_disconnect_handler([&disconnect_requested]() {
        disconnect_requested.store(true);
    });

    struct AudioRequest { ap::airplay::MediaCommand command; uint32_t position; std::string session_id; };
    std::mutex audio_request_mutex;
    std::optional<AudioRequest> audio_request;
    renderer.set_audio_control_handler([&](ap::airplay::MediaCommand command, uint32_t position) {
        ap::video::VideoRenderer::DeviceInfo device;
        renderer.active_device_snapshot(device);
        std::lock_guard<std::mutex> lock(audio_request_mutex);
        audio_request = AudioRequest{command, position, device.session_id};
    });

    // Local HTTP proxy + libavformat player only when the HLS path is
    // opted into. In the default (mirror + RAOP) mode nothing connects
    // to them anyway, but skipping the bind avoids the port claim and
    // keeps a clean log.
    ap::airplay::HlsLocalServer hls_server;
    if (hls_playback) {
        if (!hls_server.start(7100)) {
            LOG_WARN << "HLS local server could not bind 7100 — "
                        "--hls-proxy-playback playback will not work";
        }
        ctx.hls_local_port = hls_server.port();
    }

    ap::mdns::MdnsService mdns;
    if (!mdns.start(ctx, server.port())) {
        LOG_WARN << "mDNS not available — receiver won't auto-appear on iOS";
    }

    // Watch for USB-connected iPhones in parallel with the Wi-Fi mDNS
    // path. Phase 1: just logs arrivals + paired/not-paired. The full
    // QuickTime mirror session is layered on top in later phases —
    // when ready, the supervisor will hand a connected device off to
    // the same VideoRenderer / AudioOutput that mirror_listener uses.
    ap::usb::UsbSupervisor usb_supervisor;
    usb_supervisor.start();

    LOG_INFO << "Ready. Close the window or press Ctrl-C to exit.";
    auto last_saved_settings = ap::snapshot(live_settings);
    auto observed_settings = last_saved_settings;
    auto settings_changed_at = std::chrono::steady_clock::now();
    while (!g_stop && !renderer.user_closed()) {
#if defined(_WIN32)
        if (instance.activation_requested()) renderer.request_show();
#endif
        const auto receiver_name = live_settings.receiver_name();
        if (receiver_name != ctx.name) {
            mdns.stop();
            ctx.name = receiver_name;
            renderer.set_idle_info(ctx.name, display_ip);
            if (!mdns.start(ctx, server.port()))
                LOG_WARN << "Could not advertise the new receiver name";
        }
        if (disconnect_requested.exchange(false)) {
            LOG_INFO << "Disconnect requested by toolbar";
            server.disconnect_clients();
            LOG_INFO << "Disconnect completed: all client handlers drained";
        }
        std::optional<AudioRequest> request;
        {
            std::lock_guard<std::mutex> lock(audio_request_mutex);
            request.swap(audio_request);
        }
        if (request) {
            const bool success = server.control_audio(request->command, request->position, request->session_id);
            renderer.audio_command_finished(success);
        }
        ap::video::VideoRenderer::DeviceInfo current_device;
        renderer.active_device_snapshot(current_device);
        renderer.set_audio_control_available(server.audio_control_available(current_device.session_id));
        const auto current_settings = ap::snapshot(live_settings);
        const auto settings_now = std::chrono::steady_clock::now();
        if (!(current_settings == observed_settings)) {
            observed_settings = current_settings;
            settings_changed_at = settings_now;
        }
        if (!(current_settings == last_saved_settings) &&
            settings_now - settings_changed_at >= std::chrono::milliseconds(500)) {
            if (ap::save_settings(current_settings)) last_saved_settings = current_settings;
            settings_changed_at = settings_now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LOG_INFO << "Shutting down... (g_stop=" << g_stop.load()
             << " user_closed=" << renderer.user_closed() << ")";
    // Withdraw discovery and interrupt sessions before stopping peripherals.
    server.stop();
    mdns.stop();
    usb_supervisor.stop();
    hls_player.stop();
    hls_server.stop();
    renderer.stop();
    ap::save_settings(ap::snapshot(live_settings));
    SDL_Quit();
    ap::net::global_shutdown();
    return 0;
}
