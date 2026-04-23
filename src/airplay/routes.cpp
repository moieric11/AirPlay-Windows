#include "airplay/routes.h"
#include "airplay/airplay2_setup.h"
#include "airplay/client_session.h"
#include "airplay/daap.h"
#include "airplay/fcup.h"
#include "airplay/hls_session.h"
#include "airplay/info_plist.h"
#include "airplay/reverse_channel.h"
#include "airplay/sdp.h"
#include "airplay/streams.h"
#include "crypto/fairplay.h"
#include "crypto/pair_verify.h"
#include "log.h"
#include "video/hls_player.h"
#include "video/video_renderer.h"

#include <openssl/evp.h>
#if defined(HAVE_LIBPLIST)
    #include <plist/plist.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
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

    if (!session.streams) {
        session.streams = std::make_unique<StreamSession>();
        session.streams->set_renderer(session.renderer);
    }

    Airplay2SetupResponse response;

    // Branch 1: session setup (ekey + eiv present).
    if (parsed.has_keys) {
        if (parsed.is_remote_control_only) {
            LOG_ERROR << "airplay2 SETUP: client requested isRemoteControlOnly — "
                         "only legacy NTP timing is supported";
            r.status_code = 500; r.status_text = "Internal Server Error";
            return r;
        }

        // FairPlay-decrypt the 72-byte ekey into the 16-byte AES stream key
        // (requires the playfair library to be provisioned — see docs/FAIRPLAY.md).
        // UxPlay doesn't abort the SETUP if this fails; it just logs and keeps
        // going, since iOS only cares about the response shape at this stage.
        if (session.fairplay) {
            auto aes_key = session.fairplay->decrypt_stream_key(parsed.ekey);
            if (aes_key.size() == 16) {
                // Modern iOS clients require a second hashing step:
                //   aes_key = SHA-512(aes_key || ecdh_secret)[0..16]
                // where ecdh_secret is the 32-byte X25519 shared secret from
                // pair-verify. Legacy AirPlay 1 clients skip this; we always
                // apply it because iPhone / iPad / modern Mac clients do.
                // Skipping this step leaves the stream key off by a hash and
                // the H.264 payloads decrypt to noise.
                if (session.pair_verify &&
                    session.pair_verify->ecdh_secret().size() == 32) {
                    unsigned char hashed[64];
                    unsigned int  hashed_len = 0;
                    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
                    bool ok = ctx &&
                        EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) == 1 &&
                        EVP_DigestUpdate(ctx, aes_key.data(), aes_key.size()) == 1 &&
                        EVP_DigestUpdate(ctx,
                            session.pair_verify->ecdh_secret().data(), 32) == 1 &&
                        EVP_DigestFinal_ex(ctx, hashed, &hashed_len) == 1 &&
                        hashed_len == 64;
                    if (ctx) EVP_MD_CTX_free(ctx);
                    if (ok) {
                        aes_key.assign(hashed, hashed + 16);
                        LOG_INFO << "airplay2 SETUP: AES key post-hashed with "
                                    "ECDH secret (modern-client path)";
                    } else {
                        LOG_WARN << "airplay2 SETUP: SHA-512 post-hash failed; "
                                    "using raw fairplay_decrypt output";
                    }
                } else {
                    LOG_WARN << "airplay2 SETUP: no ECDH secret from pair-verify; "
                                "using raw fairplay_decrypt output (likely broken)";
                }
                session.aes_key = std::move(aes_key);
                session.aes_iv  = parsed.eiv;
                LOG_INFO << "airplay2 SETUP: stream AES key recovered (16 B) "
                            "=== AirPlay session START: "
                         << parsed.name << " (" << parsed.model << ") "
                         << session.remote_ip << " ===";
            } else {
                LOG_WARN << "airplay2 SETUP: fairplay_decrypt failed — "
                            "stream will not be decryptable";
            }
        } else {
            LOG_WARN << "airplay2 SETUP: no FairPlaySession (fp-setup skipped?)";
        }

        uint16_t event_port = 0, timing_port = 0;
        if (!session.streams->setup_session(event_port, timing_port)) {
            r.status_code = 500; r.status_text = "Internal Server Error";
            return r;
        }
        // `event_port` is bound but unused; UxPlay always returns 0.
        (void)event_port;
        response.add_session(timing_port);

        // Start the NTP client polling iOS's timing server. Without this,
        // iOS tears down the session ~8 seconds after stream SETUP because
        // it can't sync the media clock.
        if (parsed.timing_rport && !session.remote_ip.empty()) {
            session.streams->start_ntp(session.remote_ip,
                                       static_cast<uint16_t>(parsed.timing_rport));
        } else {
            LOG_WARN << "skipping NTP client: timing_rport="
                     << parsed.timing_rport
                     << " remote_ip=\"" << session.remote_ip << '"';
        }
    }

    // Branch 2: stream setup (streams[] present).
    if (parsed.has_streams) {
        std::vector<StreamAllocation> alloc(parsed.streams.size());
        for (std::size_t i = 0; i < parsed.streams.size(); ++i) {
            uint16_t d = 0, c = 0;

            // Build per-stream options — forward the session-level AES key
            // + IV, plus type-specific fields (conn ID for mirror, ct /
            // sample rate for audio).
            StreamSession::StreamOpts opts;
            opts.aes_key              = session.aes_key;
            opts.aes_iv               = session.aes_iv;
            opts.stream_connection_id = parsed.streams[i].stream_conn_id;
            opts.ct                   = parsed.streams[i].ct;
            opts.sample_rate          = 44100;  // TODO: parse from SETUP when != 44.1k

            if (!session.streams->setup_stream(parsed.streams[i].type, d, c, opts)) {
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
    // Resume-from-FLUSH path: iOS pauses with FLUSH and restarts the
    // stream with RECORD, so bring the "playing" flag back to true.
    if (session.renderer) session.renderer->push_playback_rate(1.0f);
    return r;
}

// TEARDOWN — per UxPlay raop_handler_teardown. iOS may teardown just one
// stream (type 96 audio or 110 mirror) by sending a plist body
// `{streams: [{type: N}]}`, or the whole session by sending an empty body.
// In both cases UxPlay adds `Connection: close` to the response.
Response handle_teardown(const DeviceContext& ctx, ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Connection", "close");

    // Try to parse an AirPlay 2 partial-teardown plist.
    std::vector<int> stream_types;
    if (!req.body.empty()) {
        Airplay2SetupRequest parsed;
        if (parse_airplay2_setup(req.body.data(), req.body.size(), parsed)
            && parsed.has_streams) {
            for (const auto& s : parsed.streams) stream_types.push_back(s.type);
        }
    }

    if (!stream_types.empty() && session.streams) {
        for (int t : stream_types) {
            LOG_INFO << "TEARDOWN stream type=" << t;
            session.streams->stop_stream(t);
        }
        // Keep session.streams alive — iOS may retry the stream without
        // re-doing the session setup.
    } else if (session.streams) {
        LOG_INFO << "TEARDOWN full session=" << session.streams->session_id()
                 << " — AirPlay session ended";
        session.streams.reset();
        session.sdp.reset();
        // Wipe the idle-mode UI so a new connection doesn't inherit the
        // last track's cover, metadata and progress.
        if (session.renderer) session.renderer->clear_session();
    }
    // Also stop any HLS player started by AirPlay Streaming so the
    // player thread doesn't keep fetching stale segments.
    if (ctx.hls_player) ctx.hls_player->stop();
    return r;
}

// GET_PARAMETER text/parameters. iOS queries properties of the receiver
// through this method — currently always "volume\r\n" in mirroring mode.
// UxPlay returns the current audio volume; since we don't render audio
// we answer a fixed -144.0 dB (mute) which iOS accepts.
Response handle_get_parameter(const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "text/parameters");

    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());
    std::string out;
    if (body_str.find("volume") != std::string::npos) {
        out = "volume: 0.000000\r\n";
    } else {
        LOG_WARN << "GET_PARAMETER: unknown parameter body=\"" << body_str << '"';
    }
    r.body.assign(out.begin(), out.end());
    return r;
}

