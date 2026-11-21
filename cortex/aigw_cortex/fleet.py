# SPDX-License-Identifier: Apache-2.0
"""The ISP's fleet report: every home's proactive care, together.

    python3 -m aigw_cortex fleet [--days 7] [--group G] [--json]

From what each gateway's bridge reports (agd's care: the day's checks, the
problems it found, fixed and reported, its early warnings and speed tests;
never names), for the last --days days against the days before:

  experience   each home's score (the time without a problem), the fleet's
               average and median, how many homes are good (95% or more), fair
               (80 to 95) or poor, and the change from the period before
  problems     found by the gateways, fixed by AIGW itself (no call needed;
               only where it changed something), cleared by themselves,
               reported to the ISP before anyone called; the main causes
  warnings     early warnings, the problems they saw coming and how long before
  speed        the nightly speed tests: the median, the homes below their line's speed
  outages      (v0.39: outage.py) many homes behind one uplink whose line goes slow,
               loses packets or drops within minutes of each other: the ISP's
               network, not the homes; kept as incidents that close when the homes
               recover
  attention    the homes a help desk should look at first: an open problem, a
               warning that says a fault is near, a poor score, a gateway gone quiet
  signals      (v0.32) what homes would buy, or why they may leave, as each gateway saw
               it over its last week, with the evidence: a faster plan, smart queueing,
               mesh Wi-Fi, a customer at risk, a line to repair first
  rollouts     (v0.34) staged firmware rollouts going on or changed in the period: the
               wave, how many homes kept it or got worse, and why one stopped (rollout.py)
  help desk    (v0.37) support briefs answered (brief.py), by fault, and how many
               advised a technician: the proof of faster calls and fewer visits
  before call  (v0.40) proactive care before the call (proactive.py): faults the gateways
               fixed themselves or raised to the ISP first, and the calls likely avoided
  mesh         (v0.44) mesh Wi-Fi (mesh.py): extenders, their backhaul, the homes whose extenders
               hurt more than they help, and the devices AIGW moved to a better access point
  channels     (v0.45) neighbour-aware channels (channels.py): homes with a clearly better Wi-Fi
               channel in their busy hours, and AIGW's planned moves (kept, moved back)
"""
import statistics
import time
from collections import Counter

from . import brief, channels, mesh, outage, proactive, rollout, tickets

GOOD, FAIR = 95, 80
SILENT_S = 86400
# the order a sales or care team works through the signals: what the ISP owes first, then who may leave
OFFER_ORDER = {"repair": 0, "retention": 1, "upsell": 2}
CONFIDENCE = {"high": 0, "medium": 1, "low": 2}
AREAS = (("internet", "Internet"), ("dns", "DNS"), ("wifi", "WiFi"), ("device", "Device"), ("aigw", "AIGW"))


def _score(c):
    """The share of the time watched without a problem (a gateway checks every
    10 minutes when all is well, every minute while something is wrong:
    counting checks would weigh a bad minute like ten good ones); days from
    gateways before v0.29.1 have no times: the share of the checks, unless
    some days of the period have times (then those days)."""
    watched, ok = c.get("watched_s") or 0, c.get("ok_s") or 0
    if watched:
        return round(min(ok, watched) * 100.0 / watched, 1)
    return round(c["good"] * 100.0 / c["checks"], 1) if c.get("checks") else None


def _sum(rows):
    keys = ("checks", "good", "internet", "dns", "wifi", "device", "aigw", "problems", "reported", "fixed",
            "problem_s", "warnings", "warned", "lead_s", "speed_tests", "speed_kbps_sum", "speed_low",
            "watched_s", "ok_s", "cleared")
    out = {k: sum(int(r.get(k) or 0) for r in rows) for k in keys}
    mins = [int(r["speed_kbps_min"]) for r in rows if r.get("speed_tests")]
    out["speed_kbps_min"] = min(mins) if mins else 0
    return out


def outages(store, since, ids, now=None):
    """Area outages (outage.py) open now or begun in the period, newest first, among
    these homes. Kept fields from before 0.39 (group, homes_in_group, still_open) beside
    the incident's own (id, uplink, kind, confidence, state, affected)."""
    outage.update(store, now)
    out = []
    for o in outage.incidents(store, since=since):
        if o["state"] != "open" and o["start"] < since:
            continue
        aff = [h for h in o["affected"] if h["gateway"] in ids]
        if not aff:
            continue
        o.update(group=o["uplink"], homes_in_group=o["homes_in_area"], still_open=o["still_affected"])
        out.append(o)
    return out


