#!/usr/bin/env python3
"""
Multi-scenario test suite for airplay-windows pair-setup / pair-verify.

Runs against a live `airplay-windows` process on 127.0.0.1:7000. Exits 0
when every check passes, 1 otherwise.

Scenarios:

  T1  Happy path — round-trip exactly as a well-behaved client.
  T2  iOS-style headers + absolute URI — the parser must tolerate both.
  T3  Tampered client signature in round 2 — server must return non-200.
  T4  Two concurrent sessions — each TCP connection must carry its own
      PairVerifySession state (no cross-contamination of the AES-CTR
      counter or of the client Ed25519 pubkey remembered from round 1).
  T5  /fp-setup RTSP framing — correct sizes are returned (142B for
      msg2, 32B for msg4) even when the FairPlay blobs are stubbed.
      Validates the state machine and routing, not the crypto itself
      (which needs the Apple-extracted tables; see docs/FAIRPLAY.md).
  T6  Media handshake shape — ANNOUNCE parses an SDP body, SETUP binds
      real UDP sockets and echoes allocated ports in Transport, RECORD
      preserves the Session id, TEARDOWN closes everything cleanly.

What this validates:
  * the crypto primitives and byte layouts we used on both sides match
  * our server's state machine isolates rounds per TCP connection
  * the RTSP parser doesn't choke on headers iOS actually sends, nor on
    absolute URIs (`rtsp://host:port/path` instead of `/path`)
  * corrupted sessions are rejected rather than silently accepted

What this does NOT validate:
  * that our salt strings, field orderings, and feature bits match what a
    real iOS device expects — any mismatch in both client+server would
    still pass here. Only a capture against UxPlay or a live iPhone can
    tell us that.
"""

import hashlib
import socket
import sys
import threading

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


HOST = "127.0.0.1"
PORT = 7000
AES_KEY_SALT = b"Pair-Verify-AES-Key"
AES_IV_SALT  = b"Pair-Verify-AES-IV"

IOS_HEADERS = {
    "User-Agent": "AirPlay/550.10",
    "X-Apple-ProtocolVersion": "1",
    "Active-Remote": "1986135966",
    "DACP-ID": "A1B2C3D4E5F60789",
}


# -- RTSP-over-TCP client ----------------------------------------------------

class Conn:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=3)
        self.buf  = b""
        self.cseq = 0

    def close(self):
        self.sock.close()

    def rpc(self, method, path, body=b"", extra_headers=None):
        """Returns (status:int, response_headers:dict[lower], body:bytes)."""
        self.cseq += 1
        headers = {"CSeq": str(self.cseq), "Content-Length": str(len(body))}
        if extra_headers:
            headers.update(extra_headers)
        req = f"{method} {path} RTSP/1.0\r\n"
        for k, v in headers.items():
            req += f"{k}: {v}\r\n"
        req += "\r\n"
        self.sock.sendall(req.encode() + body)

        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("peer closed during headers")
            self.buf += chunk
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        status = int(head.split(b" ", 2)[1])
        resp_headers = {}
        clen = 0
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b":")
            if not k:
                continue
            key = k.strip().lower().decode()
            val = v.strip().decode()
            resp_headers[key] = val
            if key == "content-length":
                clen = int(val)
        while len(self.buf) < clen:
            chunk = self.sock.recv(clen - len(self.buf))
            if not chunk:
                break
            self.buf += chunk
        body, self.buf = self.buf[:clen], self.buf[clen:]
        return status, resp_headers, body


def derive_key_iv(shared):
    return (
        hashlib.sha512(AES_KEY_SALT + shared).digest()[:16],
        hashlib.sha512(AES_IV_SALT + shared).digest()[:16],
    )


class ClientCrypto:
    def __init__(self):
        self.x25519_priv = x25519.X25519PrivateKey.generate()
        self.x25519_pub  = self.x25519_priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.ed_priv = ed25519.Ed25519PrivateKey.generate()
        self.ed_pub  = self.ed_priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def get_server_identity_pub(conn):
    status, _hdrs, body = conn.rpc("POST", "/pair-setup")
    assert status == 200 and len(body) == 32, \
        f"pair-setup failed: status={status} len={len(body)}"
    return body