// Save the cover-art JPEG that Apple Music streams via SET_PARAMETER to
// a fixed on-disk location. Later we'll surface it in the SDL window
// overlay instead of overwriting a single file.
void save_cover_art(const unsigned char* data, std::size_t len) {
    std::ofstream f("cover.jpg", std::ios::binary | std::ios::trunc);
    if (!f) { LOG_WARN << "cover.jpg: open for write failed"; return; }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    LOG_INFO << "cover art: wrote " << len << " bytes to cover.jpg";
}

// SET_PARAMETER carries three very different payloads depending on what
// iOS wants to tell us. Dispatch on Content-Type so each gets the right
// parser, and log a human-readable summary.
Response handle_set_parameter(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    if (req.body.empty()) return r;

    const auto ctype = req.header("content-type");

    // ---- text/parameters: "volume:", "progress:", "rate:" etc. ----------
    if (ctype.find("text/parameters") != std::string::npos ||
        ctype.empty()) {  // some iOS firmware omits the header
        const std::string body_str(
            reinterpret_cast<const char*>(req.body.data()),
            std::min<std::size_t>(req.body.size(), 256));

        if (body_str.compare(0, 7, "volume:") == 0) {
            try {
                float db = std::stof(body_str.substr(7));
                if (session.streams) session.streams->set_audio_volume_db(db);
            } catch (...) {
                LOG_WARN << "SET_PARAMETER: bad volume value";
            }
        } else if (body_str.compare(0, 9, "progress:") == 0) {
            // "progress: rtp_start/rtp_current/rtp_end" — three RTP
            // timestamps at the AirPlay audio sample rate (always
            // 44100 Hz for AAC-ELD / ALAC in the AirPlay 2 path).
            unsigned long long start = 0, curr = 0, end = 0;
            if (std::sscanf(body_str.c_str() + 9, " %llu/%llu/%llu",
                            &start, &curr, &end) == 3 && end >= start) {
                const uint64_t elapsed_ticks =
                    curr > start ? curr - start : 0;
                const uint64_t total_ticks   = end - start;
                const uint32_t elapsed_ms = static_cast<uint32_t>(
                    elapsed_ticks * 1000ULL / 44100ULL);
                const uint32_t total_ms   = static_cast<uint32_t>(
                    total_ticks   * 1000ULL / 44100ULL);

                // Skip / seek detection: a new `start` means a fresh track
                // (iOS re-anchors the RTP clock on each track); a big jump
                // in `curr` while `start` stays the same is a seek inside
                // the current track. 44100 ticks ≈ 1 s, so threshold 2 s.
                if (session.last_progress_start != 0 &&
                    start != session.last_progress_start) {
                    LOG_INFO << "skip: new track (start "
                             << session.last_progress_start << " -> " << start << ')';
                } else if (session.last_progress_curr != 0) {
                    const long long delta_ticks =
                        static_cast<long long>(curr) -
                        static_cast<long long>(session.last_progress_curr);
                    if (delta_ticks >  static_cast<long long>(2 * 44100)) {
                        LOG_INFO << "seek forward: +" << (delta_ticks / 44100) << 's';
                    } else if (delta_ticks < -static_cast<long long>(44100 / 2)) {
                        LOG_INFO << "seek back: " << (delta_ticks / 44100) << 's';
                    }
                }
                session.last_progress_start = start;
                session.last_progress_curr  = curr;

                LOG_INFO << "playback " << (elapsed_ms / 1000) << "s / "
                         << (total_ms / 1000) << "s";
                if (session.renderer) {
                    session.renderer->push_progress(elapsed_ms, total_ms);
                }
            } else {
                LOG_INFO << "playback " << body_str;
            }
        } else if (body_str.compare(0, 5, "rate:") == 0) {
            // "rate: 1.000000" = playing, "rate: 0.000000" = paused.
            try {
                const float rate = std::stof(body_str.substr(5));
                LOG_INFO << "playback rate=" << rate
                         << (rate > 0.5f ? " (playing)" : " (paused)");
                if (session.renderer) session.renderer->push_playback_rate(rate);
            } catch (...) {
                LOG_WARN << "SET_PARAMETER: bad rate value";
            }
        } else {
            LOG_INFO << "SET_PARAMETER body=\"" << body_str << '"';
        }
        return r;
    }

    // ---- image/*: cover art -------------------------------------------
    if (ctype.find("image/") != std::string::npos) {
        save_cover_art(req.body.data(), req.body.size());
        if (session.renderer) {
            session.renderer->push_cover_art(req.body.data(), req.body.size());
        }
        return r;
    }

    // ---- application/x-dmap-tagged: DAAP metadata ---------------------
    if (ctype.find("dmap") != std::string::npos ||
        // Sometimes iOS omits the header but the body starts with "mlit".
        (req.body.size() >= 4 && std::memcmp(req.body.data(), "mlit", 4) == 0)) {
        DaapMetadata md;
        if (parse_daap_mlit(req.body.data(), req.body.size(), md)) {
            LOG_INFO << "metadata: \"" << md.title
                     << "\" — " << md.artist
                     << " (" << md.album
                     << (md.duration_ms ? ", " + std::to_string(md.duration_ms / 1000) + "s"
                                        : std::string())
                     << ')';
            if (session.renderer) {
                session.renderer->push_metadata(md.title, md.artist, md.album);
            }
        } else {
            LOG_INFO << "SET_PARAMETER dmap body=" << req.body.size() << " B "
                        "(no recognisable mlit fields)";
        }
        return r;
    }

    LOG_INFO << "SET_PARAMETER ctype=\"" << ctype << "\" body=" << req.body.size() << " B";
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

// POST /reverse — reverse event channel setup. iOS keeps this TCP socket
// open so the server can push playback events (state, metadata, scrub,
// session end, …) back. We respond 101 Switching Protocols with
// `Upgrade: PTTH/1.0` (Push-Tunneled HTTP 1.0, Apple-specific). The
// connection stays alive afterwards — our server's per-connection loop
// will just block on the next reader.read() since iOS rarely sends
// additional requests on this socket. For now we don't push any events
// (reconnaissance step 3a); replying 101 is enough to stop the retry
// storm we saw with the previous 501 stub.
Response handle_reverse(ClientSession& session, const Request& req) {
    Response r;
    r.status_code = 101;
    r.status_text = "Switching Protocols";
    r.version     = "HTTP/1.1";
    copy_cseq(req, r);
    r.set_header("Upgrade",    "PTTH/1.0");
    r.set_header("Connection", "Upgrade");
    // Echo iOS's session id back so it correlates this channel with the
    // RTSP control socket.
    auto sid = req.header("x-apple-session-id");
    if (!sid.empty()) r.set_header("X-Apple-Session-ID", sid);
    auto purpose = req.header("x-apple-purpose");
    LOG_INFO << "/reverse channel opened (X-Apple-Purpose=\"" << purpose
             << "\", session=" << sid << ')';

    // Register this socket in the global registry so handle_play can
    // push a POST /event FCUP Request on it once iOS calls /play.
    // Server::handle_client will unregister on TCP close.
    if (!sid.empty() && session.fd >= 0) {
        session.reverse_session_id = sid;
        ReverseChannelRegistry::instance().register_socket(sid, session.fd);
    }
    return r;
}

// GET /server-info — capabilities plist the AirPlay Streaming path queries
// before trying to stream. Reuses the same info_plist builder as GET /info;
// GET /server-info — capabilities plist the AirPlay Streaming path queries
// before trying to stream. We build a dedicated 12-bit features bitmap
// here (NOT the 64-bit mDNS features). Bit 4 "HLS supported" is the
// switch that makes YouTube / iOS dispatch AirPlay Streaming rather
// than fall back to Screen Mirroring.
//
// UxPlay's http_handler_info returns 0x27F = bits 0,1,2,3,4,5,6,9:
//   0 video, 1 photo, 2 videoFairPlay, 3 volumeCtl, 4 HLS, 5 slideshow,
//   6 (unknown), 9 audio.
// Body is an XML plist (text/x-apple-plist+xml), not the binary /info.
Response handle_server_info(const DeviceContext& ctx, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
#if defined(HAVE_LIBPLIST)
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "features",       plist_new_uint(0x27F));
    plist_dict_set_item(root, "macAddress",     plist_new_string(ctx.deviceid.c_str()));
    plist_dict_set_item(root, "deviceid",       plist_new_string(ctx.deviceid.c_str()));
    plist_dict_set_item(root, "model",          plist_new_string(ctx.model.c_str()));
    plist_dict_set_item(root, "osBuildVersion", plist_new_string("12B435"));
    plist_dict_set_item(root, "protovers",      plist_new_string("1.0"));
    plist_dict_set_item(root, "srcvers",        plist_new_string(ctx.srcvers.c_str()));
    plist_dict_set_item(root, "vv",             plist_new_uint(2));
    char*    xml = nullptr;
    uint32_t xml_len = 0;
    plist_to_xml(root, &xml, &xml_len);
    plist_free(root);
    if (xml) {
        r.body.assign(xml, xml + xml_len);
        std::free(xml);
    }
    r.set_header("Content-Type", "text/x-apple-plist+xml");
#else
    r.set_header("Content-Type", "application/x-apple-binary-plist");
    r.body = build_info_plist(ctx);
#endif
    LOG_INFO << "/server-info served (" << r.body.size()
             << "B, features=0x27F incl. HLS bit 4)";
    return r;
}

