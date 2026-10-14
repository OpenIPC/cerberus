#!/usr/bin/env python3
"""A stand-in for the Streq coordinator: the two REST calls cerberus makes, and nothing else.

It answers exactly the shapes the real one does — `POST /v1/enroll` returns an EnrollResponse with
WireGuard join material, `POST /v1/nodes/{id}/heartbeat` returns a NodeDirective — over real TLS,
so what the agent is tested against is its own HTTP and mbedTLS code, not a mock of it.

Everything the test needs to steer goes through `/_test/*` on the same port:

    POST /_test/command   {"command": "Restart"}       one-shot, consumed by the next heartbeat
    POST /_test/join      {"hubEndpoint": "..."}       rewrites fields of the join material
    POST /_test/forget    {}                           the node is revoked: heartbeats answer 404
    POST /_test/fail      {"status": 503}              heartbeats answer this until set to 0
    POST /_test/frames    {"frames": 2}                how many frames an episode is asked for
    GET  /_test/requests                               every request so far, as a JSON list

Events are checked the way the coordinator checks them: the heartbeat hands out an eventKey, and
every event and frame must carry an HMAC under the key both sides derive from it and the node's
device key (X25519 + HKDF). Each recorded request says whether its signature held (`verified`).

With --majestic-port it also plays majestic on plain HTTP: /image.jpg behind Basic auth, answered
chunked, because an HTTP server is allowed to and the agent has to cope.

Stdlib only: the test containers have python3 and nothing more.
"""

import argparse
import base64
import hashlib
import hmac
import json
import re
import secrets
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
STATE = {
    "token": "",
    "token_uses": 0,
    "token_max_uses": 1,
    "nodes": {},          # nodeId -> {"pub": ..., "claimCodeHash": ...}
    "join": {},
    "command": None,
    "fail_status": 0,
    "frames": 0,
    "requests": [],
    "event_priv": secrets.token_bytes(32),
}

# RFC 7748 X25519, because the stdlib has none and the test containers have nothing else.
P = 2 ** 255 - 19


def x25519(k, u):
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    k = int.from_bytes(k, "little")
    u = bytearray(u)
    u[31] &= 127
    x1 = int.from_bytes(u, "little")
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in reversed(range(255)):
        bit = (k >> t) & 1
        swap ^= bit
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = bit
        a, b = (x2 + z2) % P, (x2 - z2) % P
        aa, bb = a * a % P, b * b % P
        e = (aa - bb) % P
        c, d = (x3 + z3) % P, (x3 - z3) % P
        da, cb = d * a % P, c * b % P
        x3, z3 = (da + cb) ** 2 % P, x1 * (da - cb) ** 2 % P
        x2, z2 = aa * bb % P, e * (aa + 121665 * e) % P
    if swap:
        x2, z2 = x3, z3
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


def event_pub():
    return base64.b64encode(x25519(STATE["event_priv"], (9).to_bytes(32, "little"))).decode()


def verified(node, scope, headers, raw):
    """The coordinator's EventSigning, recomputed: X25519 → HKDF(salt=nodeId) → HMAC."""
    try:
        pub = base64.b64decode(STATE["nodes"][node]["pub"])
        ts = headers.get("X-Streq-Ts") or ""
        sig = base64.b64decode(headers.get("X-Streq-Sig") or "")
    except (KeyError, ValueError):
        return False
    if not ts.isdigit() or abs(time.time() - int(ts)) > 120:
        return False
    shared = x25519(STATE["event_priv"], pub)
    prk = hmac.new(node.encode(), shared, hashlib.sha256).digest()
    key = hmac.new(prk, b"streq-events-v1\x01", hashlib.sha256).digest()
    want = hmac.new(key, f"{ts}\n{node}\n{scope}\n".encode() + raw, hashlib.sha256).digest()
    return hmac.compare_digest(want, sig)


