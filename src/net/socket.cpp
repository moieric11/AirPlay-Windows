#include "net/socket.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#else
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netdb.h>
    #include <errno.h>
#endif

namespace ap::net {

bool global_init() {
#if defined(_WIN32)
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        LOG_ERROR << "WSAStartup failed: " << rc;
        return false;
    }
#endif
    return true;
}

void global_shutdown() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

void close_socket(socket_t s) {
    if (s == INVALID_SOCK) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

int last_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string last_error_string() {
    int err = last_error();
#if defined(_WIN32)
    char* buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string msg = buf ? buf : "";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return msg + " (" + std::to_string(err) + ")";
#else
    return std::string(strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

int recv_all(socket_t s, void* buf, int len) {
    auto* p = static_cast<char*>(buf);
    int got = 0;
    while (got < len) {
        int n = ::recv(s, p + got, len - got, 0);
        if (n == 0) return got;   // peer closed
        if (n < 0)  return -1;
        got += n;
    }
    return got;
}

int send_all(socket_t s, const void* buf, int len) {
    const auto* p = static_cast<const char*>(buf);
    int sent = 0;
    while (sent < len) {
        int n = ::send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

// "Connect" a UDP socket to a public address and read back the local address
// the OS routing table assigned. No packet is actually sent — `connect` on a
// UDP socket only selects the outbound interface. This bypasses virtual
// adapters (Hyper-V, VMware, WSL) that would otherwise be picked by a naive
// "first non-loopback" enumeration.
namespace {
std::string primary_ipv4_via_route() {
    // Intentional: this helper is v4-specific by design. The IPv6
    // equivalent lives in primary_ipv6_via_route() below.
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return "0.0.0.0";

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &peer.sin_addr);   // v4-only probe

    std::string result = "0.0.0.0";
    if (::connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
        sockaddr_in local{};
#if defined(_WIN32)
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            result = buf;
        }
    }
    close_socket(s);
    return result;
}

// Same trick over IPv6: UDP-connect to Google Public DNS v6 and read
// back the local address the OS route table chose. Skip link-local
// (fe80::) — iOS needs a globally-routable address to connect back to.
std::string primary_ipv6_via_route() {
    socket_t s = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return {};

    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    peer.sin6_port   = htons(53);
    inet_pton(AF_INET6, "2001:4860:4860::8888", &peer.sin6_addr);

    std::string result;
    if (::connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
        sockaddr_in6 local{};
#if defined(_WIN32)
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &local.sin6_addr, buf, sizeof(buf));
            // Skip link-local; unusable from outside our own subnet.
            if (std::strncmp(buf, "fe80", 4) != 0 &&
                std::strncmp(buf, "::",   2) != 0) {
                result = buf;
            }
        }
    }
    close_socket(s);
    return result;
}
} // namespace

std::string primary_ipv6() {
    return primary_ipv6_via_route();
}

#if defined(_WIN32)

std::string primary_ipv4() {
    return primary_ipv4_via_route();
}

std::string primary_mac() {
    // Find the adapter whose IP (v4 or v6) matches the one we advertise
    // and return its MAC. iOS correlates `deviceid` (MAC) with the RTSP
    // socket's local IP, so the two must come from the same interface.
    // On an IPv6-only network primary_ipv4() is "0.0.0.0" — we'd never
    // match anything and fall back to a bogus 02:00:... MAC that iOS
    // refuses to re-pair with. Enumerate both families and match either.
    const std::string target_v4 = primary_ipv4();
    const std::string target_v6 = primary_ipv6();

    ULONG size = 15 * 1024;
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    DWORD rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return "02:00:00:00:00:00";

    for (auto* a = adapters; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sa = u->Address.lpSockaddr;
            if (!sa) continue;
            char ip[INET6_ADDRSTRLEN] = {0};
            if (sa->sa_family == AF_INET) {
                const auto* in4 = reinterpret_cast<const sockaddr_in*>(sa);
                inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
            } else if (sa->sa_family == AF_INET6) {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
                inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
            } else {
                continue;
            }
            const bool match =
                (!target_v4.empty() && target_v4 != "0.0.0.0" && target_v4 == ip) ||
                (!target_v6.empty() && target_v6 == ip);
            if (match) {
                char out[18];
                std::snprintf(out, sizeof(out),
                              "%02X:%02X:%02X:%02X:%02X:%02X",
                              a->PhysicalAddress[0], a->PhysicalAddress[1],
                              a->PhysicalAddress[2], a->PhysicalAddress[3],
                              a->PhysicalAddress[4], a->PhysicalAddress[5]);
                return out;
            }
        }
    }
    return "02:00:00:00:00:00";
}

#else

std::string primary_ipv4() {
    return primary_ipv4_via_route();
}

std::string primary_mac() {
    // Minimal fallback for non-Windows development builds.
    return "02:00:00:00:00:00";
}

#endif

} // namespace ap::net
