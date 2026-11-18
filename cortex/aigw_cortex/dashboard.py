# SPDX-License-Identifier: Apache-2.0
"""The operator dashboard: one web page that shows an ISP its fleet.

    python3 -m aigw_cortex dashboard [--listen 127.0.0.1:8090] [--token-file F] [--cert c --key k]

What the page shows, from what the gateways' bridges report (no new data
leaves a home for it):

  fleet health   experience scores (good, fair, poor), problems found and fixed
                 by AIGW, early warnings, outages across many homes of one group
                 (fleet.py's report)
  call first     the homes a help desk should look at first, with the reason in words
  rollouts       staged firmware rollouts (rollout.py): state, waves, kept or got
                 worse, why one stopped; pause and abort, with a reason (resuming
                 stays a person's action at the command line)
  ISP signals    a faster plan, mesh Wi-Fi, smart queueing, homes at risk, line repairs
  queueing       smart queueing decisions across the fleet, and calls-first
                 results where a bridge reports them
  per gateway    status, open problem and warning, alerts, decisions waiting for
                 approval (read only: approving stays on the gateway)

Plain HTML, CSS and JavaScript from this package (static/): no build step, no
CDN, so it works on an ISP's own servers with no internet.

Access: it listens on 127.0.0.1 by default, where reading needs no token. Bound
to any other address, every request needs the token. Pausing or aborting a
rollout always needs it. The token is made on first use, in the cortex data
directory (dashboard.token, mode 0600); the start line prints a link that
carries it in the URL's #fragment, which the browser never sends to a server.

Privacy, as in the fleet report: device names, addresses and the reasons that
quote them are shown only for homes whose bridge shares device names
(bridge setting devices = names); the bridge already keeps them home
otherwise, and the dashboard drops them again if one arrives anyway.
"""
import hmac
import ipaddress
import json
import os
import re
import secrets
import socket
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

from . import __version__, fleet, rollout

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
FILES = {"/": ("dashboard.html", "text/html; charset=utf-8"),
         "/static/dashboard.css": ("dashboard.css", "text/css; charset=utf-8"),
         "/static/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8")}
MAX_BODY = 16 * 1024
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; "
       "base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
LOCAL_HOSTS = ("127.0.0.1", "localhost", "::1")
# a decision's target names a device unless the action is one of these (a domain, the line, a radio)
TARGET_NOT_DEVICE = ("block", "sqm", "channel")
# what an alert or decision may say about a device: only with devices = names
DEVICE_FIELDS = ("device", "reason", "ip", "mac", "client", "hostname")
WHO = "a person (dashboard)"


# ---------- privacy ----------

def sharing(snap):
    """What a home's bridge shares about its devices, from its last report:
    names (bridge devices = names), anonymous, or none. Unknown counts as none."""
    devs = (snap or {}).get("devices")
    if not isinstance(devs, list) or not devs:
        return "none"
    if any(isinstance(d, dict) and (d.get("mac") or d.get("hostname")) for d in devs):
        return "names"
    return "anonymous"


def _private(item, names, keep_target=False):
    """An alert or decision with what it says about devices dropped unless names may be shown."""
    if not isinstance(item, dict):
        return None
    out = {k: v for k, v in item.items() if isinstance(v, (str, int, float, bool)) or v is None}
    if names:
        return out
    for k in DEVICE_FIELDS:
        out.pop(k, None)
    if not keep_target and str(out.get("action", "")) not in TARGET_NOT_DEVICE and "target" in out:
        out["target"] = "a device"
    out.pop("note", None)
    return out


# ---------- what the page reads ----------

def _d(x):
    return x if isinstance(x, dict) else {}


def _week_score(store, gid, now=None):
    """A home's experience score over its last 7 days, as the fleet report counts it."""
    today = int(now or time.time()) // 86400
    return fleet._score(fleet._sum(store.care_days(today - 6, today, {gid})))


def _snaps(store):
    return {g["id"]: store.snapshot(g["id"]) or {} for g in store.gateways()}


def _words(a, now):
    """A home on the call-first list, in words (as in the fleet report's text)."""
    if a["why"] == "problem":
        s = f"{a['code']} ({a['area']}) for {fleet._ago(a['since'], now)}"
        s += ", told to the ISP already" if a.get("reported") else ""
        return s + (f": {a['summary']}" if a.get("summary") else "")
    if a["why"] == "early warning":
        when = (f"a fault likely in about {a['predicted_fault_days']} days" if a.get("predicted_fault_days")
                else "worse than its normal")
        return f"early warning, {a['code']}: {when}" + (f": {a['summary']}" if a.get("summary") else "")
    if a["why"] == "poor score":
        return f"experience {a['score']}% of the time without a problem" + (
            f", mostly {a['main_cause']}" if a.get("main_cause") else "")
    return f"not heard from for {fleet._ago(a['last_seen'], now)}"


