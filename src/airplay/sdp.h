#pragma once

#include <string>
#include <vector>

namespace ap::airplay {

// One m-line + the attributes that follow it in the SDP body carried by
// ANNOUNCE. We only keep the fields we'll eventually need for setup /
// decryption; everything else is discarded.
struct SdpMedia {
    std::string type;          // "audio" | "video"
    int         payload_type = 0;   // e.g. 96
    std::string rtpmap;        // "AppleLossless", "mpeg4-generic" …
    std::string fmtp;          // codec-specific parameters
};

struct SdpSession {
    std::string origin_ip;
    std::string connection_ip;
    std::vector<SdpMedia> medias;

    // Stream-encryption keys, iOS-provided. Base64 as-carried on the wire:
    //  - rsaaeskey_b64: the 128-bit AES session key, RSA-encrypted with
    //    Apple's FairPlay public key. Cannot be decrypted without the
    //    embedded private key (FairPlay blobs).
    //  - aesiv_b64: the 128-bit IV for AES-CBC on the RTP payload.
    std::string rsaaeskey_b64;
    std::string aesiv_b64;
};

// Lenient parser — ignores unknown lines, never throws. Returns false only
// when the body is obviously not SDP (no `v=` line). On success `out` may
// still be incomplete; callers must null-check fields they depend on.
bool parse_sdp(const std::string& text, SdpSession& out);

} // namespace ap::airplay
