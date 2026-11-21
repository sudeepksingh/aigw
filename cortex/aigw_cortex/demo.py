# SPDX-License-Identifier: Apache-2.0
"""A simulated fleet, to try cortex and its dashboard without gateways (docs/try-it.md).

    python3 -m aigw_cortex demo                     a fresh simulated fleet, the dashboard on
                                                    http://127.0.0.1:8090/ (Ctrl+C ends it; nothing is kept)
    python3 -m aigw_cortex demo --dir D --listen 127.0.0.1:8091
    python3 -m aigw_cortex demo seed --dir D [--home NAME --home-group G]
                                                    only seed D; with --home, enroll a real gateway
                                                    (the demo image's own home) and print its token

24 homes in two towns, as their bridges would report them over the last two
weeks: experience scores, problems found and fixed by AIGW, an outage across
four homes of one town, an early warning, signals for the sales and care
teams, smart queueing and calls first, and two staged rollouts (one going
well, one stopped by rollbacks). The times are relative to now.

Demo data is labelled: the data directory carries a DEMO file, the dashboard
shows a banner, and `demo` refuses a directory that already has gateways of
a real fleet, so a simulated home never mixes with real ones.
"""
import os
import shutil
import sys
import tempfile
import time

from . import __version__, rollout
from .store import Store

MARK = "DEMO"
TOWNS = ["riverside"] * 14 + ["hillcrest"] * 10
OLDER = "0.40.0"


def is_demo(store_or_dir):
    d = store_or_dir.dir if hasattr(store_or_dir, "dir") else store_or_dir
    return os.path.exists(os.path.join(d, MARK))


def _day(today, k, good, **kw):
    """One of a home's care days, as the bridge reports it."""
    r = {"day": today - k, "checks": 144, "good": good, "Internet": 0, "DNS": 0, "WiFi": 144 - good, "Device": 0,
         "AIGW": 0, "problems": 1 if good < 144 else 0, "reported": 0, "fixed_by_aigw": 1 if good < 140 else 0,
         "problem_s": (144 - good) * 600, "early_warnings": 0, "warned_of": 0, "warning_lead_s": 0,
         "speed_tests": 1, "speed_kbps_sum": 180000 + k * 1000, "speed_kbps_min": 170000, "speed_low": 0,
         "watched_s": 86400, "ok_s": 86400 - (144 - good) * 600}
    r.update(kw)
    return r


