#include "airplay/daap.h"
#include "log.h"

#include <cstring>

namespace ap::airplay {
namespace {

// DMAP (Digital Media Access Protocol) envelope, per Apple's DACP spec:
//   [4 ASCII bytes tag][4 bytes BE length][length bytes of payload]
// Containers (mlit, mcon, mlcl, …) nest the same structure inside their
// payload. Leaf tags hold either a UTF-8 string, an integer, or a blob.
struct Entry {
    char        tag[4];
    uint32_t    length;
    const unsigned char* data;
};

bool parse_one(const unsigned char* p, std::size_t left, Entry& e) {
    if (left < 8) return false;
    std::memcpy(e.tag, p, 4);
    e.length = (static_cast<uint32_t>(p[4]) << 24)
             | (static_cast<uint32_t>(p[5]) << 16)
             | (static_cast<uint32_t>(p[6]) <<  8)
             |  static_cast<uint32_t>(p[7]);
    if (8 + static_cast<std::size_t>(e.length) > left) return false;
    e.data = p + 8;
    return true;
}

bool tag_equals(const char (&t)[4], const char* s) {
    return t[0] == s[0] && t[1] == s[1] && t[2] == s[2] && t[3] == s[3];
}

std::string utf8(const unsigned char* p, uint32_t n) {
    return std::string(reinterpret_cast<const char*>(p), n);
}

// Walk all children of a container, applying `visit` to each leaf and
// recursing into known containers.
void walk_children(const unsigned char* p, std::size_t total,
                   DaapMetadata& out, int depth = 0) {
    if (depth > 4) return;  // DMAP rarely nests more than a couple levels
    std::size_t pos = 0;
    while (pos + 8 <= total) {
        Entry e;
        if (!parse_one(p + pos, total - pos, e)) return;

        if      (tag_equals(e.tag, "minm")) out.title    = utf8(e.data, e.length);
        else if (tag_equals(e.tag, "asar")) out.artist   = utf8(e.data, e.length);
        else if (tag_equals(e.tag, "asal")) out.album    = utf8(e.data, e.length);
        else if (tag_equals(e.tag, "astm") && e.length == 4) {
            out.duration_ms = (static_cast<uint32_t>(e.data[0]) << 24)
                            | (static_cast<uint32_t>(e.data[1]) << 16)
                            | (static_cast<uint32_t>(e.data[2]) <<  8)
                            |  static_cast<uint32_t>(e.data[3]);
        }
        // A few containers iOS sometimes wraps metadata in.
        else if (tag_equals(e.tag, "mlit") || tag_equals(e.tag, "mcon") ||
                 tag_equals(e.tag, "mlcl")) {
            walk_children(e.data, e.length, out, depth + 1);
        }

        pos += 8 + e.length;
    }
}

} // namespace

bool parse_daap_mlit(const unsigned char* body, std::size_t len,
                     DaapMetadata& out) {
    out = {};
    if (!body || len < 8) return false;

    // Either the body IS an mlit container, or the body is a series of
    // leaves at the top level. Handle both by starting a walk at the root.
    walk_children(body, len, out);
    return !(out.title.empty() && out.artist.empty() && out.album.empty()
             && out.duration_ms == 0);
}

} // namespace ap::airplay
