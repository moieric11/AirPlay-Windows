#include "crypto/fairplay.h"
#include "crypto/fairplay_blobs.h"
#include "log.h"

#include <cstring>

namespace ap::crypto {
namespace {

constexpr std::size_t kMsg1Len = 16;
constexpr std::size_t kMsg2Len = 142;
constexpr std::size_t kMsg3Len = 164;
constexpr std::size_t kMsg4Len = 32;

constexpr unsigned char kFplyMagic[4] = { 'F', 'P', 'L', 'Y' };

bool has_fply_magic(const unsigned char* in, std::size_t len) {
    return len >= 4 && std::memcmp(in, kFplyMagic, 4) == 0;
}

// Real compute functions live here once the user drops the UxPlay blob in.
// The stub path keeps framing correct so the state machine is still testable.
//
// TODO (when blobs are provisioned): port the two compute routines from
// UxPlay/lib/fairplay.c, lines matching `fairplay_setup_msg2` and
// `fairplay_setup_msg4`. They operate on fairplay_blobs::kTable /
// ::kAesKey / ::kReplyHeaderModeN.

bool compute_msg2_stub(uint8_t mode,
                       const unsigned char* /*msg1*/,
                       std::vector<unsigned char>& out) {
    out.assign(kMsg2Len, 0);
    const auto& header = (mode == 2)
        ? fairplay_blobs::kReplyHeaderMode2
        : fairplay_blobs::kReplyHeaderMode1;
    std::memcpy(out.data(), header, fairplay_blobs::kReplyHeaderSize);
    // body (128 bytes after the 14-byte header) intentionally left zero —
    // real lookup requires the Apple-extracted table.
    return true;
}

bool compute_msg4_stub(const unsigned char* /*msg3*/,
                       std::vector<unsigned char>& out) {
    out.assign(kMsg4Len, 0);
    return true;
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

    switch (len) {
        case kMsg1Len: return handle_msg1(in, len, out);
        case kMsg3Len: return handle_msg3(in, len, out);
        default:
            LOG_ERROR << "fp-setup: unexpected body length " << len;
            state_ = State::Failed;
            return false;
    }
}

bool FairPlaySession::handle_msg1(const unsigned char* in, std::size_t /*len*/,
                                  std::vector<unsigned char>& out) {
    if (state_ != State::Fresh) {
        LOG_WARN << "fp-setup msg1 received in state " << static_cast<int>(state_);
    }

    // byte 6 carries the FairPlay mode (1 or 2). Some iOS versions use 1,
    // some 2; we simply echo whichever we get.
    mode_ = in[6];
    LOG_INFO << "fp-setup msg1 (mode " << static_cast<int>(mode_) << ")";

    if (!compute_msg2_stub(mode_, in, out)) {
        state_ = State::Failed;
        return false;
    }

    state_ = State::ExpectMsg3;
    if (!fairplay_blobs::kPresent) {
        LOG_WARN << "fp-setup: returning STUB msg2 (FairPlay blobs absent) — "
                    "iOS will reject the handshake at this step; see docs/FAIRPLAY.md";
    }
    return true;
}

bool FairPlaySession::handle_msg3(const unsigned char* in, std::size_t /*len*/,
                                  std::vector<unsigned char>& out) {
    if (state_ != State::ExpectMsg3) {
        LOG_ERROR << "fp-setup msg3 received in wrong state "
                  << static_cast<int>(state_);
        state_ = State::Failed;
        return false;
    }

    LOG_INFO << "fp-setup msg3";
    if (!compute_msg4_stub(in, out)) {
        state_ = State::Failed;
        return false;
    }

    state_ = State::Done;
    if (!fairplay_blobs::kPresent) {
        LOG_WARN << "fp-setup: returning STUB msg4 (FairPlay blobs absent)";
    }
    return true;
}

} // namespace ap::crypto
