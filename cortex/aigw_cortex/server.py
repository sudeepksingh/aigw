# SPDX-License-Identifier: Apache-2.0
"""The cortex HTTP API that gateways' bridges talk to.

    POST /v1/report              gateway state: status, devices, alerts, decisions, models;
                                 the answer's "area" says whether it is in an area outage (outage.py)
    GET  /v1/models              newest signed model (and list, e.g. threat-intel) per name
    GET  /v1/models/<file>       a model (.agm) or list (.list), or its signature (.sig)
    GET  /v1/proposals           suggestions waiting for this gateway (sent once)
    POST /v1/proposals/<id>      how the gateway took one: {ok, decision, error}
    GET  /v1/fed                 the open federated learning round: shared layer, how to train
    POST /v1/fed/update          a home's clipped, noisy change {round, delta, n, noise, clip}
    GET  /v1/firmware            a staged rollout's firmware, if this home's wave is cleared (sent once)
    POST /v1/firmware/<rollout>  how the gateway took it: {ok, current, error}
    POST /tmf621/listener[/...]  (v0.42) the ISP's TMF621 hub: a ticket it resolved, closed or
                                 cancelled (tickets.py); its own token (tmf621.listener), not a gateway's

Every request carries "Authorization: Bearer <token>" from `enroll`. Use
--cert/--key (or a reverse proxy) for HTTPS anywhere beyond your own LAN.
"""
import json
import os
import re
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import fed, outage, rollout, tickets

MAX_BODY = 4 * 1024 * 1024
FILE_RE = re.compile(r"^[a-z0-9][a-z0-9.-]{0,80}\.(agm|list|adapter)(\.sig)?$")


def make_handler(store, log=True, fed_settings=None, tickets_cfg=None, kick=None):
    fs = fed_settings or fed.Settings()
    tc = tickets_cfg or {}

    class Handler(BaseHTTPRequestHandler):
        server_version = "aigw-cortex"

        def log_message(self, fmt, *args):  # noqa: N802 - http.server API
            if log:
                sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

        def send_json(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def gateway(self):
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Bearer "):
                return None
            return store.gateway_by_token(auth[7:].strip())

        def body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0 or n > MAX_BODY:
                return None
            try:
                return json.loads(self.rfile.read(n).decode())
            except ValueError:
                return None

        def do_GET(self):  # noqa: N802
            gw = self.gateway()
            if not gw:
                return self.send_json(401, {"error": "unknown gateway token"})
            path = self.path.split("?")[0]
            if path == "/v1/models":
                return self.send_json(200, {"models": [
                    {"name": m["name"], "version": m["version"], "file": m["file"], "sig": m["file"] + ".sig",
                     "sha256": m["sha256"], "size": m["size"]} for m in store.latest_models()]})
            if path.startswith("/v1/models/"):
                name = path[len("/v1/models/"):]
                full = os.path.join(store.models_dir(), name)
                if not FILE_RE.match(name) or not os.path.isfile(full):
                    return self.send_json(404, {"error": "no such model file"})
                with open(full, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return None
            # v0.39: a home in an area outage gets no fix and no firmware until it is over
            held = outage.held(store, gw["id"])
            if path == "/v1/proposals":
                return self.send_json(200, {"proposals": store.take_proposals(
                    gw["id"], outage.HELD_ACTIONS if held else ())})
            if path == "/v1/fed":
                return self.send_json(200, fed.round_for_gateway(store, fs))
            if path == "/v1/firmware":
                return self.send_json(200, {"offer": None if held else rollout.offer(store, gw["id"])})
            return self.send_json(404, {"error": "unknown path"})

        def listener(self):
            """v0.42: an event from the ISP's TMF621 hub (tickets.on_event)."""
            ok = tickets.listener_token_ok(tc, self.headers.get("Authorization", ""))
            if ok is None:
                return self.send_json(404, {"error": "unknown path"})
            if not ok:
                return self.send_json(401, {"error": "unknown listener token"})
            code, answer = tickets.on_event(store, self.body())
            return self.send_json(code, answer)

        def do_POST(self):  # noqa: N802
            p = self.path.split("?")[0]
            if p == "/tmf621/listener" or p.startswith("/tmf621/listener/"):
                return self.listener()
            gw = self.gateway()
            if not gw:
                return self.send_json(401, {"error": "unknown gateway token"})
            body = self.body()
            if not isinstance(body, dict):
                return self.send_json(400, {"error": "expected a JSON object"})
            path = self.path.split("?")[0]
            if path == "/v1/report":
                learned = store.report(gw["id"], body)
                if kick:
                    kick()                  # v0.42: news for the ISP's tickets goes out soon, not at the next look
                # v0.39: in an area outage the gateway holds its own fixes (agd/area.c); incident 0: none
                return self.send_json(200, {"ok": True, "gateway": gw["name"], "labels": learned,
                                            "area": outage.notice(store, gw["id"])})
            if path == "/v1/fed/update":
                code, answer = fed.accept(store, fs, gw["id"], body)
                return self.send_json(code, answer)
            m = re.match(r"^/v1/firmware/(\d+)$", path)
            if m:
                ok = rollout.offer_result(store, gw["id"], int(m.group(1)), bool(body.get("ok")),
                                          bool(body.get("current")), str(body.get("error") or ""))
                return self.send_json(200 if ok else 404, {"ok": ok})
            m = re.match(r"^/v1/proposals/(\d+)$", path)
            if m:
                n = store.proposal_result(gw["id"], int(m.group(1)), bool(body.get("ok")),
                                          int(body.get("decision") or 0), str(body.get("error") or ""))
                return self.send_json(200 if n else 404, {"ok": bool(n)})
            return self.send_json(404, {"error": "unknown path"})

    return Handler


def serve(store, listen="127.0.0.1", port=8790, cert=None, key=None, log=True, fed_settings=None, tickets_cfg=None):
    tc = tickets_cfg or {}
    worker = tickets.Worker(store, tc, log)
    httpd = ThreadingHTTPServer((listen, port), make_handler(store, log, fed_settings, tc,
                                                             worker.kick if tc.get("enabled") else None))
    scheme = "http"
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https"
    def rollouts():                 # a wave whose homes went quiet still times out
        while True:
            time.sleep(60)
            try:
                rollout.step_all(store)
            except Exception as e:  # noqa: BLE001 - keep serving
                sys.stderr.write(f"rollouts: {e}\n")
            try:
                outage.update(store)    # homes gone silent together: an area dropped
            except Exception as e:  # noqa: BLE001 - keep serving
                sys.stderr.write(f"outages: {e}\n")

    threading.Thread(target=rollouts, daemon=True).start()
    if worker.start():
        print("ISP ticketing on" + (" (DRY RUN: nothing is sent)" if tc.get("dry_run") else "") + ": " +
              ", ".join(t["kind"] + " " + t["host"] for t in tickets.summary(store, tc)["targets"]), flush=True)
    print(f"cortex listening on {scheme}://{listen}:{httpd.server_address[1]}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
