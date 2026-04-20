#pragma once

#include "airplay/rtsp_parser.h"

#include <string>

namespace ap::airplay {

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
};

// Build a response for a single incoming request. Stubs log and answer 501
// for handlers that are not implemented yet.
Response dispatch(const DeviceContext& ctx, const Request& req);

} // namespace ap::airplay
