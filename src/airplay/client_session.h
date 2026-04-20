#pragma once

#include "crypto/fairplay.h"
#include "crypto/pair_verify.h"

#include <memory>

namespace ap::crypto { class Identity; }

namespace ap::airplay {

// Per-TCP-connection state. The dispatcher gets a reference so that
// handlers spanning multiple requests on the same connection (pair-verify
// rounds 1+2, fp-setup msg1+msg3, later stream-setup…) can keep their state.
//
// Sessions are created lazily to avoid paying their cost on connections
// that never reach that stage.
struct ClientSession {
    explicit ClientSession(const ap::crypto::Identity& id) : identity(id) {}

    const ap::crypto::Identity& identity;
    std::unique_ptr<ap::crypto::PairVerifySession> pair_verify;
    std::unique_ptr<ap::crypto::FairPlaySession>   fairplay;
};

} // namespace ap::airplay
