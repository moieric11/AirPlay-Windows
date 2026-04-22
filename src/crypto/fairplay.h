#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ap::crypto {

// FairPlay SAP v1 two-round handshake, dispatched over two POST /fp-setup
// requests on the same TCP connection. Algorithm layout reverse-engineered
// in RPiPlay / UxPlay / playfair; the cryptographic blobs live in
// fairplay_blobs.h (stubbed by default — see docs/FAIRPLAY.md).
//
// Round 1: client sends 16-byte msg1, server answers 142-byte msg2.
// Round 2: client sends 164-byte msg3, server answers 32-byte msg4.
//
// Length-based dispatch is safe because msg1 and msg3 never collide: msg1
// always has "FPLY" magic + 0x03 version in bytes 0-4, msg3 does too but
// differs in length.
class FairPlaySession {
public:
    enum class State { Fresh, ExpectMsg3, Done, Failed };

    // Process one FP-setup body. Returns true iff `out` is safe to send
    // back to the client (even when the underlying blobs are stubbed —
    // we still emit a well-framed response so iOS gets a clean failure
    // rather than a stalled socket).
    bool process(const unsigned char* in, std::size_t len,
                 std::vector<unsigned char>& out);

    State state() const { return state_; }

private:
    bool handle_msg1(const unsigned char* in, std::size_t len,
                     std::vector<unsigned char>& out);
    bool handle_msg3(const unsigned char* in, std::size_t len,
                     std::vector<unsigned char>& out);

    State   state_{State::Fresh};
    uint8_t mode_{0};         // selected from msg1[14]; 0..3

    // msg3 payload, kept so a future fairplay_decrypt() step can combine it
    // with the 72-byte rsaaeskey from ANNOUNCE to recover the 16-byte AES
    // session key. Populated on successful handle_msg3.
    std::vector<unsigned char> keymsg_;

public:
    // Exposes the 164-byte msg3 payload once the handshake has completed,
    // for the stream-key decryption step. Returns an empty span while
    // state() != Done.
    const std::vector<unsigned char>& keymsg() const { return keymsg_; }

    // Derive the 16-byte AES session key from the 72-byte RSA-wrapped ekey
    // iOS sends in SETUP. Wraps UxPlay's playfair_decrypt(); only available
    // when the playfair library is present at build time.
    //
    // Returns a 16-byte vector on success, empty on failure (state != Done,
    // wrong ekey size, or playfair not provisioned — check logs for cause).
    // Must be called after the handshake has completed (state() == Done).
    std::vector<unsigned char> decrypt_stream_key(
        const std::vector<unsigned char>& ekey) const;
};

} // namespace ap::crypto
