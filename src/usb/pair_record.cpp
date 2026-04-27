#include "usb/pair_record.h"
#include "log.h"

#include <fstream>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <shlobj.h>
    #pragma comment(lib, "shell32.lib")
    #pragma comment(lib, "ole32.lib")
#endif

#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

namespace ap::usb {

namespace {

std::string strip_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) if (c != '-') out.push_back(c);
    return out;
}

std::vector<std::string> candidate_filenames(const std::string& udid) {
    // iPhone X+ UDIDs are 25 chars (NNNNNNNN-NNNNNNNNNNNNNNNN); older
    // devices use a 40-char hex form. Apple writes the file with the
    // dash present; Windows USB enumeration strips it. Probe both.
    std::vector<std::string> out;
    out.push_back(udid + ".plist");
    auto stripped = strip_dashes(udid);
    if (stripped != udid) {
        out.push_back(stripped + ".plist");
    } else if (udid.size() == 24) {
        // Reverse: USB gave us the un-dashed 24-char form, re-insert
        // the dash after position 8 to match Apple's filename.
        out.push_back(udid.substr(0, 8) + "-" + udid.substr(8) + ".plist");
    }
    return out;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    auto size = ifs.tellg();
    if (size < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), size);
    }
    return ifs.good() || ifs.eof();
}

#if defined(HAVE_LIBPLIST)

std::string get_string(plist_t root, const char* key) {
    plist_t v = plist_dict_get_item(root, key);
    if (!v || plist_get_node_type(v) != PLIST_STRING) return {};
    char* raw = nullptr;
    plist_get_string_val(v, &raw);
    std::string out = raw ? raw : "";
    if (raw) free(raw);
    return out;
}

void get_data(plist_t root, const char* key, std::vector<unsigned char>& out) {
    plist_t v = plist_dict_get_item(root, key);
    if (!v || plist_get_node_type(v) != PLIST_DATA) return;
    char*    buf = nullptr;
    uint64_t len = 0;
    plist_get_data_val(v, &buf, &len);
    if (buf && len) {
        out.assign(reinterpret_cast<unsigned char*>(buf),
                   reinterpret_cast<unsigned char*>(buf) + len);
    }
    if (buf) free(buf);
}

#endif // HAVE_LIBPLIST

} // namespace

std::string lockdown_directory() {
#if defined(_WIN32)
    PWSTR programdata_w = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0,
                                    nullptr, &programdata_w))) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, programdata_w, -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string base;
    if (len > 1) {
        base.assign(static_cast<std::size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, programdata_w, -1,
                            base.data(), len, nullptr, nullptr);
    }
    CoTaskMemFree(programdata_w);
    if (base.empty()) return {};
    return base + "\\Apple\\Lockdown";
#else
    return "/var/db/lockdown";
#endif
}

bool pair_record_exists(const std::string& udid) {
    auto dir = lockdown_directory();
    if (dir.empty() || udid.empty()) return false;
    for (const auto& name : candidate_filenames(udid)) {
#if defined(_WIN32)
        std::string full = dir + "\\" + name;
#else
        std::string full = dir + "/" + name;
#endif
        std::ifstream ifs(full, std::ios::binary);
        if (ifs) return true;
    }
    return false;
}

bool load_pair_record(const std::string& udid, PairRecord& out) {
    auto dir = lockdown_directory();
    if (dir.empty() || udid.empty()) return false;
    std::vector<unsigned char> blob;
    bool found = false;
    for (const auto& name : candidate_filenames(udid)) {
#if defined(_WIN32)
        std::string full = dir + "\\" + name;
#else
        std::string full = dir + "/" + name;
#endif
        if (read_file(full, blob)) { found = true; break; }
    }
    if (!found) return false;

#if defined(HAVE_LIBPLIST)
    plist_t root = nullptr;
    // libplist ≥ 2.3 added a 4th out-param `plist_format_t*` — we
    // don't care which subformat the file is in (binary vs xml),
    // pass nullptr to ignore.
    plist_from_memory(reinterpret_cast<const char*>(blob.data()),
                      static_cast<uint32_t>(blob.size()), &root, nullptr);
    if (!root) return false;
    out = {};
    out.host_id     = get_string(root, "HostID");
    out.system_buid = get_string(root, "SystemBUID");
    out.wifi_mac    = get_string(root, "WiFiMACAddress");
    get_data(root, "HostCertificate",   out.host_certificate);
    get_data(root, "HostPrivateKey",    out.host_private_key);
    get_data(root, "RootCertificate",   out.root_certificate);
    get_data(root, "RootPrivateKey",    out.root_private_key);
    get_data(root, "DeviceCertificate", out.device_certificate);
    plist_free(root);
    return !out.host_id.empty();
#else
    (void)out;
    return true;   // existence-only when libplist isn't linked
#endif
}

} // namespace ap::usb
