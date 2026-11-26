# SPDX-License-Identifier: Apache-2.0
"""Area fault prediction: is it this home's line, or the shared plant? (docs/area-prediction.md)

    python3 -m aigw_cortex predict [--json]                 the plant faults coming, ranked; lines on their own
    python3 -m aigw_cortex predict settings [--min-homes N --share F --align D --alpha P]
    python3 -m aigw_cortex predict simulate [--seeds N --homes N --cabinets N] [--json]   precision, recall, lead
    python3 -m aigw_cortex predict backtest [--json]        on this cortex's own history: how early, how often right
    MCP tool: plant_predictions; the dashboard's "Faults coming"; GET /api/predictions

Each gateway sends its line health vector with its report (agd/line.c): for each
of its line's numbers (sync rate, noise margin, optical level, FEC and CRC
errors, drops, next-hop delay and loss, the speed test) the trend's state
(OK, Stepped, Drifting, Fault), the day it began, when it would be a fault, and
how sure it is. No devices, no addresses, no traffic.

The homes are grouped by what serves them, the area outages' key (outage.py): the
operator's group, else the id the ACS wrote on the gateway, else its WAN's next
hop. A "/" in it is a hierarchy: "olt-3/cab-12" is cabinet 12 under OLT 3.

The rules (correlate(), pure, unit-tested), node by node, the most specific first:
  - a home degrades on a metric when its trend is Stepped, Drifting or Fault with
    medium confidence or more (Fault always)
  - the homes of a node degrading on the same metric, beginning within `align`
    days of each other (their median), at least max(min_homes, share x the node's
    homes)
  - chance: how often homes elsewhere in the fleet degrade on that metric (a base
    rate), and how likely it is that this many of the node's homes would, by
    chance, within a window that narrow: a binomial tail, times the metrics tried.
    At most `alpha` (0.001): the shared plant
  - a node whose children (two or more cabinets) degrade together, the same metric
    and the same days: the parent (the OLT, the feeder) instead
  - the rest degrade on their own: this home's line
  - first calls: when the second home of the cluster would reach the fault level
    (one home is a call; two is the start of many); 0 when one is there already
  - confidence high with a tail of 1e-6 or less, 5 homes or more and the homes'
    own trends sure (60 on average); else medium
  - ranked by homes x urgency (1 for calls within a day, falling to 0.1 at 14 days;
    0.3 when not known) x confidence (1 high, 0.7 medium)
Then: one TMF621 ticket for the plant (tickets.py, kind "plant"), and none for the
homes it explains: their own line tickets (proactive care's, and cortex's "line"
ones) are suppressed while it is open, and the plant's ticket says how many.
"""
import json
import math
import time
from collections import Counter, defaultdict

from . import linehealth

SCHEMA = """
CREATE TABLE IF NOT EXISTS line_vectors (
    gateway TEXT, day INTEGER, tech TEXT, status TEXT, vec TEXT, updated INTEGER, PRIMARY KEY (gateway, day));
CREATE TABLE IF NOT EXISTS plant_predictions (
    id INTEGER PRIMARY KEY AUTOINCREMENT, node TEXT, metric TEXT, signature TEXT, state TEXT, opened INTEGER,
    updated INTEGER, closed INTEGER DEFAULT 0, homes INTEGER, homes_in_node INTEGER, onset INTEGER, calls_in INTEGER,
    confidence TEXT, p REAL, score REAL, members TEXT, words TEXT, first_fault INTEGER DEFAULT 0,
    outcome TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS line_predictions (
    gateway TEXT, metric TEXT, onset INTEGER, state TEXT, opened INTEGER, updated INTEGER, closed INTEGER DEFAULT 0,
    trend TEXT, days_left INTEGER, conf INTEGER, words TEXT, plant INTEGER DEFAULT 0, held INTEGER DEFAULT 0,
    PRIMARY KEY (gateway, metric, onset));
CREATE TABLE IF NOT EXISTS predict_settings (k TEXT PRIMARY KEY, v REAL);
"""
DEFAULTS = {
    "min_homes": 3,         # never fewer homes than this make a shared plant
    "share": 0.15,          # ... nor fewer than this share of the node's homes
    "align": 3,             # days: their trends began this close to their median
    "alpha": 0.001,         # chance alone this unlikely, or less
    "min_conf": 40,         # a home's own trend this sure, or more (a Fault always counts)
    "fresh_days": 2,        # a vector older than this is not looked at
    "base_rate": 0.05,      # a metric's chance of degrading, with too few homes elsewhere to count
}
LIMITS = {"min_homes": (2, 1000), "share": (0.0, 1.0), "align": (1, 7), "alpha": (1e-9, 0.05), "min_conf": (0, 100),
          "fresh_days": (1, 7), "base_rate": (0.001, 0.5)}
