#include "crypto/pair_verify.h"
#include "log.h"

#include <cstring>
#include <string>

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>

namespace ap::crypto {
namespace {

// Key-derivation salts used by RPiPlay / UxPlay / shairport-sync.
// Format: salt_string || shared_secret, SHA-512, take first 16 bytes.
constexpr char kAesKeySalt[] = "Pair-Verify-AES-Key";
constexpr char kAesIvSalt[]  = "Pair-Verify-AES-IV";

// Helpers -------------------------------------------------------------------

std::vector<unsigned char> x25519_derive(EVP_PKEY* our_priv,
                                         const unsigned char* peer_pub,
                                         std::size_t peer_pub_len) {
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                 peer_pub, peer_pub_len);
    if (!peer) {
        LOG_ERROR << "EVP_PKEY_new_raw_public_key(X25519 peer) failed";
        return {};
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(our_priv, nullptr);
    std::vector<unsigned char> secret(32);
    std::size_t secret_len = secret.size();

    bool ok = ctx &&
              EVP_PKEY_derive_init(ctx) == 1 &&
              EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
              EVP_PKEY_derive(ctx, secret.data(), &secret_len) == 1 &&
              secret_len == 32;

    if (ctx)  EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);

    if (!ok) {
        LOG_ERROR << "X25519 ECDH derive failed";
        return {};
    }
    return secret;
}

std::vector<unsigned char> extract_pub(EVP_PKEY* pkey) {
    std::vector<unsigned char> pub(32);
    std::size_t len = pub.size();
    if (EVP_PKEY_get_raw_public_key(pkey, pub.data(), &len) != 1 || len != 32) {
        LOG_ERROR << "EVP_PKEY_get_raw_public_key(X25519) failed";
        return {};
    }
    return pub;
}

bool ed25519_verify(const unsigned char* pub_key,
                    const unsigned char* data, std::size_t data_len,
                    const unsigned char* sig,  std::size_t sig_len) {
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                               pub_key, 32);
    if (!pk) return false;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    bool ok = mdctx &&
              EVP_DigestVerifyInit(mdctx, nullptr, nullptr, nullptr, pk) == 1 &&
              EVP_DigestVerify(mdctx, sig, sig_len, data, data_len) == 1;
    if (mdctx) EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pk);
    return ok;
}

} // namespace

// --- PairVerifySession implementation --------------------------------------

PairVerifySession::PairVerifySession(const Identity& identity)
    : identity_(identity) {}

PairVerifySession::~PairVerifySession() {
    if (aes_ctx_) EVP_CIPHER_CTX_free(aes_ctx_);
}

namespace {
bool sha512_prefixed(const char* salt, std::size_t salt_len,
                     const unsigned char* secret, std::size_t secret_len,
                     unsigned char out[64]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned int outlen = 0;
    bool ok = ctx &&
        EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, salt,   salt_len)       == 1 &&
        EVP_DigestUpdate(ctx, secret, secret_len)     == 1 &&
        EVP_DigestFinal_ex(ctx, out, &outlen)         == 1 &&
        outlen == 64;
    if (ctx) EVP_MD_CTX_free(ctx);
    return ok;
}
} // namespace

bool PairVerifySession::derive_aes_key_iv(const unsigned char* shared_secret,
                                          std::vector<unsigned char>& aes_key,
                                          std::vector<unsigned char>& aes_iv) {
    // aes_key[0..16] = SHA-512(salt_key || shared_secret)[0..16]
    // aes_iv [0..16] = SHA-512(salt_iv  || shared_secret)[0..16]
    unsigned char digest[64];

    if (!sha512_prefixed(kAesKeySalt, sizeof(kAesKeySalt) - 1,
                         shared_secret, 32, digest)) return false;
    aes_key.assign(digest, digest + 16);

    if (!sha512_prefixed(kAesIvSalt, sizeof(kAesIvSalt) - 1,
                         shared_secret, 32, digest)) return false;
    aes_iv.assign(digest, digest + 16);
    return true;
}

