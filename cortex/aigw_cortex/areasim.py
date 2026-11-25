# SPDX-License-Identifier: Apache-2.0
"""Simulated fleets with known faults, to measure area fault prediction honestly
(docs/area-prediction.md, "How good is it").

    python3 -m aigw_cortex predict simulate [--seeds 20] [--homes 60] [--cabinets 3] [--days 32] [--json]

Each run: `homes` homes on `cabinets` street cabinets under one OLT ("olt-1/cab-N"),
a month of each line's days, as their gateways would count them:
  - noise: every line its own normal and its own day-to-day wobble; one day in 20
    a bad day (a storm, a test); one home in 10 on Ethernet (no line numbers:
    next hop, speed and drops only); one home in 15 much noisier than the rest
  - a plant fault in cabinet 1 (always): a corroding joint, the noise margin of 70
    to 100% of its homes sliding 0.2 to 0.45 dB a day from about the same day (each
    home within a day of it), its CRC errors rising with it
  - a plant fault in the last cabinet, half the runs: CRC errors of most of its
    homes climbing 4 to 9 an hour a day (a fault in one to two weeks)
  - 3 to 6 homes anywhere with a fault of their own (a bad drop cable, wet wiring):
    the same signs, at a day of their own
Every day the gateways' vectors (linehealth.vector, as agd makes them) go through
correlate(), and what it says is scored against what was injected:
  plants       a plant fault found (recall), a plant found where there was none
               (false alarms, per run), how many days before the first home's
               fault (the first call) it was found
  homes        of the homes put in a plant, how many were in it (precision); of
               the plant's homes, how many it named (recall); homes with a fault of
               their own wrongly put in a plant
  tickets      ISP tickets with prediction (one per plant, one per line on its own)
               against one per degrading home without it
"""
import json
import random

from . import areapredict, linehealth

PLANT1, PLANT_LAST = "olt-1/cab-1", None


def _home(rng, tech):
    return {"tech": tech, "margin": rng.uniform(6, 14), "crc": rng.uniform(0.5, 4), "lat": rng.uniform(2, 9),
            "speed": rng.uniform(40, 300), "noise": 4.0 if rng.random() < 1 / 15 else 1.0,
            "faults": [], "plant": None}


def _value(h, m, d, rng):
    """One day's value of metric m on day d, with its injected faults."""
    n = h["noise"]
    bad = rng.random() < 0.05
    if m == "margin":
        v = h["margin"] + rng.gauss(0, 0.25 * n) - (rng.uniform(2, 4) if bad else 0)
    elif m == "crc":
        v = max(0.0, h["crc"] * (1 + rng.gauss(0, 0.2 * n)) * (8 if bad else 1))
    elif m == "drops":
        v = float(rng.random() < 0.08 * n)
    elif m == "latency":
        v = max(0.5, h["lat"] + rng.gauss(0, 0.3 * n) + (rng.uniform(5, 15) if bad else 0))
    else:
        v = max(1.0, h["speed"] * (1 + rng.gauss(0, 0.03 * n)))
    for f in h["faults"]:
        t = d - f["onset"]
        if t < 0:
            continue
        if f["kind"] == "margin" and m == "margin":
            v -= f["rate"] * (t + 1)
        elif f["kind"] == "margin" and m == "crc":
            v += h["crc"] * 1.5 * f["rate"] * (t + 1)
        elif f["kind"] == "crc" and m == "crc":
            v += f["rate"] * (t + 1)
        elif f["kind"] == "margin" and m == "drops" and h["margin"] - f["rate"] * (t + 1) < 4:
            v += 2
    return v


def _fault_day(h, f):
    """The day this fault makes the line a fault (the customer calls)."""
    if f["kind"] == "margin":                 # the margin under 3 dB
        return f["onset"] + int((h["margin"] - 3) / f["rate"])
    return f["onset"] + int(max(0.0, 5 * h["crc"] + 60 - h["crc"]) / f["rate"])   # CRC past 5 x base + 60


