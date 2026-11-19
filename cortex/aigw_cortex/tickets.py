# SPDX-License-Identifier: Apache-2.0
"""ISP ticketing (v0.42): cortex's area outages and the gateways' proactive
tickets, in the ISP's own trouble-ticket system.

    python3 -m aigw_cortex tickets [status|sync|outbox|retry|config|hub register|hub unregister] [--json]
    python3 -m aigw_cortex serve --tickets FILE     (default: $AIGW_CORTEX_TICKETS, else <data>/tickets.json)
    MCP tool: isp_tickets; the dashboard's "ISP tickets"; GET /api/tickets

Two ways out, either or both, off unless the configuration file turns them on:

  TM Forum TMF621 Trouble Ticket API, v4 (default) or v5
      POST   <url>/troubleTicket           a new outage incident or proactive ticket
      PATCH  <url>/troubleTicket/{id}      it changed (merge patch: severity, description, note)
      PATCH  ... {"status": "resolved"}    it is over (the homes recovered, AIGW or the ISP fixed it)
      GET    <url>/troubleTicket?externalId=...  before a create that may have reached the
                                           ISP already (a timeout, a restart): never a duplicate
      POST   <url>/hub                     `tickets hub register`: the ISP's events come back to
                                           cortex's API at /tmf621/listener (the ISP resolved,
                                           closed or cancelled a ticket)
  Signed generic webhooks, for ISPs without TMF621: a JSON POST per event, with
      X-AIGW-Event, X-AIGW-Delivery (unique: deduplicate on it), X-AIGW-Timestamp (unix s) and
      X-AIGW-Signature: v1=<hex HMAC-SHA256(secret, "<timestamp>." + body)>

Events: outage.opened, outage.updated, outage.resolved, proactive.opened,
proactive.updated, proactive.resolved. Which proactive tickets: those the gateway
told the ISP (Raised, Proposed: "told_isp", the default) or all of them ("all").

Durable: what is to be sent is a row in cortex.db (ticket_outbox) before anything
is sent, so it survives a restart; each (ticket, target) goes in order, with
exponential backoff on network errors, 5xx, 408 and 429 (Retry-After). A 4xx
other than those is the request's fault: the row is kept as "failed" with the
answer, and `tickets retry` sends it again after the cause is fixed. Each ticket's
externalId is "aigw:<this cortex>:<kind>:<ref>", fixed for its life.

Secrets (the OAuth2 client secret, a bearer token, the webhook secrets, the
listener's token) come from a file or an environment variable, read when used,
and never appear in logs, errors, the outbox or `tickets config`.

The field mapping is in docs/isp-ticketing.md.
"""
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import random
import secrets
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from . import __version__, outage, proactive

SCHEMA = """
CREATE TABLE IF NOT EXISTS ticket_meta (k TEXT PRIMARY KEY, v TEXT);
CREATE TABLE IF NOT EXISTS isp_tickets (
    key TEXT PRIMARY KEY, kind TEXT, ref TEXT, gateway TEXT DEFAULT '', external_id TEXT UNIQUE,
    state TEXT, fingerprint TEXT DEFAULT '', doc TEXT DEFAULT '{}', remote_id TEXT DEFAULT '',
    remote_href TEXT DEFAULT '', remote_status TEXT DEFAULT '', isp_closed INTEGER DEFAULT 0,
    sync TEXT DEFAULT 'pending', error TEXT DEFAULT '', created INTEGER, updated INTEGER, synced INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS ticket_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT, target TEXT, op TEXT, event TEXT, body TEXT DEFAULT '',
    state TEXT DEFAULT 'queued', attempts INTEGER DEFAULT 0, next_try INTEGER DEFAULT 0, lease INTEGER DEFAULT 0,
    error TEXT DEFAULT '', created INTEGER, done INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ticket_outbox_q ON ticket_outbox (state, key, target, id);
"""
EVENTS = ("outage.opened", "outage.updated", "outage.resolved",
          "proactive.opened", "proactive.updated", "proactive.resolved")
OP = {"opened": "create", "updated": "update", "resolved": "resolve", "reopened": "reopen"}
TMF = "tmf621"
LOOKBACK = outage.LOOKBACK
KEEP_DAYS = 31
ISP_CLOSED = ("resolved", "closed", "cancelled", "rejected")
SEVERITY = {"outage.DOWN": "Critical", "outage.LOSS": "Major", "outage.DNS": "Major", "outage.SLOW": "Minor",
            "proactive.problem": "Major", "proactive.warning": "Minor", "proactive.info": "Warning",
            "proactive.ok": "Warning"}
TICKET_TYPE = {"outage": "AreaOutage", "proactive": "ProactiveCare"}
AUTHOR = "AIGW OS cortex"
MAX_ENTITIES = 50
RETRY = {"base_s": 10, "max_s": 3600, "max_attempts": 30}
LEASE_S = 120


class Transient(Exception):
    """Try again later (network, 5xx, 408, 429)."""

    def __init__(self, msg, after=0):
        super().__init__(msg)
        self.after = after


class Permanent(Exception):
    """The request itself is refused (4xx): retrying the same won't help."""


class NotFound(Permanent):
    pass


# ---------- configuration ----------

def config_path(data_dir, path=None):
    return path or os.environ.get("AIGW_CORTEX_TICKETS") or os.path.join(data_dir, "tickets.json")


def _check_url(u, allow_http, what):
    p = urllib.parse.urlsplit(str(u or ""))
    if p.scheme not in ("https", "http") or not p.hostname:
        raise ValueError(f"{what}: needs an https:// URL")
    if p.username or p.password:
        raise ValueError(f"{what}: no credentials in the URL (use auth, from a file or the environment)")
    if p.scheme == "http" and not allow_http and not _loopback(p.hostname):
        raise ValueError(f"{what}: http:// only to this machine (or set allow_http)")
    return str(u).rstrip("/")


def _loopback(host):
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _no_inline(d, names, what):
    for n in names:
        if n in d:
            raise ValueError(f"{what}: {n} comes from a file ({n}_file) or the environment ({n}_env), "
                             "never the configuration itself")