// Dump a request body as readable text when possible, otherwise as hex.
// Clips at 256 bytes to keep logs manageable.
std::string snapshot_body(const std::vector<unsigned char>& body) {
    if (body.empty()) return "(empty)";
    const std::size_t max = std::min<std::size_t>(body.size(), 256);
    bool printable = true;
    for (std::size_t i = 0; i < max; ++i) {
        unsigned char c = body[i];
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') { printable = false; break; }
        if (c > 0x7e) { printable = false; break; }
    }
    std::ostringstream os;
    if (printable) {
        os << '"';
        os.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(max));
        os << (body.size() > max ? "...\"" : "\"");
    } else {
        for (std::size_t i = 0; i < max; ++i) {
            char tmp[4];
            std::snprintf(tmp, sizeof(tmp), "%02x ", body[i]);
            os << tmp;
        }
        if (body.size() > max) os << "..." << body.size() << "B total";
    }
    return os.str();
}

// POST /play: iOS asks the receiver to play a URL. This is the entry
// point for AirPlay Streaming (YouTube, Photos slideshow, Netflix,
// Apple TV+, ...). Body is either a binary plist (post-iOS 7) or the
// legacy text/parameters format.
//
// Binary plist keys of interest (reference: Apple AirPlay Protocol
// reverse-engineering notes + UxPlay's airplay_http_hls.c):
//   Content-Location     string  — HLS manifest URL
//   Start-Position       real    — fraction [0..1] of initial position
//   X-Apple-Session-UUID string  — session identifier
//   rate                 real    — 1.0 = play
//   FairPlay / FPS blob  data    — only set for DRM-protected streams
//                                   (Netflix, Apple TV+, iTunes rentals)
//
// If a FairPlay blob is present we bail: without a key-server MFi
// certificate from Apple we can't decrypt the stream. YouTube and
// Photos usually ship plain HLS that we can fetch directly.
struct PlayRequest {
    std::string url;           // Content-Location (HLS master playlist)
    std::string playback_uuid; // "uuid" key in the body (per-video)
    std::string session_id;    // X-Apple-Session-ID header (per-session)
    std::string client_proc;   // clientProcName — "YouTube" is what UxPlay gates on
    double      start_position_seconds = 0.0;
    bool        is_binary_plist        = false;
};