def scenario(seed, homes=60, cabinets=3, days=32):
    rng = random.Random(seed)
    H, nodes = {}, {}
    for i in range(homes):
        g = f"h{i:03d}"
        H[g] = _home(rng, "ethernet" if rng.random() < 0.1 else "dsl")
        nodes[g] = f"olt-1/cab-{i % cabinets + 1}"
    truth = {}
    dsl = [g for g in H if H[g]["tech"] == "dsl"]
    # cabinet 1: the noise margin of most of its homes slides from about the same day
    d0 = rng.randint(9, 13)
    c1 = [g for g in dsl if nodes[g] == PLANT1]
    hit = rng.sample(c1, max(3, int(len(c1) * rng.uniform(0.7, 1.0))))
    for g in hit:
        H[g]["faults"].append({"kind": "margin", "onset": d0 + rng.randint(-1, 1), "rate": rng.uniform(0.2, 0.45)})
        H[g]["plant"] = PLANT1
    truth[PLANT1] = {"homes": set(hit), "onset": d0}
    last = f"olt-1/cab-{cabinets}"
    if cabinets > 1 and rng.random() < 0.5:
        d1 = rng.randint(9, 15)
        cl = [g for g in dsl if nodes[g] == last]
        hit = rng.sample(cl, max(3, int(len(cl) * rng.uniform(0.6, 0.9))))
        for g in hit:
            H[g]["faults"].append({"kind": "crc", "onset": d1 + rng.randint(-1, 1), "rate": rng.uniform(4, 9)})
            H[g]["plant"] = last
        truth[last] = {"homes": set(hit), "onset": d1}
    own = rng.sample([g for g in dsl if not H[g]["plant"]], rng.randint(3, 6))
    for g in own:
        kind = rng.choice(("margin", "margin", "crc"))
        H[g]["faults"].append({"kind": kind, "onset": rng.randint(8, days - 6),
                               "rate": rng.uniform(0.2, 0.6) if kind == "margin" else rng.uniform(4, 10)})
    for node, t in truth.items():
        t["first_fault"] = min(_fault_day(H[g], f) for g in t["homes"] for f in H[g]["faults"])
    series = {g: {m: [] for m in (("margin", "crc", "drops", "latency", "speed") if h["tech"] == "dsl" else
                                  ("drops", "latency", "speed"))} for g, h in H.items()}
    for d in range(days):
        for g, h in H.items():
            for m in series[g]:
                series[g][m].append((d + 1, _value(h, m, d + 1, rng)))
    # a fault of a home's own that looks like its cabinet's plant fault: the same signs, within 5 days of it
    alike = {g for g in own if nodes[g] in truth and abs(H[g]["faults"][0]["onset"] - truth[nodes[g]]["onset"]) <= 5
             and H[g]["faults"][0]["kind"] == ("margin" if nodes[g] == PLANT1 else "crc")}
    return H, nodes, truth, set(own), series, alike


def run(seed, homes=60, cabinets=3, days=32, s=None):
    H, nodes, truth, own, series, alike = scenario(seed, homes, cabinets, days)
    r = {"plants": len(truth), "found": 0, "lead": [], "false_plants": set(), "in_plant": 0, "in_plant_right": 0,
         "plant_homes": sum(len(t["homes"]) for t in truth.values()), "named": set(), "own_blamed": set(),
         "own_found": set(), "tickets_with": 0, "tickets_without": 0}
    first = {}
    plant_ids, line_ids, degraded_any = set(), set(), set()
    known = []
    for today in range(8, days + 1):
        vecs = {g: linehealth.vector({m: v[:today - 1] for m, v in series[g].items()}, today, H[g]["tech"])
                for g in H}
        out = areapredict.correlate(vecs, nodes, s, known)
        known = [{"node": p["node"], "metric": p["metric"], "onset": p["onset"], "members": set(p["members"])}
                 for p in out["plants"]]
        for p in out["plants"]:
            node = p["node"]
            ok = node in truth or any(n.startswith(node + "/") for n in truth)
            if not ok:
                r["false_plants"].add(node)
                continue
            key = node if node in truth else next(n for n in truth if n.startswith(node + "/"))
            first.setdefault(key, today)
            plant_ids.add(key)
            for g in p["members"]:
                r["in_plant"] += 1
                if H[g]["plant"]:
                    r["in_plant_right"] += 1
                    r["named"].add(g)
                if g in own:
                    r["own_blamed"].add(g)
        for x in out["lines"]:
            if x["gateway"] in own:
                r["own_found"].add(x["gateway"])
            t = x["trend"]
            if areapredict.ticketable({"trend": t["state"], "days_left": t["days_left"], "conf": t["conf"],
                                       "held": x["held"]}):
                line_ids.add((x["gateway"], x["metric"]))
        for g, v in vecs.items():
            if areapredict.degraded(v, 40):
                degraded_any.add(g)
    for node, t in truth.items():
        if node in first:
            r["found"] += 1
            r["lead"].append(t["first_fault"] - first[node])
    r["tickets_with"] = len(plant_ids) + len(r["false_plants"]) + len(line_ids)
    r["tickets_without"] = len(degraded_any)
    r["own"] = len(own)
    r["alike"] = len(alike)
    r["alike_blamed"] = len(r["own_blamed"] & alike)
    return r