def signals(store, ids, names, groups, limit=30):
    """Each home's open signals for the ISP (v0.32): how many homes, by product, and
    the list in the order a team would call: repairs, homes at risk, then sales by
    confidence."""
    rows = store.offers_open(ids)
    by = Counter(r["product"] or r["code"] for r in rows)
    high = Counter(r["product"] or r["code"] for r in rows if r["confidence"] == "high")
    homes = {r["gateway"] for r in rows}
    rows.sort(key=lambda r: (OFFER_ORDER.get(r["kind"], 3), CONFIDENCE.get(r["confidence"], 3), -r["days"], r["since"],
                         names.get(r["gateway"], ""), r["code"]))
    return {"homes": len(homes), "by_product": dict(by.most_common()), "high": dict(high),
            "codes": dict(Counter(r["code"] for r in rows).most_common()),
            "list": [{"home": names[r["gateway"]], "group": groups[r["gateway"]], "code": r["code"], "kind": r["kind"],
                      "product": r["product"], "confidence": r["confidence"], "days": r["days"],
                      "of_days": r["of_days"], "since": r["since"], "evidence": r["evidence"]} for r in rows[:limit]]}


def report(store, days=7, group=None, now=None):
    now = int(now or time.time())
    today = now // 86400
    first, prev_first = today - days + 1, today - 2 * days + 1
    gws = [g for g in store.q("SELECT id, name, grp, last_seen FROM gateways") if group is None or g["grp"] == group]
    ids = {g["id"] for g in gws}
    names = {g["id"]: g["name"] for g in gws}
    groups = {g["id"]: g["grp"] or "" for g in gws}
    rows = store.care_days(prev_first, today, ids)
    cur = {g: _sum([r for r in rows if r["gateway"] == g and r["day"] >= first]) for g in ids}
    prev = {g: _sum([r for r in rows if r["gateway"] == g and r["day"] < first]) for g in ids}
    fleet = _sum([r for r in rows if r["day"] >= first])

    homes = {}
    for g in ids:
        c = cur[g]
        homes[g] = {"name": names[g], "group": groups[g], "score": _score(c),
                    "score_before": _score(prev[g]), "checks": c["checks"], "watched_h": round(c["watched_s"] / 3600.0, 1),
                    "problems": c["problems"], "reported": c["reported"], "fixed_by_aigw": c["fixed"],
                    "cleared_by_itself": c["cleared"],
                    "main_cause": max(AREAS, key=lambda a: c[a[0]])[1] if any(c[a[0]] for a in AREAS) else "",
                    "speed_mbps": round(c["speed_kbps_sum"] / 1000.0 / c["speed_tests"], 1) if c["speed_tests"] else None,
                    "speed_low": c["speed_low"]}
    scored = [h["score"] for h in homes.values() if h["score"] is not None]
    before = [h["score_before"] for h in homes.values() if h["score_before"] is not None]
    speeds = [h["speed_mbps"] for h in homes.values() if h["speed_mbps"] is not None]

    events = store.care_events(first * 86400, ids)
    problems = [e for e in events if e["kind"] == "problem" and e["since"] >= first * 86400]
    warnings_open = [e for e in events if e["kind"] == "warning" and not e["until"]]
    problems_open = [e for e in events if e["kind"] == "problem" and not e["until"]]

    attention = []
    found = outages(store, first * 86400, ids, now)
    in_outage = {h["gateway"]: o["id"] for o in found if o["state"] == "open" for h in o["affected"]}
    for e in sorted(problems_open, key=lambda e: e["since"]):
        attention.append({"home": names[e["gateway"]], "group": groups[e["gateway"]], "why": "problem",
                          "code": e["code"], "area": e["area"], "since": e["since"], "reported": bool(e["reported"]),
                          "summary": e["summary"]})
        if e["gateway"] in in_outage:      # the area's, not the home's: one incident, not a call each
            attention[-1]["area_outage"] = in_outage[e["gateway"]]
    for e in sorted(warnings_open, key=lambda e: (e["predict"] if e["predict"] >= 0 else 999, e["since"])):
        attention.append({"home": names[e["gateway"]], "group": groups[e["gateway"]], "why": "early warning",
                          "code": e["code"], "area": e["area"], "since": e["since"],
                          "predicted_fault_days": e["predict"] if e["predict"] >= 0 else None, "summary": e["summary"]})
    for g, h in sorted(homes.items(), key=lambda x: x[1]["score"] if x[1]["score"] is not None else 101):
        if h["score"] is not None and h["score"] < FAIR:
            attention.append({"home": h["name"], "group": h["group"], "why": "poor score", "score": h["score"],
                              "main_cause": h["main_cause"]})
    for g in gws:
        if g["last_seen"] and now - g["last_seen"] > SILENT_S:
            attention.append({"home": g["name"], "group": g["grp"] or "", "why": "silent", "last_seen": g["last_seen"]})

    return tickets.attach(store, {
        "period": {"days": days, "from": time.strftime("%Y-%m-%d", time.gmtime(first * 86400)),
                   "to": time.strftime("%Y-%m-%d", time.gmtime(today * 86400)), "group": group},
        "homes": {"total": len(gws), "reporting": len(scored),
                  "silent": sum(1 for g in gws if g["last_seen"] and now - g["last_seen"] > SILENT_S)},
        "experience": {
            "mean": round(statistics.mean(scored), 1) if scored else None,
            "median": round(statistics.median(scored), 1) if scored else None,
            "mean_before": round(statistics.mean(before), 1) if before else None,
            "good": sum(1 for s in scored if s >= GOOD), "fair": sum(1 for s in scored if FAIR <= s < GOOD),
            "poor": sum(1 for s in scored if s < FAIR), "checks": fleet["checks"]},
        "problems": {
            "found": fleet["problems"], "fixed_by_aigw": fleet["fixed"], "reported_to_isp": fleet["reported"],
            # over though AIGW changed nothing: not counted as its fix (v0.29.2)
            "cleared_by_itself": fleet["cleared"],
            "hours": round(fleet["problem_s"] / 3600.0, 1),
            "causes": {name: fleet[k] for k, name in AREAS if fleet[k]},
            "codes": dict(Counter(e["code"] for e in problems).most_common()),
            "open": len(problems_open)},
        "early_warnings": {
            "warnings": fleet["warnings"], "problems_seen_coming": fleet["warned"],
            "mean_lead_hours": round(fleet["lead_s"] / 3600.0 / fleet["warned"], 1) if fleet["warned"] else None,
            "open": len(warnings_open)},
        "speed": {"homes_tested": len(speeds), "median_mbps": round(statistics.median(speeds), 1) if speeds else None,
                  "tests": fleet["speed_tests"], "tests_below_line_speed": fleet["speed_low"],
                  "homes_below_line_speed": sum(1 for h in homes.values() if h["speed_low"])},
        "outages": found,
        "signals": signals(store, ids, names, groups),
        "rollouts": [r for r in rollout.summaries(store, since=first * 86400, now=now)
                     if group is None or r["group"] == group],
        "help_desk": brief.fleet(store, first * 86400, ids),
        "proactive": proactive.fleet(store, first * 86400, ids),
        "mesh": mesh.fleet(store, first * 86400, ids),
        "channels": channels.fleet(store, first * 86400, ids),
        "attention": attention[:30],
        "per_home": sorted(homes.values(), key=lambda h: (h["score"] is None, h["score"] or 0)),
    })      # v0.42: each outage's and open proactive ticket's ISP ticket, and the sync state (tickets.py)


