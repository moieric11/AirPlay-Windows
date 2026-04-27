#pragma once

#include <string>
#include <vector>

namespace ap::usb {

// Subset of the Apple Mobile Device Service "pair record" stored at
//   %ProgramData%\Apple\Lockdown\<UDID>.plist
// once the user tapped "Trust this computer" on the iPhone. Created
// by AMDS (installed with iTunes / Apple Devices); we only read it.
//
// Phase 1 just checks existence to surface paired/not-paired in the
// log. Later phases (lockdownd handshake, QuickTime session) need
// the certs + keys + HostID to identify themselves to the device.
struct PairRecord {
    std::string                host_id;            // UUID, e.g. "ABCD1234-..."
    std::string                system_buid;        // host machine UUID
    std::string                wifi_mac;           // device WiFi MAC iOS sends back
    std::vector<unsigned char> host_certificate;   // PEM
    std::vector<unsigned char> host_private_key;   // PEM
    std::vector<unsigned char> root_certificate;   // PEM
    std::vector<unsigned char> root_private_key;   // PEM
    std::vector<unsigned char> device_certificate; // PEM
};

// Canonical Lockdown directory for the current host (e.g.
// "C:\ProgramData\Apple\Lockdown" on Windows, "/var/db/lockdown"
// on macOS/Linux). Empty string on failure.
std::string lockdown_directory();

// True if a pair record file exists for this UDID. Probes both the
// dash-stripped and as-given filename forms — Apple files iPhone X+
// records with the dash in the basename, but USB enumeration on
// Windows occasionally drops it.
bool pair_record_exists(const std::string& udid);

// Read and parse the pair record. Returns false (and leaves `out`
// untouched) if the file is missing or unparseable. Phase 1 only
// looks at the boolean; later phases consume the certificates.
bool load_pair_record(const std::string& udid, PairRecord& out);

} // namespace ap::usb
