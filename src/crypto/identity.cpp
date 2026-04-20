#include "crypto/identity.h"
#include "log.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ap::crypto {
namespace {

// Load a 32-byte Ed25519 seed from `path`. Returns empty vector on miss.
std::vector<unsigned char> read_seed(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (buf.size() != 32) {
        LOG_WARN << "identity file " << path << " has unexpected size "
                 << buf.size() << " (expected 32) — regenerating";
        return {};
    }
    return buf;
}

bool write_seed(const std::string& path, const std::vector<unsigned char>& seed) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        LOG_ERROR << "cannot open " << path << " for writing";
        return false;
    }
    f.write(reinterpret_cast<const char*>(seed.data()),
            static_cast<std::streamsize>(seed.size()));
    return f.good();
}

std::vector<unsigned char> extract_pub(EVP_PKEY* pkey) {
    std::vector<unsigned char> pub(32);
    std::size_t len = pub.size();
    if (EVP_PKEY_get_raw_public_key(pkey, pub.data(), &len) != 1 || len != 32) {
        LOG_ERROR << "EVP_PKEY_get_raw_public_key failed";
        return {};
    }
    return pub;
}

} // namespace

std::unique_ptr<Identity> Identity::load_or_create(const std::string& path) {
    auto id = std::unique_ptr<Identity>(new Identity());

    auto seed = read_seed(path);
    if (seed.empty()) {
        seed.resize(32);
        if (RAND_bytes(seed.data(), 32) != 1) {
            LOG_ERROR << "RAND_bytes failed";
            return nullptr;
        }
        if (!write_seed(path, seed)) {
            LOG_WARN << "identity seed generated but not persisted; "
                        "receiver identity will change at next launch";
        } else {
            LOG_INFO << "new identity seed written to " << path;
        }
    } else {
        LOG_INFO << "loaded identity seed from " << path;
    }

    id->pkey_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                             seed.data(), seed.size());
    if (!id->pkey_) {
        LOG_ERROR << "EVP_PKEY_new_raw_private_key(ED25519) failed";
        return nullptr;
    }

    id->pub_ = extract_pub(id->pkey_);
    if (id->pub_.empty()) return nullptr;

    return id;
}

Identity::~Identity() {
    if (pkey_) EVP_PKEY_free(pkey_);
}

std::vector<unsigned char> Identity::sign(const unsigned char* data,
                                          std::size_t len) const {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    std::vector<unsigned char> sig(64);
    std::size_t sig_len = sig.size();

    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey_) != 1 ||
        EVP_DigestSign(ctx, sig.data(), &sig_len, data, len) != 1) {
        LOG_ERROR << "EVP_DigestSign(Ed25519) failed";
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    sig.resize(sig_len);
    return sig;
}

} // namespace ap::crypto
