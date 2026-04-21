#pragma once

#include "net/socket.h"

#include <cstdint>
#include <string>

namespace ap::airplay {

// Ports we allocate on our side. A 0 means "not used in this request".
struct StreamPorts {
    uint16_t server  = 0;   // RTP data (audio PCM/ALAC, video H.264)
    uint16_t control = 0;   // RTCP / events channel
    uint16_t timing  = 0;   // NTP-like timing
};

// Owns the UDP sockets bound by a SETUP request. Lifetime is a full media
// session: created on SETUP, freed on TEARDOWN or when the client session
// goes away. For this iteration we only bind — no per-packet processing
// yet; iOS's packets will queue in the OS buffer which is fine for a live
// test that only needs to confirm the handshake reaches this stage.
class StreamSession {
public:
    StreamSession();
    ~StreamSession();

    StreamSession(const StreamSession&)            = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    // Parse the RTSP `Transport:` header, bind matching UDP sockets, and
    // fill `allocated` with the ports the OS gave us. Returns false only
    // on hard failure (socket exhaustion).
    bool setup(const std::string& transport_header, StreamPorts& allocated);

    void teardown();

    // Generated once, persists across RECORD / GET_PARAMETER / TEARDOWN.
    const std::string& session_id() const { return session_id_; }

private:
    // Client-advertised ports (we don't send to them in this iteration,
    // but we keep them logged for the capture analysis tonight).
    StreamPorts client_ports_{};

    socket_t data_sock_   = INVALID_SOCK;
    socket_t ctrl_sock_   = INVALID_SOCK;
    socket_t timing_sock_ = INVALID_SOCK;

    std::string session_id_;
};

} // namespace ap::airplay