def _home(i, town, now):
    """What home i's bridge last reported."""
    today = now // 86400
    good = [144, 143, 142, 140, 138, 131, 120][i % 7]
    # the week before was a little worse: AIGW fixed things since
    days = [_day(today, k, good if k < 7 else max(110, good - 3 - i % 4)) for k in range(14)]
    full = i % 4                            # calls on a full line this week
    smooth = full - (1 if full and i % 3 == 0 else 0)
    care = {"days": days, "status": "ok",
            "sqm": {"status": "On" if i % 4 == 0 else "Off"},
            "calls": {"stats": {"total": 6 + i % 6 + i % 3, "calls": 4 + i % 6, "games": 2 + i % 3,
                                "on_full_line": full, "smooth_on_full_line": smooth, "prioritized": smooth}}}
    if town == "riverside" and i in (2, 3, 4, 5):
        # an outage: the same problem in four homes of one town within minutes (the ISP's network)
        care["status"] = "problem"
        care["problem"] = {"code": "ISP_DOWN", "area": "Internet", "since": now - 1500 + i * 60, "reported": 1,
                           "severity": "error", "summary": "The internet doesn't answer; the line is up."}
    if i == 16:
        care["early_warning"] = {"status": "warning", "metric": "InternetLatency", "area": "Internet",
                                 "since": now - 86400, "predicted": 1, "predicted_fault_days": 4,
                                 "normal": 12, "current": 31,
                                 "summary": "Latency to the internet rose from 12 ms to 31 ms over 9 days."}
    if i in (6, 13, 20):
        care["offers"] = [{"code": "MESH_WIFI", "kind": "upsell", "product": "mesh Wi-Fi", "confidence": "high",
                           "days": 6, "of_days": 7, "since": now - 6 * 86400,
                           "evidence": "3 devices on weak Wi-Fi in the evening, 6 of 7 days"}]
    if i in (5, 12):
        care["offers"] = [{"code": "PLAN_UPGRADE", "kind": "upsell", "product": "faster plan", "confidence": "medium",
                           "days": 4, "of_days": 7, "since": now - 4 * 86400,
                           "evidence": "the line was full for 40 min a day on 4 of 7 days"}]
    if i == 19:
        care["offers"] = [{"code": "LINE_REPAIR", "kind": "repair", "product": "a line repair", "confidence": "high",
                           "days": 3, "of_days": 7, "since": now - 3 * 86400,
                           "evidence": "the line dropped 11 times in 3 days"}]
    decs = []
    if i % 4 == 0:
        decs.append({"id": 1, "state": "kept", "action": "sqm", "target": "wan", "origin": "policy bufferbloat",
                     "created": now - 5 * 86400})
    if i % 5 == 1:
        decs.append({"id": 2, "state": "proposed", "action": "sqm", "target": "wan", "origin": "policy bufferbloat",
                     "reason": "Calls stuttered while the line was full.", "created": now - 3600})
    if i == 0:
        decs.append({"id": 3, "state": "proposed", "action": "quarantine", "target": "Living room TV",
                     "device": "Living room TV", "origin": "policy dga", "reason": "It looked up 40 random names.",
                     "created": now - 1200})
    # proactive care (v0.40): faults the customer would notice, fixed by AIGW or raised to the ISP first
    pro = {"counts": {"tickets": 0, "told_isp": 0, "fixed_by_aigw": 0, "proposed": 0, "suppressed": 0,
                      "deduplicated": 0, "flaps": 0, "called_anyway": 0, "calls_likely_avoided": 0,
                      "held_area_outage": 0}, "tickets": []}
    c = pro["counts"]
    if i % 4 == 0:
        c.update(tickets=1, fixed_by_aigw=1, calls_likely_avoided=1)
        pro["tickets"].append({"id": 1, "status": "Fixed", "open": 0, "code": "LINE_FULL", "fault": "NONE",
                               "severity": "warning", "points": 4, "since": now - 2 * 86400, "opened": now - 2 * 86400,
                               "closed": now - 2 * 86400 + 2400, "told_isp": 0, "called": 0, "acted": 1,
                               "summary": "Calls stuttered while the line was full; smart queueing fixed it.",
                               "next_step": "Nothing: smart queueing is kept."})
    if town == "riverside" and i in (2, 3, 4, 5):
        c["held_area_outage"] = 1           # the area outage: the ISP knows already, no ticket per home
    if i == 8:
        c.update(tickets=1, told_isp=1, calls_likely_avoided=0)
        pro["tickets"].append({"id": 1, "status": "Raised", "open": 1, "code": "LINE_LOSS", "fault": "LINE",
                               "severity": "problem", "points": 6, "since": now - 5 * 3600, "opened": now - 4 * 3600,
                               "told_isp": 1, "called": 0, "acted": 0,
                               "summary": "The line drops packets (4% lost) since this morning, at every hour.",
                               "evidence": "Timeline: loss to the next router 3-6% for 5 h; the Wi-Fi is fine.",
                               "next_step": "Check the line (the DSL port, the cable to the house).",
                               "customer_note": "We noticed your connection dropping now and then since this "
                                                "morning. We are checking the line; there is no need to call us."})
    if i == 11:
        c.update(tickets=1, proposed=1)
        pro["tickets"].append({"id": 1, "status": "Proposed", "open": 1, "code": "WIFI_HOPPING", "fault": "HOME_WIFI",
                               "severity": "warning", "points": 3, "since": now - 3 * 3600, "opened": now - 3 * 3600,
                               "told_isp": 1, "called": 0, "acted": 0,
                               "summary": "A laptop keeps hopping between 2.4 and 5 GHz in the evening.",
                               "next_step": "Approve keeping it on 2.4 GHz (proposed on the gateway).",
                               "customer_note": "Your laptop's Wi-Fi was unsteady this evening; your router "
                                                "suggests a fix you can approve in its app."})
    names = i % 2 == 0
    # mesh Wi-Fi (v0.44): a third of the homes have an extender; two of them on a weak wireless backhaul
    mesh = None
    if i % 3 == 1:
        weak = i in (4, 10)
        mesh = {"agents": 2, "extenders": 1, "stations": 5 + i % 4, "wireless_backhauls": 1,
                "weak_backhauls": 1 if weak else 0, "hurting": 1 if weak else 0,
                "findings": [{"kind": "Weak", "stations": 3, "hurts": True, "signal": -77 - i % 3,
                              "rate_kbps": 52000}] if weak else [],
                "counts": {"steers": 2 + i % 3, "kept": 1 + i % 3, "undone": 1, "misses": 0, "proposed": 0,
                           "channel_changes": 1 if i == 7 else 0, "channels_kept": 1 if i == 7 else 0,
                           "channels_undone": 0, "roams": 9}}
    extra = {"mesh": mesh} if mesh else {}
    # coverage (v0.46): a few homes need an extender (a weak spot two devices share), others only a device moved
    verdict = "Extender" if i % 4 == 2 else "MoveDevice" if i % 4 == 3 else "Fine"
    spot = {"where": "where a TV and an Android phone are, in the evening (about -84 dBm at the gateway)",
            "when": "in the evening", "devices": 2, "suffering": 2, "shared": True, "samples": 300, "signal": -84}
    extra["coverage"] = {"enabled": True, "names": False, "verdict": verdict, "judged": 6 + i % 9,
                         "weak_devices": 2 if verdict == "Extender" else 1 if verdict == "MoveDevice" else 0,
                         "to_move": 2 if verdict == "Extender" else 1 if verdict == "MoveDevice" else 0,
                         "weak_transmitters": 1 if i % 5 == 0 else 0, "shared_spots": 1 if verdict == "Extender" else 0,
                         "access_points": 1, "spots": [spot] if verdict == "Extender" else [],
                         "advice": "2 devices are weak in the same spot (" + spot["where"] + "): the gateway is too far "
                                   "from it. Move the gateway toward that spot (higher, in the open), or add a Wi-Fi "
                                   "extender about halfway between the gateway and it." if verdict == "Extender" else ""}
    return {**extra, "proactive": pro, "status": {"version": OLDER if i % 6 == 5 else __version__, "host": "openwrt", "devices": 6 + i % 9,
                       "alerts": 1 if i == 0 else 0, "mode": "advise",
                       "waiting": sum(1 for d in decs if d["state"] == "proposed")},
            "devices": [{"hostname": "Living room TV", "mac": "02:00:00:00:00:01"}] if names else [{"id": "a1b2c3d4e5f6"}],
            "alerts": [{"id": 1, "kind": "dga", "subject": "qxzvkp.top", "device": "Living room TV",
                        "reason": "looks machine-generated", "first": now - 1300, "last": now - 1200, "count": 40}]
            if i == 0 else [],
            "decisions": decs, "care": care}


