#pragma once

#include <memory>

namespace ap::usb {

// Polls Windows SetupAPI for connected Apple iPhone/iPad devices and
// logs new arrivals + pair-record status. Background thread; safe to
// start once at application startup and stop at shutdown.
//
// Phase 1 of the USB QuickTime mirror chain — pure observation,
// no driver claim, no media. Later phases will hand the discovered
// UDID + pair record to the QuickTime session opener.
class UsbSupervisor {
public:
    UsbSupervisor();
    ~UsbSupervisor();

    UsbSupervisor(const UsbSupervisor&)            = delete;
    UsbSupervisor& operator=(const UsbSupervisor&) = delete;

    // Spawn the polling thread. Idempotent — start() while already
    // running is a no-op that returns true.
    bool start();

    // Signal the polling thread to exit and join. Idempotent.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::usb
