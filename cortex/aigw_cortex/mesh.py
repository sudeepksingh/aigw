# SPDX-License-Identifier: Apache-2.0
"""Mesh Wi-Fi across the fleet (v0.44): homes whose extenders hurt more than they help.

    python3 -m aigw_cortex fleet          its "mesh" lines
    dashboard: "Mesh Wi-Fi", the homes whose extenders hurt more than they help

Each gateway reads its home's EasyMesh controller (prplMesh) through the Data
Elements model (agd/mesh.c): the access points, each extender's backhaul, the
devices, and what AIGW did about them (devices moved to a better access point,
kept or moved back, channel changes). The bridge sends counts only: how many
access points and extenders, how many backhauls are wireless, weak, or "hurt
more than they help" (weak or slow with devices behind them), each finding's
kind, signal and rate, and AIGW's moves. No names, no addresses.

An extender that hurts more than it helps is the ISP's lead for a visit to move
it, a cable (Ethernet, powerline, MoCA), or a better extender (6 GHz backhaul).
"""
from collections import Counter

COUNTS = ("steers", "kept", "undone", "misses", "proposed", "channel_changes", "channels_kept", "channels_undone",
          "roams")


def _d(x):
    return x if isinstance(x, dict) else {}


def _int(x):
    try:
        return int(x or 0)
    except (TypeError, ValueError):
        return 0


def of(snap):
    """A gateway's mesh Wi-Fi from its last report; {} for gateways before 0.44 or without a controller."""
    return _d(_d(snap).get("mesh"))


def fleet(store, since, ids):
    """For the fleet report and the dashboard: homes with a mesh, their extenders, the ones that hurt more
    than they help (worst first), and AIGW's moves summed."""
    counts, kinds = Counter(), Counter()
    homes = with_ext = extenders = wireless = weak = 0
    hurting = []
    for g in store.gateways():
        if g["id"] not in ids:
            continue
        m = of(store.snapshot(g["id"]))
        if not m:
            continue
        homes += 1
        ext = _int(m.get("extenders"))
        extenders += ext
        with_ext += ext > 0
        wireless += _int(m.get("wireless_backhauls"))
        weak += _int(m.get("weak_backhauls"))
        c = _d(m.get("counts"))
        for k in COUNTS:
            counts[k] += _int(c.get(k))
        found = [f for f in m.get("findings") or [] if isinstance(f, dict)]
        for f in found:
            kinds[str(f.get("kind", ""))] += 1
        if _int(m.get("hurting")):
            worst = min((f for f in found if f.get("hurts") in (1, True)), key=lambda f: _int(f.get("signal")) or 0,
                        default={})
            hurting.append({"home": g["name"], "group": g["grp"] or "", "extenders": ext,
                            "hurting": _int(m.get("hurting")), "stations": _int(worst.get("stations")),
                            "kind": worst.get("kind", ""), "signal": worst.get("signal"),
                            "rate_mbps": _int(worst.get("rate_kbps")) // 1000 if worst.get("rate_kbps") else None})
    hurting.sort(key=lambda h: (-h["hurting"], -h["stations"], h["signal"] if h["signal"] is not None else 0))
    moved = counts["steers"]
    return {"homes": homes, "homes_with_extenders": with_ext, "extenders": extenders, "wireless_backhauls": wireless,
            "weak_backhauls": weak, "homes_hurting": len(hurting), "findings": dict(kinds.most_common()),
            "counts": dict(counts), "kept_pct": round(100.0 * counts["kept"] / moved) if moved else None,
            "hurting": hurting[:30]}


def fleet_lines(r):
    """The mesh lines of the fleet report, in words."""
    m = r.get("mesh") or {}
    if not m.get("homes"):
        return []
    c = m.get("counts") or {}
    out = [f"  mesh        {m['homes']} home{'' if m['homes'] == 1 else 's'} with an EasyMesh controller (AIGW OS 0.44 on), "
           f"{m['extenders']} extender{'' if m['extenders'] == 1 else 's'} ({m['wireless_backhauls']} wireless, "
           f"{m['weak_backhauls']} weak); {m['homes_hurting']} home{'' if m['homes_hurting'] == 1 else 's'} whose "
           f"extenders hurt more than they help; AIGW moved {c.get('steers', 0)} devices (kept {c.get('kept', 0)}, "
           f"moved back {c.get('undone', 0)}, missed {c.get('misses', 0)}), {c.get('channel_changes', 0)} channel changes"]
    for h in m.get("hurting") or []:
        where = f" ({h['group']})" if h.get("group") else ""
        sig = f", {h['signal']} dBm" if h.get("signal") is not None else ""
        rate = f", {h['rate_mbps']} Mbit/s" if h.get("rate_mbps") is not None else ""
        out.append(f"    {h['home']}{where}: an extender's backhaul {str(h.get('kind', '')).lower()}{sig}{rate}, "
                   f"{h['stations']} device{'' if h['stations'] == 1 else 's'} behind it")
    return out
