#include "airplay/ntp_client.h"
#include "log.h"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

namespace ap::airplay {
namespace {

constexpr uint64_t kSecondsFrom1900To1970 = 2208988800ULL;
constexpr uint64_t kNanosPerSecond        = 1'000'000'000ULL;
constexpr auto     kPollInterval          = std::chrono::seconds(3);
constexpr auto     kRecvTimeout           = std::chrono::seconds(1);

uint64_t now_ns_since_1970() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

void put_u32_be(unsigned char* buf, int off, uint32_t v) {
    buf[off + 0] = static_cast<unsigned char>((v >> 24) & 0xff);
    buf[off + 1] = static_cast<unsigned char>((v >> 16) & 0xff);
    buf[off + 2] = static_cast<unsigned char>((v >>  8) & 0xff);
    buf[off + 3] = static_cast<unsigned char>((v >>  0) & 0xff);
}

void put_u64_be(unsigned char* buf, int off, uint64_t v) {
    put_u32_be(buf, off + 0, static_cast<uint32_t>(v >> 32));
    put_u32_be(buf, off + 4, static_cast<uint32_t>(v));
}

uint64_t get_u64_be(const unsigned char* buf, int off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | buf[off + i];
    return v;
}

// UxPlay's byteutils_put_ntp_timestamp: 32-bit big-endian seconds since 1900,
// then 32-bit big-endian fractional seconds.
void put_ntp_timestamp(unsigned char* buf, int off, uint64_t ns_since_1970) {
    uint64_t seconds    = ns_since_1970 / kNanosPerSecond + kSecondsFrom1900To1970;
    uint64_t nanos      = ns_since_1970 % kNanosPerSecond;
    uint64_t fraction   = (nanos << 32) / kNanosPerSecond;
    put_u32_be(buf, off + 0, static_cast<uint32_t>(seconds));
    put_u32_be(buf, off + 4, static_cast<uint32_t>(fraction));
}

void set_recv_timeout(socket_t s, std::chrono::milliseconds t) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(t.count());
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv;
    tv.tv_sec  = static_cast<time_t>(t.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((t.count() % 1000) * 1000);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

} // namespace

NtpClient::NtpClient() = default;

NtpClient::~NtpClient() { stop(); }

bool NtpClient::start(socket_t sock, const std::string& remote_ip, uint16_t remote_port) {
    if (sock == INVALID_SOCK) {
        LOG_ERROR << "NtpClient.start: invalid socket";
        return false;
    }
    sock_        = sock;
    remote_ip_   = remote_ip;
    remote_port_ = remote_port;
    running_     = true;
    thread_      = std::thread(&NtpClient::thread_fn, this);
    LOG_INFO << "NTP client polling " << remote_ip_ << ':' << remote_port_
             << " every 3 sec";
    return true;
}

void NtpClient::stop() {
    // The thread may have exited on its own (e.g. inet_pton failed at
    // startup because remote_ip_ is IPv6 — AF_INET rejects it and the
    // worker sets running_=false and returns). In that case running_
    // is already false here but thread_ is still JOINABLE. If we skip
    // thread_.join() and let ~NtpClient() run, the std::thread
    // destructor sees a joinable thread and calls std::terminate(),
    // silently killing the whole process on teardown. Always join
    // before returning.
    const bool was_running = running_.exchange(false);
    if (was_running && sock_ != INVALID_SOCK) {
        // Wake a blocking recvfrom immediately instead of waiting for
        // the 1 s SO_RCVTIMEO. shutdown() is non-destructive — the
        // socket remains valid for its owner to closesocket() later.
#if defined(_WIN32)
        ::shutdown(sock_, SD_BOTH);
#else
        ::shutdown(sock_, SHUT_RDWR);
#endif
    }
    if (thread_.joinable()) thread_.join();
}

void NtpClient::thread_fn() {
    // Fixed 32-byte packet (UxPlay raop_ntp_thread). Bytes [8..15] carry the
    // client_ref_time, [16..23] the recv_time, [24..31] the send_time.
    unsigned char request[32] = {
        0x80, 0xd2, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    unsigned char response[128] = {0};

    // Resolve remote_ip_ into a sockaddr matching the family of sock_.
    // bind_udp() produces a v6 dual-stack socket by default; iPhones
    // that reach us over IPv6 also present their NTP peer as an IPv6
    // address (which the old inet_pton(AF_INET, ...) silently rejected
    // and made the thread exit immediately). getaddrinfo handles both
    // families and, with AI_V4MAPPED, turns a v4 remote_ip_ into the
    // ::ffff:x.y.z.w form so a dual-stack v6 socket can sendto it.
    sockaddr_storage remote{};
    socklen_t        remote_len = 0;
    {
        sockaddr_storage local_ss{};
#if defined(_WIN32)
        int local_len = sizeof(local_ss);
#else
        socklen_t local_len = sizeof(local_ss);
#endif
        const bool got_local =
            ::getsockname(sock_, reinterpret_cast<sockaddr*>(&local_ss),
                          &local_len) == 0;
        const int sock_family =
            got_local ? local_ss.ss_family : AF_UNSPEC;

        addrinfo hints{};
        hints.ai_family   = sock_family;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags    = AI_NUMERICHOST;
        if (sock_family == AF_INET6) hints.ai_flags |= AI_V4MAPPED;
        const std::string port_str = std::to_string(remote_port_);
        addrinfo* info = nullptr;
        const int gai = ::getaddrinfo(remote_ip_.c_str(),
                                      port_str.c_str(),
                                      &hints, &info);
        if (gai != 0 || !info) {
            LOG_ERROR << "NtpClient: getaddrinfo(" << remote_ip_
                      << ") failed gai=" << gai
                      << " family=" << sock_family;
            running_ = false;
            return;
        }
        std::memcpy(&remote, info->ai_addr, info->ai_addrlen);
        remote_len = static_cast<socklen_t>(info->ai_addrlen);
        ::freeaddrinfo(info);
    }

    set_recv_timeout(sock_, kRecvTimeout);

    uint64_t recv_time       = 0;
    uint64_t client_ref_time = 0;
    uint64_t probes_sent     = 0;
    uint64_t probes_received = 0;

    while (running_) {
        uint64_t send_time = now_ns_since_1970();
        put_ntp_timestamp(request, 24, send_time);
        if (recv_time) {
            put_u64_be       (request,  8, client_ref_time);
            put_ntp_timestamp(request, 16, recv_time);
        }

        int sent = ::sendto(sock_,
                            reinterpret_cast<const char*>(request),
                            sizeof(request), 0,
                            reinterpret_cast<sockaddr*>(&remote), remote_len);
        if (sent < 0) {
            LOG_WARN << "NtpClient: sendto failed";
        } else {
            ++probes_sent;
        }

        int n = ::recvfrom(sock_,
                           reinterpret_cast<char*>(response), sizeof(response), 0,
                           nullptr, nullptr);
        if (n >= 32) {
            recv_time       = now_ns_since_1970();
            client_ref_time = get_u64_be(response, 24);
            ++probes_received;
        }
        // else: timeout / no reply; send another probe next tick anyway.

        if ((probes_sent % 10) == 0) {
            LOG_INFO << "NTP poll: sent=" << probes_sent
                     << " received=" << probes_received;
        }

        // Sleep up to 3 sec, wake up early if stop() was called.
        for (int i = 0; i < 30 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    LOG_INFO << "NtpClient thread exiting";
    LOG_INFO << "NTP client stopped (sent=" << probes_sent
             << ", received=" << probes_received << ")";
}

} // namespace ap::airplay
