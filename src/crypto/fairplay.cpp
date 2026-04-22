#include "crypto/fairplay.h"
#include "crypto/fairplay_blobs.h"
#include "log.h"

#include <cstring>

#if defined(HAVE_PLAYFAIR)
extern "C" {
    // Signature from UxPlay/lib/playfair/playfair.h (GPL-3.0).
    void playfair_decrypt(unsigned char* message3,
                          unsigned char* cipherText,
                          unsigned char* keyOut);
}
#endif

// FairPlay SAP setup/handshake — ported from UxPlay's lib/fairplay_playfair.c
// (GPL-3.0). UxPlay's approach is "replay", not "compute": the 142-byte msg2
// is looked up in a 4-entry table indexed by the mode byte of msg1, and
// msg4 is a fixed 12-byte header followed by 20 bytes copied from msg3.
// No RSA private key, no 16 KB obfuscated table, no playfair algorithm —
// all of that only becomes necessary for fairplay_decrypt() (stream key
// decryption), which is a later step handled on top of this.

namespace ap::crypto {
namespace {

constexpr std::size_t kMsg1Len = 16;
constexpr std::size_t kMsg2Len = 142;
constexpr std::size_t kMsg3Len = 164;
constexpr std::size_t kMsg4Len = 32;

constexpr unsigned char kFplyMagic[4] = { 'F', 'P', 'L', 'Y' };
constexpr unsigned char kFplyVersion  = 0x03;

bool has_fply_magic(const unsigned char* in, std::size_t len) {
    return len >= 4 && std::memcmp(in, kFplyMagic, 4) == 0;
}

} // namespace

bool FairPlaySession::process(const unsigned char* in, std::size_t len,
                              std::vector<unsigned char>& out) {
    if (!in || len == 0) {
        LOG_ERROR << "fp-setup: empty body";
        state_ = State::Failed;
        return false;
    }
    if (!has_fply_magic(in, len)) {
        LOG_ERROR << "fp-setup: missing FPLY magic";
        state_ = State::Failed;
        return false;
    }
    if (len >= 5 && in[4] != kFplyVersion) {
        LOG_ERROR << "fp-setup: unsupported FairPlay version " << static_cast<int>(in[4]);
        state_ = State::Failed;
        return false;
    }

    switch (len) {
        case kMsg1Len: return handle_msg1(in, len, out);
        case kMsg3Len: return handle_msg3(in, len, out);
        default:
            LOG_ERROR << "fp-setup: unexpected body length " << len;
            state_ = State::Failed;
            return false;
    }
}

// msg1 → msg2 : mode byte at offset 14 selects one of the 4 replay responses.
bool FairPlaySession::handle_msg1(const unsigned char* in, std::size_t /*len*/,
                                  std::vector<unsigned char>& out) {
    if (state_ != State::Fresh) {
        LOG_WARN << "fp-setup msg1 received in state " << static_cast<int>(state_);
    }

    mode_ = in[14];
    if (mode_ >= fairplay_blobs::kReplyCount) {
        LOG_ERROR << "fp-setup msg1: unknown mode " << static_cast<int>(mode_)
                  << " (only 0..3 supported)";
        state_ = State::Failed;
        return false;
    }

    out.assign(fairplay_blobs::kReplyMessage[mode_],
               fairplay_blobs::kReplyMessage[mode_] + kMsg2Len);

    state_ = State::ExpectMsg3;
    if (!fairplay_blobs::kPresent) {
        LOG_WARN << "fp-setup: STUB msg2 (FairPlay blobs absent) — iOS will "
                    "reject here; see docs/FAIRPLAY.md";
    } else {
        LOG_INFO << "fp-setup msg1 (mode " << static_cast<int>(mode_)
                 << ") → 142B replay";
    }
    return true;
}

// msg3 → msg4 : 12-byte fp_header prefix + 20 bytes copied from msg3[144..164].
// The full 164-byte msg3 is kept on the session so a later decrypt step can
// combine it with the 72-byte RSAAES key from ANNOUNCE.
std::vector<unsigned char> FairPlaySession::decrypt_stream_key(
    const std::vector<unsigned char>& ekey) const {
#if !defined(HAVE_PLAYFAIR)
    (void)ekey;
    LOG_WARN << "decrypt_stream_key: playfair not provisioned at build time";
    return {};
#else
    if (state_ != State::Done) {
        LOG_ERROR << "decrypt_stream_key: handshake not complete (state="
                  << static_cast<int>(state_) << ')';
        return {};
    }
    if (keymsg_.size() != kMsg3Len) {
        LOG_ERROR << "decrypt_stream_key: keymsg size " << keymsg_.size();
        return {};
    }
    if (ekey.size() != 72) {
        LOG_ERROR << "decrypt_stream_key: ekey size " << ekey.size() << " (want 72)";
        return {};
    }

    // playfair_decrypt mutates neither input; the non-const signature is a
    // historical artifact from UxPlay's C API. We pass copies to stay safe.
    std::vector<unsigned char> keymsg_copy = keymsg_;
    std::vector<unsigned char> ekey_copy   = ekey;
    std::vector<unsigned char> out(16, 0);

    playfair_decrypt(keymsg_copy.data(), ekey_copy.data(), out.data());
    return out;
#endif
}

bool FairPlaySession::handle_msg3(const unsigned char* in, std::size_t /*len*/,
                                  std::vector<unsigned char>& out) {
    if (state_ != State::ExpectMsg3) {
        LOG_ERROR << "fp-setup msg3 received in wrong state "
                  << static_cast<int>(state_);
        state_ = State::Failed;
        return false;
    }

    keymsg_.assign(in, in + kMsg3Len);

    out.resize(kMsg4Len);
    std::memcpy(out.data(),     fairplay_blobs::kFpHeader, fairplay_blobs::kFpHeaderSize);
    std::memcpy(out.data() + 12, in + 144, 20);

    state_ = State::Done;
    if (!fairplay_blobs::kPresent) {
        LOG_WARN << "fp-setup msg3 → STUB msg4 (FairPlay blobs absent)";
    } else {
        LOG_INFO << "fp-setup msg3 → 32B response (fp_header + msg3[144..164])";
    }
    return true;
}

} // namespace ap::crypto
