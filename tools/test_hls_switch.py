"""Regression test against a running --hls receiver (temporarily owns playback).

Emulates delayed iPhone FCUP responses across different sessions and repeated
/play in the same session. Run before reconnecting a real iPhone.
"""
import argparse
import plistlib
import re
import socket
import uuid


def read_message(sock):
    wire = b""
    while not wire.endswith(b"\r\n\r\n"):
        part = sock.recv(1)
        assert part, "reverse channel closed"
        wire += part
    length = next((int(line.split(b":", 1)[1]) for line in wire.split(b"\r\n")
                   if line.lower().startswith(b"content-length:")), 0)
    body = b""
    while len(body) < length:
        body += sock.recv(length - len(body))
    return wire, body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=7000)
    parser.add_argument("--proxy-port", type=int, default=7100)
    args = parser.parse_args()

    def request(method, path, sid="", payload=None, port=None, source=None):
        body = plistlib.dumps(payload, fmt=plistlib.FMT_BINARY) if payload is not None else b""
        with socket.create_connection(("127.0.0.1", port or args.port), timeout=10,
                                      source_address=(source, 0) if source else None) as conn:
            head = (f"{method} {path} HTTP/1.1\r\nX-Apple-Session-ID: {sid}\r\n"
                    f"Content-Type: application/x-apple-binary-plist\r\nContent-Length: {len(body)}\r\n\r\n")
            conn.sendall(head.encode() + body)
            response, data = read_message(conn)
            return int(response.split()[1]), data

    def reverse(sid):
        sock = socket.create_connection(("127.0.0.1", args.port), timeout=10)
        sock.sendall(("POST /reverse HTTP/1.1\r\nX-Apple-Session-ID: " + sid +
                      "\r\nContent-Length: 0\r\n\r\n").encode())
        head, _ = read_message(sock)
        assert b"101" in head
        return sock

    def play(sid, sock):
        status, _ = request("POST", "/play", sid, {
            "Content-Location": "mlhls://localhost/master.m3u8",
            "clientProcName": "YouTube", "uuid": str(uuid.uuid4()),
        })
        assert status == 200
        _, body = read_message(sock)
        return plistlib.loads(body)["request"]["FCUP_Response_RequestID"]

    def action(sid, request_id, marker):
        manifest = ("#EXTM3U\n#MARKER=" + marker + "\n"
                    '#EXT-X-SESSION-DATA:DATA-ID="probe",URI="mlhls://localhost/probe"\n'
                    "#EXT-X-ENDLIST\n").encode()
        assert request("POST", "/action", sid, {"type": "unhandledURLResponse", "params": {
            "FCUP_Response_URL": "mlhls://localhost/master.m3u8",
            "FCUP_Response_RequestID": request_id, "FCUP_Response_Data": manifest,
        }})[0] == 200

    def current(marker):
        status, body = request("GET", "/master.m3u8", port=args.proxy_port)
        assert status == 200 and ("#MARKER=" + marker).encode() in body, body
        return re.search(rb"/hls/\d+/", body).group().decode()

    first, second = "test-old-" + str(uuid.uuid4()), "test-new-" + str(uuid.uuid4())
    a, b = reverse(first), reverse(second)
    try:
        late_a = play(first, a)
        live_b = play(second, b)
        action(second, live_b, "B")
        old_prefix = current("B")
        action(first, late_a, "OLD-A")
        current("B")
        request("POST", "/rate?value=0", first)
        request("POST", "/stop", first)
        request("TEARDOWN", "/stream", first)
        current("B")
        _, state = request("GET", "/playback-info", second)
        assert plistlib.loads(state)["rate"] == 1.0, "stale controls changed new playback"
        late_b = play(second, b)
        live_c = play(second, b)
        action(second, live_c, "C")
        action(second, late_b, "OLD-B")
        current("C")
        assert request("GET", old_prefix + "master.m3u8", port=args.proxy_port)[0] == 410
        assert request("SETUP", "/stream", payload={"streams": []}, source="127.0.0.2")[0] == 503
    finally:
        try:
            request("POST", "/stop", second)
        finally:
            a.close()
            b.close()
    assert request("SETUP", "/stream", payload={"streams": []}, source="127.0.0.2")[0] == 200, \
        "stopped HLS session still blocks the next device"
    print("PASS: stale responses/controls/URLs rejected; stop releases receiver for the next device")


if __name__ == "__main__":
    main()