def join_material():
    return dict(STATE["join"], kind="Wireguard")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet: the suite prints what matters
        pass

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return raw, (json.loads(raw) if raw else {})
        except ValueError:
            return raw, None

    def _send(self, status, obj=None):
        data = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/_test/requests":
            with LOCK:
                return self._send(200, STATE["requests"])
        self._send(404, {"error": "not_found"})

    def do_POST(self):
        raw, body = self._body()
        with LOCK:
            m = re.fullmatch(r"/v1/nodes/([^/]+)/events/([^/]+)/frames/(\d+)", self.path)
            if m:
                ok = verified(m.group(1), f"frame/{m.group(2)}/{m.group(3)}", self.headers, raw)
                STATE["requests"].append({
                    "path": self.path, "verified": ok, "bytes": len(raw),
                    "jpeg": raw[:2] == b"\xff\xd8",
                    "contentType": self.headers.get("Content-Type"),
                })
                seq = int(m.group(3))
                answer = (200, {"more": seq + 1 < STATE["frames"], "labels": []}) if ok else (401, {"code": "bad_signature"})
                return self._send(*answer)

            entry = None
            if not self.path.startswith("/_test/"):
                entry = {
                    "path": self.path,
                    "userAgent": self.headers.get("User-Agent"),
                    "body": body if body is not None else raw.decode(errors="replace"),
                }
                STATE["requests"].append(entry)
            m = re.fullmatch(r"/v1/nodes/([^/]+)/events", self.path)
            if m and entry is not None:
                entry["verified"] = verified(m.group(1), "event", self.headers, raw)
                if not entry["verified"]:
                    return self._send(401, {"code": "bad_signature"})
                return self._send(200, {"id": "nev_test", "frames": STATE["frames"], "intervalSeconds": 1})
            status, answer = self.route(body)
        self._send(status, answer)

    def route(self, body):
        if body is None:
            return 400, {"error": "bad_json"}

        if self.path == "/_test/frames":
            STATE["frames"] = int(body.get("frames", 0))
            return 200, {}

        if self.path == "/_test/command":
            STATE["command"] = body.get("command")
            return 202, {}
        if self.path == "/_test/join":
            STATE["join"].update(body)
            return 200, join_material()
        if self.path == "/_test/forget":
            STATE["nodes"].clear()
            return 200, {}
        if self.path == "/_test/fail":
            STATE["fail_status"] = int(body.get("status", 0))
            return 200, {}

        if self.path == "/v1/enroll":
            return self.enroll(body)

        m = re.fullmatch(r"/v1/nodes/([^/]+)/heartbeat", self.path)
        if m:
            return self.heartbeat(m.group(1))

        return 404, {"error": "not_found"}

    def enroll(self, body):
        for field in ("enrollmentToken", "devicePublicKey", "kind"):
            if not body.get(field):
                return 400, {"error": f"missing {field}"}
        if body["enrollmentToken"] != STATE["token"] or STATE["token_uses"] >= STATE["token_max_uses"]:
            return 401, {"error": "invalid_token"}

        claim = body.get("claimCodeHash", "")
        if claim and not re.fullmatch(r"[0-9a-f]{64}", claim):
            return 400, {"error": "claimCodeHash is not lowercase hex sha-256"}

        STATE["token_uses"] += 1
        node = "node_" + secrets.token_hex(16)
        STATE["nodes"][node] = {"pub": body["devicePublicKey"], "claimCodeHash": claim}
        return 200, {
            "nodeId": node,
            "networkId": "net_test",
            "fabric": "Wireguard",
            "join": join_material(),
        }

    def heartbeat(self, node):
        if STATE["fail_status"]:
            return STATE["fail_status"], {"error": "injected"}
        if node not in STATE["nodes"]:
            return 404, {"error": "unknown_node"}
        command = STATE["command"] or "None"
        STATE["command"] = None
        return 200, {"command": command, "join": join_material(), "mapVersion": 1, "eventKey": event_pub()}


class Majestic(BaseHTTPRequestHandler):
    """majestic's /image.jpg: Basic auth root:12345, a JPEG-looking body, sent chunked."""
    protocol_version = "HTTP/1.1"
    FRAME = b"\xff\xd8\xff\xe0" + bytes(range(256)) * 40 + b"\xff\xd9"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        want = "Basic " + base64.b64encode(b"root:12345").decode()
        with LOCK:
            STATE["requests"].append({"path": "majestic:" + self.path,
                                      "authorized": self.headers.get("Authorization") == want})
        if not self.path.startswith("/image.jpg"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.headers.get("Authorization") != want:
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="majestic"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        for i in range(0, len(self.FRAME), 3000):
            part = self.FRAME[i:i + 3000]
            self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--token-max-uses", type=int, default=1)
    ap.add_argument("--overlay-ip", required=True)
    ap.add_argument("--network-subnet", required=True)
    ap.add_argument("--hub-overlay-ip", required=True)
    ap.add_argument("--hub-endpoint", required=True)
    ap.add_argument("--hub-public-key", required=True)
    ap.add_argument("--majestic-port", type=int, default=0)
    a = ap.parse_args()

    if a.majestic_port:
        cam = ThreadingHTTPServer(("127.0.0.1", a.majestic_port), Majestic)
        threading.Thread(target=cam.serve_forever, daemon=True).start()

    STATE["token"] = a.token
    STATE["token_max_uses"] = a.token_max_uses
    STATE["join"] = {
        "overlayIp": a.overlay_ip,
        "hubEndpoint": a.hub_endpoint,
        "hubPublicKey": a.hub_public_key,
        "allowedIps": a.network_subnet,
        "networkSubnet": a.network_subnet,
        "hubOverlayIp": a.hub_overlay_ip,
    }

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(a.cert, a.key)
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"fake coordinator on :{a.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