def _when(ts):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts)) if ts else "-"


def _ago(ts, now):
    s = max(0, int(now - ts))
    return f"{s // 60} min" if s < 7200 else f"{s // 3600} h" if s < 172800 else f"{s // 86400} days"


def _rollouts(r):
    out = []
    for ro in r.get("rollouts") or []:
        st = ", ".join(f"{v} {rollout.WORDS[k]}" for k, v in ro["states"].items())
        out.append(f"  rollout      {ro['name']} ({ro['version']}, {ro['homes']} homes): {ro['state']}, wave "
                   f"{ro['wave']} of {ro['waves']}; {st}")
        if ro["reason"]:
            out.append(f"               {ro['reason']}")
    return out


def text(r, now=None):
    """The report in words, for a person."""
    now = now or time.time()
    p, h, e, pr, w, sp = r["period"], r["homes"], r["experience"], r["problems"], r["early_warnings"], r["speed"]
    out = [f"Fleet report, {p['from']} to {p['to']}" + (f", group {p['group']}" if p["group"] else "") +
           f": {h['reporting']} of {h['total']} homes reporting" + (f", {h['silent']} silent" if h["silent"] else "")]
    if e["mean"] is None:
        out.append("  no care data yet (the bridges report proactive care from AIGW OS 0.29 on)")
        return "\n".join(out + _rollouts(r) + brief.fleet_lines(r) + proactive.fleet_lines(r) + mesh.fleet_lines(r) +
                         channels.fleet_lines(r) +
                         tickets.fleet_lines(r))
    change = ""
    if e["mean_before"] is not None:
        d = e["mean"] - e["mean_before"]
        change = f", {'+' if d >= 0 else ''}{d:.1f} from the {p['days']} days before"
    out.append(f"  experience   {e['mean']}% on average (median {e['median']}%){change}; "
               f"{e['good']} good, {e['fair']} fair, {e['poor']} poor homes; {e['checks']} check{'' if e['checks'] == 1 else 's'}")
    causes = ", ".join(f"{k} {v}" for k, v in sorted(pr["causes"].items(), key=lambda x: -x[1]))
    out.append(f"  problems     {pr['found']} found by the gateways: {pr['fixed_by_aigw']} fixed by AIGW itself "
               f"(no call needed), " +
               (f"{pr['cleared_by_itself']} cleared by themselves, " if pr.get("cleared_by_itself") else "") +
               f"{pr['reported_to_isp']} reported to the ISP before anyone called; "
               f"{pr['hours']} h in all; {pr['open']} open now")
    if causes:
        out.append(f"               bad checks by cause: {causes}")
    if pr["codes"]:
        out.append("               codes: " + ", ".join(f"{k} {v}" for k, v in pr["codes"].items()))
    lead = f", {w['mean_lead_hours']} h ahead on average" if w["mean_lead_hours"] is not None else ""
    out.append(f"  warnings     {w['warnings']} early warnings; {w['problems_seen_coming']} problems seen coming{lead}; "
               f"{w['open']} open now")
    if sp["homes_tested"]:
        out.append(f"  speed        {sp['homes_tested']} homes tested, median {sp['median_mbps']} Mbit/s; "
                   f"{sp['homes_below_line_speed']} homes below their line's speed ({sp['tests_below_line_speed']} tests)")
    for o in r["outages"]:
        codes = ", ".join(f"{k} {v}" for k, v in o["codes"].items())
        state = f"{o['still_open']} still affected" if o["state"] == "open" else f"over at {_when(o['end'])}"
        out.append(f"  OUTAGE       {o['homes']} of {o['homes_in_group']} homes" +
                   (f" in {o['group']}" if o["group"] else "") +
                   f" from {_when(o['start'])} (within {max(1, o['spread_s'] // 60)} min: {codes}): the ISP's "
                   f"network, not the homes; {state} (area outage {o['id']}, {o['kind']}, {o['confidence']} "
                   "confidence)")
    sg = r.get("signals") or {}
    if sg.get("homes"):
        prods = ", ".join(f"{k} {v}" + (f" ({sg['high'][k]} high)" if sg["high"].get(k) else "")
                          for k, v in sg["by_product"].items())
        out.append(f"  signals      {sg['homes']} home{'' if sg['homes'] == 1 else 's'}: {prods}")
        for o in sg["list"]:
            where = f" ({o['group']})" if o.get("group") else ""
            out.append(f"    {o['home']}{where}: {o['code']}, {o['product']} ({o['confidence']}, {o['days']} of "
                       f"{o['of_days']} days): {o['evidence']}")
    out += _rollouts(r)
    out += brief.fleet_lines(r)
    out += proactive.fleet_lines(r)
    out += mesh.fleet_lines(r)
    out += channels.fleet_lines(r)
    out += tickets.fleet_lines(r)
    if r["attention"]:
        out.append("  attention")
    for a in r["attention"]:
        where = f" ({a['group']})" if a.get("group") else ""
        if a["why"] == "problem":
            out.append(f"    {a['home']}{where}: {a['code']} ({a['area']}) for {_ago(a['since'], now)}"
                       f"{', reported' if a['reported'] else ''}" +
                       (f", part of area outage {a['area_outage']}" if a.get("area_outage") else "") +
                       (f": {a['summary']}" if a.get("summary") else ""))
        elif a["why"] == "early warning":
            when = (f"a fault in about {a['predicted_fault_days']} days" if a.get("predicted_fault_days")
                    else "worse than its normal")
            out.append(f"    {a['home']}{where}: early warning, {a['code']}: {when}")
        elif a["why"] == "poor score":
            out.append(f"    {a['home']}{where}: experience {a['score']}%" +
                       (f", mostly {a['main_cause']}" if a["main_cause"] else ""))
        else:
            out.append(f"    {a['home']}{where}: not heard from for {_ago(a['last_seen'], now)}")
    return "\n".join(out)
