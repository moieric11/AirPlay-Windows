#pragma once

#include "airplay/routes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace ap::mdns {

// Advertises the AirPlay receiver on the local network via Apple's Bonjour
// service (DNS-SD). Requires the Bonjour Service to be running on Windows
// (installed with Bonjour Print Services, iTunes, or standalone SDK).
//
// When built without the Bonjour SDK, start() logs a warning and returns
// false — the TCP server still runs but iOS won't auto-discover it.
class BonjourService {
public:
    BonjourService();
    ~BonjourService();

    bool start(const ap::airplay::DeviceContext& ctx, uint16_t port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::mdns
