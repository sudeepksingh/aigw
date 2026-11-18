# SPDX-License-Identifier: Apache-2.0
"""Area outages: when many homes behind one uplink go slow or drop in the same
few minutes, the fault is in the ISP's network, not in the homes.

    python3 -m aigw_cortex outages [--all] [--json]       open incidents (--all: the last days too)
    python3 -m aigw_cortex outages settings [--window S] [--min-homes N] [--share F] [--recovered F]
    python3 -m aigw_cortex group <gateway> <uplink>      the operator's uplink (area) id for a home
    MCP tool: area_outages; the dashboard's "Area outages"; GET /api/outages

Which homes share an uplink, first found wins:
  1. the operator's id in cortex (enroll --group, or `group`): an exchange, an
     OLT or DSLAM, a BNG, a street cabinet, a town
  2. the id the ISP's ACS wrote on the gateway: Device.X_AIGW_.AreaOutage.UplinkID
     (agd's area.uplink_id), sent with each report
  3. the gateway's WAN next hop ("wan:<address>"), or with no next hop (PPPoE) its
     peer, the BNG ("bng:<address>"). Private addresses (a modem in front of the
     gateway: every home has 192.168.1.1) group nothing.

What counts as a home's fault on its line, with its start and end:
  - proactive care's problem on the line or DNS (LINE_DOWN, ISP_LOSS, DNS_FAILING...)
  - the help-desk brief's timeline verdict LINE, while it is current
  - the gateway gone silent: no report for `silent_after` (only while cortex hears
    from other gateways meanwhile: cortex's own network down is not an outage);
    a gap that is over is kept as a fault from the missed report to the next one

The rules (correlate(), pure, unit-tested):
  - faults of one uplink that start within `window` (15 min) of the first one
  - at least max(min_homes, share x the uplink's homes) homes (3, and 10%): one or
    two homes are never an outage, however small the uplink
  - homes whose fault starts later, while the incident is still on, join it
  - over when `recovered` (80%) of its homes have recovered; the end is when that
    happened. A home still faulty then is its own problem again
  - confidence: high with twice the homes needed or half the uplink, else medium
  - kind: DOWN, LOSS, SLOW or DNS, the one most homes had

While an incident is open, its homes are tagged:
  - the help-desk brief (brief.py, MCP support_brief) says "known outage in your
    area", no technician, nothing to fix in the home
  - fixes in the home are held: cortex keeps proposals of sqm, stay24 and channel
    queued (sent when it is over) and offers no firmware from a staged rollout;
    the gateway is told with its report's answer ("area") and holds its own
    (smart-queueing trials, band steering, channel changes): agd/area.c, which
    raises Device.X_AIGW_.AreaOutage! for the ISP's ACS and forgets it by itself
    unless told again (hold_s), so a lost cortex never holds fixes for long
"""
import ipaddress
import json
import math
import time
from collections import Counter

SCHEMA = """
CREATE TABLE IF NOT EXISTS incidents (
    id INTEGER PRIMARY KEY AUTOINCREMENT, uplink TEXT, kind TEXT, start INTEGER, end INTEGER DEFAULT 0,
    homes INTEGER, homes_in_area INTEGER, still INTEGER, need INTEGER, confidence TEXT, codes TEXT,
    spread_s INTEGER, state TEXT, opened INTEGER, updated INTEGER);
CREATE TABLE IF NOT EXISTS incident_homes (
    incident INTEGER, gateway TEXT, since INTEGER, until INTEGER DEFAULT 0, kind TEXT, code TEXT,
    PRIMARY KEY (incident, gateway));
CREATE TABLE IF NOT EXISTS line_faults (
    gateway TEXT, source TEXT, code TEXT, since INTEGER, until INTEGER DEFAULT 0,
    PRIMARY KEY (gateway, source, code, since));
CREATE TABLE IF NOT EXISTS uplinks (
    gateway TEXT PRIMARY KEY, reported TEXT DEFAULT '', source TEXT DEFAULT '', interval_s INTEGER DEFAULT 0,
    updated INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS outage_settings (k TEXT PRIMARY KEY, v REAL);
"""
# v0.39: an incident the operator declared (the NOC knows first): every home of the uplink, until ended
MANUAL_COL = "ALTER TABLE incidents ADD COLUMN declared TEXT DEFAULT ''"