def check_config(c):
    """The configuration, checked, with defaults filled in. Raises ValueError."""
    if not isinstance(c, dict):
        raise ValueError("the ticketing configuration is a JSON object")
    out = {"enabled": bool(c.get("enabled")), "dry_run": bool(c.get("dry_run")),
           "events": [str(e) for e in c.get("events", ["outage", "proactive"])],
           "proactive": str(c.get("proactive", "told_isp")),
           "interval_s": max(5, int(c.get("interval_s", 30))),
           "retry": dict(RETRY, **{k: int(v) for k, v in (c.get("retry") or {}).items() if k in RETRY}),
           "tmf621": None, "webhooks": []}
    for e in out["events"]:
        if e not in ("outage", "proactive") and e not in EVENTS:
            raise ValueError(f"events: {e} is none of outage, proactive, " + ", ".join(EVENTS))
    if out["proactive"] not in ("told_isp", "all"):
        raise ValueError("proactive: told_isp or all")
    allow_http = bool(c.get("allow_http"))
    t = c.get("tmf621")
    if t:
        if not isinstance(t, dict):
            raise ValueError("tmf621 is an object")
        a = dict(t.get("auth") or {"type": "none"})
        kind = a.get("type", "none")
        if kind not in ("none", "bearer", "oauth2"):
            raise ValueError("tmf621.auth.type: none, bearer or oauth2")
        _no_inline(a, ("token", "client_secret"), "tmf621.auth")
        if kind == "bearer" and not (a.get("token_file") or a.get("token_env")):
            raise ValueError("tmf621.auth: bearer needs token_file or token_env")
        if kind == "oauth2":
            if not a.get("token_url") or not a.get("client_id"):
                raise ValueError("tmf621.auth: oauth2 needs token_url and client_id")
            if not (a.get("client_secret_file") or a.get("client_secret_env")):
                raise ValueError("tmf621.auth: oauth2 needs client_secret_file or client_secret_env")
            a["token_url"] = _check_url(a["token_url"], allow_http, "tmf621.auth.token_url")
        lis = dict(t.get("listener") or {})
        _no_inline(lis, ("token",), "tmf621.listener")
        version = int(t.get("version", 4))
        if version not in (4, 5):
            raise ValueError("tmf621.version: 4 or 5")
        gid = str(t.get("gateway_id", "name"))
        if gid not in ("name", "serial", "id"):
            raise ValueError("tmf621.gateway_id: name, serial or id")
        out["tmf621"] = {
            "url": _check_url(t.get("url"), allow_http, "tmf621.url"), "version": version, "auth": a,
            "timeout": float(t.get("timeout", 10)), "ca_file": t.get("ca_file") or None,
            "ticket_type": dict(TICKET_TYPE, **(t.get("ticket_type") or {})),
            "severity": dict(SEVERITY, **(t.get("severity") or {})),
            "related_party": list(t.get("related_party") or [
                {"id": "aigw-cortex", "name": AUTHOR, "role": "originator", "@referredType": "Organization"}]),
            "gateway_id": gid, "listener": lis,
            "hub_callback": _check_url(t["hub_callback"], allow_http, "tmf621.hub_callback")
            if t.get("hub_callback") else "",
            "hub_query": str(t.get("hub_query", "")),
        }
    for i, w in enumerate(c.get("webhooks") or []):
        if not isinstance(w, dict):
            raise ValueError(f"webhooks[{i}] is an object")
        _no_inline(w, ("secret",), f"webhooks[{i}]")
        if not (w.get("secret_file") or w.get("secret_env")):
            raise ValueError(f"webhooks[{i}]: needs secret_file or secret_env (every webhook is signed)")
        url = _check_url(w.get("url"), allow_http, f"webhooks[{i}].url")
        evs = [str(e) for e in w.get("events", out["events"])]
        out["webhooks"].append({"url": url, "events": evs, "secret_file": w.get("secret_file"),
                                "secret_env": w.get("secret_env"), "timeout": float(w.get("timeout", 10)),
                                "ca_file": w.get("ca_file") or None,
                                "name": "webhook:" + hashlib.sha256(url.encode()).hexdigest()[:10]})
    return out


def load_config(data_dir, path=None):
    """The configuration from its file; off (enabled false) when there is none."""
    p = config_path(data_dir, path)
    try:
        with open(p) as f:
            raw = json.load(f)
    except FileNotFoundError:
        if path:
            raise ValueError(f"{path}: no such file")
        return check_config({})
    except ValueError as e:
        raise ValueError(f"{p}: not JSON ({e})")
    cfg = check_config(raw)
    cfg["path"] = p
    return cfg


def _secret(d, name):
    """A secret from <name>_file or <name>_env; never inline, never logged."""
    if d.get(name + "_file"):
        try:
            with open(d[name + "_file"]) as f:
                v = f.read().strip()
        except OSError as e:
            raise Transient(f"can't read {name}_file ({e.strerror})")
    else:
        v = os.environ.get(str(d.get(name + "_env") or ""), "").strip()
    if not v:
        raise Transient(f"{name} is empty (from {name}_file or {name}_env)")
    return v


def wants(cfg_events, event):
    return event in cfg_events or event.split(".")[0] in cfg_events


# ---------- HTTP ----------

def _ctx(ca_file):
    ctx = ssl.create_default_context(cafile=ca_file) if ca_file else ssl.create_default_context()
    return ctx


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):        # never follow: it would carry the Authorization header elsewhere
        return None


def http(method, url, body=None, headers=None, timeout=10.0, ca_file=None):
    """One request. Returns (status, headers, parsed JSON or None). Raises Transient,
    Permanent or NotFound with a short reason (no request headers, so no secrets)."""
    data = None
    h = {"Accept": "application/json", "User-Agent": f"aigw-cortex/{__version__}"}
    h.update(headers or {})
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=data, method=method, headers=h)
    handlers = [_NoRedirect()]
    if url.startswith("https:"):
        handlers.append(urllib.request.HTTPSHandler(context=_ctx(ca_file)))
    opener = urllib.request.build_opener(*handlers)
    try:
        with opener.open(req, timeout=timeout) as r:
            raw = r.read(1024 * 1024)
            return r.status, dict(r.headers), _json(raw)
    except urllib.error.HTTPError as e:
        raw = e.read(2048) if e.fp else b""
        what = f"{method} {_path(url)}: HTTP {e.code}" + _why(raw)
        if e.code in (408, 425, 429) or e.code >= 500:
            try:
                after = int(e.headers.get("Retry-After") or 0)
            except (TypeError, ValueError):
                after = 0
            raise Transient(what, after)
        if e.code == 404:
            raise NotFound(what)
        raise Permanent(what)
    except (urllib.error.URLError, OSError, ValueError) as e:
        reason = getattr(e, "reason", None) or e
        raise Transient(f"{method} {_path(url)}: {str(reason)[:160]}")


def _json(raw):
    try:
        return json.loads(raw.decode()) if raw else None
    except ValueError:
        return None


