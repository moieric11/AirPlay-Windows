#pragma once
#include "airplay/media_control.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ap::platform {
// Owned and updated by the SDL thread. Media buttons use the existing DACP queue.
class DesktopControls {
public:
    DesktopControls();
    ~DesktopControls();
    DesktopControls(const DesktopControls&) = delete;
    DesktopControls& operator=(const DesktopControls&) = delete;
    bool start(void* window, std::function<void(bool)> visibility,
               std::function<void()> quit,
               std::function<void(airplay::MediaCommand)> command);
    void stop();
    void metadata(const std::string& title, const std::string& artist, const std::string& album);
    void artwork(const uint8_t* data, std::size_t size);
    void publish(bool active, bool playing, bool controllable, uint32_t elapsed, uint32_t total);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