def queueing(store, ids=None, snaps=None):
    """Smart queueing and calls-first across the fleet, from each home's last report.
    Smart queueing comes from its decisions ("sqm"); calls-first from care.calls,
    which bridges up to 0.38 don't send (then: not reported)."""
    snaps = snaps if snaps is not None else _snaps(store)
    sqm = {"on": 0, "waiting": 0, "tried": 0, "homes": 0}
    calls = {"homes": 0, "total": 0, "calls": 0, "games": 0, "on_full_line": 0, "smooth_on_full_line": 0,
             "prioritized": 0}
    for gid, snap in snaps.items():
        if ids is not None and gid not in ids:
            continue
        decs = [d for d in snap.get("decisions") or [] if isinstance(d, dict) and d.get("action") == "sqm"]
        states = {str(d.get("state", "")) for d in decs}
        c = snap.get("care") if isinstance(snap.get("care"), dict) else {}
        q = c.get("sqm") if isinstance(c.get("sqm"), dict) else {}
        status = str(q.get("status", "")).lower()
        if states & {"active", "kept"} or status in ("on", "trial", "kept"):
            sqm["on"] += 1
        elif "proposed" in states or status == "proposed":
            sqm["waiting"] += 1
        elif "tried" in states or status == "tried":
            sqm["tried"] += 1
        if decs or q:
            sqm["homes"] += 1
        st = (c.get("calls") or {}).get("stats") if isinstance(c.get("calls"), dict) else None
        if isinstance(st, dict):
            calls["homes"] += 1
            for k in list(calls):
                if k != "homes":
                    try:
                        calls[k] += int(st.get(k) or 0)
                    except (TypeError, ValueError):
                        pass
    calls["reported"] = calls["homes"] > 0
    return {"sqm": sqm, "calls": calls}


def overview(store, days=7, group=None, now=None):
    """The fleet report, ready for the page: the call-first list in words, no
    device names from homes that don't share them."""
    now = int(now or time.time())
    r = fleet.report(store, days, group, now)
    gws = store.gateways()
    snaps = _snaps(store)
    by_name = {g["name"]: g["id"] for g in gws}
    named = {gid for gid, s in snaps.items() if sharing(s) == "names"}
    for a in r["attention"]:
        if a.get("summary") and by_name.get(a["home"]) not in named:
            a.pop("summary")                # a Wi-Fi problem's summary can name a device
        a["words"] = _words(a, now)
    ids = {g["id"] for g in gws if group is None or (g["grp"] or "") == group}
    r["queueing"] = queueing(store, ids, snaps)
    r["groups"] = sorted({g["grp"] or "" for g in gws} - {""})
    r["version"] = __version__
    r["now"] = now
    return r


def rollouts(store, now=None):
    """Every rollout, newest first: state, waves, counts and its log (no homes' names)."""
    out = []
    for row in store.q("SELECT id FROM rollouts ORDER BY id DESC LIMIT 50"):
        s = rollout.status(store, row["id"], now)
        s["log"] = s["log"][-20:]
        s["words"] = {k: rollout.WORDS[k] for k in s["homes"]["states"]}
        s["can"] = {"pause": s["state"] == "running", "abort": s["state"] in ("running", "paused")}
        out.append(s)
    return out


def gateways(store, now=None):
    """Each gateway in a line: version, group, when last heard, score, counts."""
    now = int(now or time.time())
    scores = {h["name"]: h for h in fleet.report(store, 7, None, now)["per_home"]}
    out = []
    for g in store.gateways():
        snap = store.snapshot(g["id"]) or {}
        st = snap.get("status") if isinstance(snap.get("status"), dict) else {}
        care = snap.get("care") if isinstance(snap.get("care"), dict) else {}
        h = scores.get(g["name"], {})
        out.append({"id": g["id"], "name": g["name"], "group": g["grp"] or "", "aigw": g["aigw"],
                    "last_seen": g["last_seen"], "silent": bool(g["last_seen"] and now - g["last_seen"] > fleet.SILENT_S),
                    "score": h.get("score"), "devices": st.get("devices"), "alerts": st.get("alerts"),
                    "waiting": sum(1 for d in snap.get("decisions") or []
                                   if isinstance(d, dict) and d.get("state") == "proposed"),
                    "care": care.get("status", ""), "sharing": sharing(snap)})
    return out


