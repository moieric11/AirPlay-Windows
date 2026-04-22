#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ap::airplay {

// Fields we care about from a DAAP/DMAP "mlit" record (the payload iOS
// sends in SET_PARAMETER with Content-Type: application/x-dmap-tagged).
// Everything is optional — fields we don't find stay default-constructed.
struct DaapMetadata {
    std::string title;      // minm
    std::string artist;     // asar
    std::string album;      // asal
    uint32_t    duration_ms = 0;  // astm
};

// Lenient parser — walks the DMAP tree looking for the string fields
// above. Returns true iff `body` starts with a recognisable "mlit"
// container (that's the only shape iOS sends us).
bool parse_daap_mlit(const unsigned char* body, std::size_t len,
                     DaapMetadata& out);

} // namespace ap::airplay
