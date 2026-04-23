#include "airplay/info_plist.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

namespace ap::airplay {
namespace {

#if defined(HAVE_LIBPLIST)

// mDNS TXT advertises `features` as two 32-bit hex words "0xLOW,0xHIGH".
// The /info plist expects a single 64-bit integer: high << 32 | low.
uint64_t parse_features(const std::string& s) {
    auto comma = s.find(',');
    uint64_t low  = 0;
    uint64_t high = 0;
    try {
        low = std::stoull(s.substr(0, comma), nullptr, 16);
        if (comma != std::string::npos) {
            high = std::stoull(s.substr(comma + 1), nullptr, 16);
        }
    } catch (...) {
        LOG_WARN << "could not parse features string: " << s;
    }
    return (high << 32) | (low & 0xFFFFFFFFULL);
}

void set_str (plist_t d, const char* k, const std::string& v) {
    plist_dict_set_item(d, k, plist_new_string(v.c_str()));
}
void set_uint(plist_t d, const char* k, uint64_t v) {
    plist_dict_set_item(d, k, plist_new_uint(v));
}
void set_bool(plist_t d, const char* k, bool v) {
    plist_dict_set_item(d, k, plist_new_bool(v ? 1 : 0));
}

plist_t make_display() {
    plist_t d = plist_new_dict();
    set_uint(d, "features",      14);           // see UxPlay: display feature bits
    set_uint(d, "width",         1920);
    set_uint(d, "height",        1080);
    set_uint(d, "widthPixels",   1920);
    set_uint(d, "heightPixels",  1080);
    set_uint(d, "widthPhysical",  0);
    set_uint(d, "heightPhysical", 0);
    set_uint(d, "refreshRate",   60);
    set_uint(d, "maxFPS",        60);
    set_uint(d, "rotation",      0);
    set_bool(d, "overscanned",   false);
    set_str (d, "uuid", "e5f7a168-f1f9-4f51-9f9e-6a9cc0c8cc9f");
    return d;
}

plist_t make_audio_format(int type) {
    plist_t d = plist_new_dict();
    set_uint(d, "type",               type);
    set_uint(d, "audioInputFormats",  0x03FFFFFC);   // all PCM/ALAC/AAC variants
    set_uint(d, "audioOutputFormats", 0x03FFFFFC);
    return d;
}

plist_t make_audio_latency(int type) {
    plist_t d = plist_new_dict();
    set_str (d, "audioType",            "default");
    set_uint(d, "type",                 type);
    set_uint(d, "inputLatencyMicros",   3000);
    set_uint(d, "outputLatencyMicros",  90000);
    return d;
}

#endif // HAVE_LIBPLIST

} // namespace

std::vector<unsigned char> build_info_plist(const DeviceContext& ctx) {
#if !defined(HAVE_LIBPLIST)
    (void)ctx;
    LOG_WARN << "libplist unavailable — cannot build binary /info plist";
    return {};
#else
    plist_t root = plist_new_dict();

    set_str (root, "deviceid",      ctx.deviceid);
    set_str (root, "macAddress",    ctx.deviceid);
    set_str (root, "model",         ctx.model);
    set_str (root, "name",          ctx.name);
    set_str (root, "pi",            ctx.pi);
    set_str (root, "sourceVersion", ctx.srcvers);
    set_uint(root, "features",      parse_features(ctx.features));
    set_uint(root, "statusFlags",   68);   // UxPlay default (bits 2+6 set)
    set_uint(root, "vv",            2);
    set_bool(root, "keepAliveLowPower",        true);
    set_bool(root, "keepAliveSendStatsAsBody", true);

    // Ed25519 public key from the persisted Identity. A freshly-generated
    // receiver and a zero blob both let iOS proceed past /info, but the
    // real key is what pair-verify later matches against.
    if (!ctx.public_key.empty()) {
        plist_dict_set_item(root, "pk", plist_new_data(
            reinterpret_cast<const char*>(ctx.public_key.data()),
            ctx.public_key.size()));
    } else {
        LOG_WARN << "DeviceContext.public_key empty — /info pk=<32 zeros>";
        unsigned char zero[32] = {0};
        plist_dict_set_item(root, "pk",
            plist_new_data(reinterpret_cast<const char*>(zero), sizeof(zero)));
    }

    plist_t displays = plist_new_array();
    plist_array_append_item(displays, make_display());
    plist_dict_set_item(root, "displays", displays);

    plist_t formats = plist_new_array();
    plist_array_append_item(formats, make_audio_format(100));
    plist_array_append_item(formats, make_audio_format(101));
    plist_dict_set_item(root, "audioFormats", formats);

    plist_t latencies = plist_new_array();
    plist_array_append_item(latencies, make_audio_latency(100));
    plist_dict_set_item(root, "audioLatencies", latencies);

    char*    data = nullptr;
    uint32_t len  = 0;
    plist_to_bin(root, &data, &len);
    plist_free(root);

    if (!data || len == 0) {
        LOG_ERROR << "plist_to_bin failed";
        if (data) std::free(data);
        return {};
    }

    std::vector<unsigned char> out(data, data + len);
    std::free(data);
    return out;
#endif
}

} // namespace ap::airplay