def _path(url):
    p = urllib.parse.urlsplit(url)
    return f"{p.netloc}{p.path}"


def _why(raw):
    j = _json(raw)
    if isinstance(j, dict):
        for k in ("reason", "message", "error_description", "error"):
            if isinstance(j.get(k), str):
                return ": " + j[k][:160]
    return ""


_tokens = {}
_tok_lock = threading.Lock()


def _auth_header(t, fresh=False):
    a = t["auth"]
    if a.get("type") == "bearer":
        return {"Authorization": "Bearer " + _secret(a, "token")}
    if a.get("type") != "oauth2":
        return {}
    key = (a["token_url"], a["client_id"])
    with _tok_lock:
        tok, exp = _tokens.get(key, ("", 0))
        if tok and not fresh and time.time() < exp:
            return {"Authorization": "Bearer " + tok}
    form = {"grant_type": "client_credentials"}
    if a.get("scope"):
        form["scope"] = a["scope"]
    basic = (urllib.parse.quote(a["client_id"], safe="") + ":" +
             urllib.parse.quote(_secret(a, "client_secret"), safe="")).encode()
    try:
        _, _, j = http("POST", a["token_url"], urllib.parse.urlencode(form).encode(),
                       {"Content-Type": "application/x-www-form-urlencoded",
                        "Authorization": "Basic " + base64.b64encode(basic).decode()},
                       t["timeout"], t.get("ca_file"))
    except Permanent as e:
        raise Permanent(f"OAuth2 token: {str(e).split(': HTTP ')[-1]}")
    if not isinstance(j, dict) or not j.get("access_token"):
        raise Transient("OAuth2 token: the answer has no access_token")
    life = int(j.get("expires_in") or 300)
    with _tok_lock:
        _tokens[key] = (str(j["access_token"]), time.time() + max(10, life - 30))
    return {"Authorization": "Bearer " + str(j["access_token"])}


def tmf_call(t, method, path, body=None, ctype=None):
    """A TMF621 request with auth; a 401 gets one new OAuth2 token and one more try."""
    url = t["url"] + path
    hdr = {"Content-Type": ctype} if ctype else {}
    for fresh in (False, True):
        h = dict(hdr, **_auth_header(t, fresh))
        try:
            return http(method, url, body, h, t["timeout"], t.get("ca_file"))
        except Permanent as e:
            if ": HTTP 401" in str(e) and not fresh and t["auth"].get("type") == "oauth2":
                continue
            raise
    raise Permanent("unreachable")


# ---------- webhook signatures ----------

def sign(secret, ts, body):
    """X-AIGW-Signature for a body sent at ts (unix seconds)."""
    mac = hmac.new(secret.encode() if isinstance(secret, str) else secret, f"{int(ts)}.".encode() + body,
                   hashlib.sha256)
    return "v1=" + mac.hexdigest()


def verify(secret, ts, body, signature, now=None, tolerance=300):
    """What a receiver checks: the signature (constant time) and that the timestamp is recent."""
    try:
        ts = int(ts)
    except (TypeError, ValueError):
        return False
    if abs(int(now or time.time()) - ts) > tolerance:
        return False
    return hmac.compare_digest(sign(secret, ts, body), str(signature or ""))


# ---------- the store ----------

def _table(store):
    if not getattr(store, "_tickets_ok", False):
        with store.lock:
            store.db.executescript(SCHEMA)
        store._tickets_ok = True


def meta(store, k, default=""):
    _table(store)
    r = store.q("SELECT v FROM ticket_meta WHERE k = ?", (k,))
    return r[0]["v"] if r else default


def set_meta(store, k, v):
    _table(store)
    store.x("INSERT OR REPLACE INTO ticket_meta VALUES (?, ?)", (k, str(v)))


def instance(store):
    """This cortex's id in externalIds, made once and kept in its database."""
    i = meta(store, "instance")
    if not i:
        store.x("INSERT OR IGNORE INTO ticket_meta VALUES ('instance', ?)", (secrets.token_hex(4),))
        i = meta(store, "instance")
    return i


def iso(ts):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(int(ts))) if ts else None


# ---------- what a ticket says ----------

def _gw_ids(store, tk):
    """The names (and TR-069 serial numbers) of the gateways a ticket is about."""
    d = json.loads(tk["doc"])
    ids = {tk["gateway"]} | {h.get("gateway") for h in d.get("affected") or [] if isinstance(h, dict)}
    out = {}
    for gid in ids - {"", None}:
        g = store.gateway(gid)
        if not g:
            continue
        snap = json.loads(g["snapshot"] or "{}")
        b = snap.get("brief") if isinstance(snap.get("brief"), dict) else {}
        st = snap.get("status") if isinstance(snap.get("status"), dict) else {}
        out[g["id"]] = {"name": g["name"], "group": g["grp"] or "", "serial": str(b.get("serial") or
                                                                                  st.get("serial") or "")}
    return out


def outage_doc(inc):
    keep = ("id", "uplink", "kind", "state", "start", "end", "homes", "homes_in_area", "still_affected", "confidence",
            "codes", "spread_s", "declared", "words")
    d = {k: inc.get(k) for k in keep}
    d["affected"] = [{"gateway": h["gateway"], "home": h["home"], "since": h["since"], "until": h["until"],
                      "code": h["code"]} for h in inc.get("affected") or []][:MAX_ENTITIES]
    return d


def _fp(d, keys):
    return hashlib.sha256(json.dumps({k: d.get(k) for k in keys}, sort_keys=True).encode()).hexdigest()[:16]


OUTAGE_FP = ("kind", "homes", "homes_in_area", "still_affected", "confidence", "state")
PRO_FP = ("status", "severity", "summary", "evidence", "next_step", "customer_note", "devices", "points", "open")


def _related_entity(t, eid, name, role, rtype, v):
    if v == 5:
        return {"role": role, "@type": "RelatedEntity",
                "entity": {"id": eid, "name": name, "@referredType": rtype, "@type": "EntityRef"}}
    return {"id": eid, "name": name, "role": role, "@referredType": rtype}


def _related_party(p, v):
    if v == 5:
        inner = {k: p[k] for k in ("id", "name", "href", "@referredType") if k in p}
        inner["@type"] = "PartyRef"
        return {"role": p.get("role", "originator"), "partyOrPartyRole": inner,
                "@type": "RelatedPartyRefOrPartyRoleRef"}
    return dict(p)


def _note(nid, text, ts, v):
    n = {"id": nid, "author": AUTHOR, "date": iso(ts), "text": str(text)[:4000]}
    if v == 5:
        n["@type"] = "Note"
    return n


