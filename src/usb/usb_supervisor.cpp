#include "usb/usb_supervisor.h"
#include "usb/pair_record.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <initguid.h>      // must precede usbiodef.h to instantiate GUIDs
    #include <usbiodef.h>      // GUID_DEVINTERFACE_USB_DEVICE
    #include <setupapi.h>
    #pragma comment(lib, "setupapi.lib")
#endif

namespace ap::usb {

#if defined(_WIN32)

namespace {

constexpr uint16_t kAppleVendorId = 0x05AC;
constexpr auto     kPollInterval  = std::chrono::seconds(1);

// One Apple device discovered through SetupAPI. Friendly name is
// what Device Manager shows ("Apple iPhone", "iPhone de Eric", ...);
// the UDID is the serial-number portion of the device instance ID.
struct AppleDevice {
    std::string udid;
    std::string friendly_name;
    uint16_t    pid = 0;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

// Parse "USB\VID_05AC&PID_12A8\<serial>" → (pid, serial). Rejects
// non-Apple VIDs and composite-child instance IDs (whose third
// segment is a Windows-internal "&"-laced child id, not a serial).
bool parse_instance_id(const std::wstring& w_id,
                       uint16_t& pid, std::string& serial) {
    std::string id;
    id.reserve(w_id.size());
    for (auto wc : w_id) id.push_back(static_cast<char>(wc));

    auto vidpos = id.find("VID_");
    auto pidpos = id.find("PID_");
    if (vidpos == std::string::npos || pidpos == std::string::npos) return false;
    if (vidpos + 8 > id.size() || pidpos + 8 > id.size())            return false;

    unsigned long vid = 0, pidv = 0;
    try {
        vid  = std::stoul(id.substr(vidpos + 4, 4), nullptr, 16);
        pidv = std::stoul(id.substr(pidpos + 4, 4), nullptr, 16);
    } catch (...) { return false; }
    if (vid != kAppleVendorId) return false;

    auto last_slash = id.find_last_of("\\/");
    if (last_slash == std::string::npos || last_slash + 1 >= id.size()) return false;
    serial = id.substr(last_slash + 1);
    // Composite children look like "6&abc123&0&0000" — never a UDID.
    if (serial.find('&') != std::string::npos) return false;
    if (serial.empty()) return false;

    pid = static_cast<uint16_t>(pidv);
    return true;
}

std::string read_friendly_name(HDEVINFO devinfo, SP_DEVINFO_DATA& data) {
    wchar_t buf[256] = {};
    DWORD   needed   = 0;
    DWORD   reg_type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(
            devinfo, &data, SPDRP_FRIENDLYNAME, &reg_type,
            reinterpret_cast<PBYTE>(buf), sizeof(buf), &needed)) {
        if (!SetupDiGetDeviceRegistryPropertyW(
                devinfo, &data, SPDRP_DEVICEDESC, &reg_type,
                reinterpret_cast<PBYTE>(buf), sizeof(buf), &needed)) {
            return {};
        }
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::vector<AppleDevice> enumerate_apple_devices() {
    std::vector<AppleDevice> devices;
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE,
                                      nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) return devices;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &data); ++i) {
        wchar_t inst_id[256] = {};
        if (!SetupDiGetDeviceInstanceIdW(h, &data, inst_id,
                                         ARRAYSIZE(inst_id), nullptr)) {
            continue;
        }
        AppleDevice d{};
        if (!parse_instance_id(inst_id, d.pid, d.udid)) continue;
        d.friendly_name = read_friendly_name(h, data);
        if (d.friendly_name.empty()) d.friendly_name = "Apple device";
        devices.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(h);
    return devices;
}

void log_arrival(const AppleDevice& d) {
    bool paired = pair_record_exists(d.udid);
    LOG_INFO << "USB device found: " << d.friendly_name
             << " (pid=0x" << std::hex << d.pid << std::dec
             << ", udid=" << d.udid
             << ", paired=" << (paired ? "yes" : "no - tap 'Trust' on iPhone")
             << ")";
    // Apple deprecated direct QuickTime-over-USB streaming on iOS
    // 17/18, but Personal Hotspot via USB still creates a virtual
    // ethernet between the iPhone and the PC — and AirPlay over IP
    // works on that subnet without needing a shared Wi-Fi.
    if (paired) {
        LOG_INFO << "  tip: enable Personal Hotspot on the iPhone "
                    "to mirror over the USB cable (no Wi-Fi needed)";
    }
}

void log_removal(const AppleDevice& d) {
    LOG_INFO << "USB device removed: " << d.friendly_name
             << " (udid=" << d.udid << ")";
}

} // namespace

struct UsbSupervisor::Impl {
    std::atomic<bool>         running{false};
    std::thread               thread;
    std::set<std::string>     known;            // by lowercased UDID
    std::vector<AppleDevice>  last_snapshot;

    void run() {
        LOG_INFO << "USB supervisor: polling for Apple devices every "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(
                        kPollInterval).count() << " ms";
        while (running.load()) {
            auto current = enumerate_apple_devices();

            std::set<std::string> current_keys;
            for (auto& d : current) current_keys.insert(lower(d.udid));

            for (auto& d : current) {
                auto key = lower(d.udid);
                if (known.insert(key).second) log_arrival(d);
            }
            for (auto it = known.begin(); it != known.end(); ) {
                if (current_keys.find(*it) == current_keys.end()) {
                    auto prev = std::find_if(
                        last_snapshot.begin(), last_snapshot.end(),
                        [&](const AppleDevice& d) { return lower(d.udid) == *it; });
                    if (prev != last_snapshot.end()) log_removal(*prev);
                    it = known.erase(it);
                } else {
                    ++it;
                }
            }
            last_snapshot = std::move(current);

            // Sleep in 200 ms slices so stop() returns promptly.
            // `remaining` must be ms (not seconds) so the -= step is
            // representable without a duration cast each iteration.
            constexpr auto slice = std::chrono::milliseconds(200);
            std::chrono::milliseconds remaining = kPollInterval;
            while (remaining.count() > 0 && running.load()) {
                auto step = remaining < slice ? remaining : slice;
                std::this_thread::sleep_for(step);
                remaining -= step;
            }
        }
        LOG_INFO << "USB supervisor: stopped";
    }
};

#else  // non-Windows stub

struct UsbSupervisor::Impl {
    std::atomic<bool> running{false};
    std::thread       thread;
    void run() {}
};

#endif

UsbSupervisor::UsbSupervisor() : impl_(std::make_unique<Impl>()) {}
UsbSupervisor::~UsbSupervisor() { stop(); }

bool UsbSupervisor::start() {
#if defined(_WIN32)
    if (impl_->running.exchange(true)) return true;
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
#else
    LOG_INFO << "USB supervisor: not implemented on this platform";
    return false;
#endif
}

void UsbSupervisor::stop() {
#if defined(_WIN32)
    if (!impl_->running.exchange(false)) return;
    if (impl_->thread.joinable()) impl_->thread.join();
#endif
}

} // namespace ap::usb