DEFAULTS = {
    "window": 900,          # s: faults starting this close to the first one belong together
    "min_homes": 3,         # never fewer homes than this
    "share": 0.1,           # ... nor fewer than this share of the uplink's homes
    "recovered": 0.8,       # over when this share of its homes has recovered
    "silent_after": 900,    # s without a report (at least 3 check-ins) before a home counts as dropped
    "hold_s": 1800,         # s a gateway holds its fixes after the last notice (renewed with each report)
}
LOOKBACK = 2 * 86400        # faults this old (and over) are no longer looked at
KEEP_DAYS = 31              # incidents and faults are kept this long
DEFAULT_INTERVAL = 300
HELD_ACTIONS = ("sqm", "stay24", "channel")    # fixes in the home, held during an incident
KINDS = ("DOWN", "LOSS", "SLOW", "DNS")         # by severity: a tie goes to the first
KIND_OF = {
    "LINE_DOWN": "DOWN", "ISP_DOWN": "DOWN", "NO_ROUTE": "DOWN", "SILENT": "DOWN",
    "LINE_LOSS": "LOSS", "ISP_LOSS": "LOSS",
    "LINE_SLOW": "SLOW", "ISP_SLOW": "SLOW", "LINE_SLOW_SPEED": "SLOW",
    "DNS_FAILING": "DNS", "DNS_SLOW": "DNS",
}
WORDS = {"DOWN": "down", "LOSS": "losing packets", "SLOW": "slow", "DNS": "failing to look up names"}


def _table(store):
    if not getattr(store, "_outage_ok", False):
        with store.lock:
            store.db.executescript(SCHEMA)
            if "declared" not in {r[1] for r in store.db.execute("PRAGMA table_info(incidents)").fetchall()}:
                store.db.execute(MANUAL_COL)
                store.db.commit()
        store._outage_ok = True


# ---------- settings ----------

def settings(store):
    _table(store)
    s = dict(DEFAULTS)
    for r in store.q("SELECT k, v FROM outage_settings"):
        if r["k"] in s:
            s[r["k"]] = type(DEFAULTS[r["k"]])(r["v"])
    return s


def set_settings(store, **kw):
    """Change the rules; kept in the database, so the API, the dashboard and the MCP server agree."""
    _table(store)
    limits = {"window": (60, 7200), "min_homes": (2, 10000), "share": (0.0, 1.0), "recovered": (0.5, 1.0),
              "silent_after": (300, 86400), "hold_s": (300, 21600)}
    for k, v in kw.items():
        if v is None:
            continue
        lo, hi = limits[k]
        if not lo <= float(v) <= hi:
            raise ValueError(f"{k} must be from {lo} to {hi}")
        store.x("INSERT OR REPLACE INTO outage_settings VALUES (?, ?)", (k, float(v)))
    return settings(store)


# ---------- which uplink ----------

HOME_NETS = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "0.0.0.0/8", "fc00::/7")]


def _usable(addr):
    """An address that tells uplinks apart: not one a home network uses (a modem in front
    of the gateway: RFC 1918, IPv6 ULA), link-local or loopback. 100.64.0.0/10
    (carrier-grade NAT) is the ISP's own, so it counts."""
    try:
        a = ipaddress.ip_address(str(addr).strip())
    except ValueError:
        return False
    if a.is_loopback or a.is_link_local or a.is_unspecified or a.is_multicast:
        return False
    return not any(a.version == n.version and a in n for n in HOME_NETS)


def reported_uplink(status):
    """From a report's status: (the uplink, where it came from); ("", "") when none."""
    st = status if isinstance(status, dict) else {}
    u = str(st.get("uplink_id") or "").strip()[:64]
    if u:
        return u, "gateway"
    if _usable(st.get("wan_gateway")):
        return "wan:" + str(st["wan_gateway"]).strip(), "wan"
    if _usable(st.get("bng")):
        return "bng:" + str(st["bng"]).strip(), "bng"
    return "", ""


def _homes(store):
    """Each gateway: its uplink (the operator's first), where that came from, last_seen, interval."""
    _table(store)
    out = {}
    for r in store.q("SELECT g.id, g.name, g.grp, g.last_seen, u.reported, u.source, u.interval_s "
                     "FROM gateways g LEFT JOIN uplinks u ON u.gateway = g.id"):
        if r["grp"]:
            up, src = r["grp"], "operator"
        else:
            up, src = r["reported"] or "", r["source"] or ""
        out[r["id"]] = {"name": r["name"], "uplink": up, "source": src, "last_seen": r["last_seen"] or 0,
                        "interval": int(r["interval_s"] or 0) or DEFAULT_INTERVAL}
    return out


