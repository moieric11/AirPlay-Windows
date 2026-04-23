// AirPlay-Windows — first-skeleton entry point.
//
// Responsibilities of main():
//   1. Initialise Winsock (net::global_init).
//   2. Build a DeviceContext (identifiers echoed in both mDNS TXT and /info).
//   3. Start the AirPlay RTSP-like server on TCP 7000.
//   4. Start the native mDNS advertisement (Windows DnsService API).
//   5. Block until Ctrl-C, then tear everything down cleanly.

#include "airplay/hls_local_server.h"
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
ap::airplay::DeviceContext build_device_context() {
    ap::airplay::DeviceContext ctx;
    ctx.name     = "AirPlay-Windows";
    ctx.deviceid = ap::net::primary_mac();
    ctx.model    = "AppleTV3,2";
    ctx.pi       = "b08f5a79-db29-4384-b456-a4784d9e6055";
    // Feature bitmap advertised in mDNS TXT + /info. Starts from UxPlay's
    // default 0x5A7FFEE6 and explicitly flips on bit 0 (AirPlay video)
    // and bit 4 (HLS) — iOS keys the AirPlay Streaming dispatch on
    // these two bits; without them YouTube / Photos stay on the audio
    // fallback path. UxPlay enables them via --hls; we hard-enable.
    // Bit 42 (HEVC "Screen Multi Codec") stays OFF for now: H264Decoder
    // is pinned to AV_CODEC_ID_H264 so advertising HEVC would break
    // decoding on iOS devices that honour the bit.
    ctx.features = "0x5A7FFEF7,0x0";
    ctx.srcvers  = "220.68";
    return ctx;
}

} // namespace

int main() {
    if (!ap::net::global_init()) {
        return 1;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    auto ctx = build_device_context();

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
    renderer.set_idle_info(ctx.name, local_ip);
    ctx.renderer = &renderer;

    ap::video::HlsPlayer hls_player;
    ctx.hls_player = &hls_player;

    ap::airplay::Server server;
    if (!server.start(ctx, 7000)) {
        LOG_ERROR << "Failed to start AirPlay server";
        renderer.stop();
        ap::net::global_shutdown();
        return 1;
    }

    // Local HTTP server that fronts the HLS session registry for an
    // internal media player (see hls_local_server.h). Only used for
    // YouTube-via-AirPlay-Streaming — harmless if that path never
    // triggers, since nothing connects to it.
    ap::airplay::HlsLocalServer hls_server;
    if (!hls_server.start(7100)) {
        LOG_WARN << "HLS local server could not bind 7100 — YouTube "
                    "AirPlay Streaming playback will not work";
    }
    ctx.hls_local_port = hls_server.port();

    ap::mdns::MdnsService mdns;
    if (!mdns.start(ctx, server.port())) {
        LOG_WARN << "mDNS not available — receiver won't auto-appear on iOS";
    }

    LOG_INFO << "Ready. Close the window or press Ctrl-C to exit.";
    while (!g_stop && !renderer.user_closed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO << "Shutting down...";
    mdns.stop();
    hls_player.stop();
    hls_server.stop();
    server.stop();
    renderer.stop();
    ap::net::global_shutdown();
    return 0;
}