bool parse_play_body(const std::vector<uint8_t>& body,
                     const std::string& session_id,
                     PlayRequest& out) {
    out.session_id = session_id;
#if defined(HAVE_LIBPLIST)
    if (body.size() >= 8 &&
        std::memcmp(body.data(), "bplist00", 8) == 0) {
        out.is_binary_plist = true;
        plist_t root = nullptr;
        plist_from_bin(reinterpret_cast<const char*>(body.data()),
                       static_cast<uint32_t>(body.size()), &root);
        if (!root) return false;
        auto get_string = [](plist_t p, const char* k) -> std::string {
            plist_t n = plist_dict_get_item(p, k);
            if (!n || plist_get_node_type(n) != PLIST_STRING) return {};
            char* raw = nullptr; plist_get_string_val(n, &raw);
            std::string s = raw ? raw : ""; std::free(raw); return s;
        };
        auto get_real = [](plist_t p, const char* k, double fallback) {
            plist_t n = plist_dict_get_item(p, k);
            if (!n) return fallback;
            if (plist_get_node_type(n) == PLIST_REAL) {
                double v = 0; plist_get_real_val(n, &v); return v;
            }
            if (plist_get_node_type(n) == PLIST_UINT) {
                uint64_t v = 0; plist_get_uint_val(n, &v);
                return static_cast<double>(v);
            }
            return fallback;
        };
        // Keys match UxPlay's http_handlers.h http_handler_play.
        out.url                    = get_string(root, "Content-Location");
        out.playback_uuid          = get_string(root, "uuid");
        out.client_proc            = get_string(root, "clientProcName");
        out.start_position_seconds = get_real(root, "Start-Position-Seconds", 0.0);
        plist_free(root);
        return !out.url.empty();
    }
#endif
    // Legacy text/parameters fallback (only seen on older iOS).
    const std::string text(reinterpret_cast<const char*>(body.data()),
                           body.size());
    auto extract = [&](const std::string& key) -> std::string {
        const auto pos = text.find(key);
        if (pos == std::string::npos) return {};
        const auto start = pos + key.size();
        const auto end   = text.find_first_of("\r\n", start);
        std::string v = text.substr(start, end - start);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        return v;
    };
    out.url = extract("Content-Location:");
    try {
        const auto sp = extract("Start-Position:");
        if (!sp.empty()) out.start_position_seconds = std::stod(sp);
    } catch (...) {}
    return !out.url.empty();
}

