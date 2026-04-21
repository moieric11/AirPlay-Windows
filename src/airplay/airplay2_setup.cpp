#include "airplay/airplay2_setup.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <plist/plist.h>

namespace ap::airplay {
namespace {

// --- Small libplist helpers (same pattern as UxPlay) ------------------------

std::string get_str(plist_t parent, const char* key) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_STRING) return {};
    char* raw = nullptr;
    plist_get_string_val(n, &raw);
    std::string s = raw ? raw : "";
    std::free(raw);
    return s;
}

uint64_t get_uint(plist_t parent, const char* key, uint64_t fallback = 0) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_UINT) return fallback;
    uint64_t v = 0;
    plist_get_uint_val(n, &v);
    return v;
}

bool get_bool(plist_t parent, const char* key, bool fallback = false) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n) return fallback;
    uint8_t v = 0;
    plist_get_bool_val(n, &v);
    return v != 0;
}

std::vector<unsigned char> get_data(plist_t parent, const char* key) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_DATA) return {};
    char*    raw = nullptr;
    uint64_t len = 0;
    plist_get_data_val(n, &raw, &len);
    std::vector<unsigned char> out(
        reinterpret_cast<unsigned char*>(raw),
        reinterpret_cast<unsigned char*>(raw) + len);
    std::free(raw);
    return out;
}

void set_uint(plist_t dict, const char* key, uint64_t v) {
    plist_dict_set_item(dict, key, plist_new_uint(v));
}

} // namespace

// --- Parser -----------------------------------------------------------------

bool parse_airplay2_setup(const unsigned char* body, std::size_t len,
                          Airplay2SetupRequest& out) {
    out = {};
    if (!body || len < 8) {
        LOG_WARN << "airplay2 setup: body too short (" << len << " bytes)";
        return false;
    }

    plist_t root = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(body),
                   static_cast<uint32_t>(len), &root);
    if (!root) {
        LOG_WARN << "airplay2 setup: body is not a valid bplist";
        return false;
    }
    if (plist_get_node_type(root) != PLIST_DICT) {
        LOG_WARN << "airplay2 setup: top-level is not a dict";
        plist_free(root);
        return false;
    }

    // --- Session-setup branch: detected by ekey + eiv presence ------------
    out.ekey = get_data(root, "ekey");
    out.eiv  = get_data(root, "eiv");
    if (!out.ekey.empty() && !out.eiv.empty()) {
        out.has_keys               = true;
        out.device_id              = get_str(root, "deviceID");
        out.model                  = get_str(root, "model");
        out.name                   = get_str(root, "name");
        out.timing_protocol        = get_str(root, "timingProtocol");
        out.timing_rport           = get_uint(root, "timingPort");
        out.is_remote_control_only = get_bool(root, "isRemoteControlOnly");

        LOG_INFO << "airplay2 SETUP (session) deviceID=" << out.device_id
                 << " model=" << out.model
                 << " name=" << out.name
                 << " timingProtocol=" << out.timing_protocol
                 << " timingPort(remote)=" << out.timing_rport
                 << " ekey=" << out.ekey.size() << "B"
                 << " eiv=" << out.eiv.size() << "B";
    }

    // --- Stream-setup branch: detected by streams[] presence ---------------
    plist_t streams = plist_dict_get_item(root, "streams");
    if (streams && plist_get_node_type(streams) == PLIST_ARRAY) {
        out.has_streams = true;
        uint32_t n = plist_array_get_size(streams);
        out.streams.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            plist_t s = plist_array_get_item(streams, i);
            if (!s || plist_get_node_type(s) != PLIST_DICT) continue;
            StreamRequest sr;
            sr.type              = static_cast<int>(get_uint(s, "type"));
            sr.stream_conn_id    = get_uint(s, "streamConnectionID");
            sr.remote_control_port = static_cast<int>(get_uint(s, "controlPort"));
            sr.ct                = static_cast<int>(get_uint(s, "ct"));
            sr.spf               = static_cast<int>(get_uint(s, "spf"));
            sr.audio_format      = get_uint(s, "audioFormat");
            out.streams.push_back(sr);
            LOG_INFO << "  stream[" << i << "] type=" << sr.type
                     << " streamConnectionID=" << sr.stream_conn_id
                     << " remoteControlPort=" << sr.remote_control_port;
        }
    }

    plist_free(root);
    return out.has_keys || out.has_streams;
}

// --- Response builder -------------------------------------------------------

struct Airplay2SetupResponse::Impl {
    plist_t root = plist_new_dict();
    ~Impl() { if (root) plist_free(root); }
};

Airplay2SetupResponse::Airplay2SetupResponse()
    : impl_(std::make_unique<Impl>()) {}
Airplay2SetupResponse::~Airplay2SetupResponse() = default;

void Airplay2SetupResponse::add_session(uint16_t timing_lport) {
    // eventPort is ALWAYS 0 in mirror/audio mode per UxPlay — only timingPort
    // carries a real bound value.
    set_uint(impl_->root, "eventPort",  0);
    set_uint(impl_->root, "timingPort", timing_lport);
    LOG_INFO << "airplay2 SETUP response: eventPort=0  timingPort=" << timing_lport;
}

void Airplay2SetupResponse::add_streams(
    const std::vector<StreamRequest>&    requests,
    const std::vector<StreamAllocation>& allocated) {

    plist_t arr = plist_new_array();
    for (std::size_t i = 0; i < requests.size() && i < allocated.size(); ++i) {
        plist_t s = plist_new_dict();
        const int type = requests[i].type;
        set_uint(s, "type",     type);
        set_uint(s, "dataPort", allocated[i].data_port);
        // Per UxPlay: type 96 (audio) also carries controlPort in the response,
        // type 110 (mirror video) doesn't.
        if (type == 96) {
            set_uint(s, "controlPort", allocated[i].control_port);
        }
        plist_array_append_item(arr, s);
        LOG_INFO << "  stream[" << i << "] type=" << type
                 << " -> dataPort=" << allocated[i].data_port
                 << (type == 96 ? " controlPort=" : "")
                 << (type == 96 ? std::to_string(allocated[i].control_port) : std::string());
    }
    plist_dict_set_item(impl_->root, "streams", arr);
}

std::vector<unsigned char> Airplay2SetupResponse::serialize() {
    char*    buf = nullptr;
    uint32_t len = 0;
    plist_to_bin(impl_->root, &buf, &len);
    std::vector<unsigned char> out;
    if (buf) {
        out.assign(reinterpret_cast<unsigned char*>(buf),
                   reinterpret_cast<unsigned char*>(buf) + len);
        std::free(buf);
    }
    return out;
}

} // namespace ap::airplay
