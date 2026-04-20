#pragma once

#include "crypto/pair_verify.h"

#include <memory>

namespace ap::crypto { class Identity; }

namespace ap::airplay {

// Per-TCP-connection state. The dispatcher gets a reference so that
// handlers spanning multiple requests on the same connection (pair-verify
// rounds 1+2, later stream-setup…) can keep their state.
//
// The PairVerifySession is created lazily on the first /pair-verify request
// to avoid allocating an OpenSSL CTX for connections that never pair.
struct ClientSession {
    explicit ClientSession(const ap::crypto::Identity& id) : identity(id) {}

    const ap::crypto::Identity& identity;
    std::unique_ptr<ap::crypto::PairVerifySession> pair_verify;
};

} // namespace ap::airplay
