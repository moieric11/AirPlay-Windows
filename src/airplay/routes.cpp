#include "airplay/routes.h"
#include "airplay/client_session.h"
#include "airplay/info_plist.h"
#include "crypto/pair_verify.h"
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

// /info returns a binary plist ("bplist00") describing the receiver's
// capabilities. iOS parses this to decide whether to proceed to pair-setup.
// Payload structure is copied from UxPlay — see info_plist.cpp.
Response handle_info(const DeviceContext& ctx, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);

    auto body = build_info_plist(ctx);
    if (!body.empty()) {
        r.set_header("Content-Type", "application/x-apple-binary-plist");
        r.body = std::move(body);
    } else {
        // Degraded fallback (libplist missing at build time).
        r.set_header("Content-Type", "text/plain");
        std::string s = "name=" + ctx.name + "\ndeviceid=" + ctx.deviceid + '\n';
        r.body.assign(s.begin(), s.end());
        LOG_WARN << "/info serving text fallback — iOS won't accept this";
    }
    return r;
}

// /pair-setup (no-SRP variant used by UxPlay for headless mirroring).
// iOS POSTs an empty (or 4-byte) body; we respond with our 32-byte Ed25519
// public key. This is the path that skips the 4-digit PIN flow entirely —
// acceptable because our `statusFlags=4` advertises "no password required".
// Full SRP-6a is only needed if we later want PIN-protected pairing.
Response handle_pair_setup(const DeviceContext& ctx, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "application/octet-stream");

    if (ctx.public_key.size() != 32) {
        LOG_ERROR << "pair-setup: public_key size=" << ctx.public_key.size()
                  << " (expected 32) — identity not loaded?";
        r.status_code = 500;
        r.status_text = "Internal Server Error";
        return r;
    }
    r.body = ctx.public_key;
    LOG_INFO << "pair-setup: returned 32-byte Ed25519 public key";
    return r;
}

// /pair-verify is a two-round handshake. iOS distinguishes the rounds by
// the first byte of the body: non-zero = round 1 (send pubkeys), zero =
// round 2 (send encrypted signature). We dispatch based on body length
// as an extra safety net (68 bytes in both rounds, but the content differs).
Response handle_pair_verify(const DeviceContext& ctx, ClientSession& session,
                            const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "application/octet-stream");

    const auto& body = req.body;
    if (body.size() < 4) {
        LOG_ERROR << "pair-verify: body too short (" << body.size() << ")";
        r.status_code = 400; r.status_text = "Bad Request";
        return r;
    }

    if (!session.pair_verify) {
        session.pair_verify = std::make_unique<ap::crypto::PairVerifySession>(
            *ctx.identity);
    }

    const bool is_round1 = body[0] != 0;
    if (is_round1) {
        std::vector<unsigned char> out;
        if (!session.pair_verify->handle_message1(body.data(), body.size(), out)) {
            r.status_code = 470; r.status_text = "Connection Authorization Required";
            return r;
        }
        r.body = std::move(out);
        return r;
    }

    if (!session.pair_verify->handle_message2(body.data(), body.size())) {
        r.status_code = 470; r.status_text = "Connection Authorization Required";
        return r;
    }
    // Verified: empty 200 OK.
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

Response dispatch(const DeviceContext& ctx, ClientSession& session,
                  const Request& req) {
    LOG_INFO << req.method << ' ' << req.uri
             << "  body=" << req.body.size() << "B";

    // Order matches the rough handshake sequence iOS initiates.
    if (req.method == "GET"  && req.uri == "/info")          return handle_info(ctx, req);
    if (req.method == "POST" && req.uri == "/pair-setup")    return handle_pair_setup(ctx, req);
    if (req.method == "POST" && req.uri == "/pair-verify")   return handle_pair_verify(ctx, session, req);
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