def _gw_entity(t, gw, gws):
    g = gws.get(gw) or {"name": gw, "serial": ""}
    eid = {"id": gw, "name": g["name"], "serial": g.get("serial") or g["name"]}[t["gateway_id"]]
    return _related_entity(t, eid, g["name"], "affectedGateway", "Resource", t["version"])


def tmf_body(t, tk, gws, now):
    """The TroubleTicket for a ticket row (its doc), as TMF621 v4 or v5 wants it."""
    v = t["version"]
    d = json.loads(tk["doc"])
    ext = tk["external_id"]
    if tk["kind"] == "outage":
        sev = t["severity"].get("outage." + str(d.get("kind")), "Major")
        what = outage.WORDS.get(d.get("kind"), str(d.get("kind", "")).lower())
        name = f"Area outage {d['id']} on {d['uplink']}: {d['homes']} of {d['homes_in_area']} homes {what}"
        ents = [_related_entity(t, d["uplink"], d["uplink"], "uplink", "Resource", v)]
        ents += [_gw_entity(t, h["gateway"], gws) for h in d.get("affected") or []]
        codes = ", ".join(f"{k} {n}" for k, n in (d.get("codes") or {}).items())
        notes = [_note("aigw-brief", d.get("words") or name, d.get("start") or now, v),
                 _note("aigw-evidence", f"{d['homes']} of {d['homes_in_area']} homes behind {d['uplink']} went "
                                        f"{what} within {max(1, int(d.get('spread_s') or 0) // 60)} min of "
                                        f"{iso(d.get('start'))}" + (f" ({codes})" if codes else "") +
                                        f"; {d.get('still_affected', 0)} still affected; confidence "
                                        f"{d.get('confidence')}." + (f" Declared: {d['declared']}." if d.get("declared")
                                                                     else ""), now, v),
                 _note("aigw-next-step", "The fault is in the access network behind this uplink, not in the homes: "
                                         "no visits to the homes. AIGW holds its own fixes in them until it is over, "
                                         "and the help desk's brief says there is a known outage.", now, v)]
        desc = d.get("words") or name
    else:
        sev = t["severity"].get("proactive." + str(d.get("severity")), "Minor")
        home = (gws.get(tk["gateway"]) or {}).get("name", tk["gateway"])
        name = f"{d.get('code')} at {home}" + (f": {d['summary']}" if d.get("summary") else "")
        ents = [_gw_entity(t, tk["gateway"], gws)]
        if d.get("uplink"):
            ents.append(_related_entity(t, d["uplink"], d["uplink"], "uplink", "Resource", v))
        notes = [_note("aigw-brief", d.get("summary") or name, d.get("opened") or now, v)]
        for k, nid, label in (("evidence", "aigw-evidence", ""), ("next_step", "aigw-next-step", "Next step: "),
                              ("customer_note", "aigw-customer-note", "For the customer: ")):
            if d.get(k):
                notes.append(_note(nid, label + d[k], now, v))
        notes.append(_note("aigw-status", f"On the gateway: {d.get('status')} ({d.get('fault')}, {d.get('points')} "
                                          f"points, {d.get('devices') or 0} devices)." +
                                          (f" {d['reason']}" if d.get("reason") else ""), now, v))
        desc = d.get("summary") or name
    body = {"name": name[:200], "description": str(desc)[:4000], "severity": sev,
            "ticketType": t["ticket_type"].get(tk["kind"], tk["kind"]),
            "relatedEntity": ents[:MAX_ENTITIES + 1],
            "relatedParty": [_related_party(p, v) for p in t["related_party"]],
            "note": notes, "@type": "TroubleTicket"}
    if v == 5:
        body["externalIdentifier"] = [{"id": ext, "owner": AUTHOR, "externalIdentifierType": "aigw",
                                       "@type": "ExternalIdentifier"}]
    else:
        body["externalId"] = ext
    return body


def webhook_body(event, tk, now, delivery):
    d = json.loads(tk["doc"])
    return {"id": delivery, "type": event, "time": iso(now), "source": f"aigw-cortex/{__version__}",
            "externalId": tk["external_id"], "kind": tk["kind"], "ref": tk["ref"], "data": d}


# ---------- what is new: scan ----------

_scan_lock = threading.Lock()


def _enqueue(store, cfg, tk, event, now):
    """Rows for the targets that want this event. A TMF621 update already queued (not
    yet tried) covers this one too: it sends the ticket as it is when it goes."""
    t = cfg.get("tmf621")
    op = OP[event.split(".")[1]]
    if t and wants(cfg["events"], event if op != "reopen" else event.replace("reopened", "updated")) \
            and not tk.get("isp_closed"):
        dup = store.q("SELECT 1 FROM ticket_outbox WHERE key = ? AND target = ? AND op = ? AND state = 'queued' "
                      "AND attempts = 0", (tk["key"], TMF, op)) if op == "update" else []
        if not dup:
            store.x("INSERT INTO ticket_outbox (key, target, op, event, created, next_try) VALUES (?, ?, ?, ?, ?, ?)",
                    (tk["key"], TMF, op, event, now, now))
    ev = event.replace("reopened", "updated")
    for w in cfg.get("webhooks") or []:
        if wants(w["events"], ev):
            delivery = secrets.token_hex(12)
            store.x("INSERT INTO ticket_outbox (key, target, op, event, body, created, next_try) VALUES "
                    "(?, ?, 'post', ?, ?, ?, ?)",
                    (tk["key"], w["name"], ev, json.dumps(webhook_body(ev, tk, now, delivery)), now, now))


def _wanted(cfg, kind):
    """Does any target want this kind's events?"""
    evs = [f"{kind}.{x}" for x in ("opened", "updated", "resolved")]
    return _tmf_on(cfg, kind) or any(wants(w["events"], e) for w in cfg.get("webhooks") or [] for e in evs)


def _tmf_on(cfg, kind):
    return bool(cfg.get("tmf621")) and any(wants(cfg["events"], f"{kind}.{x}") for x in ("opened", "updated",
                                                                                          "resolved"))


