#include "mdns/bonjour_service.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <thread>

#if defined(HAVE_BONJOUR)
    #include <dns_sd.h>
#endif

namespace ap::mdns {

struct BonjourService::Impl {
#if defined(HAVE_BONJOUR)
    DNSServiceRef airplay_ref = nullptr;
    DNSServiceRef raop_ref    = nullptr;
    std::thread   loop;
    std::atomic<bool> running{false};

    static void DNSSD_API on_register(DNSServiceRef, DNSServiceFlags,
                                      DNSServiceErrorType err,
                                      const char* name, const char* regtype,
                                      const char* /*domain*/, void* /*ctx*/) {
        if (err == kDNSServiceErr_NoError) {
            LOG_INFO << "mDNS registered: " << name << " " << regtype;
        } else {
            LOG_ERROR << "mDNS register error " << err << " for " << regtype;
        }
    }

    static bool add_txt(TXTRecordRef& txt, const char* key, const std::string& val) {
        auto rc = TXTRecordSetValue(&txt, key,
                                    static_cast<uint8_t>(val.size()),
                                    val.data());
        if (rc != kDNSServiceErr_NoError) {
            LOG_ERROR << "TXTRecordSetValue(" << key << ") failed: " << rc;
            return false;
        }
        return true;
    }

    void run_loop() {
        while (running) {
            // Block on whichever ref has data; Bonjour handles both internally
            // as long as we call DNSServiceProcessResult per-ref. Two refs =
            // poll both cheaply with a short alternating wait.
            if (airplay_ref) DNSServiceProcessResult(airplay_ref);
            if (raop_ref)    DNSServiceProcessResult(raop_ref);
        }
    }
#endif

    bool start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
#if !defined(HAVE_BONJOUR)
        (void)ctx; (void)port;
        LOG_WARN << "Built without Bonjour SDK — mDNS advertising disabled. "
                    "Install the Bonjour SDK and rebuild for iOS discovery.";
        return false;
#else
        // _airplay._tcp TXT record (subset — enough for detection; extend later).
        TXTRecordRef ap_txt;
        TXTRecordCreate(&ap_txt, 0, nullptr);
        add_txt(ap_txt, "deviceid", ctx.deviceid);
        add_txt(ap_txt, "features", ctx.features);
        add_txt(ap_txt, "flags",    "0x4");
        add_txt(ap_txt, "model",    ctx.model);
        add_txt(ap_txt, "pi",       ctx.pi);
        add_txt(ap_txt, "srcvers",  ctx.srcvers);
        add_txt(ap_txt, "vv",       "2");

        uint16_t port_be = htons(port);
        auto rc = DNSServiceRegister(
            &airplay_ref, 0, 0,
            ctx.name.c_str(),
            "_airplay._tcp",
            nullptr, nullptr,
            port_be,
            TXTRecordGetLength(&ap_txt),
            TXTRecordGetBytesPtr(&ap_txt),
            &Impl::on_register, this);
        TXTRecordDeallocate(&ap_txt);

        if (rc != kDNSServiceErr_NoError) {
            LOG_ERROR << "DNSServiceRegister _airplay._tcp failed: " << rc;
            return false;
        }

        // _raop._tcp TXT record — required for AirPlay 2 mirroring handshake.
        // Name convention: "<8 hex chars from deviceid>@<DeviceName>".
        TXTRecordRef raop_txt;
        TXTRecordCreate(&raop_txt, 0, nullptr);
        add_txt(raop_txt, "cn", "0,1,2,3");   // compression (PCM, ALAC, AAC, AAC-ELD)
        add_txt(raop_txt, "da", "true");
        add_txt(raop_txt, "et", "0,3,5");     // encryption types
        add_txt(raop_txt, "ft", ctx.features);
        add_txt(raop_txt, "md", "0,1,2");     // metadata
        add_txt(raop_txt, "am", ctx.model);
        add_txt(raop_txt, "sf", "0x4");
        add_txt(raop_txt, "tp", "UDP");
        add_txt(raop_txt, "vn", "65537");
        add_txt(raop_txt, "vs", ctx.srcvers);
        add_txt(raop_txt, "vv", "2");
        add_txt(raop_txt, "pi", ctx.pi);

        // Strip colons from deviceid to build the 12-hex-char service prefix.
        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        std::string raop_name = hex + "@" + ctx.name;

        rc = DNSServiceRegister(
            &raop_ref, 0, 0,
            raop_name.c_str(),
            "_raop._tcp",
            nullptr, nullptr,
            port_be,
            TXTRecordGetLength(&raop_txt),
            TXTRecordGetBytesPtr(&raop_txt),
            &Impl::on_register, this);
        TXTRecordDeallocate(&raop_txt);

        if (rc != kDNSServiceErr_NoError) {
            LOG_WARN << "DNSServiceRegister _raop._tcp failed: " << rc
                     << " (mirroring may still be offered)";
            raop_ref = nullptr;
        }

        running = true;
        loop = std::thread([this]{ run_loop(); });
        return true;
#endif
    }

    void stop() {
#if defined(HAVE_BONJOUR)
        running = false;
        if (airplay_ref) { DNSServiceRefDeallocate(airplay_ref); airplay_ref = nullptr; }
        if (raop_ref)    { DNSServiceRefDeallocate(raop_ref);    raop_ref    = nullptr; }
        if (loop.joinable()) loop.join();
#endif
    }
};

BonjourService::BonjourService() : impl_(std::make_unique<Impl>()) {}
BonjourService::~BonjourService() { stop(); }

bool BonjourService::start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
    return impl_->start(ctx, port);
}

void BonjourService::stop() {
    if (impl_) impl_->stop();
}

} // namespace ap::mdns
