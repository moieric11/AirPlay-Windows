#pragma once

#include "airplay/routes.h"

#include <cstdint>
#include <memory>

namespace ap::mdns {

// Advertises the AirPlay receiver on the local network via mDNS / DNS-SD.
//
// Windows 10 1809+ / Windows 11: uses the native `DnsServiceRegister` API
// in dnsapi.dll. No external SDK or extra service to install — the OS
// mDNS responder (dnsapi) is built in.
//
// Linux: stub. The project targets Windows; Linux builds exist only for
// dev iteration, where mDNS is not exercised. Returns false from start().
class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    bool start(const ap::airplay::DeviceContext& ctx, uint16_t port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::mdns