# ---------- the rules ----------

def kind_of(code, area=""):
    return KIND_OF.get(code) or ("DNS" if area == "DNS" else "SLOW")


def need_for(size, s):
    return max(int(s["min_homes"]), int(math.ceil(float(s["share"]) * size - 1e-9)))


def _merge(faults):
    """A home's faults in one incident as one: the first start, open if any is, else the last end."""
    homes = {}
    for f in faults:
        h = homes.setdefault(f["gateway"], {"since": f["since"], "until": f.get("until") or 0, "open": False,
                                            "kinds": Counter(), "code": f["code"]})
        if f["since"] < h["since"]:
            h["since"], h["code"] = f["since"], f["code"]
        h["open"] = h["open"] or not f.get("until")
        h["until"] = max(h["until"], f.get("until") or 0)
        h["kinds"][f["kind"]] += 1
    for h in homes.values():
        if h["open"]:
            h["until"] = 0
        h["kind"] = min(h["kinds"], key=lambda k: (-h["kinds"][k], KINDS.index(k) if k in KINDS else 9))
    return homes


def _judge(faults, t0, size, s):
    homes = _merge(faults)
    peak = len(homes)
    left = int(math.floor(peak * (1.0 - float(s["recovered"])) + 1e-9))     # stragglers allowed when over
    still = sum(1 for h in homes.values() if not h["until"])
    end = 0
    if still <= left:
        ends = sorted(h["until"] for h in homes.values() if h["until"])
        end = ends[peak - left - 1] if len(ends) >= peak - left else (ends[-1] if ends else 0)
    kinds = Counter(h["kind"] for h in homes.values())
    kind = min(kinds, key=lambda k: (-kinds[k], KINDS.index(k) if k in KINDS else 9))
    need = need_for(size, s)
    conf = "high" if peak >= 2 * need or (size and peak * 2 >= size) else "medium"
    window_starts = [f["since"] for f in faults if f["since"] <= t0 + s["window"]]
    return {"start": t0, "end": end, "homes": peak, "still": still, "need": need, "kind": kind, "confidence": conf,
            "codes": dict(Counter(h["code"] for h in homes.values()).most_common()),
            "spread_s": max(window_starts) - t0 if window_starts else 0, "members": homes}


def correlate(faults, sizes, s=None, now=None):
    """Area incidents from the homes' line faults.

    faults: dicts with gateway, uplink, code, kind, since, until (0: still on)
    sizes:  uplink -> how many homes it has
    Returns incidents (see _judge), each with its uplink and homes_in_area, oldest first."""
    s = dict(DEFAULTS, **(s or {}))
    now = int(now or time.time())
    by = {}
    for f in faults:
        if f.get("uplink"):
            by.setdefault(f["uplink"], []).append(f)
    found = []
    for up, fs in by.items():
        fs = sorted(fs, key=lambda f: (f["since"], f["gateway"]))
        size = max(int(sizes.get(up, 0)), len({f["gateway"] for f in fs}))
        need = need_for(size, s)
        used = set()
        for i, first in enumerate(fs):
            if i in used:
                continue
            t0 = first["since"]
            members = [j for j in range(i, len(fs)) if j not in used and fs[j]["since"] <= t0 + s["window"]]
            if len({fs[j]["gateway"] for j in members}) < need:
                continue
            inc = _judge([fs[j] for j in members], t0, size, s)
            # homes whose line went bad later, while it was still on, are part of it too
            for _ in range(3):
                more = [j for j in range(len(fs)) if j not in used and j not in members and
                        t0 + s["window"] < fs[j]["since"] < (inc["end"] or now + 1)]
                if not more:
                    break
                members += more
                inc = _judge([fs[j] for j in members], t0, size, s)
            used.update(members)
            inc["uplink"] = up
            inc["homes_in_area"] = size
            found.append(inc)
    return sorted(found, key=lambda x: (x["start"], x["uplink"]))


# ---------- what the homes report ----------

LINE_BRIEF = {c for c in KIND_OF if c != "SILENT"}


def _brief_since(b):
    times = [int(e.get("time") or 0) for e in b.get("evidence") or []
             if isinstance(e, dict) and e.get("source") in ("timeline", "care", "now") and e.get("time")]
    return min(times) if times else int(b.get("made") or 0)