KEEP_DAYS = 45
HOLD_S = 2 * 86400          # the gateway shows cortex's word this long unless told again


def _table(store):
    if not getattr(store, "_predict_ok", False):
        with store.lock:
            store.db.executescript(SCHEMA)
        store._predict_ok = True


def settings(store):
    _table(store)
    s = dict(DEFAULTS)
    for r in store.q("SELECT k, v FROM predict_settings"):
        if r["k"] in s:
            s[r["k"]] = type(DEFAULTS[r["k"]])(r["v"])
    return s


def set_settings(store, **kw):
    _table(store)
    for k, v in kw.items():
        if v is None:
            continue
        lo, hi = LIMITS[k]
        if not lo <= float(v) <= hi:
            raise ValueError(f"{k} must be from {lo} to {hi}")
        store.x("INSERT OR REPLACE INTO predict_settings VALUES (?, ?)", (k, float(v)))
    return settings(store)


# ---------- the rules ----------

def binom_tail(n, k, p):
    """P(X >= k) for X ~ Binomial(n, p)."""
    if k <= 0:
        return 1.0
    if k > n:
        return 0.0
    p = min(max(p, 1e-12), 1 - 1e-12)
    tot = 0.0
    for i in range(k, n + 1):
        tot += math.exp(math.lgamma(n + 1) - math.lgamma(i + 1) - math.lgamma(n - i + 1) +
                        i * math.log(p) + (n - i) * math.log(1 - p))
    return min(1.0, tot)


def degraded(vec, min_conf):
    """A vector's degrading metrics: {metric: its trend}."""
    out = {}
    for e in (vec or {}).get("metrics") or []:
        if not isinstance(e, dict) or e.get("metric") not in linehealth.METRICS:
            continue
        st = e.get("state")
        if st == "Fault" or (st in ("Stepped", "Drifting") and int(e.get("conf") or 0) >= min_conf):
            out[e["metric"]] = e
    return out


def early(vec):
    """Early signs: metrics whose last 7 days slide steadily worse (Kendall's tau 0.6 or more, by at least the
    change worth saying), though the home can't prove it alone yet: {metric: its trend, state "Early"}."""
    out = {}
    day = int((vec or {}).get("day") or 0)
    for e in (vec or {}).get("metrics") or []:
        if not isinstance(e, dict) or e.get("metric") not in linehealth.METRICS:
            continue
        lower, _, _, fl_abs, fl_rel = linehealth.DEF[e["metric"]]
        s7, t7, base = e.get("slope7"), e.get("tau7"), e.get("base")
        if s7 is None or t7 is None or base is None or e.get("state") in ("", "Learning", None):
            continue
        worse = -s7 if lower else s7
        if t7 >= 0.6 and worse > 0 and worse * 6 >= max(fl_abs, fl_rel * abs(base)):
            out[e["metric"]] = dict(e, state="Early", onset=day - 4, days_left=-1, conf=int(round(50 * t7)))
    return out


def present(vec):
    return {e.get("metric") for e in (vec or {}).get("metrics") or [] if isinstance(e, dict) and
            e.get("state") not in ("", "Learning", None)}


def ancestors(node):
    """'a/b/c' -> ['a/b/c', 'a/b', 'a'] (the most specific first)."""
    parts = [p for p in str(node or "").split("/") if p]
    return ["/".join(parts[:i]) for i in range(len(parts), 0, -1)]


def _pooled_left(metric, t, ms):
    """A home's days to the fault: its own when it knows, else at the rate the plant's homes slide together."""
    if int(t.get("days_left", -1)) >= 0:
        return int(t["days_left"])
    lower = linehealth.DEF[metric][0]
    sg = -1.0 if lower else 1.0
    rates = [sg * (x.get("slope") if x.get("state") == "Drifting" else x.get("slope7") or 0.0) for x in ms.values()]
    rates = [r for r in rates if r > 0]
    if not rates or t.get("fault") is None or t.get("level") is None:
        return -1
    gap = sg * t["fault"] - sg * t["level"]
    return 0 if gap <= 0 else int(math.ceil(gap / linehealth.median(rates)))