Response handle_play(ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);

    const std::string session_id = req.header("x-apple-session-id");
    PlayRequest pr;
    if (!parse_play_body(req.body, session_id, pr)) {
        LOG_WARN << "POST /play: could not parse body ("
                 << req.body.size() << "B)  " << snapshot_body(req.body);
        return r;
    }
    LOG_INFO << "POST /play [" << pr.client_proc << "]"
             << (pr.is_binary_plist ? " (bplist00)" : " (text)")
             << "  url=" << pr.url;
    LOG_INFO << "         uuid=" << pr.playback_uuid
             << "  start=" << pr.start_position_seconds << 's'
             << "  session=" << pr.session_id;

    // UxPlay gates on clientProcName == "YouTube" — other apps (Netflix,
    // Apple TV+, Music in non-audio mode) ship FairPlay-protected HLS
    // that we can't decrypt without an MFi key-server certificate.
    if (pr.client_proc != "YouTube") {
        LOG_WARN << "POST /play: clientProcName \"" << pr.client_proc
                 << "\" not in supported list [YouTube]; skipping playback";
        return r;
    }

    // Look up the /reverse socket iOS opened for this X-Apple-Session-ID
    // and send a POST /event FCUP Request on it, asking iOS to deliver
    // the master playlist via POST /action.
    const int reverse_fd =
        ReverseChannelRegistry::instance().socket_for(pr.session_id);
    if (reverse_fd < 0) {
        LOG_WARN << "POST /play: no /reverse socket for session "
                 << pr.session_id << "; cannot send FCUP Request";
        return r;
    }
    // Remember master URL on the HLS session so handle_action can tell
    // "master" from "media" playlists by URL comparison.
    auto* hls = HlsSessionRegistry::instance().get_or_create(pr.session_id);
    hls->master_url      = pr.url;
    hls->next_request_id = 2;   // 1 is reserved for the master we're about to request

    if (!send_fcup_request(reverse_fd, pr.url, pr.session_id, /*request_id*/ 1)) {
        LOG_WARN << "POST /play: FCUP Request send failed";
    } else {
        LOG_INFO << "POST /play: FCUP Request sent for url=" << pr.url;
    }
    (void)session;
    return r;
}

