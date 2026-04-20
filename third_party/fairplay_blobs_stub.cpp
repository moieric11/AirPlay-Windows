// Stub implementation of the FairPlay SAP binary blobs. See
// `docs/FAIRPLAY.md` for how to replace this file with the real data
// extracted from Apple's AirPlayReceiver framework (as shipped by UxPlay
// under `lib/fairplay_playfair.c`).
//
// With this stub in place, POST /fp-setup returns correctly-framed but
// cryptographically wrong responses: iOS will fail the handshake at this
// step. Everything up to and including pair-verify continues to work.

#include "crypto/fairplay_blobs.h"

namespace ap::crypto::fairplay_blobs {

const bool kPresent = false;

const unsigned char kTable[kTableSize] = { 0 };
const unsigned char kAesKey[kAesKeySize] = { 0 };

// The mode-specific reply headers begin with the "FPLY" magic and a
// version byte; the trailing bytes come from Apple's framework. Even the
// stub keeps the magic so Wireshark dumps are still recognisable.
const unsigned char kReplyHeaderMode1[kReplyHeaderSize] = {
    0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x82, 0x00, 0x00,
};
const unsigned char kReplyHeaderMode2[kReplyHeaderSize] = {
    0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x82, 0x00, 0x00,
};

} // namespace ap::crypto::fairplay_blobs
