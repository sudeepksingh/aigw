# SPDX-License-Identifier: Apache-2.0
"""Fleet learning, the reference aggregator: every home teaches the next.

An ISP runs this (or its own, by the same rules) to add up the gateways'
summaries and publish the fleet's priors back to them.

    summary ── POST /v1/fleet/summary from a home's bridge, only when the home
       │       shares (fleet.share 1 on the gateway; the default is 0):
       │       {"v": 1, "line": "fibre",
       │        "outcomes": [[code, action, bucket, tried, helped, worse, undone], ...],
       │        "normals":  [[metric, hour band, normal, typical deviation], ...]}
       │       anything else is refused: bucket fields come from a fixed vocabulary,
       │       so no MAC, IP, name or SNI can travel in one
       ▼
    merge ──── each home's latest summary replaces its earlier one (counts are its
       │       totals, noised the same way each time); summaries older than
       │       max_age are dropped; a home counts at most cap per bucket (one home
       │       can't outweigh many); outcomes add up over homes at five levels,
       │       from every context dimension to none (fl_key, ag/fleet.h)
       ▼
    k ──────── a bucket, or a normal, that fewer than k homes add up to is left
       │       out (the gateway checks this again, and refuses a file that breaks it)
       ▼
    publish ── "fleet-priors", signed with cortex's key, through /v1/models; at
               most every gap seconds, and only when it changed

Normals: per line type (and "*" for every line), metric and hour band, the
median of the homes' normals; the deviation is the median of theirs widened by
how much the homes differ (1.4826 x the median absolute deviation).
"""
import hashlib
import json
import math
import re
import threading
import time

from . import fed

NAME = "fleet-priors"
DIMS = 6
# which context dimensions each level keeps: band, width, congestion, device, hours, line
MASKS = ((1, 1, 1, 1, 1, 1), (1, 1, 1, 1, 0, 1), (1, 0, 1, 0, 0, 1), (1, 0, 0, 0, 0, 0), (0, 0, 0, 0, 0, 0))
VOCAB = ({"2.4", "5", "6", "-"}, {"20", "40", "80+", "-"}, {"low", "mid", "high", "-"},
         {"phone", "laptop", "tv", "iot", "other", "-"}, {"night", "morning", "afternoon", "evening", "-"},
         {"fibre", "dsl", "cable", "fwa", "satellite", "other", "-"})
LINES = VOCAB[5]
CODE_RE = re.compile(r"^[A-Z0-9_]{1,23}$")
ACTION_RE = re.compile(r"^[a-z0-9_.-]{1,15}$")
METRICS = 5                 # enum care_metric: NextHopLatency, InternetLatency, InternetLoss, DNSLatency, DownloadSpeed
MAX_ROWS = 2000
MAX_COUNT = 100000

_lock = threading.RLock()


class Settings:
    def __init__(self, enabled=True, k=5, cap=50, gap=3600, max_age=30 * 86400):
        self.enabled, self.k, self.cap = enabled, max(2, int(k)), max(1, int(cap))
        self.gap, self.max_age = int(gap), int(max_age)


def project(key, level):
    """A bucket at a coarser level: the dimensions it leaves out become '*'."""
    return "/".join(p if m else "*" for p, m in zip(key.split("/"), MASKS[level]))


