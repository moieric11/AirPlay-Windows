#include "airplay/routes.h"
#include "airplay/airplay2_setup.h"
#include "airplay/client_session.h"
#include "airplay/info_plist.h"
#include "airplay/sdp.h"
#include "airplay/streams.h"
#include "crypto/fairplay.h"
#include "crypto/pair_verify.h"
#include "log.h"

#include <sstream>
#include <string>

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

// /fp-setup is the FairPlay SAP handshake. Two POSTs on the same TCP
// connection: 16-byte msg1 → 142-byte msg2, then 164-byte msg3 → 32-byte
// msg4. Crypto blobs live in third_party/fairplay_blobs_stub.cpp (replaced
// by the real UxPlay-extracted file at provision time — see
// docs/FAIRPLAY.md). Until then the framing is correct but the crypto is
// zeros, so iOS will reject at this step.
Response handle_fp_setup(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "application/octet-stream");

    if (!session.fairplay) {
        session.fairplay = std::make_unique<ap::crypto::FairPlaySession>();
    }

    std::vector<unsigned char> out;
    if (!session.fairplay->process(req.body.data(), req.body.size(), out)) {
        r.status_code = 400;
        r.status_text = "Bad Request";
        return r;
    }
    r.body = std::move(out);
    return r;
}

// ANNOUNCE carries an SDP body describing the media the client will send.
// We parse what we can (iOS is friendly enough to include codec info even
// when the body is heavily FairPlay-encrypted-key-laden) and stash it on
// the session so SETUP can reference it.
Response handle_announce(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);

    auto sdp = std::make_unique<SdpSession>();
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());
    if (!parse_sdp(body_str, *sdp)) {
        LOG_WARN << "ANNOUNCE: SDP body rejected (not SDP?)";
        r.status_code = 400;
        r.status_text = "Bad Request";
        return r;
    }
    LOG_INFO << "ANNOUNCE medias=" << sdp->medias.size()
             << "  rsaaeskey="    << (sdp->rsaaeskey_b64.empty() ? "no" : "yes")
             << "  aesiv="        << (sdp->aesiv_b64.empty()     ? "no" : "yes");
    for (const auto& m : sdp->medias) {
        LOG_INFO << "  m=" << m.type << " pt=" << m.payload_type
                 << " rtpmap=\"" << m.rtpmap << "\"";
    }
    session.sdp = std::move(sdp);
    return r;
}

// Legacy RTSP SETUP (no body, Transport header). Kept for the Python test
// suite and completeness — real iOS uses the AirPlay 2 plist body path.
Response setup_legacy_path(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);

    auto transport = req.header("transport");
    if (!session.streams) session.streams = std::make_unique<StreamSession>();

    StreamPorts allocated;
    if (!session.streams->setup_legacy(transport, allocated)) {
        r.status_code = 500; r.status_text = "Internal Server Error";
        return r;
    }

    std::ostringstream os;
    os << "RTP/AVP/UDP;unicast;mode=record"
       << ";server_port=" << allocated.server;
    if (allocated.control) os << ";control_port=" << allocated.control;
    if (allocated.timing)  os << ";timing_port="  << allocated.timing;
    r.set_header("Transport", os.str());
    r.set_header("Session",   session.streams->session_id());
    return r;
}

