#include "mdns/mdns_service.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
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

        // Hex-encode the Ed25519 public key for the `pk` TXT value (64
        // chars for 32 bytes). iOS uses it to match the receiver
        // advertised over mDNS with the one it later pair-verifies.
        std::wstring pk_hex;
        {
            static const wchar_t* hexd = L"0123456789abcdef";
            pk_hex.reserve(ctx.public_key.size() * 2);
            for (unsigned char b : ctx.public_key) {
                pk_hex.push_back(hexd[b >> 4]);
                pk_hex.push_back(hexd[b & 0x0f]);
            }
        }

        // ---- _airplay._tcp --------------------------------------------------
        airplay.host_name     = local_host;
        airplay.instance_name = widen(ctx.name) + L"._airplay._tcp.local";
        airplay.keys          = { L"deviceid", L"features", L"flags", L"model",
                                  L"pi", L"pk", L"pw", L"srcvers", L"vv" };
        airplay.values        = { widen(ctx.deviceid), widen(ctx.features),
                                  L"0x4",             widen(ctx.model),
                                  widen(ctx.pi),      pk_hex,
                                  L"false",           widen(ctx.srcvers),
                                  L"2" };
        if (!register_service(airplay, port)) return false;

        // ---- _raop._tcp -----------------------------------------------------
        // Instance name: "<12-hex-of-deviceid>@<DeviceName>" (UxPlay convention).
        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        std::string raop_name = hex + "@" + ctx.name;

        raop.host_name     = local_host;
        raop.instance_name = widen(raop_name) + L"._raop._tcp.local";
        raop.keys          = { L"txtvers", L"ch",  L"cn",  L"da", L"et",
                               L"ft",      L"md",  L"am",  L"rhd", L"pk",
                               L"pw",      L"sf",  L"sr",  L"ss",  L"sv",
                               L"tp",      L"vn",  L"vs",  L"vv" };
        raop.values        = { L"1",                L"2",
                               L"0,1,2,3",          L"true",
                               L"0,3,5",            widen(ctx.features),
                               L"0,1,2",            widen(ctx.model),
                               L"5.6.0.0",          pk_hex,
                               L"false",            L"0x4",
                               L"44100",            L"16",
                               L"false",            L"UDP",
                               L"65537",            widen(ctx.srcvers),
                               L"2" };
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

#elif defined(HAVE_AVAHI)

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

// Dev/hacking aid for running the receiver on Linux. Uses the native
// avahi-client API + a dedicated poll thread so start() returns
// immediately. Same _airplay._tcp and _raop._tcp TXT keys as the
// Windows path, so iOS sees the device identically.
struct MdnsService::Impl {
    ~Impl() { stop(); }

    AvahiSimplePoll*      poll   = nullptr;
    AvahiClient*          client = nullptr;
    AvahiEntryGroup*      group  = nullptr;
    std::thread           thread;
    std::atomic<bool>     running{false};

    // Captured config for the client-state callback, which runs on the
    // poll thread and only receives the Impl* via userdata.
    ap::airplay::DeviceContext ctx;
    uint16_t                   port = 0;

    static AvahiStringList* txt_list(
        const std::vector<std::pair<const char*, std::string>>& kv) {
        AvahiStringList* list = nullptr;
        for (auto& [k, v] : kv) {
            list = avahi_string_list_add_pair(list, k, v.c_str());
        }
        return list;
    }

    static std::string pk_hex(const std::vector<unsigned char>& pk) {
        static const char* hexd = "0123456789abcdef";
        std::string out;
        out.reserve(pk.size() * 2);
        for (unsigned char b : pk) {
            out.push_back(hexd[b >> 4]);
            out.push_back(hexd[b & 0x0f]);
        }
        return out;
    }