def _calls_in(trends):
    left = sorted(t["days_left"] for t in trends if int(t.get("days_left", -1)) >= 0)
    if not left:
        return -1
    if left[0] == 0:
        return 0
    return left[1] if len(left) > 1 else left[0]


def urgency(calls_in, horizon=14):
    if calls_in < 0:
        return 0.3
    if calls_in <= 1:
        return 1.0
    return max(0.1, 1.0 - calls_in / horizon)


def _cluster(cands, align):
    """The largest set of homes whose onsets lie within align days of their median."""
    if not cands:
        return []
    best = []
    for g0, t0 in cands.items():
        c = [g for g, t in cands.items() if abs(t["onset"] - t0["onset"]) <= align]
        med = linehealth.median([cands[g]["onset"] for g in c])
        c = [g for g, t in cands.items() if abs(t["onset"] - med) <= align]
        if len(c) > len(best) or (len(c) == len(best) and sorted(c) < sorted(best)):
            best = c
    return sorted(best)


def correlate(vecs, nodes, s=None, known=None):
    """Plant predictions and lines on their own, from the homes' fresh vectors.

    vecs:  gateway -> line health vector
    nodes: gateway -> its node ("olt-3/cab-12"; "" none)
    Returns {"plants": [...], "lines": [...]}; each plant: node, metric, signature,
    homes, homes_in_node, members {gw: trend}, onset, calls_in, p, confidence, score.
    known: the plants open already, [{node, metric, onset, members}]: their homes stay while they show it, and a
    newcomer must have begun within lign days of the plant's own onset (a ramp's start leaves the 14 days)."""
    kn = {(k["node"], k["metric"]): k for k in known or []}
    s = dict(DEFAULTS, **(s or {}))
    deg = {g: degraded(v, s["min_conf"]) for g, v in vecs.items()}
    # what the cabinet's homes show together: proven trends, and early signs no home could prove alone
    sign = {g: dict(early(v), **deg[g]) for g, v in vecs.items()}
    pres = {g: present(v) for g, v in vecs.items()}
    tree = defaultdict(set)
    for g in vecs:
        for a in ancestors(nodes.get(g, "")):
            tree[a].add(g)
    total_d, total_p = Counter(), Counter()
    for g in vecs:
        for m in pres[g]:
            total_p[m] += 1
        for m in sign[g]:
            total_d[m] += 1
    claimed = {}          # gw -> plant index
    plants = []
    window = linehealth.PARAMS["window"]
    for node in sorted(tree, key=lambda n: (-n.count("/"), n)):
        homes = tree[node]
        size = len(homes)
        need = max(int(s["min_homes"]), int(math.ceil(float(s["share"]) * size - 1e-9)))
        # children that degrade together: their parent is the shared plant
        kids = [i for i, p in enumerate(plants) if p and p["node"] != node and p["node"].startswith(node + "/")]
        by_m = defaultdict(list)
        for i in kids:
            by_m[plants[i]["metric"]].append(i)
        for m, idx in by_m.items():
            if len({plants[i]["node"] for i in idx}) < 2:
                continue
            med = linehealth.median([plants[i]["onset"] for i in idx])
            near = [i for i in idx if abs(plants[i]["onset"] - med) <= s["align"]]
            if len({plants[i]["node"] for i in near}) < 2:
                continue
            members, pv = {}, 1.0
            for i in near:
                members.update(plants[i]["members"])
                pv = min(pv, plants[i]["p"])        # each child was unlikely by chance already
                plants[i] = None
            plants.append(_plant(node, m, members, size, pv, merged=True))
            for g in members:
                claimed[g] = len(plants) - 1
        # the node's own homes not claimed yet; a cabinet below with a plant of its own explains its homes' other
        # signs too (they don't join a search higher up)
        busy = {p["node"] for p in plants if p and p["node"] != node and p["node"].startswith(node + "/")}
        free = [g for g in homes if g not in claimed and
                not any(a in busy for a in ancestors(nodes.get(g, "")))]
        best = None
        tried = 0
        for m in linehealth.METRICS:
            cands = {g: sign[g][m] for g in free if m in sign[g]}
            if not any(m in deg[g] for g in cands):
                continue                    # one home at least has proven it
            if len(cands) < need:
                continue
            tried += 1
            k = kn.get((node, m)) or next((x for key, x in kn.items() if key[1] == m and
                                           key[0].startswith(node + "/")), None)
            if k:
                c = sorted(g for g, t in cands.items() if g in k["members"] or
                           abs(int(t.get("onset") or 0) - int(k["onset"])) <= s["align"])
            else:
                c = _cluster({g: dict(t, onset=int(t.get("onset") or 0)) for g, t in cands.items()}, s["align"])
            if len(c) < need:
                continue
            n_m = sum(1 for g in homes if m in pres[g]) or size
            out_p = total_p[m] - sum(1 for g in homes if m in pres[g])
            out_d = total_d[m] - sum(1 for g in homes if m in sign[g])
            rate = (out_d + 1) / (out_p + 2) if out_p >= 10 else float(s["base_rate"])
            rate = max(rate, 0.005)
            p0 = rate * min(1.0, (2 * s["align"] + 1) / window)
            pv = binom_tail(n_m, len(c), p0)
            if best is None or pv < best[0]:
                best = (pv, m, c, k)
        if best is None:
            continue
        pv = min(1.0, best[0] * max(1, tried))
        if pv > s["alpha"]:
            continue
        # nearly all in one cabinet under this node: that cabinet's plant, not the node's
        where = node
        kids_of = Counter(next((a for a in ancestors(nodes.get(g, "")) if a.startswith(node + "/") and
                                a.count("/") == node.count("/") + 1), node) for g in best[2])
        top, n_top = kids_of.most_common(1)[0]
        if top != node and n_top * 5 >= 4 * len(best[2]):
            where = top
        pl = _plant(where, best[1], {g: sign[g][best[1]] for g in best[2]}, len(tree[where]), pv)
        if best[3]:
            pl["onset"] = int(best[3]["onset"])
        plants.append(pl)
        for g in pl["members"]:
            claimed[g] = len(plants) - 1
    plants = [p for p in plants if p]
    for p in plants:
        _finish(p, deg)
    plants.sort(key=lambda p: (-p["score"], p["node"]))
    lines = []
    owner = {g: p["node"] for p in plants for g in p["members"]}
    for g, d in deg.items():
        if not d or g in owner:
            continue
        m, t = min(d.items(), key=lambda kv: (-linehealth.RANK[kv[1]["state"]],
                                              kv[1]["days_left"] if kv[1]["days_left"] >= 0 else 999,
                                              -int(kv[1].get("conf") or 0)))
        # others on its node show the same signs: wait (up to the plant's finding), unless it is a fault already
        peers = [h for h in tree.get(nodes.get(g, ""), ()) if h != g and h not in owner and m in sign[h]]
        lines.append({"gateway": g, "node": nodes.get(g, ""), "metric": m, "trend": t, "metrics": sorted(d),
                      "held": bool(peers) and t["state"] != "Fault", "peers": len(peers)})
    small = sorted(n for n, h in tree.items() if len(h) < int(s["min_homes"]) and "/" not in n)
    return {"plants": plants, "lines": lines, "too_small": small}


