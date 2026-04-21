#include "mdns/mdns_service.h"
#include "log.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <windns.h>
    #pragma comment(lib, "dnsapi.lib")
#endif

namespace ap::mdns {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

std::string narrow(PCWSTR ws) {
    if (!ws) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

// One registered service (either _airplay._tcp or _raop._tcp). Owns the
// backing string storage so the PWSTR pointers inside DNS_SERVICE_INSTANCE
// stay valid until DnsServiceRegisterCancel is called.
struct RegisteredService {
    std::wstring              instance_name;   // "Name._type._tcp.local"
    std::wstring              host_name;       // "pc.local"
    std::vector<std::wstring> keys;
    std::vector<std::wstring> values;
    std::vector<PWSTR>        key_ptrs;
    std::vector<PWSTR>        value_ptrs;
    DNS_SERVICE_INSTANCE      instance{};
    DNS_SERVICE_CANCEL        cancel{};
    bool                      active{false};

    void fill_ptrs() {
        key_ptrs.clear();
        value_ptrs.clear();
        for (auto& s : keys)   key_ptrs.push_back(s.data());
        for (auto& s : values) value_ptrs.push_back(s.data());
    }
};

struct MdnsService::Impl {
    RegisteredService airplay;
    RegisteredService raop;

    static void WINAPI on_register(DWORD status, PVOID /*ctx*/,
                                   PDNS_SERVICE_INSTANCE inst) {
        if (status == ERROR_SUCCESS && inst) {
            LOG_INFO << "mDNS registered: " << narrow(inst->pszInstanceName);
        } else {
            LOG_ERROR << "mDNS register failed (status=" << status << ")";
        }
        if (inst) DnsServiceFreeInstance(inst);
    }

    bool register_service(RegisteredService& s, uint16_t port) {
        s.fill_ptrs();

        s.instance = {};
        s.instance.pszInstanceName = s.instance_name.data();
        s.instance.pszHostName     = s.host_name.data();
        s.instance.wPort           = port;
        s.instance.dwPropertyCount = static_cast<DWORD>(s.keys.size());
        s.instance.keys            = s.key_ptrs.data();
        s.instance.values          = s.value_ptrs.data();

        DNS_SERVICE_REGISTER_REQUEST req{};
        req.Version                    = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex             = 0;                 // all interfaces
        req.pServiceInstance           = &s.instance;
        req.pRegisterCompletionCallback = &on_register;
        req.pQueryContext              = this;
        req.unicastEnabled             = FALSE;

        DWORD rc = DnsServiceRegister(&req, &s.cancel);
        if (rc != DNS_REQUEST_PENDING) {
            LOG_ERROR << "DnsServiceRegister(" << narrow(s.instance_name.c_str())
                      << ") failed: " << rc;
            return false;
        }
        s.active = true;
        return true;
    }

    bool start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
        wchar_t host[256];
        DWORD   sz = ARRAYSIZE(host);
        if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &sz)) {
            LOG_ERROR << "GetComputerNameExW failed: " << GetLastError();
            return false;
        }
        std::wstring local_host = std::wstring(host) + L".local";

        // ---- _airplay._tcp --------------------------------------------------
        airplay.host_name     = local_host;
        airplay.instance_name = widen(ctx.name) + L"._airplay._tcp.local";
        airplay.keys          = { L"deviceid", L"features", L"flags", L"model",
                                  L"pi", L"srcvers", L"vv" };
        airplay.values        = { widen(ctx.deviceid), widen(ctx.features),
                                  L"0x4",             widen(ctx.model),
                                  widen(ctx.pi),      widen(ctx.srcvers),
                                  L"2" };
        if (!register_service(airplay, port)) return false;

        // ---- _raop._tcp -----------------------------------------------------
        // Instance name: "<12-hex-of-deviceid>@<DeviceName>" (UxPlay convention).
        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        std::string raop_name = hex + "@" + ctx.name;

        raop.host_name     = local_host;
        raop.instance_name = widen(raop_name) + L"._raop._tcp.local";
        raop.keys          = { L"cn", L"da",  L"et", L"ft", L"md", L"am",
                               L"sf", L"tp",  L"vn", L"vs", L"vv", L"pi" };
        raop.values        = { L"0,1,2,3",           L"true",
                               L"0,3,5",             widen(ctx.features),
                               L"0,1,2",             widen(ctx.model),
                               L"0x4",               L"UDP",
                               L"65537",             widen(ctx.srcvers),
                               L"2",                 widen(ctx.pi) };
        if (!register_service(raop, port)) {
            LOG_WARN << "_raop._tcp registration failed; mirroring may still be offered";
        }

        return true;
    }

    void stop() {
        if (airplay.active) {
            DnsServiceRegisterCancel(&airplay.cancel);
            airplay.active = false;
        }
        if (raop.active) {
            DnsServiceRegisterCancel(&raop.cancel);
            raop.active = false;
        }
    }
};

#else // non-Windows: dev-only stub

struct MdnsService::Impl {
    bool start(const ap::airplay::DeviceContext&, uint16_t) {
        LOG_WARN << "mDNS advertising is Windows-only in this build "
                    "(Linux targets are dev-only, no Avahi integration)";
        return false;
    }
    void stop() {}
};

#endif

MdnsService::MdnsService() : impl_(std::make_unique<Impl>()) {}
MdnsService::~MdnsService() { stop(); }

bool MdnsService::start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
    return impl_->start(ctx, port);
}

void MdnsService::stop() {
    if (impl_) impl_->stop();
}

} // namespace ap::mdns