def on_report(store, gw, rep, prev_seen, now):
    """A gateway reported: its uplink and interval, its brief's line fault, a gap in its reports."""
    _table(store)
    now = int(now)
    st = rep.get("status") if isinstance(rep.get("status"), dict) else {}
    up, src = reported_uplink(st)
    try:
        interval = max(2, min(86400, int(rep.get("interval") or 0))) if rep.get("interval") else 0
    except (TypeError, ValueError):
        interval = 0
    store.x("INSERT INTO uplinks (gateway, reported, source, interval_s, updated) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(gateway) DO UPDATE SET reported = excluded.reported, source = excluded.source, "
            "interval_s = CASE WHEN excluded.interval_s > 0 THEN excluded.interval_s ELSE uplinks.interval_s END, "
            "updated = excluded.updated", (gw, up, src, interval, now))
    s = settings(store)
    # the brief's verdict: the line, now
    b = rep.get("brief")
    if isinstance(b, dict) and b.get("fault") == "LINE" and b.get("current") and b.get("code") in LINE_BRIEF:
        code = str(b["code"])
        if not store.q("SELECT 1 FROM line_faults WHERE gateway = ? AND source = 'brief' AND code = ? AND until = 0",
                       (gw, code)):
            store.x("INSERT OR IGNORE INTO line_faults VALUES (?, 'brief', ?, ?, 0)", (gw, code, _brief_since(b) or now))
        store.x("UPDATE line_faults SET until = ? WHERE gateway = ? AND source = 'brief' AND until = 0 AND code != ?",
                (now, gw, code))
    else:               # fine now, or no brief this time (brief.enabled 0): over
        store.x("UPDATE line_faults SET until = ? WHERE gateway = ? AND source = 'brief' AND until = 0", (now, gw))
    # a gap: no report for a while, while cortex heard from other homes (else cortex itself was away)
    iv = interval or (_homes(store).get(gw) or {}).get("interval") or DEFAULT_INTERVAL
    after = max(int(s["silent_after"]), 3 * iv)
    if prev_seen and now - prev_seen > after:
        others = store.q("SELECT COUNT(*) n FROM history WHERE gateway != ? AND ts > ? AND ts < ?",
                         (gw, prev_seen, now))
        if others and others[0]["n"]:
            store.x("INSERT OR IGNORE INTO line_faults VALUES (?, 'silent', 'SILENT', ?, ?)",
                    (gw, prev_seen + iv, now))


def faults(store, homes, now, s):
    """Every home's line faults of the last days, and those going on now."""
    since = now - LOOKBACK
    out = []
    for e in store.q("SELECT gateway, code, area, since, until FROM care_events WHERE kind = 'problem' "
                     "AND area IN ('Internet', 'DNS') AND (until = 0 OR since >= ?)", (since,)):
        out.append({"gateway": e["gateway"], "code": e["code"], "kind": kind_of(e["code"], e["area"]),
                    "since": e["since"], "until": e["until"] or 0, "source": "care"})
    for e in store.q("SELECT * FROM line_faults WHERE until = 0 OR since >= ?", (since,)):
        out.append({"gateway": e["gateway"], "code": e["code"], "kind": kind_of(e["code"]), "since": e["since"],
                    "until": e["until"] or 0, "source": e["source"]})
    # silent now: only while cortex hears from someone (else it is cortex that is cut off)
    def after(h):
        return max(int(s["silent_after"]), 3 * h["interval"])
    quiet = {g for g, h in homes.items() if h["last_seen"] and now - h["last_seen"] > after(h)}
    heard = any(h["last_seen"] and now - h["last_seen"] <= after(h) for g, h in homes.items() if g not in quiet)
    if heard:
        for g in quiet:
            h = homes[g]
            if now - h["last_seen"] < LOOKBACK:
                out.append({"gateway": g, "code": "SILENT", "kind": "DOWN", "since": h["last_seen"] + h["interval"],
                            "until": 0, "source": "silent"})
    for f in out:
        f["uplink"] = (homes.get(f["gateway"]) or {}).get("uplink", "")
    return [f for f in out if f["gateway"] in homes]


# ---------- incidents, kept ----------

