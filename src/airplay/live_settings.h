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
};

} // namespace ap::airplay