def _plant(node, metric, members, size, pv, merged=False):
    return {"node": node, "metric": metric, "members": dict(members), "homes_in_node": size, "p": pv,
            "merged": merged, "onset": int(linehealth.median([int(t.get("onset") or 0) for t in members.values()]))}


def _finish(p, deg):
    ms = p["members"]
    p["homes"] = len(ms)
    p["onset"] = int(linehealth.median([int(t.get("onset") or 0) for t in ms.values()]))
    sig = Counter(m for g in ms for m in deg[g])
    p["signature"] = sorted(m for m, n in sig.items() if n * 2 >= len(ms))
    p["calls_in"] = _calls_in([dict(t, days_left=_pooled_left(p["metric"], t, ms)) for t in ms.values()])
    conf = sum(int(t.get("conf") or 0) for t in ms.values()) / max(1, len(ms))
    p["confidence"] = "high" if p["p"] <= 1e-6 and len(ms) >= 5 and conf >= 60 else "medium"
    p["score"] = round(len(ms) * urgency(p["calls_in"]) * (1.0 if p["confidence"] == "high" else 0.7), 2)


# ---------- words ----------

def day_text(day):
    return time.strftime("%Y-%m-%d", time.gmtime(int(day) * 86400)) if day else "-"


def plant_words(p):
    i = linehealth.METRICS.index(p["metric"]) if p["metric"] in linehealth.METRICS else 0
    what = linehealth.WORDS[i]
    calls = ("first calls likely now (a home is at the fault level already)" if p["calls_in"] == 0 else
             f"first calls likely in about {p['calls_in']} day{'' if p['calls_in'] == 1 else 's'}"
             if p["calls_in"] > 0 else "when it becomes a fault isn't known (it stepped worse, it isn't sliding)")
    also = [linehealth.WORDS[linehealth.METRICS.index(m)] for m in p.get("signature") or [] if m != p["metric"]]
    return (f"The plant serving {p['homes']} of {p['homes_in_node']} homes on {p['node']} is degrading: "
            f"{what} worse in all of them since about {day_text(p['onset'])}" +
            (f" (with {', '.join(also)})" if also else "") +
            f"; {calls}; confidence {p['confidence']}. One ticket for the plant, not {p['homes']}.")