// POST /action: iOS pushes back FCUP data. The request we care about
// has type=="unhandledURLResponse" with a `params` dict containing
// FCUP_Response_URL + FCUP_Response_Data (HLS playlist bytes).
// Ported from UxPlay's http_handler_action (lib/http_handlers.h).
Response handle_action(const DeviceContext& ctx, ClientSession& session, const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    const std::string sid = req.header("x-apple-session-id");

#if defined(HAVE_LIBPLIST)
    if (req.body.size() < 8 ||
        std::memcmp(req.body.data(), "bplist00", 8) != 0) {
        LOG_WARN << "POST /action: body is not a binary plist ("
                 << req.body.size() << "B)";
        return r;
    }
    plist_t root = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(req.body.data()),
                   static_cast<uint32_t>(req.body.size()), &root);
    if (!root) {
        LOG_WARN << "POST /action: plist_from_bin failed";
        return r;
    }

    auto get_string_of = [](plist_t p, const char* key, std::string& out) {
        plist_t n = plist_dict_get_item(p, key);
        if (!n || plist_get_node_type(n) != PLIST_STRING) return false;
        char* raw = nullptr; plist_get_string_val(n, &raw);
        if (!raw) return false;
        out.assign(raw);
        std::free(raw);
        return true;
    };

    std::string type;
    get_string_of(root, "type", type);

    plist_t params = plist_dict_get_item(root, "params");
    if (type != "unhandledURLResponse" || !params) {
        LOG_INFO << "POST /action type=\"" << type << "\" (unhandled)";
        plist_free(root);
        return r;
    }

    std::string url;
    get_string_of(params, "FCUP_Response_URL", url);

    plist_t data_node = plist_dict_get_item(params, "FCUP_Response_Data");
    const char* data_ptr = nullptr;
    uint64_t    data_len = 0;
    if (data_node && plist_get_node_type(data_node) == PLIST_DATA) {
        char* buf = nullptr;
        plist_get_data_val(data_node, &buf, &data_len);
        data_ptr = buf;
        // buf must be freed by the caller — we copy into a std::string
        // below and free immediately.
    }

    std::string playlist;
    if (data_ptr && data_len > 0) {
        playlist.assign(data_ptr, data_ptr + data_len);
        std::free(const_cast<char*>(data_ptr));
    }

    // Inspect FCUP_Response_StatusCode: segments on YouTube come back
    // as 302 redirects to a signed googlevideo.com URL. When data is
    // empty and we see a 302, extract the Location header and route
    // through segment_redirect so the local HLS server can 302 the
    // player down to the CDN directly.
    std::string redirect_location;
    {
        plist_t sc_node = plist_dict_get_item(params, "FCUP_Response_StatusCode");
        uint64_t status = 0;
        if (sc_node && plist_get_node_type(sc_node) == PLIST_UINT) {
            plist_get_uint_val(sc_node, &status);
        }
        if (status == 302) {
            plist_t hdrs = plist_dict_get_item(params, "FCUP_Response_Headers");
            if (hdrs && plist_get_node_type(hdrs) == PLIST_DICT) {
                get_string_of(hdrs, "Location", redirect_location);
            }
        }
    }

    LOG_INFO << "POST /action FCUP response url=" << url
             << "  playlist=" << playlist.size() << "B";

    auto* hls = HlsSessionRegistry::instance().find(sid);
    if (!hls) {
        LOG_WARN << "POST /action: no HLS session for " << sid;
        plist_free(root);
        return r;
    }

    const bool is_master = (!hls->master_url.empty() && url == hls->master_url);
    if (is_master) {
        hls->master_playlist = playlist;
        hls->media_uris      = extract_media_uris(playlist);
        LOG_INFO << "POST /action: master playlist stored ("
                 << hls->media_uris.size() << " child URIs)";
        for (std::size_t i = 0; i < hls->media_uris.size(); ++i) {
            LOG_INFO << "         [" << i << "] " << hls->media_uris[i];
        }

        // iOS only pre-delivers a subset of the child playlists (the
        // itags actually selected for playback). The others are
        // fetched on demand by the player itself, which our local
        // HTTP server forwards as FCUP misses. So don't wait for
        // all 16 to arrive — start the player as soon as we have
        // the master in hand.
        if (ctx.hls_player && ctx.renderer) {
            const std::string local_master =
                "http://localhost:" + std::to_string(ctx.hls_local_port) +
                "/master.m3u8";
            LOG_INFO << "POST /action: master stored — starting HlsPlayer on "
                     << local_master;
            ctx.hls_player->stop();   // idempotent; clear any prior video
            ctx.hls_player->start(local_master, ctx.renderer);
        }
    } else if (url.size() >= 5 &&
               url.compare(url.size() - 5, 5, ".m3u8") == 0) {
        // Media (child) playlist. Two-pass:
        //   1. Expand YouTube's "condensed" URL form (builds
        //      googlevideo.com URLs from BASE-URI + PARAMS).
        //   2. Rewrite every resulting segment URL to a local
        //      "/seg/<n>" path on http://localhost:<port>/ so FFmpeg
        //      fetches through us and we proxy via libavio. Required
        //      because the CDN URLs bind the signature to the
        //      iPhone's IP — the PC can't reach them directly.
        const std::string expanded = expand_yt_condensed_playlist(playlist);
        std::size_t rewritten = 0;
        const std::string localised = rewrite_segments_to_local(
            *hls, ctx.hls_local_port, expanded, rewritten);

        // Assert no googlevideo.com URL survived.
        const auto gv_count = [](const std::string& s) {
            std::size_t n = 0, p = 0;
            while ((p = s.find("googlevideo.com", p)) != std::string::npos) {
                ++n; p += 15;
            }
            return n;
        };
        const std::size_t after_gv = gv_count(localised);
        if (after_gv != 0) {
            LOG_WARN << "POST /action: " << after_gv
                     << " googlevideo.com URL(s) still in media playlist "
                     << "after rewrite — FFmpeg will bypass the proxy";
        }

        if (hls->media_playlists.empty()) {
            auto slice_around_first_chunk = [](const std::string& s) {
                const auto p = s.find("#EXTINF");
                if (p == std::string::npos) {
                    return s.substr(0, std::min<std::size_t>(600, s.size()));
                }
                return s.substr(p, std::min<std::size_t>(1200, s.size() - p));
            };
            LOG_INFO << "POST /action: first media playlist first chunk RAW:\n"
                     << slice_around_first_chunk(playlist);
            LOG_INFO << "POST /action: first media playlist first chunk EXPANDED:\n"
                     << slice_around_first_chunk(expanded);
            LOG_INFO << "POST /action: first media playlist first chunk LOCALISED:\n"
                     << slice_around_first_chunk(localised);
        }
        hls->media_playlists[url] = localised;
        LOG_INFO << "POST /action: media playlist stored "
                 << hls->media_playlists.size() << '/'
                 << hls->media_uris.size() << " ("
                 << playlist.size() << "B raw, "
                 << localised.size() << "B localised, "
                 << rewritten << " segs rewritten, "
                 << after_gv << " googlevideo leaks)";
        HlsSessionRegistry::instance().notify_playlist_arrived(sid, url);
    } else if (!redirect_location.empty()) {
        // 302 to a signed CDN URL — forward to FFmpeg.
        LOG_INFO << "POST /action: segment " << url
                 << " — 302 redirect (Location=" << redirect_location.size()
                 << "B)";
        HlsSessionRegistry::instance().deliver_segment_redirect(
            sid, url, std::move(redirect_location));
    } else {
        // Fallback: iOS actually shipped bytes inline.
        LOG_INFO << "POST /action: segment " << url
                 << " (" << playlist.size() << "B) — deliver";
        HlsSessionRegistry::instance().deliver_segment(sid, url, playlist);
    }

    (void)session;
    plist_free(root);
