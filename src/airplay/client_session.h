#pragma once

#include "airplay/sdp.h"
#include "airplay/streams.h"
#include "crypto/fairplay.h"
#include "crypto/pair_verify.h"

#include <memory>

namespace ap::crypto { class Identity; }

namespace ap::airplay {

// Per-TCP-connection state. The dispatcher gets a reference so handlers
// spanning multiple requests on the same connection (pair-verify rounds,
// fp-setup rounds, ANNOUNCE→SETUP→RECORD→TEARDOWN) can keep their state.
//
// Sessions are allocated lazily to avoid paying their cost on connections
// that never reach the corresponding stage.
struct ClientSession {
    ClientSession(const ap::crypto::Identity& id, std::string peer_ip)
        : identity(id), remote_ip(std::move(peer_ip)) {}

    const ap::crypto::Identity& identity;

    // iOS IP only (no port). Used as destination for the NTP client probes
    // we send once SETUP is done.
    std::string remote_ip;

    std::unique_ptr<ap::crypto::PairVerifySession> pair_verify;
    std::unique_ptr<ap::crypto::FairPlaySession>   fairplay;

    std::unique_ptr<SdpSession>    sdp;       // populated by ANNOUNCE
    std::unique_ptr<StreamSession> streams;   // populated by SETUP

    // Stream decryption keys, populated during AirPlay 2 session SETUP once
    // fairplay_decrypt() turns the 72-byte ekey into a 16-byte AES key.
    // Combined with aes_iv and the per-stream streamConnectionID, these
    // derive the AES-CTR context that decrypts the H.264 NAL units.
    std::vector<unsigned char> aes_key;    // 16 bytes
    std::vector<unsigned char> aes_iv;     // 16 bytes
};

} // namespace ap::airplay
