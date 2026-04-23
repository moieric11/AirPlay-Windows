#pragma once

#include "airplay/sdp.h"
#include "airplay/streams.h"
#include "crypto/fairplay.h"
#include "crypto/pair_verify.h"

#include <memory>

namespace ap::crypto { class Identity; }
namespace ap::video  { class VideoRenderer; }

namespace ap::airplay {

// Per-TCP-connection state. The dispatcher gets a reference so handlers
// spanning multiple requests on the same connection (pair-verify rounds,
// fp-setup rounds, ANNOUNCE→SETUP→RECORD→TEARDOWN) can keep their state.
//
// Sessions are allocated lazily to avoid paying their cost on connections
// that never reach the corresponding stage.
struct ClientSession {
    ClientSession(const ap::crypto::Identity& id, std::string peer_ip,
                  int socket_fd = -1,
                  ap::video::VideoRenderer* r = nullptr)
        : identity(id), remote_ip(std::move(peer_ip)),
          fd(socket_fd), renderer(r) {}

    const ap::crypto::Identity& identity;

    // iOS IP only (no port). Used as destination for the NTP client probes
    // we send once SETUP is done.
    std::string remote_ip;

    // TCP socket fd of the underlying control connection. Used by
    // handle_reverse to register itself in ReverseChannelRegistry so
    // handle_play can push a POST /event (FCUP Request) back on the
    // right socket. -1 when unknown (legacy callers).
    int fd{-1};

    // For the /reverse connection: session id iOS sent in the
    // X-Apple-Session-ID header. We remember it so handle_client can
    // unregister the fd from ReverseChannelRegistry when the TCP
    // connection closes.
    std::string reverse_session_id;

    // Non-owning; handed down to the StreamSession on first SETUP so the
    // mirror stream's decoded frames can be drawn.
    ap::video::VideoRenderer* renderer{nullptr};

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

    // Remember the last "progress: start/curr/end" values so the next
    // push can spot seeks (curr jumps within the same track) and track
    // changes (start itself changes). 0 means nothing seen yet.
    unsigned long long last_progress_start = 0;
    unsigned long long last_progress_curr  = 0;
};

} // namespace ap::airplay