def evaluate(seeds=20, homes=60, cabinets=3, days=32, s=None):
    tot = {"runs": 0, "plants": 0, "found": 0, "false_alarms": 0, "lead": [], "in_plant": 0, "in_plant_right": 0,
           "plant_homes": 0, "named": 0, "own": 0, "own_blamed": 0, "own_found": 0, "tickets_with": 0,
           "tickets_without": 0, "alike": 0, "alike_blamed": 0}
    for seed in range(1, seeds + 1):
        r = run(seed, homes, cabinets, days, s)
        tot["runs"] += 1
        for k in ("plants", "found", "in_plant", "in_plant_right", "plant_homes", "own", "tickets_with",
                  "tickets_without", "alike", "alike_blamed"):
            tot[k] += r[k]
        tot["false_alarms"] += len(r["false_plants"])
        tot["lead"] += r["lead"]
        tot["named"] += len(r["named"])
        tot["own_blamed"] += len(r["own_blamed"])
        tot["own_found"] += len(r["own_found"])
    lead = sorted(tot["lead"])
    out = {
        "runs": tot["runs"], "homes": homes, "cabinets": cabinets, "days": days,
        "plant_faults": tot["plants"], "plant_recall": _ratio(tot["found"], tot["plants"]),
        "plant_precision": _ratio(tot["found"], tot["found"] + tot["false_alarms"]),
        "false_alarms_per_run": round(tot["false_alarms"] / max(1, tot["runs"]), 2),
        "lead_days_median": linehealth.median(lead) if lead else None,
        "lead_days_min": lead[0] if lead else None,
        "warned_before_first_call": _ratio(sum(1 for x in lead if x > 0), tot["plants"]),
        "member_precision": _ratio(tot["in_plant_right"], tot["in_plant"]),
        "member_recall": _ratio(tot["named"], tot["plant_homes"]),
        "own_faults": tot["own"], "own_found_as_own": _ratio(tot["own_found"], tot["own"]),
        "own_blamed_on_plant": _ratio(tot["own_blamed"] - tot["alike_blamed"], tot["own"] - tot["alike"]),
        "own_look_alike": tot["alike"], "look_alike_blamed": _ratio(tot["alike_blamed"], tot["alike"]),
        "tickets_with_prediction": tot["tickets_with"], "tickets_without": tot["tickets_without"],
    }
    return out


def _ratio(a, b):
    return round(a / b, 3) if b else None


def text(e):
    pct = lambda v: "-" if v is None else f"{100 * v:.0f}%"      # noqa: E731
    return "\n".join([
        f"Simulated: {e['runs']} runs of {e['homes']} homes on {e['cabinets']} cabinets, {e['days']} days each; "
        f"{e['plant_faults']} plant faults and {e['own_faults']} faults of a home's own injected, with noise.",
        f"  plant faults found      {pct(e['plant_recall'])} (recall); of plants named, real: "
        f"{pct(e['plant_precision'])}; false alarms {e['false_alarms_per_run']} a run",
        f"  how early               a median {e['lead_days_median']} days before the first home's fault "
        f"(the first call), {e['lead_days_min']} at worst; before the first call: {pct(e['warned_before_first_call'])}",
        f"  homes in a plant        right {pct(e['member_precision'])} (precision); of the plant's homes named: "
        f"{pct(e['member_recall'])} (recall: some homes' lines were still fine when it was found)",
        f"  homes' own faults       seen as their own line: {pct(e['own_found_as_own'])}; wrongly put in a plant: "
        f"{pct(e['own_blamed_on_plant'])}; of the {e['own_look_alike']} that look alike (the faulty cabinet's signs "
        f"within 5 days of it) put in the plant: {pct(e['look_alike_blamed'])} (can't be told apart)",
        f"  ISP tickets             {e['tickets_with_prediction']} with prediction, against "
        f"{e['tickets_without']} (one per degrading home) without",
    ])


def as_json(e):
    return json.dumps(e, indent=2)
