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
#include "video/video_renderer.h"

#include <atomic>
#include <chrono>
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
    bool mirror_hwaccel = false;
    int  mirror_w = 2560;   // matches DeviceContext default; CLI override
    int  mirror_h = 1440;   //   below this comment.
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
        } else if (arg == "--help" || arg == "-h") {
            std::puts(
                "AirPlay-Windows [--hls-proxy-playback] [--mirror-res WxH]\n"
                "                [--mirror-hwaccel]\n"
                "  --hls-proxy-playback  advertise HLS capability so iOS\n"
                "                        hands video URLs to our proxy\n"
                "                        (experimental, VOD-style latency)\n"
                "  --mirror-res WxH      advertise the given display\n"
                "                        resolution to iOS (default 2560x1440;\n"
                "                        higher = sharper portrait at the cost\n"
                "                        of bandwidth, common: 1920x1080,\n"
                "                        2560x1440, 3840x2160)\n"
                "  --mirror-hwaccel      decode mirror H.264/HEVC on the GPU\n"
                "                        via D3D11VA (Windows only). Falls back\n"
                "                        to software when the platform / driver\n"
                "                        can't honor it. Useful for very high\n"
                "                        resolutions / HEVC where software\n"
                "                        decode would saturate the CPU.");
            ap::net::global_shutdown();
            return 0;
        }
    }

    auto ctx = build_device_context(hls_playback);
    ctx.mirror_width   = mirror_w;
    ctx.mirror_height  = mirror_h;
    ctx.mirror_hwaccel = mirror_hwaccel;
    LOG_INFO << "Mirror display advertised: " << mirror_w << 'x' << mirror_h;
    LOG_INFO << "Mirror decoder: "
             << (mirror_hwaccel ? "D3D11VA hwaccel (--mirror-hwaccel)"
                                : "software (libavcodec)");

    // Live-mutable settings exposed to the overlay UI. Seed from the
    // CLI defaults; the UI thread can change values at runtime, and
    // /info reads them on every iPhone handshake (so changes take
    // effect on the next reconnect).
    ap::airplay::LiveSettings live_settings;
    live_settings.mirror_width.store(mirror_w);
    live_settings.mirror_height.store(mirror_h);
    live_settings.hevc_enabled.store(true);
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
    if (!renderer.start("AirPlay-Windows")) {
        LOG_WARN << "VideoRenderer could not start — running headless";
    }
    // Prefer the v4 address on the idle screen (shorter, more familiar),
    // but fall back to v6 if we're on a v6-only network. Without this,
    // an v6-only host would display "0.0.0.0" to the user.
    const std::string display_ip =
        (local_ip.empty() || local_ip == "0.0.0.0") && !local_ipv6.empty()
            ? local_ipv6 : local_ip;
    renderer.set_idle_info(ctx.name, display_ip);
    renderer.set_live_settings(&live_settings);
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

    LOG_INFO << "Ready. Close the window or press Ctrl-C to exit.";
    while (!g_stop && !renderer.user_closed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO << "Shutting down... (g_stop=" << g_stop.load()
             << " user_closed=" << renderer.user_closed() << ")";
    mdns.stop();
    hls_player.stop();
    hls_server.stop();
    server.stop();
    renderer.stop();
    ap::net::global_shutdown();
    return 0;
}
