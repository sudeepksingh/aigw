# SPDX-License-Identifier: Apache-2.0
"""Wi-Fi coverage across the fleet (v0.46): the homes that need an extender.

    python3 -m aigw_cortex fleet          its "coverage" lines
    dashboard: "Wi-Fi coverage", the homes that need an extender (a sales lead)

Each gateway learns where its Wi-Fi is weak (agd/coverage.c): which devices,
when, and whether they share one weak spot. The bridge sends the verdict, the
counts and the spots by device kind (names only with brief.names), never a MAC:

  Extender       a weak spot two or more suffering devices share: the gateway is
                 badly placed for it. Moving it toward the spot, or an extender
                 about halfway, fixes it: the ISP's lead for an extender (or a
                 visit to move the gateway)
  UseExtender    the home has an extender that hears the spot well: the devices
                 belong on it (no sale: steering, or a word to the customer)
  MoveDevice     one device in a far spot: moving it fixes it (no sale)
  WeakDevice     only devices whose own radio is weak (a cheap plug): nothing to sell
  Fine, Learning

So the ISP sells an extender to the homes where one would help, and not to the
home whose one phone is weak in the bedroom at night.
"""
from collections import Counter

LEAD = "Extender"


def _d(x):
    return x if isinstance(x, dict) else {}


def _int(x):
    try:
        return int(x or 0)
    except (TypeError, ValueError):
        return 0


def of(snap):
    """A gateway's coverage from its last report; {} for gateways before 0.46 (or coverage.share 0)."""
    return _d(_d(snap).get("coverage"))


def fleet(store, since, ids):
    """For the fleet report and the dashboard: homes by verdict, and the extender leads (most devices first)."""
    verdicts = Counter()
    homes = weak = movers = weak_tx = 0
    leads = []
    for g in store.gateways():
        if g["id"] not in ids:
            continue
        c = of(store.snapshot(g["id"]))
        if not c or not c.get("enabled", True):
            continue
        homes += 1
        v = str(c.get("verdict") or "Learning")
        verdicts[v] += 1
        weak += _int(c.get("weak_devices"))
        movers += _int(c.get("to_move"))
        weak_tx += _int(c.get("weak_transmitters"))
        if v != LEAD:
            continue
        spots = [s for s in c.get("spots") or [] if isinstance(s, dict) and s.get("shared") in (1, True)]
        top = max(spots, key=lambda s: (_int(s.get("suffering")), _int(s.get("samples"))), default={})
        leads.append({"home": g["name"], "group": g["grp"] or "", "devices": _int(top.get("suffering")),
                      "shared_spots": _int(c.get("shared_spots")) or len(spots), "where": str(top.get("where", "")),
                      "when": str(top.get("when", "")), "signal": top.get("signal"),
                      "mesh": _int(c.get("access_points")) > 1, "advice": str(c.get("advice", ""))})
    leads.sort(key=lambda h: (-h["devices"], -h["shared_spots"], h["signal"] if isinstance(h["signal"], int) else 0))
    return {"homes": homes, "verdicts": dict(verdicts.most_common()), "need_extender": verdicts[LEAD],
            "use_extender": verdicts["UseExtender"], "move_device": verdicts["MoveDevice"],
            "weak_device_only": verdicts["WeakDevice"], "weak_devices": weak, "devices_to_move": movers,
            "weak_transmitters": weak_tx, "leads": leads[:30]}


def fleet_lines(r):
    """The coverage lines of the fleet report, in words."""
    c = r.get("coverage") or {}
    if not c.get("homes"):
        return []
    n = c["need_extender"]
    out = [f"  coverage    {c['homes']} home{'' if c['homes'] == 1 else 's'} judged (AIGW OS 0.46 on): "
           f"{n} need{'s' if n == 1 else ''} an extender (a weak spot two or more devices share), "
           f"{c['use_extender']} should use the extender they have, {c['move_device']} only a device to move, "
           f"{c['weak_device_only']} only a device with a weak radio of its own"]
    for h in c.get("leads") or []:
        where = f" ({h['group']})" if h.get("group") else ""
        out.append(f"    {h['home']}{where}: {h['devices']} devices weak in one spot {h['where']}"
                   + (f" {h['when']}" if h["when"] and h["when"] not in h["where"] else "")
                   + ("; has a mesh" if h["mesh"] else ""))
    return out
