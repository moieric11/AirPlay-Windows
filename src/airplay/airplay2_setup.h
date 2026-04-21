#pragma once

// AirPlay 2 SETUP: binary-plist body instead of the legacy RTSP
// `Transport:` header. Two request flavours arrive on the same
// connection:
//
//   1. "Session setup" — dictionary without a `streams` key. iOS tells
//      us its sessionUUID, timingProtocol, etc. We answer with the
//      two UDP ports we bound for event + timing.
//
//   2. "Stream setup" — dictionary with `streams: [ ... ]`. Each
//      entry describes one media stream (mirroring video, buffered
//      audio, realtime audio). For each we bind a (data, control)
//      UDP pair and echo the allocated ports in the response.
//
// Parsing and response building live here; the handler in routes.cpp
// just picks which to call based on whether `streams` is present.

#include "airplay/streams.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ap::airplay {

// One `streams[]` entry.
struct StreamRequest {
    int      type           = 0;   // 96 = buffered audio, 103 = realtime audio, 110 = mirroring video
    int      control_port   = 0;   // iOS's own control port (info, we don't need to bind to it)
    uint64_t stream_conn_id = 0;   // opaque ID echoed back
};

// Parsed high-level view of the request body.
struct Airplay2SetupRequest {
    bool                       has_streams = false;
    std::vector<StreamRequest> streams;
};

// Returns true iff body is a valid bplist00 and we could parse enough.
bool parse_airplay2_setup(const unsigned char* body, std::size_t len,
                          Airplay2SetupRequest& out);

// Build the plist response for a "session setup" (no streams). The
// caller has already bound UDP sockets for `event` and `timing`.
std::vector<unsigned char>
build_airplay2_setup_session_response(uint16_t event_port, uint16_t timing_port);

// Build the plist response for a "stream setup". `allocated[i]` is the
// (data, control) pair for `request[i]`.
struct StreamAllocation {
    uint16_t data_port    = 0;
    uint16_t control_port = 0;
};
std::vector<unsigned char>
build_airplay2_setup_streams_response(
    const std::vector<StreamRequest>&    requests,
    const std::vector<StreamAllocation>& allocated);

} // namespace ap::airplay
