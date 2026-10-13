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
    GET  /_test/requests                               every request so far, as a JSON list

Stdlib only: the test containers have python3 and nothing more.
"""

import argparse
import json
import re
import secrets
import ssl
import threading
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
    "requests": [],
}


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
            if not self.path.startswith("/_test/"):
                STATE["requests"].append({
                    "path": self.path,
                    "userAgent": self.headers.get("User-Agent"),
                    "body": body if body is not None else raw.decode(errors="replace"),
                })
            status, answer = self.route(body)
        self._send(status, answer)

    def route(self, body):
        if body is None:
            return 400, {"error": "bad_json"}

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
        return 200, {"command": command, "join": join_material(), "mapVersion": 1}


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
    a = ap.parse_args()

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
