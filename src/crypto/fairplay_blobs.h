#pragma once

#include <cstddef>

// Binary blobs extracted from Apple's AirPlayReceiver framework. These are
// NOT checked into the repository because of their copyright status; the
// build links against a zero-filled stub (`third_party/fairplay_blobs_stub.cpp`)
// by default. To enable real FairPlay SAP, replace the stub with the
// corresponding file from UxPlay — see `docs/FAIRPLAY.md`.
//
// The sizes below mirror the public reverse-engineered layout (RPiPlay /
// UxPlay / playfair). A build-time assertion verifies they match.

namespace ap::crypto::fairplay_blobs {

// Flipped to `true` by a real blob file. Stub keeps it false so callers can
// log a clear "FairPlay not provisioned" warning instead of silently
// returning garbage.
extern const bool kPresent;

// ~16 KB lookup table used to build the 128-byte body of msg2.
constexpr std::size_t kTableSize = 16 * 1024;
extern const unsigned char kTable[kTableSize];

// 16-byte AES-128 key used to decrypt msg3 payload into msg4.
constexpr std::size_t kAesKeySize = 16;
extern const unsigned char kAesKey[kAesKeySize];

// 16-byte header mixed into msg2. Real value comes from UxPlay.
constexpr std::size_t kReplyHeaderSize = 14;
extern const unsigned char kReplyHeaderMode1[kReplyHeaderSize];
extern const unsigned char kReplyHeaderMode2[kReplyHeaderSize];

} // namespace ap::crypto::fairplay_blobs
