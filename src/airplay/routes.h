#pragma once

#include "airplay/rtsp_parser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ap::crypto { class Identity; }
namespace ap::video  { class VideoRenderer; class HlsPlayer; }

namespace ap::airplay {

struct LiveSettings;

// DeviceContext carries the identity fields the AirPlay handshake replays back
// (deviceid, model, name, features, pi, ...). They are generated once at
// startup and referenced by both the mDNS TXT record and the HTTP handlers.
struct DeviceContext {
    std::string name;       // human-readable, advertised in Bonjour
    std::string deviceid;   // "AA:BB:CC:DD:EE:FF"
    std::string model;      // e.g. "AppleTV3,2" — some clients gate features on this
    std::string pi;         // public-id (UUID-ish), stable per install
    std::string features;   // hex bitmask, see docs/PROTOCOL.md
    std::string srcvers;    // AirPlay source version string ("220.68")

    // 32-byte raw Ed25519 public key — exposed as `pk` in /info and used by
    // pair-verify to prove the receiver's identity.
    std::vector<unsigned char> public_key;

    // Non-owning pointer to the loaded identity. The dispatcher uses it to
    // sign pair-verify challenges. Lifetime: owned by main().
    const ap::crypto::Identity* identity = nullptr;

    // Non-owning pointer to the shared renderer. Handed down to each
    // StreamSession so mirror frames can be drawn. May be null (headless).
    ap::video::VideoRenderer*   renderer = nullptr;

    // Non-owning pointer to the global HLS media player. Started by
    // handle_action once every media playlist referenced by the master
    // has been fetched, stopped by handle_teardown / clear_session.
    ap::video::HlsPlayer*       hls_player = nullptr;

    // Port of the local HLS HTTP server (started in main()). The HLS
    // player opens http://localhost:<port>/master.m3u8 to fetch from
    // our own server which is proxying iOS via FCUP.
    uint16_t                    hls_local_port = 7100;

    // Resolution we advertise to iOS in /info. iOS encodes the mirror
    // stream to fit within this box (preserving aspect): a portrait
    // iPhone session caps at this height and gets a proportional
    // width. Default 2560x1440 — gives portrait phones ~664×1440
    // instead of the 498×1080 you'd see at the legacy 1920x1080
    // ceiling. Override with --mirror-res WxH.
    //
    // These two ints are the static fallback used when `live` is
    // null. With `live` set the /info builder reads from there, so
    // the UI can change resolution at runtime (effective on the
    // next iPhone connection).
    int                         mirror_width  = 2560;
    int                         mirror_height = 1440;

    // When true, the H.264/HEVC mirror decoder requests D3D11VA
    // hardware acceleration on Windows — frames decode on the GPU,
    // get pulled to system memory, converted to I420, and fed to
    // the renderer. Software fallback is automatic when the
    // platform / driver can't honor the request.
    bool                        mirror_hwaccel = false;

    // Mutable settings exposed by the overlay UI. Optional — when
    // null, /info falls back to the static fields above. Lifetime
    // owned by main(); held non-owning here.
    LiveSettings*               live = nullptr;
};

struct ClientSession;

// Build a response for a single incoming request. Stubs log and answer 501
// for handlers that are not implemented yet. `session` carries per-connection
// state (pair-verify rounds, later stream keys).
Response dispatch(const DeviceContext& ctx, ClientSession& session,
                  const Request& req);

} // namespace ap::airplay
