#pragma once

#include "airplay/mirror_listener.h"
#include "airplay/ntp_client.h"
#include "audio/audio_receiver.h"
#include "net/socket.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::airplay {

// Ports we allocate on our side. A 0 means "not used in this request".
struct StreamPorts {
    uint16_t server  = 0;   // RTP data (audio PCM/ALAC, video H.264)
    uint16_t control = 0;   // RTCP / events channel
    uint16_t timing  = 0;   // NTP-like timing
};

// One media stream in the AirPlay 2 model. iOS sends `streams[]` in the
// SETUP body; for each entry we bind a (data, control) UDP pair.
struct StreamChannel {
    int      type          = 0;  // 96 / 103 / 110
    uint16_t data_port     = 0;
    uint16_t control_port  = 0;
    socket_t data_sock     = INVALID_SOCK;
    socket_t control_sock  = INVALID_SOCK;
};

// Owns every UDP socket bound on behalf of one TCP session. Two entry
// points:
//
//   - setup_legacy()     : old RTSP form with Transport header (keeps our
//                          Python test suite green).
//   - setup_session()    : AirPlay 2 first-round SETUP — binds a single
//                          event + timing pair.
//   - setup_stream()     : AirPlay 2 per-stream SETUP — binds data + ctrl.
//
// Lifetime: from first SETUP to TEARDOWN (or socket close). Destruction
// closes every socket.
class StreamSession {
public:
    StreamSession();
    ~StreamSession();

    StreamSession(const StreamSession&)            = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    // Non-owning renderer hook. Must be set before setup_stream(110) if you
    // want decoded frames to hit a window.
    void set_renderer(ap::video::VideoRenderer* r) { renderer_ = r; }

    bool setup_legacy(const std::string& transport_header, StreamPorts& allocated);
    bool setup_session(uint16_t& event_port, uint16_t& timing_port);

    // Per-stream setup options.
    //   Type 110 (mirror video, TCP): aes_key + stream_connection_id drive
    //     the AES-CTR decryption of the H.264 NAL payloads.
    //   Type 96 (audio, UDP):         aes_key + aes_iv feed the AES-CBC
    //     decryptor; ct + sample_rate select the codec used by the
    //     downstream decoder.
    struct StreamOpts {
        std::vector<unsigned char> aes_key;
        std::vector<unsigned char> aes_iv;
        uint64_t                   stream_connection_id = 0;
        int                        ct          = 0;
        int                        sample_rate = 44100;
    };

    bool setup_stream(int type, uint16_t& data_port, uint16_t& control_port,
                      const StreamOpts& opts);

    // Start the NTP client thread that polls iOS's timing server at
    // (remote_ip, remote_port) from our timing socket bound by setup_session().
    // Must be called AFTER setup_session and before iOS gives up on the
    // session (~8 sec).
    bool start_ntp(const std::string& remote_ip, uint16_t remote_port);

    // Partial teardown for AirPlay 2 stream-level TEARDOWN requests.
    void stop_stream(int type);

    void teardown();

    const std::string& session_id() const { return session_id_; }

private:
    // Legacy path state (used by the RTSP-Transport path).
    StreamPorts client_ports_{};
    socket_t    data_sock_   = INVALID_SOCK;
    socket_t    ctrl_sock_   = INVALID_SOCK;
    socket_t    timing_sock_ = INVALID_SOCK;

    // AirPlay 2 path state.
    socket_t    event_sock_     = INVALID_SOCK;
    socket_t    ap2_timing_sock = INVALID_SOCK;
    std::vector<StreamChannel>                    channels_;   // audio streams (UDP)
    std::unique_ptr<MirrorListener>               mirror_;     // video stream (TCP)
    std::unique_ptr<NtpClient>                    ntp_;
    std::unique_ptr<ap::audio::AudioReceiver>     audio_;      // type 96 UDP
    ap::video::VideoRenderer*                     renderer_{nullptr};

    std::string session_id_;
};

} // namespace ap::airplay
