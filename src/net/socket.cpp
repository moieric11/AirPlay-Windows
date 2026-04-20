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

#if defined(_WIN32)

std::string primary_ipv4() {
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buf;
    IP_ADAPTER_ADDRESSES* adapters = nullptr;
    DWORD rc;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                  nullptr, adapters, &size);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (rc != NO_ERROR) return "0.0.0.0";

    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, s, sizeof(s));
            return s;
        }
    }
    return "0.0.0.0";
}

std::string primary_mac() {
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    DWORD rc = GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return "02:00:00:00:00:00";

    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->PhysicalAddressLength != 6) continue;
        char out[18];
        std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
                      a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2],
                      a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
        return out;
    }
    return "02:00:00:00:00:00";
}

#else

std::string primary_ipv4() {
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return "0.0.0.0";
    std::string result = "0.0.0.0";
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        char s[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, s, sizeof(s));
        result = s;
        break;
    }
    freeifaddrs(ifa);
    return result;
}

std::string primary_mac() {
    // Minimal fallback for non-Windows development builds.
    return "02:00:00:00:00:00";
}

#endif

} // namespace ap::net