bool PairVerifySession::handle_message1(const unsigned char* in,
                                        std::size_t len,
                                        std::vector<unsigned char>& out) {
    if (state_ != State::Fresh) {
        LOG_WARN << "pair-verify msg1 received in state " << static_cast<int>(state_);
    }
    if (len < 4 + 32 + 32) {
        LOG_ERROR << "pair-verify msg1: short body (" << len << " bytes)";
        state_ = State::Failed;
        return false;
    }

    const unsigned char* client_x25519 = in + 4;
    const unsigned char* client_ed25519 = in + 4 + 32;
    client_x25519_pub_.assign(client_x25519, client_x25519 + 32);
    client_ed25519_pub_.assign(client_ed25519, client_ed25519 + 32);

    // 1. Generate ephemeral X25519 keypair
    EVP_PKEY* ephemeral = EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519");
    if (!ephemeral) {
        LOG_ERROR << "X25519 keygen failed";
        state_ = State::Failed;
        return false;
    }
    server_x25519_pub_ = extract_pub(ephemeral);
    if (server_x25519_pub_.empty()) {
        EVP_PKEY_free(ephemeral);
        state_ = State::Failed;
        return false;
    }

    // 2. ECDH
    auto shared = x25519_derive(ephemeral, client_x25519, 32);
    EVP_PKEY_free(ephemeral);
    if (shared.size() != 32) {
        state_ = State::Failed;
        return false;
    }
    // Keep a copy — AirPlay 2 SETUP re-hashes the fairplay-decrypted AES
    // stream key against this secret to obtain the actual stream key.
    ecdh_secret_ = shared;

    // 3. Derive AES key / IV
    std::vector<unsigned char> aes_key, aes_iv;
    if (!derive_aes_key_iv(shared.data(), aes_key, aes_iv)) {
        state_ = State::Failed;
        return false;
    }

    // 4. Build signature data = server_x25519_pub || client_x25519_pub
    std::vector<unsigned char> sig_data;
    sig_data.reserve(64);
    sig_data.insert(sig_data.end(), server_x25519_pub_.begin(), server_x25519_pub_.end());
    sig_data.insert(sig_data.end(), client_x25519_pub_.begin(), client_x25519_pub_.end());

    auto sig = identity_.sign(sig_data.data(), sig_data.size());
    if (sig.size() != 64) {
        LOG_ERROR << "Ed25519 sign returned " << sig.size() << " bytes (want 64)";
        state_ = State::Failed;
        return false;
    }

    // 5. Set up AES-128-CTR context — counter state persists into round 2.
    aes_ctx_ = EVP_CIPHER_CTX_new();
    if (!aes_ctx_ ||
        EVP_EncryptInit_ex(aes_ctx_, EVP_aes_128_ctr(), nullptr,
                           aes_key.data(), aes_iv.data()) != 1) {
        LOG_ERROR << "EVP_EncryptInit_ex(aes-128-ctr) failed";
        state_ = State::Failed;
        return false;
    }

    std::vector<unsigned char> enc_sig(64);
    int outlen = 0;
    if (EVP_EncryptUpdate(aes_ctx_, enc_sig.data(), &outlen,
                          sig.data(), static_cast<int>(sig.size())) != 1 ||
        outlen != 64) {
        LOG_ERROR << "AES-CTR encrypt of server sig failed (" << outlen << "/64)";
        state_ = State::Failed;
        return false;
    }

    // 6. Compose response body: server_x25519_pub(32) || enc_sig(64)
    out.clear();
    out.reserve(96);
    out.insert(out.end(), server_x25519_pub_.begin(), server_x25519_pub_.end());
    out.insert(out.end(), enc_sig.begin(), enc_sig.end());

    state_ = State::Round1Done;
    LOG_INFO << "pair-verify round1: derived keys, returning 96-byte response";
    return true;
}

bool PairVerifySession::handle_message2(const unsigned char* in,
                                        std::size_t len) {
    if (state_ != State::Round1Done) {
        LOG_ERROR << "pair-verify msg2 received in wrong state "
                  << static_cast<int>(state_);
        state_ = State::Failed;
        return false;
    }
    if (len < 4 + 64) {
        LOG_ERROR << "pair-verify msg2: short body (" << len << " bytes)";
        state_ = State::Failed;
        return false;
    }

    const unsigned char* enc_client_sig = in + 4;

    // Decrypt — AES-CTR is symmetric; continue the counter from round 1.
    std::vector<unsigned char> client_sig(64);
    int outlen = 0;
    if (EVP_EncryptUpdate(aes_ctx_, client_sig.data(), &outlen,
                          enc_client_sig, 64) != 1 || outlen != 64) {
        LOG_ERROR << "AES-CTR decrypt of client sig failed";
        state_ = State::Failed;
        return false;
    }

    // Verify: client signs client_x25519_pub || server_x25519_pub
    std::vector<unsigned char> sig_data;
    sig_data.reserve(64);
    sig_data.insert(sig_data.end(), client_x25519_pub_.begin(), client_x25519_pub_.end());
    sig_data.insert(sig_data.end(), server_x25519_pub_.begin(), server_x25519_pub_.end());

    if (!ed25519_verify(client_ed25519_pub_.data(),
                        sig_data.data(), sig_data.size(),
                        client_sig.data(), client_sig.size())) {
        LOG_WARN << "pair-verify: client Ed25519 signature REJECTED";
        state_ = State::Failed;
        return false;
    }

    state_ = State::Verified;
    LOG_INFO << "pair-verify: client signature OK — session VERIFIED";
    return true;
}

} // namespace ap::crypto
