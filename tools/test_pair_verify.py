#!/usr/bin/env python3
"""
End-to-end test for pair-setup + pair-verify, playing the role iOS plays.

Runs against a live `airplay-windows` process on 127.0.0.1:7000 and walks
through:

  1. POST /pair-setup            -> receive server's 32-byte Ed25519 public key
  2. POST /pair-verify (round 1) -> send our X25519 + Ed25519 pubkeys,
                                   receive server X25519 pub + encrypted sig,
                                   decrypt, verify server's signature
  3. POST /pair-verify (round 2) -> sign our own challenge, encrypt,
                                   send, expect 200 OK (AES-CTR counter
                                   must continue from round 1)

Exits non-zero on any failure, with a line-by-line trace on stdout.
"""

import hashlib
import socket
import struct
import sys

from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import serialization


HOST = "127.0.0.1"
PORT = 7000
AES_KEY_SALT = b"Pair-Verify-AES-Key"
AES_IV_SALT  = b"Pair-Verify-AES-IV"


class Conn:
    """Persistent RTSP-like connection; pair-verify state is per-connection."""
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=3)
        self.buf  = b""
        self._cseq = 0

    def close(self) -> None:
        self.sock.close()

    def _recv_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(max(4096, n - len(self.buf)))
            if not chunk:
                raise RuntimeError("peer closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def rpc(self, method: str, path: str, body: bytes = b"") -> tuple[int, bytes]:
        self._cseq += 1
        req = (
            f"{method} {path} RTSP/1.0\r\n"
            f"CSeq: {self._cseq}\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"\r\n"
        ).encode() + body
        self.sock.sendall(req)

        # Read headers until CRLFCRLF.
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("peer closed before response")
            self.buf += chunk
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")

        status = int(head.split(b" ", 2)[1])
        clen = 0
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b":")
            if k.strip().lower() == b"content-length":
                clen = int(v.strip())
        body = self._recv_exact(clen) if clen else b""
        return status, body


def derive_key_iv(shared: bytes) -> tuple[bytes, bytes]:
    k = hashlib.sha512(AES_KEY_SALT + shared).digest()[:16]
    i = hashlib.sha512(AES_IV_SALT  + shared).digest()[:16]
    return k, i


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def main() -> None:
    # Persistent connection; the two pair-verify rounds must share the same
    # TCP session so the server-side AES-CTR counter continues across them.
    c = Conn(HOST, PORT)

    try:
        # 1. pair-setup (any connection works — stateless server-side).
        status, body = c.rpc("POST", "/pair-setup")
        if status != 200 or len(body) != 32:
            fail(f"/pair-setup status={status} body_len={len(body)}")
        server_identity_pub = ed25519.Ed25519PublicKey.from_public_bytes(body)
        print(f"OK  /pair-setup: server Ed25519 pub = {body.hex()}")

        client_x25519_priv = x25519.X25519PrivateKey.generate()
        client_x25519_pub_b = client_x25519_priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        client_ed_priv = ed25519.Ed25519PrivateKey.generate()
        client_ed_pub_b = client_ed_priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )

        # 2. pair-verify round 1 — first byte non-zero signals round 1.
        msg1 = b"\x01\x00\x00\x00" + client_x25519_pub_b + client_ed_pub_b
        status, resp = c.rpc("POST", "/pair-verify", msg1)
        if status != 200 or len(resp) != 96:
            fail(f"pair-verify r1 status={status} resp_len={len(resp)}")

        server_x25519_pub_b = resp[:32]
        server_enc_sig      = resp[32:96]
        print(f"OK  /pair-verify r1: server X25519 pub = {server_x25519_pub_b.hex()}")

        server_x25519_pub = x25519.X25519PublicKey.from_public_bytes(server_x25519_pub_b)
        shared = client_x25519_priv.exchange(server_x25519_pub)
        aes_key, aes_iv = derive_key_iv(shared)
        print(f"    aes_key={aes_key.hex()}  aes_iv={aes_iv.hex()}")

        # Same cipher object across rounds -> CTR counter persists.
        cipher    = Cipher(algorithms.AES(aes_key), modes.CTR(aes_iv))
        decryptor = cipher.decryptor()

        server_sig = decryptor.update(server_enc_sig)
        server_sig_data = server_x25519_pub_b + client_x25519_pub_b
        try:
            server_identity_pub.verify(server_sig, server_sig_data)
        except Exception as e:
            fail(f"server Ed25519 signature invalid: {e}")
        print("OK  server Ed25519 signature verifies")

        # 3. pair-verify round 2.
        client_sig_data = client_x25519_pub_b + server_x25519_pub_b
        client_sig = client_ed_priv.sign(client_sig_data)
        enc_client_sig = decryptor.update(client_sig)

        msg2 = b"\x00\x00\x00\x00" + enc_client_sig
        status, resp = c.rpc("POST", "/pair-verify", msg2)
        if status != 200:
            fail(f"pair-verify r2 status={status} body={resp!r}")
        print(f"OK  /pair-verify r2: status=200, body_len={len(resp)}")

        print("\nALL CHECKS PASSED — pair-verify round-trip succeeded.")
    finally:
        c.close()


if __name__ == "__main__":
    main()