def _count(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= MAX_COUNT


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and 0 <= v < 1e6


def check(body):
    """A summary as it may be kept: (clean summary, "") or (None, why)."""
    if not isinstance(body, dict) or body.get("v") != 1:
        return None, "expected a fleet summary (v 1)"
    extra = set(body) - {"v", "line", "outcomes", "normals"}
    if extra:
        return None, "unknown fields: " + ", ".join(sorted(extra))
    line = body.get("line", "-")
    if line not in LINES:
        return None, "line must be one of " + ", ".join(sorted(LINES))
    outs, norms = body.get("outcomes", []), body.get("normals", [])
    if not isinstance(outs, list) or not isinstance(norms, list) or len(outs) > MAX_ROWS or len(norms) > 40:
        return None, "outcomes and normals must be lists (at most 2000 and 40)"
    clean = {"v": 1, "line": line, "outcomes": [], "normals": []}
    for o in outs:
        if not isinstance(o, list) or len(o) != 7:
            return None, "an outcome is [code, action, bucket, tried, helped, worse, undone]"
        code, action, key, t, h, w, u = o
        if not isinstance(code, str) or not CODE_RE.match(code) or not isinstance(action, str) or \
                not ACTION_RE.match(action):
            return None, "an outcome's code or action is not one"
        parts = key.split("/") if isinstance(key, str) else []
        if len(parts) != DIMS or any(p not in VOCAB[i] for i, p in enumerate(parts)):
            return None, "a bucket is band/width/congestion/device/hours/line, each from its fixed words"
        if not all(_count(x) for x in (t, h, w, u)) or h + w + u > t:
            return None, "counts must be whole numbers, helped + worse + undone at most tried"
        clean["outcomes"].append([code, action, key, t, h, w, u])
    for q in norms:
        if not isinstance(q, list) or len(q) != 4 or q[0] not in range(METRICS) or q[1] not in range(4) or \
                not _num(q[2]) or not _num(q[3]):
            return None, "a normal is [metric 0-4, hour band 0-3, normal, deviation]"
        clean["normals"].append([int(q[0]), int(q[1]), float(q[2]), float(q[3])])
    return clean, ""


def accept(store, s, gw, body):
    """A home's summary. Returns (http code, answer)."""
    if not s.enabled:
        return 409, {"ok": False, "error": "fleet learning is off on this cortex"}
    clean, why = check(body)
    if not clean:
        return 400, {"ok": False, "error": why}
    with _lock:
        store.x("INSERT OR REPLACE INTO fleet_summaries (gateway, ts, body) VALUES (?, ?, ?)",
                (gw, int(time.time()), json.dumps(clean)))
        r = maybe_publish(store, s)
    return 200, {"ok": True, "published": r.get("version", 0) if r else 0}


def _median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def aggregate(store, s, now=None):
    """The fleet's priors from the homes' latest summaries: {"homes", "buckets": [...], "normals": [...]}."""
    now = int(now or time.time())
    rows = store.q("SELECT gateway, body FROM fleet_summaries WHERE ts >= ?", (now - s.max_age,))
    per = {}                        # (code, action, bucket) -> {gateway: [t, h, w, u]}
    norms = {}                      # (line, metric, band) -> {gateway: (mean, dev)}
    for r in rows:
        b = json.loads(r["body"])
        for code, action, key, t, h, w, u in b["outcomes"]:
            for level in range(len(MASKS)):
                cell = per.setdefault((code, action, project(key, level)), {}).setdefault(r["gateway"], [0, 0, 0, 0])
                for i, v in enumerate((t, h, w, u)):
                    cell[i] += v
        for m, band, mean, dev in b["normals"]:
            for line in (b["line"], "*"):
                if line != "-":
                    norms.setdefault((line, m, band), {})[r["gateway"]] = (mean, dev)
    buckets = []
    for (code, action, key), homes in sorted(per.items()):
        if len(homes) < s.k:
            continue                # k-anonymity: never a bucket fewer than k homes add up to
        tot = [0, 0, 0, 0]
        for c in homes.values():
            t = min(c[0], s.cap)            # one home counts at most cap tries per bucket
            scale = t / c[0] if c[0] else 0
            parts = [t] + [int(round(x * scale)) for x in c[1:]]
            while sum(parts[1:]) > parts[0]:
                parts[1 + parts[1:].index(max(parts[1:]))] -= 1
            tot = [a + b for a, b in zip(tot, parts)]
        if tot[0]:
            buckets.append([code, action, key, len(homes)] + tot)
    normals = []
    for (line, m, band), homes in sorted(norms.items()):
        if len(homes) < s.k:
            continue
        means = [v[0] for v in homes.values()]
        med = _median(means)
        mad = 1.4826 * _median([abs(x - med) for x in means])
        dev = math.sqrt(_median([v[1] for v in homes.values()]) ** 2 + mad ** 2)
        normals.append([line, m, band, len(homes), round(med, 2), round(dev, 2)])
    return {"homes": len(rows), "buckets": buckets, "normals": normals}


def render(agg, s, version, stamp):
    body = [f"o {c} {a} {k} {h} {t} {he} {w} {u}" for c, a, k, h, t, he, w, u in agg["buckets"]]
    body += [f"n {line} {m} {b} {h} {mean:g} {dev:g}" for line, m, b, h, mean, dev in agg["normals"]]
    head = ["# aigw-priors " + NAME, f"# version {version}", f"# published {stamp}", f"# k {s.k}",
            f"# homes {agg['homes']}",
            "# o <fault> <action> <band/width/congestion/device/hours/line> <homes> <tried> <helped> <worse> <undone>",
            "# n <line> <metric> <hour band> <homes> <normal> <deviation>"]
    return ("\n".join(head + body) + "\n").encode(), hashlib.sha256("\n".join(body).encode()).hexdigest()[:16]


def last(store):
    r = store.q("SELECT version, published, notes FROM models WHERE name = ? ORDER BY version DESC LIMIT 1", (NAME,))
    return r[0] if r else None


def maybe_publish(store, s, force=False):
    """Publish new priors when they changed and gap seconds have passed (force: now). None when not."""
    with _lock:
        prev = last(store)
        if prev and not force and time.time() - prev["published"] < s.gap:
            return None
        agg = aggregate(store, s)
        if not agg["buckets"] and not agg["normals"]:
            return None
        _, digest = render(agg, s, 0, "")
        if prev and prev["notes"].endswith(f"[{digest}]"):
            return None             # nothing new
        version = store.next_version(NAME)
        data, _ = render(agg, s, version, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        note = (f"{len(agg['buckets'])} buckets, {len(agg['normals'])} normals from {agg['homes']} homes "
                f"(k {s.k}) [{digest}]")
        fed.publish_signed(store, NAME, ".priors", version, data, note)
        return {"version": version, "buckets": len(agg["buckets"]), "normals": len(agg["normals"]),
                "homes": agg["homes"], "note": note}


def status(store, s):
    agg = aggregate(store, s)
    p = last(store)
    return {"enabled": s.enabled, "k": s.k, "cap": s.cap, "gap": s.gap, "homes_sharing": agg["homes"],
            "buckets": len(agg["buckets"]), "normals": len(agg["normals"]),
            "published": {"version": p["version"], "at": p["published"], "note": p["notes"]} if p else None}
