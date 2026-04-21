#include "airplay/airplay2_setup.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <plist/plist.h>

namespace ap::airplay {
namespace {

// Small helpers that handle missing / wrong-type nodes gracefully.
uint64_t get_uint(plist_t parent, const char* key, uint64_t fallback = 0) {
    plist_t node = plist_dict_get_item(parent, key);
    if (!node || plist_get_node_type(node) != PLIST_UINT) return fallback;
    uint64_t v = 0;
    plist_get_uint_val(node, &v);
    return v;
}

void dict_set_uint(plist_t dict, const char* key, uint64_t v) {
    plist_dict_set_item(dict, key, plist_new_uint(v));
}

std::vector<unsigned char> plist_to_vec(plist_t root) {
    char*    buf = nullptr;
    uint32_t len = 0;
    plist_to_bin(root, &buf, &len);
    std::vector<unsigned char> out;
    if (buf) {
        out.assign(reinterpret_cast<unsigned char*>(buf),
                   reinterpret_cast<unsigned char*>(buf) + len);
        std::free(buf);
    }
    return out;
}

} // namespace

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
        LOG_WARN << "airplay2 setup: top-level node is not a dict";
        plist_free(root);
        return false;
    }

    plist_t streams = plist_dict_get_item(root, "streams");
    if (streams && plist_get_node_type(streams) == PLIST_ARRAY) {
        out.has_streams = true;
        uint32_t n = plist_array_get_size(streams);
        out.streams.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            plist_t s = plist_array_get_item(streams, i);
            if (!s || plist_get_node_type(s) != PLIST_DICT) continue;
            StreamRequest req;
            req.type           = static_cast<int>(get_uint(s, "type"));
            req.control_port   = static_cast<int>(get_uint(s, "controlPort"));
            req.stream_conn_id = get_uint(s, "streamConnectionID");
            out.streams.push_back(req);
            LOG_INFO << "  stream[" << i << "] type=" << req.type
                     << " controlPort=" << req.control_port;
        }
    } else {
        // Session setup — log a few helpful fields.
        plist_t uuid = plist_dict_get_item(root, "sessionUUID");
        char* uuid_s = nullptr;
        if (uuid && plist_get_node_type(uuid) == PLIST_STRING) {
            plist_get_string_val(uuid, &uuid_s);
        }
        uint64_t tp = get_uint(root, "timingPort");
        LOG_INFO << "airplay2 setup: session"
                 << "  sessionUUID=" << (uuid_s ? uuid_s : "?")
                 << "  timingPort=" << tp;
        if (uuid_s) std::free(uuid_s);
    }

    plist_free(root);
    return true;
}

std::vector<unsigned char>
build_airplay2_setup_session_response(uint16_t event_port, uint16_t timing_port) {
    plist_t root = plist_new_dict();
    dict_set_uint(root, "eventPort",  event_port);
    dict_set_uint(root, "timingPort", timing_port);
    auto out = plist_to_vec(root);
    plist_free(root);
    LOG_INFO << "airplay2 setup session response: event="
             << event_port << " timing=" << timing_port;
    return out;
}

std::vector<unsigned char>
build_airplay2_setup_streams_response(
    const std::vector<StreamRequest>&    requests,
    const std::vector<StreamAllocation>& allocated) {

    plist_t root = plist_new_dict();
    plist_t arr  = plist_new_array();

    for (std::size_t i = 0; i < requests.size() && i < allocated.size(); ++i) {
        plist_t s = plist_new_dict();
        dict_set_uint(s, "type",        requests[i].type);
        dict_set_uint(s, "dataPort",    allocated[i].data_port);
        dict_set_uint(s, "controlPort", allocated[i].control_port);
        if (requests[i].stream_conn_id) {
            dict_set_uint(s, "streamConnectionID", requests[i].stream_conn_id);
        }
        plist_array_append_item(arr, s);
        LOG_INFO << "airplay2 setup stream[" << i << "] type="
                 << requests[i].type << " -> data=" << allocated[i].data_port
                 << " control=" << allocated[i].control_port;
    }
    plist_dict_set_item(root, "streams", arr);

    auto out = plist_to_vec(root);
    plist_free(root);
    return out;
}

} // namespace ap::airplay
