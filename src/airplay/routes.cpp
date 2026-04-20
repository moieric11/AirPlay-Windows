#include "airplay/routes.h"
#include "log.h"

#include <sstream>

namespace ap::airplay {
namespace {

Response make(int code, const std::string& text) {
    Response r;
    r.status_code = code;
    r.status_text = text;
    return r;
}

void copy_cseq(const Request& req, Response& res) {
    auto cseq = req.header("cseq");
    if (!cseq.empty()) res.set_header("CSeq", cseq);
}

// Minimal /info response. A real implementation returns a binary plist;
// iOS is tolerant enough in logs to show us progress with a placeholder
// text body while we plug the plist serializer in a later iteration.
Response handle_info(const DeviceContext& ctx, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "text/plain");

    std::ostringstream os;
    os << "name="     << ctx.name     << '\n'
       << "deviceid=" << ctx.deviceid << '\n'
       << "model="    << ctx.model    << '\n'
       << "pi="       << ctx.pi       << '\n'
       << "features=" << ctx.features << '\n'
       << "srcvers="  << ctx.srcvers  << '\n';
    std::string s = os.str();
    r.body.assign(s.begin(), s.end());
    LOG_WARN << "/info returning placeholder text; TODO: binary plist";
    return r;
}

Response handle_unimplemented(const Request& req, const char* what) {
    Response r = make(501, "Not Implemented");
    copy_cseq(req, r);
    r.set_header("Content-Type", "text/plain");
    std::string body = std::string("TODO: ") + what + " not implemented in skeleton\n";
    r.body.assign(body.begin(), body.end());
    LOG_WARN << "stub response for " << req.method << ' ' << req.uri
             << " (" << what << ')';
    return r;
}

} // namespace

Response dispatch(const DeviceContext& ctx, const Request& req) {
    LOG_INFO << req.method << ' ' << req.uri
             << "  body=" << req.body.size() << "B";

    // Order matches the rough handshake sequence iOS initiates.
    if (req.method == "GET"  && req.uri == "/info")          return handle_info(ctx, req);
    if (req.method == "POST" && req.uri == "/pair-setup")    return handle_unimplemented(req, "pair-setup (SRP-6a)");
    if (req.method == "POST" && req.uri == "/pair-verify")   return handle_unimplemented(req, "pair-verify (Curve25519)");
    if (req.method == "POST" && req.uri == "/fp-setup")      return handle_unimplemented(req, "FairPlay SAP");
    if (req.method == "POST" && req.uri == "/auth-setup")    return handle_unimplemented(req, "auth-setup");
    if (req.method == "POST" && req.uri == "/feedback")      {
        Response r = make(200, "OK");
        copy_cseq(req, r);
        return r;
    }
    if (req.method == "OPTIONS") {
        Response r = make(200, "OK");
        copy_cseq(req, r);
        r.set_header("Public",
            "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
            "GET_PARAMETER, SET_PARAMETER, POST, GET");
        return r;
    }
    if (req.method == "ANNOUNCE")    return handle_unimplemented(req, "ANNOUNCE (SDP)");
    if (req.method == "SETUP")       return handle_unimplemented(req, "SETUP (stream ports)");
    if (req.method == "RECORD")      return handle_unimplemented(req, "RECORD");
    if (req.method == "TEARDOWN")    {
        Response r = make(200, "OK");
        copy_cseq(req, r);
        return r;
    }
    if (req.method == "GET_PARAMETER" || req.method == "SET_PARAMETER") {
        Response r = make(200, "OK");
        copy_cseq(req, r);
        return r;
    }

    return handle_unimplemented(req, "unknown route");
}

} // namespace ap::airplay
