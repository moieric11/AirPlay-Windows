#include "airplay/sdp.h"
#include "log.h"

#include <sstream>
#include <string>

namespace ap::airplay {
namespace {

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r");
    auto b = s.find_last_not_of(" \t\r");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

// Split "<key>:<value>" — keeps value intact (useful for base64 blobs that
// contain '=' at the tail). Returns empty strings if no ':' found.
std::pair<std::string, std::string> split_colon(const std::string& s) {
    auto p = s.find(':');
    if (p == std::string::npos) return {s, ""};
    return {s.substr(0, p), s.substr(p + 1)};
}

} // namespace

bool parse_sdp(const std::string& text, SdpSession& out) {
    out = {};

    std::istringstream is(text);
    std::string line;
    bool saw_v = false;
    SdpMedia* current = nullptr;

    while (std::getline(is, line)) {
        line = trim(line);
        if (line.size() < 2 || line[1] != '=') continue;
        const char type  = line[0];
        const std::string val = line.substr(2);

        switch (type) {
            case 'v':
                saw_v = true;
                break;
            case 'o': {
                // o=<username> <session> <version> <nettype> <addrtype> <addr>
                std::istringstream ls(val);
                std::string user, sess, ver, nett, addrt, addr;
                ls >> user >> sess >> ver >> nett >> addrt >> addr;
                out.origin_ip = addr;
                break;
            }
            case 'c': {
                // c=<nettype> <addrtype> <addr>
                std::istringstream ls(val);
                std::string nett, addrt, addr;
                ls >> nett >> addrt >> addr;
                out.connection_ip = addr;
                break;
            }
            case 'm': {
                // m=<type> <port> <proto> <fmt...>
                std::istringstream ls(val);
                SdpMedia m;
                std::string port, proto;
                ls >> m.type >> port >> proto >> m.payload_type;
                out.medias.push_back(m);
                current = &out.medias.back();
                break;
            }
            case 'a': {
                auto [k, v] = split_colon(val);
                if      (k == "rtpmap"      && current) current->rtpmap = v;
                else if (k == "fmtp"        && current) current->fmtp   = v;
                else if (k == "rsaaeskey")               out.rsaaeskey_b64 = v;
                else if (k == "aesiv")                   out.aesiv_b64     = v;
                break;
            }
            default:
                break;
        }
    }

    if (!saw_v) {
        LOG_WARN << "SDP parse: body missing `v=` line";
        return false;
    }
    return true;
}

} // namespace ap::airplay