#endif
    return r;
}

// GET /playback-info: iOS polls this every ~1 s after /play. If we
// don't answer with a valid readyToPlay plist within about 600 ms, iOS
// assumes we're broken and TEARDOWNs the session. While the playlist
// fetch (FCUP round trip) is in flight we need at least a buffering
// response so iOS keeps waiting. Once a real player is wired up this
// will grow duration/position/rate. For now: readyToPlay=0 signals
// "still loading", duration=0 (unknown), position=0.
Response handle_playback_info(const Request& req) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    r.set_header("Content-Type", "text/x-apple-plist+xml");
#if defined(HAVE_LIBPLIST)
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "duration",                 plist_new_real(0.0));
    plist_dict_set_item(root, "position",                 plist_new_real(0.0));
    plist_dict_set_item(root, "rate",                     plist_new_real(0.0));
    plist_dict_set_item(root, "readyToPlay",              plist_new_uint(0));
    plist_dict_set_item(root, "playbackBufferEmpty",      plist_new_uint(1));
    plist_dict_set_item(root, "playbackBufferFull",       plist_new_uint(0));
    plist_dict_set_item(root, "playbackLikelyToKeepUp",   plist_new_uint(0));
    plist_dict_set_item(root, "loadedTimeRanges",         plist_new_array());
    plist_dict_set_item(root, "seekableTimeRanges",       plist_new_array());
    char*    xml = nullptr;
    uint32_t xml_len = 0;
    plist_to_xml(root, &xml, &xml_len);
    plist_free(root);
    if (xml) {
        r.body.assign(xml, xml + xml_len);
        std::free(xml);
    }
#endif
    return r;
}

// Generic 200 OK stub that logs the body. Used for all the AirPlay
// Streaming routes we haven't implemented yet (/play, /stop, /rate,
// /scrub, /setProperty, /getProperty, /audioMode, /action, /command, …)
// so we can observe what iOS sends during a YouTube / Netflix session.
Response handle_stream_stub(const Request& req, const char* tag) {
    Response r = make(200, "OK");
    copy_cseq(req, r);
    LOG_INFO << "[streaming] " << tag << " " << req.uri
             << "  body=" << req.body.size() << "B  " << snapshot_body(req.body);
    auto sid = req.header("x-apple-session-id");
    if (!sid.empty()) LOG_INFO << "  X-Apple-Session-ID=" << sid;
    return r;
}

} // namespace

