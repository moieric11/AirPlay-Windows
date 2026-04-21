#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace ap::airplay {

// AirPlay NTP client — ported from UxPlay's raop_ntp.c (GPL-3.0).
//
// Contrary to what the name suggests, the SERVER is the NTP client in
// AirPlay: iOS runs a small NTP responder on its announced `timingPort`
// (advertised in the SETUP body), and we poll it every 3 seconds.
// Without this traffic iOS concludes we can't sync the media clock and
// tears the session down after ~8 seconds.
//
// Packet layout (32 bytes):
//   [0..7]   : fixed header 0x80 0xd2 0x00 0x07 0x00 0x00 0x00 0x00
//   [8..15]  : client_ref_time — big-endian uint64 (0 on first request)
//   [16..23] : recv_time as NTP timestamp (0 on first request)
//   [24..31] : send_time as NTP timestamp (our "now")
//
// NTP timestamp = uint32 seconds since 1900-01-01 (big-endian) + uint32
// fractional seconds (big-endian). We don't actually use the responses
// for clock correction yet — we just drain the socket. iOS only cares
// that we're polling regularly.
class NtpClient {
public:
    NtpClient();
    ~NtpClient();

    NtpClient(const NtpClient&)            = delete;
    NtpClient& operator=(const NtpClient&) = delete;

    // Start polling. `sock` is our already-bound UDP socket (our
    // `timing_lport`), `remote_ip` is the iPhone's IP, `remote_port` is the
    // `timingPort` value iOS told us in the SETUP body. The NtpClient does
    // NOT take ownership of the socket — the StreamSession owns it, we
    // just borrow it for the thread's lifetime.
    bool start(socket_t sock, const std::string& remote_ip, uint16_t remote_port);

    // Stops and joins the thread. Safe to call multiple times.
    void stop();

private:
    void thread_fn();

    socket_t            sock_{INVALID_SOCK};
    std::string         remote_ip_;
    uint16_t            remote_port_{0};
    std::atomic<bool>   running_{false};
    std::thread         thread_;
};

} // namespace ap::airplay