def _upsert(store, cfg, key, kind, ref, gw, doc, fp, is_open, since, now):
    """One ticket's news: opened, updated, resolved or reopened. Returns the event or None."""
    rows = store.q("SELECT * FROM isp_tickets WHERE key = ?", (key,))
    if not rows:
        if not is_open and (doc.get("opened") or doc.get("start") or 0) < since:
            return None                     # over before ticketing was on: not the ISP's news now
        ext = f"aigw:{instance(store)}:{kind}:{ref}"
        store.x("INSERT OR IGNORE INTO isp_tickets (key, kind, ref, gateway, external_id, state, fingerprint, doc, "
                "created, updated, sync) VALUES (?, ?, ?, ?, ?, 'open', ?, ?, ?, ?, ?)",
                (key, kind, ref, gw, ext, fp, json.dumps(doc), now, now, "pending" if _tmf_on(cfg, kind) else "none"))
        tk = store.q("SELECT * FROM isp_tickets WHERE key = ?", (key,))[0]
        _enqueue(store, cfg, tk, f"{kind}.opened", now)
        if not is_open:
            store.x("UPDATE isp_tickets SET state = 'resolved' WHERE key = ?", (key,))
            _enqueue(store, cfg, tk, f"{kind}.resolved", now)
            return f"{kind}.opened+resolved"
        return f"{kind}.opened"
    tk = rows[0]
    if tk["fingerprint"] == fp and (tk["state"] == "open") == is_open:
        return None
    store.x("UPDATE isp_tickets SET fingerprint = ?, doc = ?, updated = ?, sync = CASE WHEN isp_closed OR NOT ? "
            "THEN sync ELSE 'pending' END WHERE key = ?", (fp, json.dumps(doc), now, int(_tmf_on(cfg, kind)), key))
    tk = dict(tk, doc=json.dumps(doc), fingerprint=fp)
    if tk["state"] == "open" and not is_open:
        store.x("UPDATE isp_tickets SET state = 'resolved' WHERE key = ?", (key,))
        ev = f"{kind}.resolved"
    elif tk["state"] != "open" and is_open:
        store.x("UPDATE isp_tickets SET state = 'open' WHERE key = ?", (key,))
        ev = f"{kind}.reopened"
    else:
        ev = f"{kind}.updated"
    _enqueue(store, cfg, tk, ev, now)
    return ev


def scan(store, cfg, now=None):
    """Find what is new since the last look and queue it. Returns {event: count}."""
    _table(store)
    now = int(now or time.time())
    if not cfg.get("enabled"):
        return {}
    since = int(meta(store, "since") or 0)
    if not since:
        set_meta(store, "since", now)
        since = now
    seen = {}
    with _scan_lock:
        if _wanted(cfg, "outage"):
            for inc in outage.incidents(store, since=now - LOOKBACK):
                doc = outage_doc(inc)
                ev = _upsert(store, cfg, f"outage:{inc['id']}", "outage", str(inc["id"]), "", doc,
                             _fp(doc, OUTAGE_FP), inc["state"] == "open", since, now)
                if ev:
                    seen[ev] = seen.get(ev, 0) + 1
        if _wanted(cfg, "proactive"):
            _scan_proactive(store, cfg, since, now, seen)
        store.x("DELETE FROM ticket_outbox WHERE state != 'queued' AND done < ?", (now - KEEP_DAYS * 86400,))
        store.x("DELETE FROM isp_tickets WHERE state != 'open' AND updated < ? AND key NOT IN "
                "(SELECT key FROM ticket_outbox WHERE state = 'queued')", (now - KEEP_DAYS * 86400,))
    return seen


def _scan_proactive(store, cfg, since, now, seen):
    homes = outage._homes(store)
    tracked = {r["key"]: r for r in store.q("SELECT key, state, gateway FROM isp_tickets WHERE kind = 'proactive'")}
    present, reporting = set(), set()
    gws = store.gateways()
    for g in gws:
        p = proactive.of(store.snapshot(g["id"]))
        if p:
            reporting.add(g["id"])
        for t in p.get("tickets") or []:
            if not isinstance(t, dict) or "id" not in t:
                continue
            ref = f"{g['id']}:{t['id']}:{proactive._int(t.get('opened'))}"
            key = "proactive:" + ref
            present.add(key)
            told = bool(t.get("told_isp")) or t.get("status") in ("Raised", "Proposed")
            if key not in tracked and cfg["proactive"] == "told_isp" and not told:
                continue
            doc = proactive._ticket(t)
            doc["uplink"] = (homes.get(g["id"]) or {}).get("uplink", "")
            is_open = t.get("open") in (1, True)
            ev = _upsert(store, cfg, key, "proactive", ref, g["id"], doc, _fp(doc, PRO_FP), is_open, since, now)
            if ev:
                seen[ev] = seen.get(ev, 0) + 1
    # a gateway keeps its last 16 tickets: one we follow that is gone from its report (or whose
    # gateway was removed) is over
    known = {g["id"] for g in gws}
    for key, r in tracked.items():
        if r["state"] != "open" or key in present or (r["gateway"] in known and r["gateway"] not in reporting):
            continue
        tk = store.q("SELECT * FROM isp_tickets WHERE key = ?", (key,))[0]
        doc = dict(json.loads(tk["doc"]), open=False, status="Gone")
        ev = _upsert(store, cfg, key, "proactive", tk["ref"], tk["gateway"], doc, _fp(doc, PRO_FP), False, since, now)
        if ev:
            seen[ev] = seen.get(ev, 0) + 1


# ---------- sending: flush ----------

def _lookup(t, ext):
    """A ticket the ISP may have from an earlier try: by externalId (v4) or externalIdentifier.id (v5)."""
    q = ("externalIdentifier.id=" if t["version"] == 5 else "externalId=") + urllib.parse.quote(ext, safe="")
    _, _, j = tmf_call(t, "GET", "/troubleTicket?" + q)
    items = j if isinstance(j, list) else (j.get("items") if isinstance(j, dict) else None) or []
    for it in items:
        if not isinstance(it, dict):
            continue
        got = it.get("externalId")
        ids = [x.get("id") for x in it.get("externalIdentifier") or [] if isinstance(x, dict)]
        if got == ext or ext in ids:
            return it
    return None


def _adopt(store, key, it, now):
    store.x("UPDATE isp_tickets SET remote_id = ?, remote_href = ?, remote_status = ?, synced = ? WHERE key = ?",
            (str(it.get("id") or ""), str(it.get("href") or ""), str(it.get("status") or ""), now, key))


def _ensure_remote(store, t, tk, row, now):
    """The ISP's id of this ticket, creating it if no earlier try did. Returns the id."""
    if tk["remote_id"]:
        return tk["remote_id"]
    if row["attempts"] > 1 or meta(store, "tried:" + tk["key"]):
        it = _lookup(t, tk["external_id"])
        if it:
            _adopt(store, tk["key"], it, now)
            return str(it.get("id") or "")
    set_meta(store, "tried:" + tk["key"], now)          # kept before the POST: a crash after it looks first
    _, _, j = tmf_call(t, "POST", "/troubleTicket", tmf_body(t, tk, _gw_ids(store, tk), now))
    if not isinstance(j, dict) or not j.get("id"):
        raise Transient("POST troubleTicket: the answer has no id")
    _adopt(store, tk["key"], j, now)
    store.x("DELETE FROM ticket_meta WHERE k = ?", ("tried:" + tk["key"],))
    return str(j["id"])