def update(store, now=None, uplink=None):
    """Find the incidents now and keep them: the same incident keeps its id while it
    grows and recovers. uplink: only that one (after a report). Returns the open ones."""
    _table(store)
    now = int(now or time.time())
    s = settings(store)
    homes = _homes(store)
    sizes = Counter(h["uplink"] for h in homes.values() if h["uplink"])
    fs = [f for f in faults(store, homes, now, s) if uplink is None or f["uplink"] == uplink]
    found = correlate(fs, sizes, s, now)
    _declared(store, homes, now, uplink)
    rows = store.q("SELECT * FROM incidents WHERE declared = '' AND (state = 'open' OR end >= ? OR start >= ?)" +
                   (" AND uplink = ?" if uplink is not None else ""),
                   (now - LOOKBACK, now - LOOKBACK) + ((uplink,) if uplink is not None else ()))
    taken = set()
    for inc in found:
        match = None
        for r in rows:
            if r["id"] in taken or r["uplink"] != inc["uplink"]:
                continue
            members = {x["gateway"] for x in store.q("SELECT gateway FROM incident_homes WHERE incident = ?", (r["id"],))}
            if abs(r["start"] - inc["start"]) <= s["window"] or (members & set(inc["members"]) and
                                                                 (r["state"] == "open" or r["end"] >= inc["start"])):
                match = r
                break
        state = "open" if not inc["end"] else "closed"
        vals = (inc["kind"], inc["start"], inc["end"], inc["homes"], inc["homes_in_area"], inc["still"], inc["need"],
                inc["confidence"], json.dumps(inc["codes"]), inc["spread_s"], state, now)
        if match:
            taken.add(match["id"])
            iid = match["id"]
            store.x("UPDATE incidents SET kind = ?, start = ?, end = ?, homes = ?, homes_in_area = ?, still = ?, "
                    "need = ?, confidence = ?, codes = ?, spread_s = ?, state = ?, updated = ? WHERE id = ?",
                    vals + (iid,))
        else:
            iid = store.x("INSERT INTO incidents (kind, start, end, homes, homes_in_area, still, need, confidence, "
                          "codes, spread_s, state, updated, uplink, opened) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                          "?, ?)", vals + (inc["uplink"], now)).lastrowid
        store.x("DELETE FROM incident_homes WHERE incident = ?", (iid,))
        for g, h in inc["members"].items():
            store.x("INSERT INTO incident_homes VALUES (?, ?, ?, ?, ?, ?)",
                    (iid, g, h["since"], h["until"], h["kind"], h["code"]))
    # one that is no longer found (its faults aged out, or a home moved to another uplink): over
    for r in rows:
        if r["id"] not in taken and r["state"] == "open":
            store.x("UPDATE incidents SET state = 'closed', end = ?, still = 0, updated = ? WHERE id = ?",
                    (now, now, r["id"]))
    store.x("DELETE FROM incident_homes WHERE incident IN (SELECT id FROM incidents WHERE state = 'closed' AND "
            "end < ?)", (now - KEEP_DAYS * 86400,))
    store.x("DELETE FROM incidents WHERE state = 'closed' AND end < ?", (now - KEEP_DAYS * 86400,))
    store.x("DELETE FROM line_faults WHERE until != 0 AND until < ?", (now - KEEP_DAYS * 86400,))
    return incidents(store, open_only=True)


def _declared(store, homes, now, uplink=None):
    """A declared incident covers every home of its uplink, those that join it too."""
    for r in store.q("SELECT id, uplink FROM incidents WHERE declared != '' AND state = 'open'"):
        if uplink is not None and r["uplink"] != uplink:
            continue
        members = [g for g, h in homes.items() if h["uplink"] == r["uplink"]]
        have = {x["gateway"] for x in store.q("SELECT gateway FROM incident_homes WHERE incident = ?", (r["id"],))}
        for g in members:
            if g not in have:
                store.x("INSERT INTO incident_homes VALUES (?, ?, ?, 0, (SELECT kind FROM incidents WHERE id = ?), "
                        "'DECLARED')", (r["id"], g, now, r["id"]))
        store.x("UPDATE incidents SET homes = ?, homes_in_area = ?, still = ?, updated = ? WHERE id = ?",
                (len(members), len(members), len(members), now, r["id"]))