Response dispatch(const DeviceContext& ctx, ClientSession& session,
                  const Request& req) {
    // iOS replies to our POST /event FCUP Requests with plain HTTP
    // responses ("HTTP/1.1 200 OK", "HTTP/1.1 404 Not Found") on the
    // same socket. Our generic request reader happily parses those as
    // new requests with method=="HTTP/1.1", uri=="404" — don't dispatch
    // or reply to them, just drop silently. Response.silent skips send.
    if (req.method.compare(0, 5, "HTTP/") == 0) {
        Response r = make(200, "OK");
        r.silent = true;
        return r;
    }

    LOG_INFO << req.method << ' ' << req.uri
             << "  body=" << req.body.size() << "B";

    // Strip query string from URI for dispatch matching: iOS sends
    // "/rate?value=0.000000" but our route table uses "/rate". The
    // handler still has access to the full uri via req.uri.
    const auto qpos = req.uri.find('?');
    const std::string path = (qpos == std::string::npos)
                                 ? req.uri : req.uri.substr(0, qpos);

    // Order matches the rough handshake sequence iOS initiates.
    if (req.method == "GET"  && path == "/info")          return handle_info(ctx, req);
    if (req.method == "POST" && path == "/pair-setup")    return handle_pair_setup(ctx, req);
    if (req.method == "POST" && path == "/pair-verify")   return handle_pair_verify(ctx, session, req);
    if (req.method == "POST" && path == "/fp-setup")      return handle_fp_setup(session, req);
    if (req.method == "POST" && path == "/auth-setup")    return handle_unimplemented(req, "auth-setup");
    if (req.method == "POST" && path == "/feedback")      {
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
    if (req.method == "TEARDOWN")    return handle_teardown(ctx, session, req);
    if (req.method == "GET_PARAMETER")  return handle_get_parameter(req);
    if (req.method == "SET_PARAMETER")  return handle_set_parameter(session, req);

    // RTSP FLUSH is iOS's ambiguous "buffer reset" verb. Observed flows:
    //   - pre-playback / pre-track-change FLUSH followed by audio pkts
    //     within ~1 ms (safe to ignore)
    //   - user-pause FLUSH followed by 20+ seconds of silence, then a
    //     SETUP or TEARDOWN on resume
    // We can't distinguish the two up front, so anticipate pause here
    // (push rate=0). The AudioReceiver silence watchdog flips rate back
    // to 1 as soon as the next packet arrives, so a spurious mid-play
    // FLUSH only shows pause for a fraction of a frame.
    if (req.method == "FLUSH" || req.method == "PAUSE") {
        LOG_INFO << req.method << ": tentative pause";
        if (session.renderer) {
            session.renderer->note_flush();
            session.renderer->push_playback_rate(0.0f);
        }
        Response r = make(200, "OK");
        copy_cseq(req, r);
        return r;
    }

    // POST /rate?value=N: AirPlay Streaming pause/play (iOS also uses it
    // for audio in some flows). value=0 → paused, value=1 → playing.
    if (req.method == "POST" && path == "/rate") {
        const auto eq = req.uri.find("value=");
        if (eq != std::string::npos) {
            try {
                const float rate = std::stof(req.uri.substr(eq + 6));
                LOG_INFO << "POST /rate value=" << rate
                         << (rate > 0.5f ? " (playing)" : " (paused)");
                if (session.renderer) session.renderer->push_playback_rate(rate);
            } catch (...) {}
        }
        return handle_stream_stub(req, "rate");
    }

    // AirPlay Streaming reconnaissance (step 3a): answer the routes iOS
    // hammers at session setup so we stop the retry storm and can observe
    // what else it sends for YouTube / Netflix / Apple TV+ playback.
    if (req.method == "POST" && path == "/reverse")          return handle_reverse(session, req);
    if (req.method == "GET"  && path == "/server-info")      return handle_server_info(ctx, req);
    if (req.method == "POST" && path == "/audioMode")        {
        // Observed as the resume-from-pause signal on some iOS flows
        // (no RECORD, no new SETUP — just an audioMode announcement
        // followed by the RTP stream kicking back in). Flip the renderer
        // to playing; the silence watchdog will confirm via packets.
        if (session.renderer) session.renderer->push_playback_rate(1.0f);
        return handle_stream_stub(req, "audioMode");
    }
    if (req.method == "POST" && path == "/play")             return handle_play(session, req);
    if (req.method == "POST" && path == "/action")           return handle_action(ctx, session, req);
    if (req.method == "POST" && path == "/stop")             return handle_stream_stub(req, "stop");
    if (req.method == "POST" && path == "/scrub")            return handle_stream_stub(req, "scrub");
    if (req.method == "POST" && path == "/setProperty")      return handle_stream_stub(req, "setProperty");
    if (req.method == "POST" && path == "/getProperty")      return handle_stream_stub(req, "getProperty");
    if (req.method == "GET"  && path == "/playback-info")    return handle_playback_info(req);
    // "/action" handled above (real parser).
    if (req.method == "POST" && path == "/command")          return handle_stream_stub(req, "command");

    return handle_unimplemented(req, "unknown route");
}

} // namespace ap::airplay