def _resolve_body(t, tk, now):
    d = json.loads(tk["doc"])
    if tk["kind"] == "outage":
        why = (f"Over: {d.get('homes')} homes behind {d.get('uplink')}, recovered by {iso(d.get('end') or now)}"
               if not d.get("declared") else f"Ended by the operator at {iso(d.get('end') or now)}")
    else:
        st = d.get("status")
        why = {"Fixed": "Fixed by AIGW on the gateway", "Resolved": "Over on the gateway (the ISP had been told)",
               "Passed": "Over by itself on the gateway", "Gone": "No longer reported by the gateway"}.get(
            st, f"Over on the gateway ({st})")
    body = {"status": "resolved", "statusChangeReason": why, "resolutionDate": iso(now)}
    b = tmf_body(t, tk, {}, now)
    body["note"] = b["note"] + [_note("aigw-resolution", why, now, t["version"])]
    return body


def _send_tmf(store, cfg, row, now):
    t = cfg["tmf621"]
    tk = store.q("SELECT * FROM isp_tickets WHERE key = ?", (row["key"],))
    if not tk:
        return "dropped"
    tk = tk[0]
    if tk["isp_closed"] and row["op"] != "create":
        return "skipped: the ISP closed it"
    if cfg.get("dry_run"):
        body = tmf_body(t, tk, _gw_ids(store, tk), now) if row["op"] == "create" else (
            _resolve_body(t, tk, now) if row["op"] == "resolve" else _patch_body(store, t, tk, now, row["op"]))
        store.x("UPDATE ticket_outbox SET body = ? WHERE id = ?", (json.dumps(body), row["id"]))
        return "dry-run"
    rid = _ensure_remote(store, t, tk, row, now)
    if row["op"] == "create":
        return "sent"
    tk = store.q("SELECT * FROM isp_tickets WHERE key = ?", (row["key"],))[0]
    body = _resolve_body(t, tk, now) if row["op"] == "resolve" else _patch_body(store, t, tk, now, row["op"])
    try:
        _, _, j = tmf_call(t, "PATCH", "/troubleTicket/" + urllib.parse.quote(rid, safe=""), body,
                           "application/merge-patch+json")
    except NotFound:
        store.x("UPDATE isp_tickets SET isp_closed = 1, remote_status = 'deleted' WHERE key = ?", (row["key"],))
        return "skipped: the ISP deleted it"
    if isinstance(j, dict) and j.get("status"):
        store.x("UPDATE isp_tickets SET remote_status = ? WHERE key = ?", (str(j["status"]), row["key"]))
    return "sent"


def _patch_body(store, t, tk, now, op):
    b = tmf_body(t, tk, _gw_ids(store, tk), now)
    body = {k: b[k] for k in ("name", "description", "severity", "relatedEntity", "note")}
    if op == "reopen":
        body["status"] = "inProgress"
        body["statusChangeReason"] = "It came back on the gateway"
    return body


def _send_hook(cfg, row, now):
    w = next((x for x in cfg.get("webhooks") or [] if x["name"] == row["target"]), None)
    if not w:
        raise Permanent("this webhook is no longer configured")
    if cfg.get("dry_run"):
        return "dry-run"
    body = row["body"].encode()
    ts = int(time.time())
    j = json.loads(row["body"])
    h = {"Content-Type": "application/json", "X-AIGW-Event": row["event"], "X-AIGW-Delivery": j.get("id", ""),
         "X-AIGW-Timestamp": str(ts), "X-AIGW-Signature": sign(_secret(w, "secret"), ts, body)}
    http("POST", w["url"], body, h, w["timeout"], w.get("ca_file"))
    return "sent"


def _backoff(cfg, attempts, after=0):
    r = cfg["retry"]
    d = min(r["max_s"], r["base_s"] * (2 ** max(0, attempts - 1)))
    return max(int(after), int(d * random.uniform(0.8, 1.2)))


def flush(store, cfg, now=None, limit=200):
    """Send what is due, in order per (ticket, target). Returns {result: count}."""
    _table(store)
    now = int(now or time.time())
    if not cfg.get("enabled"):
        return {}
    out = {}
    rows =store.q("SELECT * FROM ticket_outbox WHERE state = 'queued' ORDER BY id LIMIT ?", (int(limit),))
    blocked = set()
    for row in rows:
        kt = (row["key"], row["target"])
        if kt in blocked:
            continue
        if row["next_try"] > now or row["lease"] > now:
            blocked.add(kt)
            continue
        claim = store.x("UPDATE ticket_outbox SET lease = ?, attempts = attempts + 1 WHERE id = ? AND state = 'queued' "
                        "AND lease <= ?", (now + LEASE_S, row["id"], now))
        if not claim.rowcount:
            blocked.add(kt)
            continue
        row["attempts"] += 1
        try:
            res = _send_tmf(store, cfg, row, now) if row["target"] == TMF else _send_hook(cfg, row, now)
            state = "dry-run" if res == "dry-run" else "done"
            store.x("UPDATE ticket_outbox SET state = ?, done = ?, error = ?, lease = 0 WHERE id = ?",
                    (state, now, "" if res in ("sent", "dry-run") else res, row["id"]))
            if row["target"] == TMF and res in ("sent", "dry-run"):
                store.x("UPDATE isp_tickets SET sync = ?, error = '', synced = ? WHERE key = ?",
                        ("dry-run" if res == "dry-run" else "synced", now, row["key"]))
            elif row["target"] == TMF:
                store.x("UPDATE isp_tickets SET sync = 'synced', error = '' WHERE key = ? AND sync != 'dry-run'",
                        (row["key"],))
            set_meta(store, "last_ok", now)
            out[res.split(":")[0]] = out.get(res.split(":")[0], 0) + 1
        except Transient as e:
            blocked.add(kt)
            dead = row["attempts"] >= cfg["retry"]["max_attempts"]
            store.x("UPDATE ticket_outbox SET state = ?, next_try = ?, error = ?, lease = 0, done = ? WHERE id = ?",
                    ("failed" if dead else "queued", now + _backoff(cfg, row["attempts"], e.after), str(e)[:300],
                     now if dead else 0, row["id"]))
            _failed(store, row, str(e), now, "failed" if dead else "retrying")
            out["retrying"] = out.get("retrying", 0) + 1
        except Permanent as e:
            blocked.add(kt)
            store.x("UPDATE ticket_outbox SET state = 'failed', error = ?, lease = 0, done = ? WHERE id = ?",
                    (str(e)[:300], now, row["id"]))
            _failed(store, row, str(e), now, "failed")
            out["failed"] = out.get("failed", 0) + 1
    return out


