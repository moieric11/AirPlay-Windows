#pragma once
#include "settings.h"

#include <atomic>
#include <mutex>
#include <string>

namespace ap::airplay {

// User-facing knobs that the UI mutates and that downstream code
// (the /info plist builder, the features-bits assembler) reads at
// AirPlay handshake time. Lives in a single instance owned by main()
// and is referenced by both the DeviceContext and the VideoRenderer
// so the overlay can change values that take effect on the next
// iPhone /info request — practically: on the next reconnect.
//
// Numeric fields are atomic; the receiver name is protected by a mutex.
// The UI and RTSP threads can access these independently without racing.
struct LiveSettings {
    ap::UiPreferences ui_preferences() const {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        return ui_preferences_;
    }
    void set_ui_preferences(const ap::UiPreferences& value) {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        ui_preferences_ = value;
    }
    std::string receiver_name() const {
        std::lock_guard<std::mutex> lock(name_mutex_);
        return receiver_name_;
    }
    void set_receiver_name(const std::string& name) {
        std::lock_guard<std::mutex> lock(name_mutex_);
        receiver_name_ = name;
    }
    // Display box advertised in /info — see DeviceContext::mirror_*.
    std::atomic<int>  mirror_width{2560};
    std::atomic<int>  mirror_height{1440};

    // True → advertise the "Screen Multi Codec" feature bit (high
    // word 0x400) so iOS is allowed to encode the mirror stream
    // with HEVC. False → drop the bit and pin iOS to H.264, which
    // is more compatible with weaker decoders but uses ~50% more
    // bandwidth at the same visual quality.
    std::atomic<bool> hevc_enabled{true};
    // Saved preference; changes require an app restart to rebuild services.
    std::atomic<bool> hls_playback{false};
    bool hls_playback_active = false; // Set before starting the UI thread.

    // Frame-rate hints sent in the displays plist. iOS uses these
    // to cap the rate at which it emits encoded mirror frames —
    // useful to push a 120 Hz iPhone into 120 fps for slow-mo
    // playback, or to force a deterministic 30 fps. The encoder
    // may still produce fewer frames if the ASIC can't keep up at
    // the requested resolution+codec combo.
    std::atomic<int>  max_fps{60};
    std::atomic<int>  refresh_rate{60};

    // False: software decoding. True (default): D3D11VA decoding,
    // GPU color conversion and shared-texture presentation on the
    // same GPU adapter, without CPU readback for display.
    std::atomic<bool> mirror_hwaccel{true};

    // SDL_RENDERER_PRESENTVSYNC. False → present immediately when a
    // frame is ready, lower latency (up to ~16 ms saved at 60 Hz)
    // at the cost of horizontal tearing during fast pans. Read at
    // VideoRenderer init time and on the fly when toggled (the
    // renderer rebuilds its SDL_Renderer when the value changes).
    std::atomic<bool> vsync_enabled{true};

    std::atomic<bool> fullscreen{false};
    std::atomic<bool> chromeless{false};
    // Display only: -1 follows the orientation encoded by iOS; 0..3 adds
    // clockwise quarter turns to the received image. Recording is unchanged.
    std::atomic<int> video_rotation{-1};

private:
    mutable std::mutex ui_mutex_;
    ap::UiPreferences ui_preferences_;
    mutable std::mutex name_mutex_;
    std::string receiver_name_ = "AirPlay-Windows";
};

} // namespace ap::airplay