    bool register_services() {
        if (!client) return false;
        if (group) avahi_entry_group_reset(group);
        else       group = avahi_entry_group_new(client, nullptr, nullptr);
        if (!group) {
            LOG_ERROR << "avahi_entry_group_new failed: "
                      << avahi_strerror(avahi_client_errno(client));
            return false;
        }

        const std::string pk = pk_hex(ctx.public_key);

        // _airplay._tcp — match the Windows TXT set byte-for-byte.
        AvahiStringList* airplay_txt = txt_list({
            {"deviceid", ctx.deviceid},
            {"features", ctx.features},
            {"flags",    std::string("0x4")},
            {"model",    ctx.model},
            {"pi",       ctx.pi},
            {"pk",       pk},
            {"pw",       std::string("false")},
            {"srcvers",  ctx.srcvers},
            {"vv",       std::string("2")},
        });
        int rc = avahi_entry_group_add_service_strlst(
            group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
            AvahiPublishFlags(0),
            ctx.name.c_str(), "_airplay._tcp",
            nullptr, nullptr, port, airplay_txt);
        avahi_string_list_free(airplay_txt);
        if (rc < 0) {
            LOG_ERROR << "Avahi add _airplay._tcp failed: "
                      << avahi_strerror(rc);
            return false;
        }

        // _raop._tcp instance name: "<12-hex-of-deviceid>@<DeviceName>".
        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        const std::string raop_name = hex + "@" + ctx.name;
        AvahiStringList* raop_txt = txt_list({
            {"txtvers", std::string("1")},
            {"ch",      std::string("2")},
            {"cn",      std::string("0,1,2,3")},
            {"da",      std::string("true")},
            {"et",      std::string("0,3,5")},
            {"ft",      ctx.features},
            {"md",      std::string("0,1,2")},
            {"am",      ctx.model},
            {"rhd",     std::string("5.6.0.0")},
            {"pk",      pk},
            {"pw",      std::string("false")},
            {"sf",      std::string("0x4")},
            {"sr",      std::string("44100")},
            {"ss",      std::string("16")},
            {"sv",      std::string("false")},
            {"tp",      std::string("UDP")},
            {"vn",      std::string("65537")},
            {"vs",      ctx.srcvers},
            {"vv",      std::string("2")},
        });
        rc = avahi_entry_group_add_service_strlst(
            group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
            AvahiPublishFlags(0),
            raop_name.c_str(), "_raop._tcp",
            nullptr, nullptr, port, raop_txt);
        avahi_string_list_free(raop_txt);
        if (rc < 0) {
            LOG_WARN << "Avahi add _raop._tcp failed: " << avahi_strerror(rc);
        }

        rc = avahi_entry_group_commit(group);
        if (rc < 0) {
            LOG_ERROR << "avahi_entry_group_commit failed: "
                      << avahi_strerror(rc);
            return false;
        }
        LOG_INFO << "Avahi registered: " << ctx.name
                 << "._airplay._tcp.local and _raop._tcp.local on port " << port;
        return true;
    }

    static void client_cb(AvahiClient* c, AvahiClientState state, void* ud) {
        auto* self = static_cast<Impl*>(ud);
        self->client = c;
        if (state == AVAHI_CLIENT_S_RUNNING) {
            self->register_services();
        } else if (state == AVAHI_CLIENT_FAILURE) {
            LOG_ERROR << "Avahi client failure: "
                      << avahi_strerror(avahi_client_errno(c));
        }
    }

    bool start(const ap::airplay::DeviceContext& ctx_in, uint16_t port_in) {
        ctx  = ctx_in;
        port = port_in;
        poll = avahi_simple_poll_new();
        if (!poll) { LOG_ERROR << "avahi_simple_poll_new failed"; return false; }
        int err = 0;
        client = avahi_client_new(avahi_simple_poll_get(poll),
                                  AvahiClientFlags(0),
                                  &Impl::client_cb, this, &err);
        if (!client) {
            LOG_ERROR << "avahi_client_new failed: " << avahi_strerror(err);
            avahi_simple_poll_free(poll); poll = nullptr;
            return false;
        }
        running = true;
        thread = std::thread([this]() {
            avahi_simple_poll_loop(poll);
        });
        return true;
    }

    void stop() {
        if (!running.exchange(false)) return;
        if (poll)   avahi_simple_poll_quit(poll);
        if (thread.joinable()) thread.join();
        if (client) avahi_client_free(client);
        if (poll)   avahi_simple_poll_free(poll);
        client = nullptr; poll = nullptr; group = nullptr;
    }
};

#else // non-Windows without Avahi: stub

struct MdnsService::Impl {
    bool start(const ap::airplay::DeviceContext&, uint16_t) {
        LOG_WARN << "mDNS advertising unavailable in this build "
                    "(install libavahi-client-dev + reconfigure to enable on Linux)";
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