def _failed(store, row, err, now, sync):
    set_meta(store, "last_error", json.dumps({"time": now, "target": row["target"], "text": err[:300]}))
    if row["target"] == TMF:
        store.x("UPDATE isp_tickets SET sync = ?, error = ? WHERE key = ?", (sync, err[:300], row["key"]))


def retry(store, key=None):
    """Send the failed rows again (after their cause is fixed). Returns how many."""
    _table(store)
    sql = "UPDATE ticket_outbox SET state = 'queued', attempts = 0, next_try = 0, lease = 0, done = 0 " \
          "WHERE state = 'failed'"
    n = store.x(sql + (" AND key = ?" if key else ""), (key,) if key else ()).rowcount
    store.x("UPDATE isp_tickets SET sync = 'pending' WHERE sync IN ('failed', 'retrying')" +
            (" AND key = ?" if key else ""), (key,) if key else ())
    return n


def sync(store, cfg, now=None):
    """Scan, then send: what `tickets sync` and the server's worker do."""
    return {"found": scan(store, cfg, now), "sent": flush(store, cfg, now)}


# ---------- the ISP's events (TMF621 hub) ----------

def hub_register(store, cfg):
    t = cfg.get("tmf621")
    if not t or not t.get("hub_callback"):
        raise ValueError("needs tmf621.hub_callback: where the ISP sends its events (cortex's API + /tmf621/listener)")
    if not (t["listener"].get("token_file") or t["listener"].get("token_env")):
        raise ValueError("needs tmf621.listener.token_file or token_env: the token the ISP's hub sends")
    body = {"callback": t["hub_callback"]}
    if t.get("hub_query"):
        body["query"] = t["hub_query"]
    try:
        _, _, j = tmf_call(t, "POST", "/hub", body)
    except (Transient, Permanent) as e:
        raise ValueError(str(e))
    hid = str((j or {}).get("id") or "")
    set_meta(store, "hub_id", hid)
    return hid


def hub_unregister(store, cfg):
    t = cfg.get("tmf621")
    hid = meta(store, "hub_id")
    if not t or not hid:
        raise ValueError("not registered")
    try:
        tmf_call(t, "DELETE", "/hub/" + urllib.parse.quote(hid, safe=""))
    except NotFound:
        pass
    except (Transient, Permanent) as e:
        raise ValueError(str(e))
    set_meta(store, "hub_id", "")
    return hid


def listener_token_ok(cfg, auth):
    t = cfg.get("tmf621") if cfg.get("enabled") else None
    if not t:
        return None                         # off: the path doesn't exist
    lis = t.get("listener") or {}
    if not (lis.get("token_file") or lis.get("token_env")):
        return None
    try:
        want = _secret(lis, "token")
    except Transient:
        return False
    given = auth[7:].strip() if str(auth or "").startswith("Bearer ") else ""
    return bool(given) and hmac.compare_digest(given.encode(), want.encode())


def on_event(store, ev, now=None):
    """An event from the ISP's hub: a TroubleTicket*Event. Records the ISP's status; a
    ticket the ISP resolved, closed, cancelled or rejected gets no more updates, and a
    declared area outage it resolved is ended. Returns (code, answer)."""
    _table(store)
    now = int(now or time.time())
    if not isinstance(ev, dict):
        return 400, {"error": "expected an event"}
    tt = (ev.get("event") or {}).get("troubleTicket") if isinstance(ev.get("event"), dict) else None
    if not isinstance(tt, dict):
        return 400, {"error": "expected event.troubleTicket"}
    rid = str(tt.get("id") or "")
    exts = [tt.get("externalId")] + [x.get("id") for x in tt.get("externalIdentifier") or [] if isinstance(x, dict)]
    rows = store.q("SELECT * FROM isp_tickets WHERE (remote_id != '' AND remote_id = ?) OR external_id IN (?, ?)",
                   (rid, str(exts[0] or "-"), str((exts[1:] or ["-"])[0] or "-")))
    if not rows:
        return 202, {"ok": True, "known": False}      # not one of ours: fine, nothing to do
    tk = rows[0]
    status = str(tt.get("status") or "")
    closed = status.lower() in ISP_CLOSED or str(ev.get("eventType", "")) == "TroubleTicketDeleteEvent"
    store.x("UPDATE isp_tickets SET remote_status = ?, isp_closed = CASE WHEN ? THEN 1 ELSE isp_closed END, "
            "remote_id = CASE WHEN remote_id = '' THEN ? ELSE remote_id END, updated = ? WHERE key = ?",
            (status or ("deleted" if closed else tk["remote_status"]), int(closed), rid, now, tk["key"]))
    ended = None
    if closed and tk["kind"] == "outage":
        r = store.q("SELECT declared, state FROM incidents WHERE id = ?", (int(tk["ref"]),))
        if r and r[0]["declared"] and r[0]["state"] == "open":
            outage.end(store, int(tk["ref"]), now)
            ended = int(tk["ref"])
    return 201, {"ok": True, "known": True, "status": status, "ended_outage": ended}


# ---------- what people see ----------

