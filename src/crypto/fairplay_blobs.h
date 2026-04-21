#pragma once

#include <cstddef>

// FairPlay SAP "blobs". UxPlay takes a replay-only approach: instead of
// recomputing the 142-byte msg2 with Apple's obfuscated algorithm, it
// stores four pre-recorded responses captured from a real Apple TV and
// looks them up by the mode byte (msg1[14]). This covers all four modes
// iOS uses for mirroring and costs ~600 bytes total — no 16 KB table, no
// RSA private key, no playfair algorithm needed.
//
// A separate `fairplay_decrypt()` step (72→16 bytes) IS algorithmic and
// does require `lib/playfair/` from UxPlay. We don't implement it here;
// without it we can complete the RTSP handshake (iOS reaches ANNOUNCE /
// SETUP / RECORD and starts pushing RTP on UDP) but can't decrypt the
// media stream. That's a later milestone.
//
// The real blob is provisioned by creating `third_party/fairplay_blobs_real.cpp`
// (gitignored). See docs/FAIRPLAY.md.

namespace ap::crypto::fairplay_blobs {

extern const bool kPresent;

// Four pre-recorded msg2 responses indexed by msg1[14] (the mode byte).
// Every response starts with "FPLY 03 01 02 00 00 00 00 82 02 0<N>" where
// <N> is the mode index, followed by 128 bytes of mode-specific payload.
constexpr std::size_t kReplyCount = 4;
constexpr std::size_t kReplySize  = 142;
extern const unsigned char kReplyMessage[kReplyCount][kReplySize];

// 12-byte header prepended to msg4. The remaining 20 bytes of msg4 are
// copied verbatim from msg3[144..164] (iOS-provided data), giving a total
// response of 32 bytes.
constexpr std::size_t kFpHeaderSize = 12;
extern const unsigned char kFpHeader[kFpHeaderSize];

} // namespace ap::crypto::fairplay_blobs
