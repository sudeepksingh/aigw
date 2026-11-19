# SPDX-License-Identifier: Apache-2.0
"""A small TM Forum TMF621 Trouble Ticket API (v4 and v5) for the tests: an
OAuth2 token endpoint, POST/GET/PATCH troubleTicket, the hub, and a webhook
receiver. On 127.0.0.1 only; it never calls anywhere.

    m = Mock(); m.start()          # m.url: http://127.0.0.1:<port>
    m.fail = [503]                 # the next request answers 503 (then as usual)
    m.lose_create_reply = True     # the next POST is kept, but answers 500 (the reply was lost)
    m.stop()
"""
import json
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

V4 = "/tmf-api/troubleTicket/v4"
V5 = "/tmf-api/troubleTicketManagement/v5"
TOKEN = "/oauth/token"
REQUIRED = ("description", "severity", "ticketType")


class Mock:
    def __init__(self, port=0, client=("aigw", "s3cret-client"), require_auth=True):
        self.port, self.client, self.require_auth = port, client, require_auth
        self.tickets, self.hubs, self.hooks, self.log = {}, {}, [], []
        self.tokens = set()
        self.fail, self.lose_create_reply = [], False
        self.lock = threading.Lock()
        self.httpd = None

    @property
    def url(self):
        return f"http://127.0.0.1:{self.port}"

    def start(self):
        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), self.handler())
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None

    def requests(self, method=None):
        return [x for x in self.log if method is None or x[0] == method]

    def handler(self):
        mock = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, obj=None, headers=None):
                body = json.dumps(obj).encode() if obj is not None else b""
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)

            def body(self):
                n = int(self.headers.get("Content-Length") or 0)
                return self.rfile.read(n) if n else b""

            def authed(self):
                if not mock.require_auth:
                    return True
                a = self.headers.get("Authorization", "")
                return a.startswith("Bearer ") and a[7:] in mock.tokens

            def route(self, method):
                u = urlsplit(self.path)
                raw = self.body()
                with mock.lock:
                    mock.log.append((method, u.path, dict(self.headers), raw))
                    if mock.fail:
                        code = mock.fail.pop(0)
                        return self.reply(code, {"reason": "injected"}, {"Retry-After": "1"} if code == 429 else None)
                if u.path == "/hook":
                    mock.hooks.append({"headers": {k.lower(): v for k, v in self.headers.items()}, "body": raw})
                    return self.reply(204)
                if u.path == TOKEN:
                    return self.token(raw)
                for base, v in ((V4, 4), (V5, 5)):
                    if u.path.startswith(base):
                        if not self.authed():
                            return self.reply(401, {"reason": "bad token"})
                        return self.api(method, u.path[len(base):], parse_qs(u.query), raw, base, v)
                return self.reply(404, {"reason": "no such path"})

            def token(self, raw):
                import base64
                a = self.headers.get("Authorization", "")
                want = base64.b64encode(f"{mock.client[0]}:{mock.client[1]}".encode()).decode()
                form = parse_qs(raw.decode())
                if a != "Basic " + want or form.get("grant_type") != ["client_credentials"]:
                    return self.reply(401, {"error": "invalid_client"})
                tok = "tok-" + secrets.token_hex(8)
                mock.tokens.add(tok)
                return self.reply(200, {"access_token": tok, "token_type": "Bearer", "expires_in": 3600})

            def api(self, method, path, qs, raw, base, v):
                parts = [p for p in path.split("/") if p]
                if parts[:1] == ["hub"]:
                    if method == "POST":
                        j = json.loads(raw)
                        hid = str(len(mock.hubs) + 1)
                        mock.hubs[hid] = j
                        return self.reply(201, dict(j, id=hid))
                    if method == "DELETE" and len(parts) == 2:
                        return self.reply(204) if mock.hubs.pop(parts[1], None) else self.reply(404, {})
                if parts[:1] != ["troubleTicket"]:
                    return self.reply(404, {"reason": "no such resource"})
                if method == "POST" and len(parts) == 1:
                    j = json.loads(raw)
                    missing = [k for k in REQUIRED if not j.get(k)]
                    if missing or j.get("ticketType") == "Refused":
                        return self.reply(400, {"reason": "missing " + ", ".join(missing) if missing else "refused"})
                    with mock.lock:
                        tid = str(1000 + len(mock.tickets))
                        t = dict(j, id=tid, href=f"{base}/troubleTicket/{tid}", status="acknowledged")
                        mock.tickets[tid] = t
                        if mock.lose_create_reply:
                            mock.lose_create_reply = False
                            return self.reply(500, {"reason": "the reply was lost"})
                    return self.reply(201, t)
                if method == "GET" and len(parts) == 1:
                    out = list(mock.tickets.values())
                    if "externalId" in qs:
                        out = [t for t in out if t.get("externalId") == qs["externalId"][0]]
                    if "externalIdentifier.id" in qs:
                        out = [t for t in out if any(x.get("id") == qs["externalIdentifier.id"][0]
                                                     for x in t.get("externalIdentifier") or [])]
                    return self.reply(200, out)
                if len(parts) == 2:
                    t = mock.tickets.get(parts[1])
                    if not t:
                        return self.reply(404, {"reason": "no such ticket"})
                    if method == "GET":
                        return self.reply(200, t)
                    if method == "PATCH":
                        if self.headers.get("Content-Type") != "application/merge-patch+json":
                            return self.reply(415, {"reason": "merge-patch+json"})
                        t.update(json.loads(raw))
                        t.setdefault("patches", 0)
                        t["patches"] += 1
                        return self.reply(200, t)
                return self.reply(405, {"reason": "method"})

            def do_GET(self):  # noqa: N802
                self.route("GET")

            def do_POST(self):  # noqa: N802
                self.route("POST")

            def do_PATCH(self):  # noqa: N802
                self.route("PATCH")

            def do_DELETE(self):  # noqa: N802
                self.route("DELETE")

        return H