def do_pair_verify(conn, client, server_identity_pub,
                   break_client_sig=False,
                   headers=None,
                   path="/pair-verify"):
    """Returns (round1_status, round2_status, server_sig_ok)."""
    msg1 = b"\x01\x00\x00\x00" + client.x25519_pub + client.ed_pub
    s1, _h1, resp = conn.rpc("POST", path, msg1, headers)
    if s1 != 200 or len(resp) != 96:
        return s1, None, False

    server_x25519_pub = resp[:32]
    server_enc_sig    = resp[32:96]

    shared = client.x25519_priv.exchange(
        x25519.X25519PublicKey.from_public_bytes(server_x25519_pub))
    aes_key, aes_iv = derive_key_iv(shared)
    dec = Cipher(algorithms.AES(aes_key), modes.CTR(aes_iv)).decryptor()

    server_sig = dec.update(server_enc_sig)
    sig_data_server = server_x25519_pub + client.x25519_pub
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(server_identity_pub).verify(
            server_sig, sig_data_server)
        server_sig_ok = True
    except Exception:
        server_sig_ok = False

    client_sig = client.ed_priv.sign(client.x25519_pub + server_x25519_pub)
    if break_client_sig:
        client_sig = bytes([client_sig[0] ^ 0x01]) + client_sig[1:]
    msg2 = b"\x00\x00\x00\x00" + dec.update(client_sig)
    s2, _h2, _ = conn.rpc("POST", path, msg2, headers)
    return s1, s2, server_sig_ok


# -- Scenarios ---------------------------------------------------------------

class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, cond, label):
        print(f"  {'OK  ' if cond else 'FAIL'}  {label}")
        if cond: self.passed += 1
        else:    self.failed += 1


def t1_happy_path(r):
    print("T1  happy path")
    c = Conn(HOST, PORT)
    try:
        spk = get_server_identity_pub(c)
        s1, s2, ok = do_pair_verify(c, ClientCrypto(), spk)
        r.check(s1 == 200,  f"round 1 status = {s1}")
        r.check(ok,         "server Ed25519 signature verifies against /pair-setup pk")
        r.check(s2 == 200,  f"round 2 status = {s2}")
    finally:
        c.close()


def t2_ios_headers_and_absolute_uri(r):
    print("T2  iOS-style headers + absolute URI")
    c = Conn(HOST, PORT)
    try:
        spk = get_server_identity_pub(c)
        s1, s2, ok = do_pair_verify(
            c, ClientCrypto(), spk,
            headers=IOS_HEADERS,
            path=f"rtsp://{HOST}:{PORT}/pair-verify")
        r.check(s1 == 200,  f"round 1 status = {s1}")
        r.check(ok,         "server signature verifies through absolute URI")
        r.check(s2 == 200,  f"round 2 status = {s2}")
    finally:
        c.close()


def t3_bad_client_signature(r):
    print("T3  tampered client signature must be rejected")
    c = Conn(HOST, PORT)
    try:
        spk = get_server_identity_pub(c)
        s1, s2, _ok = do_pair_verify(c, ClientCrypto(), spk,
                                     break_client_sig=True)
        r.check(s1 == 200,  "round 1 still accepted")
        r.check(s2 != 200,  f"round 2 rejected (got {s2})")
    finally:
        c.close()


def t4_concurrent_sessions(r):
    print("T4  two concurrent sessions are independent")
    results = [None, None]

    def worker(idx):
        try:
            c = Conn(HOST, PORT)
            spk = get_server_identity_pub(c)
            s1, s2, ok = do_pair_verify(c, ClientCrypto(), spk)
            results[idx] = (s1, s2, ok)
            c.close()
        except Exception as e:
            results[idx] = ("EXC", str(e), False)

    ts = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
    for t in ts: t.start()
    for t in ts: t.join()
    for i, res in enumerate(results):
        r.check(res == (200, 200, True), f"session #{i} -> {res}")