def line_words(gw_name, x):
    t = x["trend"]
    return f"{gw_name}'s own line: " + linehealth.words(x["metric"], t) + \
        (" Other homes on " + x["node"] + " are fine: not the shared plant." if x.get("node") else "")


# ---------- what the homes report ----------

def on_report(store, gw, rep, now=None):
    """A report: keep its line health vector (one a day). 1 when it changed what is known."""
    _table(store)
    now = int(now or time.time())
    v = rep.get("line")
    if not isinstance(v, dict) or not isinstance(v.get("metrics"), list):
        return 0
    try:
        day = int(v.get("day") or now // 86400)
    except (TypeError, ValueError):
        return 0
    vec = {"v": 1, "tech": str(v.get("tech") or "")[:12], "day": day, "status": str(v.get("status") or "")[:12],
           "worst": str(v.get("worst") or "")[:12], "days_left": _int(v.get("days_left"), -1),
           "conf": _int(v.get("conf"), 0), "metrics": []}
    for e in v["metrics"][:len(linehealth.METRICS)]:
        if not isinstance(e, dict) or e.get("metric") not in linehealth.METRICS:
            continue
        vec["metrics"].append({"metric": e["metric"], "state": str(e.get("state") or "")[:12],
                               "days": _int(e.get("days"), 0), "level": _num(e.get("level")),
                               "base": _num(e.get("base")), "slope": _num(e.get("slope")),
                               "fault": _num(e.get("fault")), "onset": _int(e.get("onset"), 0),
                               "days_left": _int(e.get("days_left"), -1), "conf": _int(e.get("conf"), 0)})
    old = store.q("SELECT vec FROM line_vectors WHERE gateway = ? AND day = ?", (gw, day))
    js = json.dumps(vec, sort_keys=True)
    store.x("INSERT OR REPLACE INTO line_vectors VALUES (?, ?, ?, ?, ?, ?)", (gw, day, vec["tech"], vec["status"], js,
                                                                              now))
    return 0 if old and old[0]["vec"] == js else 1


def _int(v, d):
    try:
        return int(v)
    except (TypeError, ValueError):
        return d


def _num(v):
    try:
        f = float(v)
        return f if math.isfinite(f) else None
    except (TypeError, ValueError):
        return None


def fresh(store, now, s):
    day = int(now) // 86400
    out = {}
    for r in store.q("SELECT gateway, day, vec FROM line_vectors WHERE day >= ? ORDER BY day", (day - s["fresh_days"],)):
        out[r["gateway"]] = json.loads(r["vec"])
    return out


def _nodes(store):
    from . import outage
    return {g: h["uplink"] for g, h in outage._homes(store).items()}


# ---------- predictions, kept ----------

def update(store, now=None):
    """Correlate the fresh vectors and keep the predictions: a plant keeps its number
    while it lasts. Returns the open ones."""
    _table(store)
    now = int(now or time.time())
    s = settings(store)
    nodes = _nodes(store)
    vecs = {g: v for g, v in fresh(store, now, s).items() if g in nodes}
    rows = store.q("SELECT * FROM plant_predictions WHERE state = 'open'")
    known = [{"node": r["node"], "metric": r["metric"], "onset": r["onset"],
              "members": set(json.loads(r["members"] or "{}"))} for r in rows]
    found = correlate(vecs, nodes, s, known)
    taken = set()
    for p in found["plants"]:
        match = None
        for r in rows:
            if r["id"] in taken:
                continue
            members = set(json.loads(r["members"] or "{}"))
            same = r["node"] == p["node"] or r["node"].startswith(p["node"] + "/") or \
                p["node"].startswith(r["node"] + "/")
            if same and r["metric"] == p["metric"] and members & set(p["members"]):
                match = r
                break
        words = plant_words(p)
        mem = json.dumps({g: {k: t.get(k) for k in ("state", "onset", "days_left", "conf", "level", "base", "slope")}
                          for g, t in p["members"].items()}, sort_keys=True)
        vals = (p["node"], p["metric"], ",".join(p["signature"]), p["homes"], p["homes_in_node"], p["onset"],
                p["calls_in"], p["confidence"], p["p"], p["score"], mem, words, now)
        if match:
            taken.add(match["id"])
            store.x("UPDATE plant_predictions SET node = ?, metric = ?, signature = ?, homes = ?, homes_in_node = ?, "
                    "onset = ?, calls_in = ?, confidence = ?, p = ?, score = ?, members = ?, words = ?, updated = ? "
                    "WHERE id = ?", vals + (match["id"],))
            pid = match["id"]
        else:
            pid = store.x("INSERT INTO plant_predictions (node, metric, signature, homes, homes_in_node, onset, "
                          "calls_in, confidence, p, score, members, words, updated, state, opened) VALUES "
                          "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'open', ?)", vals + (now,)).lastrowid
        p["id"] = pid
        _first_fault(store, pid, p, now)
    for r in rows:
        if r["id"] not in taken:
            store.x("UPDATE plant_predictions SET state = 'closed', closed = ?, updated = ?, outcome = CASE WHEN "
                    "first_fault THEN 'failed' ELSE 'recovered' END WHERE id = ?", (now, now, r["id"]))
    # the lines on their own
    seen = set()
    owner = {g: p["id"] for p in found["plants"] for g in p["members"]}
    for x in found["lines"]:
        t = x["trend"]
        key = (x["gateway"], x["metric"], int(t.get("onset") or 0))
        seen.add(key)
        g = store.gateway(x["gateway"])
        store.x("INSERT OR IGNORE INTO line_predictions (gateway, metric, onset, state, opened, updated) VALUES "
                "(?, ?, ?, 'open', ?, ?)", key + (now, now))
        store.x("UPDATE line_predictions SET state = 'open', closed = 0, updated = ?, trend = ?, days_left = ?, "
                "conf = ?, words = ?, plant = 0, held = ? WHERE gateway = ? AND metric = ? AND onset = ?",
                (now, t.get("state"), int(t.get("days_left", -1)), int(t.get("conf") or 0),
                 line_words(g["name"] if g else x["gateway"], x), int(x["held"])) + key)
    for r in store.q("SELECT gateway, metric, onset FROM line_predictions WHERE state = 'open'"):
        key = (r["gateway"], r["metric"], r["onset"])
        if key in seen:
            continue
        # explained by a plant now, or fine again
        store.x("UPDATE line_predictions SET state = 'closed', closed = ?, updated = ?, plant = ? WHERE gateway = ? AND "
                "metric = ? AND onset = ?", (now, now, owner.get(r["gateway"], 0)) + key)
    cut = now - KEEP_DAYS * 86400
    store.x("DELETE FROM line_vectors WHERE day < ?", (cut // 86400,))
    store.x("DELETE FROM plant_predictions WHERE state = 'closed' AND closed < ?", (cut,))
    store.x("DELETE FROM line_predictions WHERE state = 'closed' AND closed < ?", (cut,))
    return predictions(store, open_only=True)


def _first_fault(store, pid, p, now):
    """The first sign that it failed: a member at the fault level, a line problem, an area outage."""
    r = store.q("SELECT opened, first_fault FROM plant_predictions WHERE id = ?", (pid,))[0]
    if r["first_fault"]:
        return
    at = 0
    if any(t.get("state") == "Fault" for t in p["members"].values()):
        at = now
    ids = list(p["members"])
    q = "SELECT MIN(since) t FROM care_events WHERE kind = 'problem' AND area = 'Internet' AND since >= ? AND gateway IN (" + \
        ",".join("?" * len(ids)) + ")"
    e = store.q(q, [r["opened"]] + ids) if ids else []
    if e and e[0]["t"]:
        at = min(at or e[0]["t"], e[0]["t"])
    if at:
        store.x("UPDATE plant_predictions SET first_fault = ? WHERE id = ?", (at, pid))


def _row(store, r, names=None):
    names = names if names is not None else {g["id"]: g["name"] for g in store.gateways()}
    mem = json.loads(r["members"] or "{}")
    lead = (r["first_fault"] - r["opened"]) / 86400 if r["first_fault"] else None
    return {"id": r["id"], "node": r["node"], "metric": r["metric"],
            "signature": [m for m in (r["signature"] or "").split(",") if m], "state": r["state"],
            "opened": r["opened"], "updated": r["updated"], "closed": r["closed"], "homes": r["homes"],
            "homes_in_node": r["homes_in_node"], "onset": r["onset"], "onset_day": day_text(r["onset"]),
            "calls_in": r["calls_in"], "confidence": r["confidence"], "p": r["p"], "score": r["score"],
            "words": r["words"], "first_fault": r["first_fault"], "outcome": r["outcome"],
            "lead_days": round(lead, 1) if lead is not None else None,
            "members": [dict(t, gateway=g, home=names.get(g, g)) for g, t in sorted(mem.items())]}


def predictions(store, open_only=False, since=0):
    _table(store)
    sql = "SELECT * FROM plant_predictions WHERE " + ("state = 'open'" if open_only else "(state = 'open' OR closed >= ?)")
    names = {g["id"]: g["name"] for g in store.gateways()}
    rows = store.q(sql + " ORDER BY state = 'open' DESC, score DESC, id DESC", () if open_only else (since,))
    return [_row(store, r, names) for r in rows]


def lines(store, open_only=True):
    _table(store)
    names = {g["id"]: g["name"] for g in store.gateways()}
    rows = store.q("SELECT * FROM line_predictions" + (" WHERE state = 'open'" if open_only else "") +
                   " ORDER BY days_left < 0, days_left, conf DESC")
    return [dict(r, home=names.get(r["gateway"], r["gateway"])) for r in rows]


def ticketable(x):
    """A line on its own worth an ISP ticket: a fault, a drift heading to one, a sure step; not while other homes
    of its node show the same (it may be the plant: one ticket then, not one each)."""
    if x.get("held"):
        return False
    return x["trend"] == "Fault" or (x["trend"] == "Drifting" and 0 <= int(x["days_left"]) <= 14) or \
        (x["trend"] == "Stepped" and int(x["conf"]) >= 70)


def for_gateway(store, gw):
    """The open plant prediction a home is part of; None."""
    for p in predictions(store, open_only=True):
        if any(m["gateway"] == gw for m in p["members"]):
            return p
    return None


def explained(store):
    """gateway -> open plant id, for every home a plant explains."""
    return {m["gateway"]: p["id"] for p in predictions(store, open_only=True) for m in p["members"]}


def notice(store, gw):
    """What the report's answer tells the gateway (agd/line.c): the plant, or plant 0."""
    _table(store)
    p = for_gateway(store, gw)
    if not p:
        return {"plant": 0}
    from . import tickets
    return {"plant": p["id"], "node": p["node"][:63], "kind": "plant", "homes": p["homes"], "of": p["homes_in_node"],
            "metric": p["metric"], "calls_in": p["calls_in"], "confidence": p["confidence"],
            "ticket": tickets.external_of(store, f"plant:{p['id']}")[:79], "words": p["words"][:290],
            "hold_s": HOLD_S}


# ---------- back-test: on this cortex's own history ----------

def backtest(store, now=None):
    """Replay the kept vectors day by day, as correlate() would have seen them, and compare
    each plant it would have found with the first line problem its homes had after (proactive
    care's, from the reports): how many days early it would have warned."""
    _table(store)
    now = int(now or time.time())
    s = settings(store)
    nodes = _nodes(store)
    names = {g["id"]: g["name"] for g in store.gateways()}
    rows = store.q("SELECT gateway, day, vec FROM line_vectors ORDER BY day")
    if not rows:
        return {"days": 0, "plants": [], "note": "no line health vectors kept yet (gateways send them from 0.54)"}
    by_day = defaultdict(dict)
    for r in rows:
        by_day[r["day"]][r["gateway"]] = json.loads(r["vec"])
    first, last = min(by_day), max(by_day)
    latest, known, found = {}, [], {}
    for day in range(first, last + 1):
        for g, v in by_day.get(day, {}).items():
            latest[g] = v
        vecs = {g: v for g, v in latest.items() if g in nodes and int(v.get("day") or 0) >= day - s["fresh_days"]}
        out = correlate(vecs, nodes, s, known)
        known = [{"node": p["node"], "metric": p["metric"], "onset": p["onset"], "members": set(p["members"])}
                 for p in out["plants"]]
        for p in out["plants"]:
            f = found.setdefault((p["node"], p["metric"]), {"node": p["node"], "metric": p["metric"], "day": day,
                                                            "members": set()})
            f["members"] |= set(p["members"])
    plants = []
    for f in found.values():
        ids = sorted(f["members"])
        e = store.q("SELECT MIN(since) t FROM care_events WHERE kind = 'problem' AND area = 'Internet' AND since >= ? "
                    "AND gateway IN (" + ",".join("?" * len(ids)) + ")", [f["day"] * 86400] + ids)
        t = e[0]["t"] if e and e[0]["t"] else 0
        plants.append({"node": f["node"], "metric": f["metric"], "found": day_text(f["day"]), "homes": len(ids),
                       "first_problem": time.strftime("%Y-%m-%d %H:%M", time.gmtime(t)) if t else None,
                       "lead_days": round(t / 86400 - f["day"], 1) if t else None,
                       "home_names": [names.get(g, g) for g in ids][:20]})
    leads = [p["lead_days"] for p in plants if p["lead_days"] is not None]
    return {"days": last - first + 1, "from": day_text(first), "to": day_text(last), "plants": plants,
            "followed_by_a_problem": len(leads), "median_lead_days": linehealth.median(leads) if leads else None}


def backtest_text(b):
    if not b["days"]:
        return b["note"]
    out = [f"Back-test over {b['days']} days ({b['from']} to {b['to']}): {len(b['plants'])} shared plant fault"
           f"{'' if len(b['plants']) == 1 else 's'} would have been found; {b['followed_by_a_problem']} followed by a "
           "line problem in its homes" + (f", a median {b['median_lead_days']} days after the warning" if
                                           b["median_lead_days"] is not None else "")]
    for p in b["plants"]:
        out.append(f"  {p['node']} ({p['metric']}, {p['homes']} homes): found {p['found']}; " +
                   (f"first problem {p['first_problem']}, {p['lead_days']} days later" if p["first_problem"] else
                    "no problem in its homes since (fixed in time, or not a fault)"))
    return "\n".join(out)


# ---------- the operator's view ----------

def summary(store, now=None):
    now = int(now or time.time())
    open_ = predictions(store, open_only=True)
    past = [p for p in predictions(store, since=now - 30 * 86400) if p["state"] != "open"]
    lead = [p["lead_days"] for p in predictions(store, since=now - 30 * 86400) if p["lead_days"] is not None]
    avoided = sum(p["homes"] for p in past if p["outcome"] == "recovered")
    return {"open": open_, "lines": lines(store), "settings": settings(store),
            "last_30_days": {"plants": len(past) + len(open_), "failed": sum(1 for p in past if p["outcome"] == "failed"),
                             "recovered": sum(1 for p in past if p["outcome"] == "recovered"),
                             "median_lead_days": linehealth.median(lead) if lead else None,
                             "calls_likely_avoided": avoided}}


def text(s):
    out = []
    if not s["open"]:
        out.append("No shared plant degrading.")
    for i, p in enumerate(s["open"], 1):
        out.append(f"{i}. [{p['score']}] plant {p['id']}: {p['words']}")
        out.append("     homes: " + ", ".join(f"{m['home']} ({m['state']}" +
                                            (f", fault in {m['days_left']} d" if (m.get('days_left') or -1) >= 0 else "") +
                                            ")" for m in p["members"][:20]) +
                   (" ..." if len(p["members"]) > 20 else ""))
    ls = s["lines"]
    if ls:
        out.append(f"Lines degrading on their own ({len(ls)}): this home's line, not the shared plant")
        for x in ls[:20]:
            out.append(f"   {x['words']}")
    l3 = s["last_30_days"]
    if l3["plants"]:
        out.append(f"Last 30 days: {l3['plants']} plant fault{'' if l3['plants'] == 1 else 's'} predicted, "
                   f"{l3['failed']} failed after the warning, {l3['recovered']} recovered before failing (calls "
                   f"likely avoided: {l3['calls_likely_avoided']})" +
                   (f"; warned a median {l3['median_lead_days']} days before the first fault" if
                    l3["median_lead_days"] is not None else ""))
    return "\n".join(out)


def fleet_lines(r):
    p = r.get("predictions") or {}
    if not p.get("open") and not p.get("lines"):
        return []
    out = [f"  faults coming {len(p['open'])} shared plant{'' if len(p['open']) == 1 else 's'}, "
           f"{len(p['lines'])} line{'' if len(p['lines']) == 1 else 's'} on their own (v0.54)"]
    for x in p["open"][:5]:
        out.append(f"               {x['words']}")
    l3 = p.get("last_30_days") or {}
    if l3.get("plants"):
        out.append(f"               last 30 days: {l3['plants']} predicted, {l3['recovered']} fixed before failing "
                   f"(calls likely avoided: {l3['calls_likely_avoided']} homes), {l3['failed']} failed after the "
                   "warning")
    return out