// AirPlay 2 SETUP. Ported from UxPlay's raop_handler_setup: two independent
// branches (session / streams) may both fire on the SAME request, so we
// NEVER if/else between them. A request with ekey+eiv is a "session" setup;
// a request with streams[] is a "stream" setup; a request with both is a
// combined setup (rare but allowed).
//
// NOTE: even when this handler returns 200 with a correct plist, iPhone
// will disconnect after ~30s because the UDP timing port is bound but
// nothing is listening for NTP queries from iOS. Porting the NTP
// responder from UxPlay's lib/raop_ntp.c is the next milestone.
Response setup_airplay2_path(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "application/x-apple-binary-plist");

    Airplay2SetupRequest parsed;
    if (!parse_airplay2_setup(req.body.data(), req.body.size(), parsed)) {
        r.status_code = 400; r.status_text = "Bad Request";
        return r;
    }

    if (!session.streams) session.streams = std::make_unique<StreamSession>();

    Airplay2SetupResponse response;

    // Branch 1: session setup (ekey + eiv present).
    if (parsed.has_keys) {
        if (parsed.is_remote_control_only) {
            LOG_ERROR << "airplay2 SETUP: client requested isRemoteControlOnly — "
                         "only legacy NTP timing is supported";
            r.status_code = 500; r.status_text = "Internal Server Error";
            return r;
        }
        // TODO: pass parsed.ekey through fairplay_decrypt() to recover the
        // AES-128 stream key, then stash it + parsed.eiv on the session.
        // Requires level-B playfair — see docs/FAIRPLAY.md. For now we just
        // log and keep going: iPhone only checks fairplay return during
        // actual stream decryption, not during SETUP itself.
        LOG_WARN << "airplay2 SETUP: fairplay_decrypt skipped (playfair not yet wired)";

        uint16_t event_port = 0, timing_port = 0;
        if (!session.streams->setup_session(event_port, timing_port)) {
            r.status_code = 500; r.status_text = "Internal Server Error";
            return r;
        }
        // `event_port` is bound but unused; UxPlay always returns 0.
        (void)event_port;
        response.add_session(timing_port);
    }

    // Branch 2: stream setup (streams[] present).
    if (parsed.has_streams) {
        std::vector<StreamAllocation> alloc(parsed.streams.size());
        for (std::size_t i = 0; i < parsed.streams.size(); ++i) {
            uint16_t d = 0, c = 0;
            if (!session.streams->setup_stream(parsed.streams[i].type, d, c)) {
                LOG_ERROR << "airplay2 SETUP: could not bind stream #" << i;
                r.status_code = 500; r.status_text = "Internal Server Error";
                return r;
            }
            alloc[i].data_port    = d;
            alloc[i].control_port = c;
        }
        response.add_streams(parsed.streams, alloc);
    }

    r.body = response.serialize();
    r.set_header("Session", session.streams->session_id());
    return r;
}

// Dispatch on whether SETUP carries a plist body (AirPlay 2) or a legacy
// Transport: header.
Response handle_setup(ClientSession& session, const Request& req) {
    if (!req.body.empty()) return setup_airplay2_path(session, req);

    auto transport = req.header("transport");
    if (transport.empty()) {
        Response r = make(400, "Bad Request");
        copy_cseq(req, r);
        LOG_ERROR << "SETUP: neither body (AirPlay 2) nor Transport header (legacy)";
        return r;
    }
    return setup_legacy_path(session, req);
}

Response handle_record(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);

    if (!session.streams) {
        LOG_WARN << "RECORD without prior SETUP";
        r.status_code = 455; r.status_text = "Method Not Valid In This State";
        return r;
    }
    // iOS expects Session on RECORD response as well.
    r.set_header("Session", session.streams->session_id());
    // TODO: once FairPlay blobs are provisioned and stream keys decrypted,
    // spin up workers here that read from the bound UDP sockets. For now
    // iOS's packets queue in the kernel — enough to confirm via Wireshark
    // that the client did reach RECORD and is actively pushing media.
    LOG_INFO << "RECORD: session=" << session.streams->session_id()
             << " — stream worker not started (FairPlay blobs required)";
    return r;
}

Response handle_teardown(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    if (session.streams) {
        LOG_INFO << "TEARDOWN session=" << session.streams->session_id();
        session.streams.reset();
    }
    session.sdp.reset();
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
    if (req.method == "POST" && req.uri == "/fp-setup")      return handle_fp_setup(session, req);
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
    if (req.method == "ANNOUNCE")    return handle_announce(session, req);
    if (req.method == "SETUP")       return handle_setup   (session, req);
    if (req.method == "RECORD")      return handle_record  (session, req);
    if (req.method == "TEARDOWN")    return handle_teardown(session, req);
    if (req.method == "GET_PARAMETER" || req.method == "SET_PARAMETER") {
        Response r = make(200, "OK");
        copy_cseq(req, r);
        return r;
    }

    return handle_unimplemented(req, "unknown route");
}

} // namespace ap::airplay