def t5_fp_setup_framing(r):
    print("T5  fp-setup RTSP framing (msg1->142B, msg3->32B)")
    c = Conn(HOST, PORT)
    try:
        # msg1: 16 bytes starting with "FPLY" magic, mode byte at index 6.
        msg1 = b"FPLY" + b"\x03\x01\x01\x00\x00\x00\x00\x82\x02\x00\x0f\x9f"
        assert len(msg1) == 16
        s1, _h, body1 = c.rpc("POST", "/fp-setup", msg1)
        r.check(s1 == 200,           f"fp-setup msg1 status = {s1}")
        r.check(len(body1) == 142,   f"fp-setup msg2 size = {len(body1)} (expected 142)")
        r.check(body1[:4] == b"FPLY", "fp-setup msg2 starts with FPLY magic")

        # msg3: 164 bytes starting with FPLY magic, rest zeros is fine for framing test.
        msg3 = b"FPLY" + b"\x03\x01\x02" + (b"\x00" * (164 - 7))
        assert len(msg3) == 164
        s2, _h, body2 = c.rpc("POST", "/fp-setup", msg3)
        r.check(s2 == 200,          f"fp-setup msg3 status = {s2}")
        r.check(len(body2) == 32,   f"fp-setup msg4 size = {len(body2)} (expected 32)")
    finally:
        c.close()

    # Reject-unknown-length check on a separate connection.
    c = Conn(HOST, PORT)
    try:
        s3, _h, _ = c.rpc("POST", "/fp-setup", b"FPLY" + b"\x00" * 38)  # 42 bytes
        r.check(s3 != 200, f"fp-setup with bogus length ({s3}) -> non-200")
    finally:
        c.close()


SAMPLE_SDP = (
    "v=0\r\n"
    "o=iTunes 999999 0 IN IP4 127.0.0.1\r\n"
    "s=iTunes\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 AppleLossless\r\n"
    "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
    "a=rsaaeskey:ZmFrZWFlc2tleWJsb2I=\r\n"
    "a=aesiv:ZmFrZWFlc2l2YmxvYg==\r\n"
)


def t6_media_handshake_shape(r):
    print("T6  ANNOUNCE -> SETUP -> RECORD -> TEARDOWN")
    c = Conn(HOST, PORT)
    try:
        url = f"rtsp://{HOST}:{PORT}/stream"
        # ANNOUNCE with a realistic audio-only SDP.
        s, _h, _b = c.rpc("ANNOUNCE", url, SAMPLE_SDP.encode(),
                          {"Content-Type": "application/sdp"})
        r.check(s == 200, f"ANNOUNCE status = {s}")

        # SETUP with iOS-style Transport header.
        s, hdrs, _b = c.rpc("SETUP", url, b"",
                            {"Transport": "RTP/AVP/UDP;unicast;mode=record;"
                                          "control_port=60001;timing_port=60002"})
        r.check(s == 200, f"SETUP status = {s}")

        # Verify server echoed back a Transport with real allocated ports and
        # a Session id that persists across later requests.
        transport = hdrs.get("transport", "")
        session_id = hdrs.get("session", "")
        r.check("server_port=" in transport,  f"Transport has server_port: {transport!r}")
        r.check("control_port=" in transport, f"Transport has control_port: {transport!r}")
        r.check("timing_port="  in transport, f"Transport has timing_port: {transport!r}")
        r.check(session_id != "",              f"SETUP returned Session id: {session_id!r}")

        def port_of(t, k):
            import re
            m = re.search(rf"{k}=(\d+)", t)
            return int(m.group(1)) if m else 0
        sp = port_of(transport, "server_port")
        r.check(sp > 0 and sp != 60001,        f"server_port allocated (got {sp})")

        s, hdrs2, _ = c.rpc("RECORD", url, b"", {"Session": session_id})
        r.check(s == 200, f"RECORD status = {s}")
        r.check(hdrs2.get("session", "") == session_id,
                "RECORD echoes same Session id")

        s, _h, _ = c.rpc("TEARDOWN", url, b"", {"Session": session_id})
        r.check(s == 200, f"TEARDOWN status = {s}")
    finally:
        c.close()


def main():
    r = Runner()
    for fn in (t1_happy_path,
               t2_ios_headers_and_absolute_uri,
               t3_bad_client_signature,
               t4_concurrent_sessions,
               t5_fp_setup_framing,
               t6_media_handshake_shape):
        try:
            fn(r)
        except Exception as e:
            print(f"  EXC   {fn.__name__}: {e}")
            r.failed += 1
        print()

    total = r.passed + r.failed
    print(f"=== {r.passed}/{total} checks passed"
          f"{'' if r.failed == 0 else f', {r.failed} FAILED'} ===")
    sys.exit(0 if r.failed == 0 else 1)


if __name__ == "__main__":
    main()