def declare(store, uplink, kind="DOWN", note="", now=None):
    """The operator declares an outage on an uplink (the NOC knows first): every home of it
    is tagged and holds its fixes until end(). Returns the incident."""
    _table(store)
    now = int(now or time.time())
    uplink = str(uplink or "").strip()[:64]
    if not uplink:
        raise ValueError("needs the uplink (area) id, as `group` set it or a gateway reports it")
    if kind not in KINDS:
        raise ValueError("kind is " + ", ".join(KINDS))
    if store.q("SELECT 1 FROM incidents WHERE declared != '' AND state = 'open' AND uplink = ?", (uplink,)):
        raise ValueError(f"an outage on {uplink} is declared already")
    homes = _homes(store)
    if not any(h["uplink"] == uplink for h in homes.values()):
        raise ValueError(f"no home is on {uplink}")
    iid = store.x("INSERT INTO incidents (uplink, kind, start, end, homes, homes_in_area, still, need, confidence, codes, "
                  "spread_s, state, opened, updated, declared) VALUES (?, ?, ?, 0, 0, 0, 0, 0, 'declared', '{}', 0, "
                  "'open', ?, ?, ?)", (uplink, kind, now, now, now, (note or "by the operator")[:200])).lastrowid
    _declared(store, homes, now, uplink)
    return _row(store, store.q("SELECT * FROM incidents WHERE id = ?", (iid,))[0])


def end(store, iid, now=None):
    """The operator ends a declared incident (one found ends when its homes recover)."""
    _table(store)
    now = int(now or time.time())
    r = store.q("SELECT * FROM incidents WHERE id = ? AND state = 'open'", (int(iid),))
    if not r:
        raise ValueError(f"no open area outage {iid}")
    if not r[0]["declared"]:
        raise ValueError(f"area outage {iid} was found, not declared: it ends when its homes recover")
    store.x("UPDATE incidents SET state = 'closed', end = ?, still = 0, updated = ? WHERE id = ?", (now, now, int(iid)))
    store.x("UPDATE incident_homes SET until = ? WHERE incident = ? AND until = 0", (now, int(iid)))
    return _row(store, store.q("SELECT * FROM incidents WHERE id = ?", (int(iid),))[0])


def _row(store, r, homes=None, with_homes=True):
    homes = homes if homes is not None else _homes(store)
    out = {"id": r["id"], "uplink": r["uplink"], "kind": r["kind"], "state": r["state"], "start": r["start"],
           "end": r["end"], "homes": r["homes"], "homes_in_area": r["homes_in_area"], "still_affected": r["still"],
           "needed": r["need"], "confidence": r["confidence"], "codes": json.loads(r["codes"] or "{}"),
           "spread_s": r["spread_s"], "opened": r["opened"], "updated": r["updated"],
           "declared": r["declared"] or ""}
    srcs = Counter(homes[g]["source"] for g in homes if homes[g]["uplink"] == r["uplink"])
    out["uplink_from"] = srcs.most_common(1)[0][0] if srcs else ""
    if with_homes:
        out["affected"] = [{"home": (homes.get(x["gateway"]) or {}).get("name", x["gateway"]), "gateway": x["gateway"],
                            "since": x["since"], "until": x["until"], "kind": x["kind"], "code": x["code"]}
                           for x in store.q("SELECT * FROM incident_homes WHERE incident = ? ORDER BY since",
                                            (r["id"],))]
    out["words"] = words(out)
    return out


def incidents(store, open_only=False, since=0, uplink=None, with_homes=True):
    """Kept incidents, newest first."""
    _table(store)
    sql = "SELECT * FROM incidents WHERE " + ("state = 'open'" if open_only else "(state = 'open' OR end >= ? OR start >= ?)")
    args = () if open_only else (since, since)
    if uplink:
        sql += " AND uplink = ?"
        args += (uplink,)
    homes = _homes(store)
    return [_row(store, r, homes, with_homes) for r in store.q(sql + " ORDER BY start DESC, id DESC", args)]


def for_gateway(store, gw):
    """The open incident a home is part of; None."""
    _table(store)
    r = store.q("SELECT i.* FROM incidents i JOIN incident_homes h ON h.incident = i.id "
                "WHERE i.state = 'open' AND h.gateway = ? ORDER BY i.start DESC LIMIT 1", (gw,))
    return _row(store, r[0]) if r else None


