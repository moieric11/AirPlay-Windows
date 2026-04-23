#include "airplay/fcup.h"
#include "log.h"
#include "net/socket.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

namespace ap::airplay {

// Port of UxPlay's create_fcup_request (lib/fcup_request.h).
//
// Builds a "POST /event" request (PTTH/1.0 "reverse HTTP") framed as
// HTTP/1.1 on the wire with an XML plist body. The plist asks iOS to
// fetch `url` on its side and push the bytes back via POST /action
// with type=unhandledURLResponse and matching FCUP_Response_RequestID.
//
// The magic numbers sessionID=1, FCUP_Response_ClientInfo=1 and
// FCUP_Response_ClientRef=40030004 are lifted from UxPlay verbatim
// (they're from apsdk-public and appear to be arbitrary but iOS
// accepts them).
bool send_fcup_request(int fd,
                       const std::string& url,
                       const std::string& session_id,
                       int request_id) {
    if (fd < 0 || url.empty() || session_id.empty()) return false;

#if !defined(HAVE_LIBPLIST)
    (void)request_id;
    LOG_WARN << "FCUP: libplist missing, cannot build request";
    return false;
#else
    plist_t root    = plist_new_dict();
    plist_dict_set_item(root, "sessionID", plist_new_uint(1));
    plist_dict_set_item(root, "type",
        plist_new_string("unhandledURLRequest"));

    plist_t fcup = plist_new_dict();
    plist_dict_set_item(fcup, "FCUP_Response_ClientInfo",
        plist_new_uint(1));
    plist_dict_set_item(fcup, "FCUP_Response_ClientRef",
        plist_new_uint(40030004));
    plist_dict_set_item(fcup, "FCUP_Response_RequestID",
        plist_new_uint(request_id));
    plist_dict_set_item(fcup, "FCUP_Response_URL",
        plist_new_string(url.c_str()));
    plist_dict_set_item(fcup, "sessionID", plist_new_uint(1));

    plist_t hdrs = plist_new_dict();
    plist_dict_set_item(hdrs, "X-Playback-Session-Id",
        plist_new_string(session_id.c_str()));
    plist_dict_set_item(hdrs, "User-Agent",
        plist_new_string(
            "AppleCoreMedia/1.0.0.11B554a (Apple TV; U; CPU OS 7_0_4 "
            "like Mac OS X; en_us"));
    plist_dict_set_item(fcup, "FCUP_Response_Headers", hdrs);

    plist_dict_set_item(root, "request", fcup);

    char*    xml     = nullptr;
    uint32_t xml_len = 0;
    plist_to_xml(root, &xml, &xml_len);
    plist_free(root);
    if (!xml || xml_len == 0) {
        if (xml) std::free(xml);
        LOG_ERROR << "FCUP: plist_to_xml failed";
        return false;
    }

    std::string req;
    req.reserve(256 + xml_len);
    req.append("POST /event HTTP/1.1\r\n");
    req.append("X-Apple-Session-ID: ").append(session_id).append("\r\n");
    req.append("Content-Type: text/x-apple-plist+xml\r\n");
    char cl[64];
    std::snprintf(cl, sizeof(cl), "Content-Length: %u\r\n", xml_len);
    req.append(cl);
    req.append("\r\n");
    req.append(xml, xml_len);
    std::free(xml);

    const int n = ap::net::send_all(fd, req.data(),
                                    static_cast<int>(req.size()));
    if (n < 0) {
        LOG_ERROR << "FCUP: send_all failed on reverse fd=" << fd;
        return false;
    }
    LOG_INFO << "FCUP sent " << req.size() << "B on reverse fd=" << fd
             << " (request_id=" << request_id << ", url=" << url << ')';
    return true;
#endif
}

} // namespace ap::airplay
