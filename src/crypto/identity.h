#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare OpenSSL type to keep the header consumer-free of openssl headers.
struct evp_pkey_st;
typedef struct evp_pkey_st EVP_PKEY;

namespace ap::crypto {

// Persistent Ed25519 identity — used to sign the pair-verify challenge and
// advertised as `pk` in the /info plist. The private seed is stored on disk
// so the same receiver always presents the same identity (iOS caches it).
//
// Modelled after UxPlay's pairing_session_init() + keypair_generate().
class Identity {
public:
    // Load from `path` if it exists (32-byte raw seed), otherwise generate
    // a fresh keypair and persist it. Returns nullptr on hard failure.
    static std::unique_ptr<Identity> load_or_create(const std::string& path);

    ~Identity();
    Identity(const Identity&)            = delete;
    Identity& operator=(const Identity&) = delete;

    // 32-byte raw Ed25519 public key.
    const std::vector<unsigned char>& public_key() const { return pub_; }

    // Sign `len` bytes of `data`, returns the 64-byte Ed25519 signature.
    // Empty vector on error.
    std::vector<unsigned char> sign(const unsigned char* data, std::size_t len) const;

    // OpenSSL handle for callers that need it (e.g. pair-verify code).
    EVP_PKEY* pkey() const { return pkey_; }

private:
    Identity() = default;
    EVP_PKEY* pkey_ = nullptr;
    std::vector<unsigned char> pub_;
};

} // namespace ap::crypto
