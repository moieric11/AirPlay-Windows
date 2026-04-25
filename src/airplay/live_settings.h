#pragma once

#include <atomic>

namespace ap::airplay {

// User-facing knobs that the UI mutates and that downstream code
// (the /info plist builder, the features-bits assembler) reads at
// AirPlay handshake time. Lives in a single instance owned by main()
// and is referenced by both the DeviceContext and the VideoRenderer
// so the overlay can change values that take effect on the next
// iPhone /info request — practically: on the next reconnect.
//
// Atomics on every field so the writer (UI thread) and the readers
// (RTSP server thread serving /info) never race; values are
// independent so a torn write across them is harmless.
struct LiveSettings {
    // Display box advertised in /info — see DeviceContext::mirror_*.
    std::atomic<int>  mirror_width{2560};
    std::atomic<int>  mirror_height{1440};

    // True → advertise the "Screen Multi Codec" feature bit (high
    // word 0x400) so iOS is allowed to encode the mirror stream
    // with HEVC. False → drop the bit and pin iOS to H.264, which
    // is more compatible with weaker decoders but uses ~50% more
    // bandwidth at the same visual quality.
    std::atomic<bool> hevc_enabled{true};

    // Frame-rate hints sent in the displays plist. iOS uses these
    // to cap the rate at which it emits encoded mirror frames —
    // useful to push a 120 Hz iPhone into 120 fps for slow-mo
    // playback, or to force a deterministic 30 fps. The encoder
    // may still produce fewer frames if the ASIC can't keep up at
    // the requested resolution+codec combo.
    std::atomic<int>  max_fps{60};
    std::atomic<int>  refresh_rate{60};

    // False → keep the software libavcodec path (default). On
    // benchmarking with cuvid (NVDEC on RTX 3080) and D3D11VA we
    // measured ~30-38 ms of local-pipeline latency vs ~12 ms in
    // software, because the HW path pays two synchronous PCIe
    // round-trips (av_hwframe_transfer_data + SDL_UpdateNVTexture)
    // that single-stream mirror doesn't amortise. Software wins
    // for single-stream low-latency mirror; users who want CPU
    // offload (multi-stream or weak CPUs) flip this to true.
    std::atomic<bool> mirror_hwaccel{false};

    // SDL_RENDERER_PRESENTVSYNC. False → present immediately when a
    // frame is ready, lower latency (up to ~16 ms saved at 60 Hz)
    // at the cost of horizontal tearing during fast pans. Read at
    // VideoRenderer init time and on the fly when toggled (the
    // renderer rebuilds its SDL_Renderer when the value changes).
    std::atomic<bool> vsync_enabled{true};
};

} // namespace ap::airplay
