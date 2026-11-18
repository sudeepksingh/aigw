# SPDX-License-Identifier: Apache-2.0
"""Proactive care before the call (v0.40): the tickets the gateways raised
themselves, across the fleet.

    python3 -m aigw_cortex proactive [home] [--open] [--json]
    MCP tool: proactive_tickets
    dashboard: "calls likely avoided", and the open tickets

Each gateway finds the faults its customer is likely to notice (agd/proactive.c,
core/libag/proactive.c): AIGW fixes what it can itself, as its act mode allows
(enforce applies, advise only proposes), and raises a ticket to the ISP for the
rest, with the brief, the evidence, the next step and a short note in plain
words the ISP can send the customer. The bridge sends the tickets and the
gateway's counts with each report (texts follow brief.names: device kinds, not
names; a device's MAC never leaves the home).

The counts are each gateway's own, since it began: worth acting on, fixed by
AIGW, proposed, told to the ISP, kept at home (the day's limit, or an outage
across the area), repeats not told again, and calls likely avoided (fixed, or
the customer heard first, and nobody called the help desk while it was open).
The tickets in the period come from each gateway's last tickets (it keeps 16).
"""
import time
from collections import Counter

COUNTS = ("tickets", "told_isp", "fixed_by_aigw", "proposed", "suppressed", "deduplicated", "flaps", "called_anyway",
          "calls_likely_avoided")
OPEN_ORDER = {"Raised": 0, "Proposed": 1, "Watching": 2, "Fixing": 3, "Suppressed": 4}
TEXTS = ("summary", "evidence", "next_step", "customer_note", "reason")


def _d(x):
    return x if isinstance(x, dict) else {}


def _int(x):
    try:
        return int(x or 0)
    except (TypeError, ValueError):
        return 0


def of(snap):
    """A gateway's proactive care from its last report; {} for gateways before 0.40."""
    return _d(_d(snap).get("proactive"))


def _ticket(t, home=None, group=None):
    keep = ("id", "status", "open", "code", "fault", "severity", "points", "reason", "devices", "episodes", "flaps",
            "since", "opened", "closed", "told_isp", "called", "acted", "decision") + TEXTS
    out = {k: t[k] for k in keep if k in t and isinstance(t[k], (str, int, float, bool))}
    if home is not None:
        out["home"] = home
        out["group"] = group or ""
    return out


def fleet(store, since, ids):
    """For the fleet report and the dashboard: the gateways' counts summed, the
    tickets of the period by status and code, and the open ones (worst first)."""
    counts = Counter()
    status, codes = Counter(), Counter()
    homes = with_open = 0
    opened, period_avoided = [], 0
    for g in store.gateways():
        if g["id"] not in ids:
            continue
        p = of(store.snapshot(g["id"]))
        if not p:
            continue
        homes += 1
        c = _d(p.get("counts"))
        for k in COUNTS:
            counts[k] += _int(c.get(k))
        mine = 0
        for t in p.get("tickets") or []:
            if not isinstance(t, dict):
                continue
            if t.get("open") in (1, True):
                mine += 1
                opened.append(_ticket(t, g["name"], g["grp"]))
            if _int(t.get("opened")) >= since:
                status[str(t.get("status", ""))] += 1
                codes[str(t.get("code", ""))] += 1
                if not t.get("open") and not t.get("called") and (t.get("status") == "Fixed" or t.get("told_isp")):
                    period_avoided += 1
        with_open += mine > 0
    opened.sort(key=lambda t: (OPEN_ORDER.get(t.get("status"), 9), _int(t.get("since"))))
    return {"homes": homes, "homes_with_open": with_open, "open": len(opened), "counts": dict(counts),
            "calls_likely_avoided": counts["calls_likely_avoided"], "in_period": {
                "tickets": sum(status.values()), "status": dict(status.most_common()), "codes": dict(codes.most_common()),
                "calls_likely_avoided": period_avoided},
            "open_tickets": opened[:30]}


def fleet_lines(r):
    """The proactive care lines of the fleet report, in words."""
    p = r.get("proactive") or {}
    if not p.get("homes"):
        return []
    c = p.get("counts") or {}
    out = [f"  before call  {p['homes']} home{'' if p['homes'] == 1 else 's'} (AIGW OS 0.40 on): "
           f"{c.get('tickets', 0)} faults worth acting on, {c.get('fixed_by_aigw', 0)} fixed by AIGW itself, "
           f"{c.get('proposed', 0)} fixes proposed, {c.get('told_isp', 0)} told to the ISP, "
           f"{c.get('suppressed', 0)} kept at home, {c.get('deduplicated', 0)} repeats not told again; "
           f"calls likely avoided {c.get('calls_likely_avoided', 0)} (called anyway {c.get('called_anyway', 0)}); "
           f"{p['open']} open now"]
    ip = p.get("in_period") or {}
    if ip.get("tickets"):
        out.append("               in the period: " + ", ".join(f"{k} {v}" for k, v in ip["status"].items()) +
                   f"; codes: " + ", ".join(f"{k} {v}" for k, v in ip["codes"].items()))
    for t in p.get("open_tickets") or []:
        where = f" ({t['group']})" if t.get("group") else ""
        out.append(f"    {t['home']}{where}: #{t.get('id')} {t.get('status')} {t.get('code')} ({t.get('fault')}): "
                   f"{t.get('next_step', '')}")
    return out


def tickets(store, ref=None, open_only=False):
    """The tickets of one home (by name or id), or of every home, newest first."""
    gws = [store.gateway(ref)] if ref else store.gateways()
    if ref and not gws[0]:
        raise ValueError(f"no home called {ref}")
    out = []
    for g in gws:
        p = of(store.snapshot(g["id"]))
        for t in p.get("tickets") or []:
            if isinstance(t, dict) and (not open_only or t.get("open") in (1, True)):
                out.append(_ticket(t, g["name"], g["grp"]))
    out.sort(key=lambda t: (not t.get("open"), -_int(t.get("opened"))))
    return out


def summary(store, ref=None, open_only=False, now=None):
    """What the CLI and the MCP tool answer: the counts and the tickets."""
    now = int(now or time.time())
    ids = {g["id"] for g in ([store.gateway(ref)] if ref else store.gateways()) if g}
    if ref and not ids:
        raise ValueError(f"no home called {ref}")
    f = fleet(store, now - 7 * 86400, ids)
    return {"homes": f["homes"], "counts": f["counts"], "open": f["open"],
            "calls_likely_avoided": f["calls_likely_avoided"], "tickets": tickets(store, ref, open_only)}


def text(s, now=None):
    now = now or time.time()
    c = s.get("counts") or {}
    out = [f"Before the call: {s['homes']} home{'' if s['homes'] == 1 else 's'} reporting it; "
           f"{c.get('tickets', 0)} worth acting on, {c.get('fixed_by_aigw', 0)} fixed by AIGW, "
           f"{c.get('told_isp', 0)} told to the ISP; calls likely avoided {s['calls_likely_avoided']}; {s['open']} open"]
    for t in s.get("tickets") or []:
        age = max(0, int(now - _int(t.get("since"))))
        out.append(f"  {t['home']} #{t.get('id')} {t.get('status')} {t.get('code')} ({t.get('fault')}), "
                   f"{'open' if t.get('open') else 'over'}, since {age // 60} min")
        for k, label in (("summary", "what"), ("next_step", "next"), ("customer_note", "customer")):
            if t.get(k):
                out.append(f"    {label:9s}{t[k]}")
    if not s.get("tickets"):
        out.append("  no tickets")
    return "\n".join(out)