def seed(d, now=None, home=None, home_group="riverside"):
    """Seed directory d with the simulated fleet. Returns (store, token of `home` or None)."""
    now = int(now or time.time())
    os.makedirs(d, exist_ok=True)
    store = Store(d)
    if store.gateways() and not is_demo(d):
        raise ValueError(f"{d} already has gateways of a fleet: a demo needs a directory of its own")
    if store.gateways():
        raise ValueError(f"{d} has a simulated fleet already: start with a new directory")
    with open(os.path.join(d, MARK), "w") as f:
        f.write("A simulated fleet (python3 -m aigw_cortex demo): no real homes.\n")
    ids = []
    for i, town in enumerate(TOWNS):
        gid, _ = store.enroll(f"home-{town[:4]}-{i:02d}", town)
        ids.append(gid)
        store.report(gid, _home(i, town, now), now - 120 - i * 7)
    # two staged rollouts: one going well in hillcrest, one stopped by rollbacks in riverside
    r = rollout.create(store, f"spring-{__version__}", __version__, "", waves=[10, 50, 100], group="hillcrest",
                       now=now - 86400)
    for h in store.q("SELECT gateway FROM rollout_homes WHERE rollout = ? AND wave = 1", (r,)):
        rollout.acs_result(store, f"spring-{__version__}", h["gateway"], "Kept", 97, 98, now=now - 40000)
    r = rollout.create(store, f"beta-{OLDER}", OLDER, "", waves=[20, 100], group="riverside", now=now - 3 * 86400)
    for k, h in enumerate(store.q("SELECT gateway FROM rollout_homes WHERE rollout = ? AND wave = 1", (r,))):
        rollout.acs_result(store, f"beta-{OLDER}", h["gateway"], "RolledBack" if k < 2 else "Kept",
                           now=now - 2 * 86400)
    token = None
    if home:
        _, token = store.enroll(home, home_group)
    return store, token


def main(a):
    """python3 -m aigw_cortex demo [seed] ..."""
    if a.what == "seed":
        if not a.dir:
            sys.exit("demo seed: --dir is needed (a directory of its own)")
        try:
            _, token = seed(a.dir, home=a.home, home_group=a.home_group)
        except ValueError as e:
            sys.exit(f"demo: {e}")
        print(f"seeded a simulated fleet in {a.dir}", file=sys.stderr)
        if token:
            print(token)
        return
    from .dashboard import serve as dashboard
    tmp = None
    d = a.dir
    if not d:
        tmp = d = tempfile.mkdtemp(prefix="aigw-demo-")
    try:
        if not is_demo(d) or not Store(d).gateways():
            seed(d)
        print("DEMO: a simulated fleet of 24 homes (nothing here is a real home)", flush=True)
        dashboard(Store(d), a.listen, None, None, None, log=not a.quiet)
    except (OSError, ValueError) as e:
        sys.exit(f"demo: {e}")
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