def summary(store, cfg=None, now=None):
    """Ticket sync state, for the dashboard, the fleet report and MCP."""
    _table(store)
    now = int(now or time.time())
    cfg = cfg or {}
    targets = []
    if cfg.get("tmf621"):
        targets.append({"name": TMF, "kind": f"TMF621 v{cfg['tmf621']['version']}",
                        "host": urllib.parse.urlsplit(cfg["tmf621"]["url"]).netloc})
    for w in cfg.get("webhooks") or []:
        targets.append({"name": w["name"], "kind": "webhook", "host": urllib.parse.urlsplit(w["url"]).netloc})
    sync_c = {r["sync"]: r["n"] for r in store.q("SELECT sync, COUNT(*) n FROM isp_tickets GROUP BY sync")}
    state_c = {r["state"]: r["n"] for r in store.q("SELECT state, COUNT(*) n FROM isp_tickets GROUP BY state")}
    box = {r["state"]: r["n"] for r in store.q("SELECT state, COUNT(*) n FROM ticket_outbox GROUP BY state")}
    try:
        last_err = json.loads(meta(store, "last_error") or "null")
    except ValueError:
        last_err = None
    problems = [ticket_row(r) for r in store.q(
        "SELECT * FROM isp_tickets WHERE sync IN ('failed', 'retrying') ORDER BY updated DESC LIMIT 10")]
    return {"enabled": bool(cfg.get("enabled")), "dry_run": bool(cfg.get("dry_run")), "targets": targets,
            "events": cfg.get("events") or [], "tickets": {"open": state_c.get("open", 0),
                                                           "resolved": state_c.get("resolved", 0),
                                                           "isp_closed": store.q(
                                                               "SELECT COUNT(*) n FROM isp_tickets WHERE isp_closed = 1"
                                                           )[0]["n"]},
            "sync": sync_c, "outbox": {"queued": box.get("queued", 0), "failed": box.get("failed", 0),
                                       "dry_run": box.get("dry-run", 0), "sent": box.get("done", 0)},
            "last_ok": int(meta(store, "last_ok") or 0) or None, "last_error": last_err,
            "hub": meta(store, "hub_id") or None, "problems": problems}


def state(store):
    """summary() with this cortex's own configuration (an unreadable one says why)."""
    try:
        cfg = load_config(store.dir)
    except ValueError as e:
        cfg = {"error": str(e)}
    s = summary(store, cfg)
    if cfg.get("error"):
        s["config_error"] = cfg["error"]
    return s


def attach(store, r):
    """The fleet report (and so the dashboard's overview) with each outage's and open
    proactive ticket's ISP ticket, and the sync state."""
    isp = isp_of(store)
    for o in r.get("outages") or []:
        o["isp_ticket"] = isp.get(f"outage:{o.get('id')}")
    ids = {g["name"]: g["id"] for g in store.gateways()}
    for t in (r.get("proactive") or {}).get("open_tickets") or []:
        t["isp_ticket"] = isp.get(proactive_key(ids.get(t.get("home"), ""), t))
    r["isp_tickets"] = state(store)
    return r


def fleet_lines(r):
    s = r.get("isp_tickets") or {}
    if not s.get("enabled"):
        return []
    tk, ob = s["tickets"], s["outbox"]
    line = (f"  ISP tickets  {'DRY RUN, ' if s.get('dry_run') else ''}{tk['open']} open, {tk['resolved']} resolved, "
            f"{tk['isp_closed']} closed by the ISP; {ob['queued']} to send, {ob['failed']} failed")
    if s.get("last_error") and ob["failed"]:
        line += f"; last error: {s['last_error'].get('text')}"
    return [line]


def ticket_row(r):
    return {"key": r["key"], "kind": r["kind"], "ref": r["ref"], "external_id": r["external_id"], "state": r["state"],
            "sync": r["sync"], "remote_id": r["remote_id"] or None, "remote_status": r["remote_status"] or None,
            "isp_closed": bool(r["isp_closed"]), "error": r["error"] or None, "updated": r["updated"],
            "synced": r["synced"] or None}


def isp_of(store):
    """key -> what the ISP's system has of it, for attaching to outages and proactive tickets."""
    _table(store)
    return {r["key"]: {"sync": r["sync"], "remote_id": r["remote_id"] or None,
                       "remote_status": r["remote_status"] or None, "isp_closed": bool(r["isp_closed"]),
                       "error": r["error"] or None}
            for r in store.q("SELECT * FROM isp_tickets")}


def proactive_key(gw, t):
    return f"proactive:{gw}:{t.get('id')}:{proactive._int(t.get('opened'))}"


def listing(store, open_only=False, limit=100):
    _table(store)
    return [ticket_row(r) for r in store.q("SELECT * FROM isp_tickets" + (" WHERE state = 'open'" if open_only else "") +
                                           " ORDER BY updated DESC LIMIT ?", (int(limit),))]


def outbox(store, limit=50):
    _table(store)
    rows = store.q("SELECT id, key, target, op, event, state, attempts, next_try, error, created, done "
                   "FROM ticket_outbox ORDER BY id DESC LIMIT ?", (int(limit),))
    return rows


def text(s, now=None):
    now = int(now or time.time())
    if not s["enabled"]:
        return "ISP ticketing is off (tickets.json: enabled true; see docs/isp-ticketing.md)."
    out = [("ISP ticketing, DRY RUN (nothing is sent): " if s["dry_run"] else "ISP ticketing: ") +
           (", ".join(f"{t['kind']} {t['host']}" for t in s["targets"]) or "no targets") +
           f"; events {', '.join(s['events'])}"]
    tk, ob = s["tickets"], s["outbox"]
    out.append(f"  tickets      {tk['open']} open, {tk['resolved']} resolved, {tk['isp_closed']} closed by the ISP; "
               "sync " + (", ".join(f"{k} {v}" for k, v in sorted(s["sync"].items())) or "none"))
    out.append(f"  outbox       {ob['queued']} to send, {ob['failed']} failed, {ob['sent']} sent" +
               (f", {ob['dry_run']} dry run" if ob["dry_run"] else ""))
    if s.get("last_ok"):
        out.append(f"  last sent    {max(0, now - s['last_ok']) // 60} min ago")
    if s.get("last_error"):
        e = s["last_error"]
        out.append(f"  last error   {e.get('target')}: {e.get('text')} ({max(0, now - int(e.get('time') or now)) // 60} "
                   "min ago)")
    for p in s["problems"]:
        out.append(f"    {p['key']} ({p['external_id']}): {p['sync']}: {p['error']}")
    return "\n".join(out)


# ---------- the server's worker ----------

class Worker:
    """Scans and sends every interval_s, and soon after a gateway reports (kick)."""

    def __init__(self, store, cfg, log=True):
        self.store, self.cfg, self.log = store, cfg, log
        self.wake = threading.Event()

    def kick(self):
        self.wake.set()

    def start(self):
        if not self.cfg.get("enabled"):
            return None
        th = threading.Thread(target=self.run, daemon=True, name="tickets")
        th.start()
        return th

    def run(self):
        while True:
            try:
                r = sync(self.store, self.cfg)
                if self.log and (r["found"] or r["sent"]):
                    sys.stderr.write(f"tickets: {r}\n")
            except Exception as e:  # noqa: BLE001 - keep serving
                sys.stderr.write(f"tickets: {type(e).__name__}: {str(e)[:200]}\n")
            self.wake.wait(self.cfg["interval_s"])
            self.wake.clear()
            time.sleep(1)               # a burst of reports is one look