def gateway(store, ref, now=None):
    """One gateway: status, its open problem and early warning, alerts, decisions
    (waiting for approval first) and cortex's suggestions to it. Read only."""
    g = store.gateway(ref)
    if not g:
        return None
    snap = store.snapshot(g["id"]) or {}
    names = sharing(snap) == "names"
    st = snap.get("status") if isinstance(snap.get("status"), dict) else {}
    care = snap.get("care") if isinstance(snap.get("care"), dict) else {}
    problem = care.get("problem") if isinstance(care.get("problem"), dict) else None
    if problem:
        problem = {k: problem.get(k) for k in ("code", "area", "severity", "summary", "advice", "since", "reported",
                                                "owner") if k in problem}
        if not names:
            problem.pop("summary", None)
    warn = care.get("early_warning") if isinstance(care.get("early_warning"), dict) else None
    if warn and warn.get("status") != "warning":
        warn = None
    if warn:
        warn = {k: warn.get(k) for k in ("metric", "area", "summary", "advice", "since", "normal", "current",
                                          "predicted", "predicted_fault_days") if k in warn}
    alerts = [a for a in (_private(a, names, True) for a in (snap.get("alerts") or [])[:50]) if a]
    decs = [d for d in (_private(d, names) for d in snap.get("decisions") or []) if d]
    decs.sort(key=lambda d: (d.get("state") != "proposed", -int(d.get("created") or 0)))
    props = [{k: p[k] for k in ("id", "action", "state", "origin", "created", "updated", "error")}
             for p in store.proposals(g["id"])[:20]]
    for p, full in zip(props, store.proposals(g["id"])[:20]):
        p["target"] = full["target"] if names or full["action"] in TARGET_NOT_DEVICE else "a device"
        p["reason"] = full["reason"] if names else ""
    keep = ("version", "host", "mode", "devices", "alerts", "waiting", "active", "uptime_s", "plugins")
    return {"id": g["id"], "name": g["name"], "group": g["grp"] or "", "aigw": g["aigw"],
            "last_seen": g["last_seen"], "sharing": sharing(snap),
            "status": {k: st[k] for k in keep if k in st and isinstance(st[k], (str, int, float, bool))},
            "problem": problem, "early_warning": warn,
            "score": _week_score(store, g["id"], now),
            "alerts": alerts, "decisions": decs[:50],
            "waiting": sum(1 for d in decs if d.get("state") == "proposed"), "proposals": props,
            "queueing": queueing(store, {g["id"]}, {g["id"]: snap})}


# ---------- access ----------

def load_token(path):
    """The dashboard's token, made on first use (0600)."""
    try:
        with open(path) as f:
            tok = f.read().strip()
        if tok:
            return tok
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tok = secrets.token_urlsafe(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(tok + "\n")
    return tok


def is_loopback(host):
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host.strip("[]")).is_loopback
    except ValueError:
        return False


def parse_listen(s, default_port=8090):
    """127.0.0.1:8090, [::1]:8090, 0.0.0.0, :8090 -> (host, port)."""
    s = (s or "").strip()
    m = re.match(r"^\[([^\]]+)\](?::(\d+))?$", s)
    if m:
        return m.group(1), int(m.group(2) or default_port)
    if s.count(":") == 1:
        host, port = s.split(":")
        return host or "127.0.0.1", int(port)
    if s.count(":") > 1:
        return s, default_port              # a bare IPv6 address
    return s or "127.0.0.1", default_port