def nearby(store, gw):
    """An open incident on a home's uplink that the home itself is not part of; None."""
    h = _homes(store).get(gw)
    if not h or not h["uplink"]:
        return None
    r = store.q("SELECT * FROM incidents WHERE state = 'open' AND uplink = ? AND id NOT IN "
                "(SELECT incident FROM incident_homes WHERE gateway = ?) ORDER BY start DESC LIMIT 1",
                (h["uplink"], gw))
    return _row(store, r[0], with_homes=False) if r else None


def held(store, gw):
    """Fixes in this home are held now (it is part of an open incident)."""
    _table(store)
    return bool(store.q("SELECT 1 FROM incidents i JOIN incident_homes h ON h.incident = i.id "
                        "WHERE i.state = 'open' AND h.gateway = ? LIMIT 1", (gw,)))


def notice(store, gw, now=None):
    """What the report's answer tells the gateway (agd/area.c): the incident, or incident 0."""
    inc = for_gateway(store, gw)
    if not inc:
        return {"incident": 0}
    s = settings(store)
    return {"incident": inc["id"], "kind": inc["kind"], "since": inc["start"], "homes": inc["homes"],
            "of": inc["homes_in_area"], "confidence": inc["confidence"], "uplink": inc["uplink"][:64],
            "hold_s": int(s["hold_s"])}


# ---------- words ----------

def clock(ts):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts)) if ts else "-"


def words(o):
    st = (f"{o['still_affected']} still affected" if o["state"] == "open" else f"over at {clock(o['end'])}")
    if o.get("declared"):
        return (f"Area outage {o['id']}, declared ({o['declared']}): the {o['homes']} home{'' if o['homes'] == 1 else 's'} "
                f"on {o['uplink']} "
                f"{WORDS.get(o['kind'], o['kind'].lower())} from {clock(o['start'])}; " +
                ("until ended" if o["state"] == "open" else st))
    return (f"Area outage {o['id']}: {o['homes']} of {o['homes_in_area']} homes on {o['uplink']} "
            f"{WORDS.get(o['kind'], o['kind'].lower())} from {clock(o['start'])} (within "
            f"{max(1, o['spread_s'] // 60)} min; {o['confidence']} confidence): the ISP's network, not the homes; {st}")


def tag_brief(b, inc):
    """The help desk's brief for a home in an open incident: a known outage in its area."""
    b["area_outage"] = {k: inc[k] for k in ("id", "uplink", "kind", "start", "homes", "homes_in_area", "confidence",
                                            "still_affected")}
    b["area_outage"]["words"] = inc["words"]
    since = time.strftime("%H:%M", time.localtime(inc["start"]))
    own = (f" The home's own view: {b['verdict']}" if b.get("verdict") and
           not str(b["verdict"]).startswith("Known outage") else "")      # the gateway said so itself already
    b["fault"] = "LINE"
    b["confidence"] = "high"
    b["verdict"] = (f"Known outage in the area: {inc['homes']} of {inc['homes_in_area']} homes on {inc['uplink']} "
                    f"{WORDS.get(inc['kind'], 'affected')} since {since} (area outage {inc['id']}). "
                    f"The ISP's network, not this home.{own}")[:600]
    b["evidence"] = [{"source": "area", "time": inc["start"], "text": inc["words"]}] + list(b.get("evidence") or [])
    b["tell"] = (f"There is a known outage in your area since {since}; our network team is working on it. Your "
                 "equipment at home is fine: there is no need to restart or reset anything.")
    b["fix"] = {"possible": False, "held": True,
                "text": "Nothing to fix in the home. AIGW holds its own fixes there (smart queueing, steering, "
                        "channel changes) until the area is back, and cortex holds proposals for it."}
    b["visit"] = "No"
    b["visit_reason"] = (f"No: a known outage in the area (area outage {inc['id']}, {inc['homes']} homes); a visit to "
                         "this home would not help.")
    return b


def note_nearby(b, inc):
    b["area_note"] = (f"An outage affects {inc['homes']} of {inc['homes_in_area']} homes on {inc['uplink']} since "
                      f"{time.strftime('%H:%M', time.localtime(inc['start']))} (area outage {inc['id']}); this home "
                      "hasn't shown it.")
    return b


def text(rows):
    if not rows:
        return "No area outages."
    out = []
    for o in rows:
        out.append(o["words"])
        for h in o.get("affected") or []:
            out.append(f"    {h['home']}: {h['code']} from {clock(h['since'])}" +
                       (f" to {clock(h['until'])}" if h["until"] else ", still affected"))
    return "\n".join(out)
