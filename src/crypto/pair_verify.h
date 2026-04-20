#pragma once

#include "crypto/identity.h"

#include <cstddef>
#include <vector>

// Forward-declare OpenSSL types so this header stays self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::crypto {

// Two-round pair-verify state machine, copied algorithmically from UxPlay's
// lib/pairing.c. Lifetime is tied to a single TCP connection — the AES-CTR
// counter must persist between round 1 (server encrypts its signature) and
// round 2 (server decrypts the client's signature).
//
// Round 1 request  (68 bytes) : flags(4) | client_x25519_pub(32) | client_ed25519_pub(32)
// Round 1 response (96 bytes) : server_x25519_pub(32)           | enc(server_sig)(64)
// Round 2 request  (68 bytes) : flags(4) | enc(client_sig)(64)
// Round 2 response ( 0 bytes) : empty 200 OK on success
//
// Signature data (both directions) : own_x25519_pub || peer_x25519_pub (64 bytes)
// Keys           : SHA-512("Pair-Verify-AES-Key"||shared_secret)[0..16]
//                  SHA-512("Pair-Verify-AES-IV" ||shared_secret)[0..16]
class PairVerifySession {
public:
    enum class State { Fresh, Round1Done, Verified, Failed };

    explicit PairVerifySession(const Identity& identity);
    ~PairVerifySession();

    PairVerifySession(const PairVerifySession&)            = delete;
    PairVerifySession& operator=(const PairVerifySession&) = delete;

    // Returns true iff msg1 was well-formed and keys derived. `out` receives
    // the 96-byte response body on success.
    bool handle_message1(const unsigned char* in, std::size_t len,
                         std::vector<unsigned char>& out);

    // Returns true iff the client's signature verifies against the pubkey
    // presented in round 1. No response body on success.
    bool handle_message2(const unsigned char* in, std::size_t len);

    State state() const { return state_; }

private:
    bool derive_aes_key_iv(const unsigned char* shared_secret,
                           std::vector<unsigned char>& aes_key,
                           std::vector<unsigned char>& aes_iv);

    const Identity& identity_;
    State state_{State::Fresh};

    std::vector<unsigned char> server_x25519_pub_;   // 32
    std::vector<unsigned char> client_x25519_pub_;   // 32
    std::vector<unsigned char> client_ed25519_pub_;  // 32

    EVP_CIPHER_CTX* aes_ctx_{nullptr};
};

} // namespace ap::crypto