def make_handler(store, token, local, log=True):
    """local: bound to a loopback address (reading needs no token there)."""

    class Handler(BaseHTTPRequestHandler):
        server_version = "aigw-cortex-dashboard"

        def log_message(self, fmt, *args):  # noqa: N802 - http.server API
            if log:
                sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

        def headers_common(self, ctype, n):
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(n))
            self.send_header("Content-Security-Policy", CSP)
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Cache-Control", "no-store")

        def send_json(self, code, obj):
            # < > & as \u escapes: the JSON is never HTML, even if something renders it as such
            body = (json.dumps(obj).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")).encode()
            self.send_response(code)
            self.headers_common("application/json; charset=utf-8", len(body))
            if code == 401:
                self.send_header("WWW-Authenticate", 'Bearer realm="aigw-dashboard"')
            self.end_headers()
            self.wfile.write(body)

        def token_ok(self):
            auth = self.headers.get("Authorization", "")
            given = auth[7:].strip() if auth.startswith("Bearer ") else ""
            return bool(given) and hmac.compare_digest(given.encode(), token.encode())

        def host_local(self):
            """On a loopback address, a page from elsewhere could reach us through DNS
            rebinding: reading without a token only for a local Host."""
            h = (self.headers.get("Host") or "").strip()
            m = re.match(r"^\[([^\]]+)\](?::\d+)?$", h)
            name = m.group(1) if m else h.rsplit(":", 1)[0] if h.count(":") == 1 else h
            return name.lower() in LOCAL_HOSTS

        def may_read(self):
            return self.token_ok() or (local and self.host_local())

        def do_GET(self):  # noqa: N802
            u = urlsplit(self.path)
            path, qs = u.path, parse_qs(u.query)
            if path in FILES:
                name, ctype = FILES[path]
                with open(os.path.join(STATIC, name), "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.headers_common(ctype, len(data))
                self.end_headers()
                self.wfile.write(data)
                return None
            if not path.startswith("/api/"):
                return self.send_json(404, {"error": "unknown path"})
            if not self.may_read():
                return self.send_json(401, {"error": "this dashboard needs its token"})
            try:
                if path == "/api/overview":
                    days = max(1, min(31, int((qs.get("days") or ["7"])[0])))
                    group = (qs.get("group") or [""])[0] or None
                    return self.send_json(200, overview(store, days, group))
                if path == "/api/rollouts":
                    return self.send_json(200, {"rollouts": rollouts(store)})
                if path == "/api/gateways":
                    return self.send_json(200, {"gateways": gateways(store)})
                m = re.match(r"^/api/gateways/([^/]+)$", path)
                if m:
                    g = gateway(store, unquote(m.group(1)))
                    return self.send_json(200, g) if g else self.send_json(404, {"error": "no such gateway"})
            except ValueError as e:
                return self.send_json(400, {"error": str(e)})
            return self.send_json(404, {"error": "unknown path"})

        def do_POST(self):  # noqa: N802
            path = urlsplit(self.path).path
            if not self.token_ok():         # every change needs the token, on any address
                return self.send_json(401, {"error": "pausing or aborting a rollout needs the dashboard's token"})
            m = re.match(r"^/api/rollouts/([^/]+)/(pause|resume|abort)$", path)
            if not m:
                return self.send_json(404, {"error": "unknown path"})
            name, what = unquote(m.group(1)), m.group(2)
            if what == "resume":
                return self.send_json(403, {"error": "resuming a rollout is done at the command line: "
                                                     "python3 -m aigw_cortex rollout resume <name>"})
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0 or n > MAX_BODY:
                return self.send_json(400, {"error": "expected a JSON object with a reason"})
            try:
                body = json.loads(self.rfile.read(n).decode())
            except ValueError:
                body = None
            reason = str(body.get("reason", "")).strip() if isinstance(body, dict) else ""
            if not reason:
                return self.send_json(400, {"error": "say why, in a few words: it is kept in the rollout's log"})
            try:
                rollout.control(store, name, what, reason[:200], WHO)
            except ValueError as e:
                return self.send_json(409, {"error": str(e)})
            return self.send_json(200, {"ok": True, "rollouts": rollouts(store)})

    return Handler


class _Server(ThreadingHTTPServer):
    daemon_threads = True


class _Server6(_Server):
    address_family = socket.AF_INET6


def serve(store, listen="127.0.0.1:8090", token_file=None, cert=None, key=None, log=True):
    host, port = parse_listen(listen)
    local = is_loopback(host)
    token = load_token(token_file or os.path.join(store.dir, "dashboard.token"))
    httpd = (_Server6 if ":" in host else _Server)((host, port), make_handler(store, token, local, log))
    scheme = "http"
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https"
    shown = f"[{host}]" if ":" in host else ("127.0.0.1" if host in ("", "0.0.0.0") else host)
    port = httpd.server_address[1]
    print(f"dashboard on {scheme}://{shown}:{port}/" + (" (reading needs no token here)" if local else
                                                       " (every request needs the token)"), flush=True)
    print(f"  with the token (needed to pause or abort a rollout): {scheme}://{shown}:{port}/#token={token}",
          file=sys.stderr, flush=True)
    if not local and not cert:
        print("  this is not localhost: put it behind HTTPS (--cert/--key or a reverse proxy)", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
