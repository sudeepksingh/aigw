# SPDX-License-Identifier: Apache-2.0
"""Neighbour-aware channels across the fleet (v0.45): homes with a clearly better Wi-Fi channel.

    python3 -m aigw_cortex fleet          its "channels" lines

Each gateway learns its channels hour by hour (agd/chan.c): the air others use,
the neighbouring networks, its own busy hours. It moves only when another
channel is clearly better over those hours, with a trial (kept or moved back).
The bridge sends each radio's band, channel, verdict and counts: no names, no
addresses, no table.

Many homes in one area with a better channel found, or moves moved back, point
at a crowded area (a neighbour ISP's fixed wireless, say): 5 GHz or 6 GHz
devices help there more than any channel.
"""
from collections import Counter

LIVE = ("Confirming", "Planned", "Trial")


def _d(x):
    return x if isinstance(x, dict) else {}


def _int(x):
    try:
        return int(x or 0)
    except (TypeError, ValueError):
        return 0


def of(snap):
    """A gateway's radios from its last report; [] for gateways before 0.45."""
    return [r for r in _d(_d(snap).get("channels")).get("radios") or [] if isinstance(r, dict)]


def fleet(store, since, ids):
    """For the fleet report: homes learning, those with a clearly better channel (best first), the moves summed."""
    counts = Counter()
    homes = radios = 0
    better = []
    for g in store.gateways():
        if g["id"] not in ids:
            continue
        rs = of(store.snapshot(g["id"]))
        if not rs:
            continue
        homes += 1
        for r in rs:
            radios += 1
            for k in ("proposed", "kept", "tried"):
                counts[k] += _int(r.get(k))
            if r.get("status") in LIVE and _int(r.get("best")):
                better.append({"home": g["name"], "group": g["grp"] or "", "band": str(r.get("band", "")),
                               "channel": _int(r.get("channel")), "best": _int(r.get("best")),
                               "gain_pct": _int(r.get("gain_pct")), "metric": str(r.get("metric", "")),
                               "status": r.get("status")})
    better.sort(key=lambda b: -b["gain_pct"])
    return {"homes": homes, "radios": radios, "homes_better": len({b["home"] for b in better}),
            "counts": dict(counts), "better": better[:30]}


def fleet_lines(r):
    """The channels lines of the fleet report, in words."""
    m = r.get("channels") or {}
    if not m.get("homes"):
        return []
    c = m.get("counts") or {}
    out = [f"  channels    {m['homes']} home{'' if m['homes'] == 1 else 's'} learning their Wi-Fi channels by the hour "
           f"(AIGW OS 0.45 on): {m['homes_better']} with a clearly better channel in their busy hours; AIGW moved "
           f"{c.get('kept', 0) + c.get('tried', 0)} (kept {c.get('kept', 0)}, moved back {c.get('tried', 0)})"]
    for b in m.get("better") or []:
        where = f" ({b['group']})" if b.get("group") else ""
        by = " (neighbours)" if b.get("metric") == "neighbours" else " (air)" if b.get("metric") == "air" else ""
        out.append(f"    {b['home']}{where}: {b['band']} GHz channel {b['channel']} -> {b['best']}, "
                   f"{b['gain_pct']}% less load in the busy hours{by}, {str(b.get('status', '')).lower()}")
    return out
