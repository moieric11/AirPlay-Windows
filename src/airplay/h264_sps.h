#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ap::airplay {

// Fields we care about from an H.264 Sequence Parameter Set (SPS).
// Full SPS parsing covers ~30 fields; we only keep what's useful for a
// human-readable log line about the incoming stream.
struct SpsInfo {
    uint8_t  profile_idc = 0;
    uint8_t  level_idc   = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;
    bool     interlaced  = false;   // !frame_mbs_only_flag
};

// Best-effort SPS parser — returns true on success, fills `out`. On any
// malformed bit sequence it returns false rather than reading past the
// buffer. Handles the H.264 "emulation prevention byte" (0x03) removal
// before bit-parsing.
//
// `sps` must point to the SPS RBSP starting with the NAL header byte
// (the one whose low 5 bits equal 7).
bool parse_h264_sps(const uint8_t* sps, std::size_t len, SpsInfo& out);

// "High", "Main", "Baseline", ... or "profile_idc <N>" if unknown.
std::string profile_name(uint8_t profile_idc);

} // namespace ap::airplay
