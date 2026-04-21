#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace ap::airplay {

// TCP accept loop for AirPlay 2 mirror stream (type 110).
//
// Ported from UxPlay's raop_rtp_mirror.c: the mirror video stream is NOT
// a UDP RTP flow — it's a TCP socket. iOS opens one connection to our
// allocated data port right after SETUP, then streams H.264 NAL units
// over the TCP byte stream until TEARDOWN.
//
// For now we only drain the bytes and count them. Decrypting + decoding
// comes later (requires level-B playfair for the AES key from ekey, plus
// an H.264 decoder).
class MirrorListener {
public:
    MirrorListener();
    ~MirrorListener();

    MirrorListener(const MirrorListener&)            = delete;
    MirrorListener& operator=(const MirrorListener&) = delete;

    // Bind a TCP listener on 0.0.0.0:0, start the accept/recv thread, and
    // return the allocated port through `port`. Returns false on failure.
    bool start(uint16_t& port);

    // Signals the thread to exit, shuts down sockets, joins the thread.
    void stop();

private:
    void run();

    socket_t          listen_sock_{INVALID_SOCK};
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace ap::airplay
