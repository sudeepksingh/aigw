# SPDX-License-Identifier: Apache-2.0
"""Area fault prediction (v0.54): cortex/aigw_cortex/{linehealth,areapredict,areasim}.py.

    python3 tests/test_areapredict.py        (AIGW_LINE_DUMP=<build>/tests/line_dump: also against the C rules)

  rules        the same cases as tests/test_line.c, the same answers; and 300 random
               series through both the gateway's C and cortex's Python: the same state
  correlate    a cabinet's homes sliding together are the plant; one home alone is its
               line; two homes are too few; two cabinets together are their OLT; chance
               (a busy fleet) needs more homes
  simulation   60 homes on 3 cabinets, known plant faults, homes' own faults and noise:
               recall, precision, how many days early, tickets with and without
  cortex       reports in, a plant found and kept, the gateway's notice, one TMF621
               ticket for the plant and none for the line tickets it explains, back-test
"""
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cortex"))

from aigw_cortex import areapredict, areasim, linehealth, tickets  # noqa: E402
from aigw_cortex.store import Store  # noqa: E402

TODAY = 20000
WOB = (0.3, -0.8, 0.5, -0.2, 0.9, -0.6, 0.1, -0.4, 0.7, -0.9, 0.2, 0.6, -0.3, -0.7)


def wob(i, w):
    return w * WOB[i % 14]


def series(vals):
    n = len(vals)
    return [(TODAY - n + i, v) for i, v in enumerate(vals)]


def run(m, vals):
    return linehealth.analyse(series(vals), TODAY, m)


class Rules(unittest.TestCase):
    """tests/test_line.c's cases, through the Python copy."""

    def test_cases(self):
        t = run("margin", [10 + wob(i, 0.2) for i in range(14)])
        self.assertEqual(t["state"], "OK")
        t = run("margin", [(10 if i < 4 else 10 - 0.4 * (i - 3)) + wob(i, 0.05) for i in range(14)])
        self.assertEqual(t["state"], "Drifting")
        self.assertAlmostEqual(t["slope"], -0.4, delta=0.05)
        self.assertTrue(7 <= t["days_left"] <= 9, t)
        self.assertTrue(TODAY - 11 <= t["onset"] <= TODAY - 8, t)
        clean = t["conf"]
        self.assertIn("a fault (3 dB) in about", linehealth.words("margin", t))
        t = run("crc", [(2 if i < 7 else 40) + wob(i, 1) for i in range(14)])
        self.assertEqual((t["state"], t["onset"], t["days_left"]), ("Stepped", TODAY - 7, -1))
        self.assertEqual(run("margin", [4 if i == 9 else 10 + wob(i, 0.2) for i in range(14)])["state"], "OK")
        t = run("margin", [9 - 0.5 * i + wob(i, 0.05) for i in range(14)])
        self.assertEqual((t["state"], t["days_left"]), ("Fault", 0))
        self.assertEqual(run("margin", [10, 10, 10])["state"], "Learning")
        self.assertEqual(run("margin", [6 + 0.3 * i + wob(i, 0.05) for i in range(14)])["state"], "OK")
        t = run("sync", [(100000 if i < 6 else 100000 - 3000 * (i - 5)) + wob(i, 200) for i in range(14)])
        self.assertEqual(t["state"], "Drifting")
        self.assertTrue(2 <= t["days_left"] <= 3, t)
        t = run("margin", [2.5 + wob(i, 0.1) for i in range(14)])
        self.assertEqual(t["state"], "OK")
        self.assertIsNone(t["fault"])
        t = run("margin", [(10 if i < 4 else 10 - 0.4 * (i - 3)) + wob(i, 0.8) for i in range(14)])
        self.assertEqual(t["state"], "Drifting")
        self.assertLess(t["conf"], clean)

    @unittest.skipUnless(os.environ.get("AIGW_LINE_DUMP"), "AIGW_LINE_DUMP: the C rules (build/tests/line_dump)")
    def test_same_as_the_gateway(self):
        rng = random.Random(7)
        cases = []
        for _ in range(300):
            m = rng.choice(linehealth.METRICS)
            base = {"sync": 80000, "margin": 10, "optical": -20, "fec": 300, "crc": 3, "drops": 0.5, "latency": 5,
                    "loss": 0.2, "speed": 200}[m]
            kind, n = rng.choice(("flat", "drift", "step", "noisy")), rng.randint(3, 14)
            rate = abs(base) * rng.uniform(0.01, 0.08) * (1 if m in ("fec", "crc", "drops", "latency", "loss") else -1)
            at = rng.randint(2, 10)
            vals = []
            for i in range(n):
                v = base + abs(base) * rng.gauss(0, 0.03 if kind != "noisy" else 0.15)
                if kind == "drift" and i >= at:
                    v += rate * (i - at + 1)
                if kind == "step" and i >= at:
                    v += rate * 6
                vals.append(round(max(v, 0) if m not in ("optical",) else v, 3))
            cases.append((m, vals))
        inp = "".join(f"{m} {TODAY} " + " ".join(f"{d}:{v}" for d, v in series(vals)) + "\n" for m, vals in cases)
        out = subprocess.run([os.environ["AIGW_LINE_DUMP"]], input=inp, capture_output=True, text=True,
                             check=True).stdout.splitlines()
        self.assertEqual(len(out), len(cases))
        same = 0
        for (m, vals), line in zip(cases, out):
            st, left, conf, level, slope, onset, tau7, slope7 = line.split()
            t = run(m, vals)
            c_state = "" if st == "-" else st
            if c_state == t["state"] and abs(int(left) - t["days_left"]) <= 1:
                same += 1
            else:                    # float (C) and double (Python) may differ only at an edge
                print(f"  differs: {m} {vals}: C {line} / Python {t}")
        self.assertGreaterEqual(same, 297, f"C and Python agree on {same} of 300")


def vec(day, metric="margin", state="Drifting", onset=None, left=5, conf=80, level=6.0, base=10.0, slope=-0.4,
        tau7=0.9, slope7=-0.4, others=()):
    ms = [{"metric": metric, "state": state, "days": 14, "level": level, "base": base, "slope": slope, "fault": 3.0,
           "onset": onset if onset is not None else day - 6, "days_left": left, "conf": conf, "tau7": tau7,
           "slope7": slope7}]
    for m in others:
        ms.append({"metric": m, "state": "OK", "days": 14, "level": 5, "base": 5, "slope": 0, "fault": 35,
                   "onset": 0, "days_left": -1, "conf": 0, "tau7": 0.0, "slope7": 0.0})
    return {"v": 1, "tech": "dsl", "day": day, "status": state, "metrics": ms}


def ok(day):
    return vec(day, state="OK", left=-1, conf=0, level=10.0, slope=0.0, tau7=0.1, slope7=0.0, others=("latency",))


class Correlate(unittest.TestCase):
    def fleet(self, n_per=20, cabs=3):
        nodes = {f"c{c}h{i}": f"olt-1/cab-{c}" for c in range(1, cabs + 1) for i in range(n_per)}
        return nodes, {g: ok(TODAY) for g in nodes}

    def test_cabinet_is_the_plant(self):
        nodes, v = self.fleet()
        for i in range(14):
            v[f"c1h{i}"] = vec(TODAY, onset=TODAY - 6 + i % 3, left=3 + i % 5)
        r = areapredict.correlate(v, nodes)
        self.assertEqual(len(r["plants"]), 1)
        p = r["plants"][0]
        self.assertEqual((p["node"], p["metric"], p["homes"], p["homes_in_node"]), ("olt-1/cab-1", "margin", 14, 20))
        self.assertEqual(p["calls_in"], 3)              # the second home to reach the fault level
        self.assertEqual(p["confidence"], "high")
        self.assertLess(p["p"], 1e-6)
        self.assertEqual(r["lines"], [])                # its homes: no line of their own
        w = areapredict.plant_words(p)
        self.assertIn("The plant serving 14 of 20 homes on olt-1/cab-1 is degrading", w)
        self.assertIn("first calls likely in about 3 days", w)
        self.assertIn("One ticket for the plant, not 14", w)

    def test_early_signs_pool(self):
        nodes, v = self.fleet()
        # two homes proved it; eight more only slide in their last 7 days (no single one could say so)
        for i in range(2):
            v[f"c2h{i}"] = vec(TODAY, onset=TODAY - 5, left=6)
        for i in range(2, 10):
            v[f"c2h{i}"] = vec(TODAY, state="OK", left=-1, conf=0, level=8.6, slope=0.0, tau7=0.8, slope7=-0.3)
        r = areapredict.correlate(v, nodes)
        self.assertEqual([(p["node"], p["homes"]) for p in r["plants"]], [("olt-1/cab-2", 10)])
        self.assertGreater(r["plants"][0]["calls_in"], 0)       # their own rate, pooled: when the next ones get there
        self.assertEqual(sum(1 for t in r["plants"][0]["members"].values() if t["state"] == "Early"), 8)

    def test_one_home_is_its_own_line(self):
        nodes, v = self.fleet()
        v["c3h4"] = vec(TODAY)
        r = areapredict.correlate(v, nodes)
        self.assertEqual(r["plants"], [])
        self.assertEqual([(x["gateway"], x["metric"], x["held"]) for x in r["lines"]], [("c3h4", "margin", False)])
        self.assertTrue(areapredict.ticketable({"trend": "Drifting", "days_left": 5, "conf": 80, "held": False}))

    def test_two_homes_too_few_and_held(self):
        nodes, v = self.fleet()
        v["c3h1"] = vec(TODAY)
        v["c3h2"] = vec(TODAY, onset=TODAY - 5)
        r = areapredict.correlate(v, nodes)
        self.assertEqual(r["plants"], [])
        self.assertEqual(sorted(x["gateway"] for x in r["lines"]), ["c3h1", "c3h2"])
        # each waits: the other on its cabinet shows the same (it may be the plant: one ticket then)
        self.assertTrue(all(x["held"] for x in r["lines"]))

    def test_not_aligned_not_the_plant(self):
        nodes, v = self.fleet()
        for i in range(6):
            v[f"c1h{i}"] = vec(TODAY, onset=TODAY - 13 + 2 * i)     # began days apart: 6 lines of their own
        r = areapredict.correlate(v, nodes)
        self.assertEqual(r["plants"], [])

    def test_two_cabinets_are_the_olt(self):
        nodes, v = self.fleet()
        for c in (1, 2):
            for i in range(12):
                v[f"c{c}h{i}"] = vec(TODAY, metric="crc", level=40, base=3, slope=6, left=5, tau7=0.9, slope7=6,
                                     onset=TODAY - 5)
        for g in v:
            for e in v[g]["metrics"]:
                if e["metric"] == "crc":
                    e["fault"] = 75
        r = areapredict.correlate(v, nodes)
        self.assertEqual([(p["node"], p["homes"], p["merged"]) for p in r["plants"]], [("olt-1", 24, True)])

    def test_a_busy_fleet_needs_more(self):
        # elsewhere a third of the homes degrade on the margin anyway: 4 of 20 here is no proof
        nodes, v = self.fleet(20, 4)
        for c in (2, 3, 4):
            for i in range(7):
                v[f"c{c}h{i}"] = vec(TODAY, onset=TODAY - 12 + c + 3 * i)
        for i in range(4):
            v[f"c1h{i}"] = vec(TODAY, onset=TODAY - 5)
        r = areapredict.correlate(v, nodes)
        self.assertNotIn("olt-1/cab-1", [p["node"] for p in r["plants"]])


class Simulation(unittest.TestCase):
    def test_fleet(self):
        e = areasim.evaluate(seeds=6, homes=60, cabinets=3)
        print("\n" + areasim.text(e))
        self.assertGreaterEqual(e["plant_recall"], 0.9)
        self.assertGreaterEqual(e["plant_precision"], 0.9)
        self.assertLessEqual(e["false_alarms_per_run"], 0.2)
        self.assertGreaterEqual(e["warned_before_first_call"], 0.8)
        self.assertGreaterEqual(e["lead_days_median"], 2)
        self.assertGreaterEqual(e["member_precision"], 0.9)
        self.assertGreaterEqual(e["own_found_as_own"], 0.9)
        self.assertLess(e["tickets_with_prediction"] * 2, e["tickets_without"])


class Cortex(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.s = Store(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_reports_tickets_suppression_backtest(self):
        s, now = self.s, TODAY * 86400 + 3600
        gws = {}
        for c in (1, 2):
            for i in range(10):
                gws[(c, i)] = s.enroll(f"cab{c}-home{i}", group=f"olt-9/cab-{c}")[0]
        # four days of reports: cabinet 1's homes start sliding together on day -6; one home on cabinet 2 on its own
        for d in range(-3, 1):
            for (c, i), g in gws.items():
                day = TODAY + d
                if c == 1 and i < 8 and d >= -2:
                    v = vec(day, onset=TODAY - 6, left=2 + i)
                elif c == 2 and i == 0 and d >= -2:
                    v = vec(day, onset=TODAY - 4, left=4)
                else:
                    v = ok(day)
                rep = {"status": {"version": "0.54.0"}, "line": v}
                if d == 0 and c == 1 and i == 0:          # its gateway raised a line ticket of its own too
                    rep["proactive"] = {"tickets": [{"id": 1, "status": "Raised", "open": 1, "code": "LINE_LOSS",
                                                     "fault": "LINE", "severity": "problem", "opened": now - 600,
                                                     "since": now - 900, "told_isp": 1, "summary": "loss on the line"}]}
                if d == 0 and c == 2 and i == 0:
                    rep["proactive"] = {"tickets": [{"id": 1, "status": "Raised", "open": 1, "code": "LINE_LOSS",
                                                     "fault": "LINE", "severity": "problem", "opened": now - 600,
                                                     "since": now - 900, "told_isp": 1, "summary": "loss on the line"}]}
                s.report(g, rep, day * 86400 + 3600)
        ps = areapredict.predictions(s, open_only=True)
        self.assertEqual([(p["node"], p["homes"], p["metric"]) for p in ps], [("olt-9/cab-1", 8, "margin")])
        pid = ps[0]["id"]
        # the gateway's answer: a member is told, a home elsewhere isn't
        n = areapredict.notice(s, gws[(1, 3)])
        self.assertEqual((n["plant"], n["homes"], n["of"], n["calls_in"]), (pid, 8, 10, 3))
        self.assertEqual(areapredict.notice(s, gws[(2, 5)]), {"plant": 0})
        lines = areapredict.lines(s)
        self.assertEqual([x["gateway"] for x in lines], [gws[(2, 0)]])
        # tickets: one for the plant; the member's own line ticket suppressed (counted on the plant's); the home on
        # cabinet 2 gets its own
        cfg = tickets.check_config({"enabled": True, "dry_run": True,
                                    "events": ["outage", "proactive", "plant", "line"],
                                    "tmf621": {"url": "https://tt.example/tmf621/v4", "auth": {"type": "none"}}})
        tickets.set_meta(s, "since", now - 86400)
        tickets.scan(s, cfg, now)
        keys = sorted(r["key"] for r in s.q("SELECT key FROM isp_tickets"))
        self.assertIn(f"plant:{pid}", keys)
        self.assertIn(f"proactive:{gws[(2, 0)]}:1:{now - 600}", keys)
        self.assertNotIn(f"proactive:{gws[(1, 0)]}:1:{now - 600}", keys)
        self.assertTrue(any(k.startswith(f"line:{gws[(2, 0)]}:margin:") for k in keys), keys)
        self.assertEqual(sum(1 for k in keys if k.startswith("line:")), 1)
        doc = json.loads(s.q("SELECT doc FROM isp_tickets WHERE key = ?", (f"plant:{pid}",))[0]["doc"])
        self.assertEqual(doc["explains"], 1)            # the member's proactive line ticket, not sent
        tickets.flush(s, cfg, now)
        body = json.loads(s.q("SELECT body FROM ticket_outbox WHERE key = ? AND op = 'create'",
                              (f"plant:{pid}",))[0]["body"])
        self.assertEqual(body["ticketType"], "PredictedPlantFault")
        self.assertTrue(body["name"].startswith("Predicted plant fault"))
        self.assertTrue(any("explains 1 of" in nt["text"] for nt in body["note"]))
        self.assertEqual(len([e for e in body["relatedEntity"] if e["role"] == "affectedGateway"]), 8)
        # the same plant keeps its number next day; a later problem in it is the first fault (lead time)
        s.report(gws[(1, 1)], {"status": {}, "line": vec(TODAY + 1, onset=TODAY - 6, left=1),
                               "care": {"status": "problem", "problem": {"code": "LINE_LOSS", "area": "Internet",
                                                                          "since": now + 86400, "reported": True}}},
                 now + 86400)
        ps2 = areapredict.predictions(s, open_only=True)
        self.assertEqual(ps2[0]["id"], pid)
        self.assertAlmostEqual(ps2[0]["lead_days"], 3.0, delta=0.1)    # found on day -2, the problem on day +1
        b = areapredict.backtest(s, now + 86400)
        self.assertEqual([p["node"] for p in b["plants"]], ["olt-9/cab-1"])
        self.assertIsNotNone(b["plants"][0]["lead_days"])
        self.assertIn("would have been found", areapredict.backtest_text(b))
        # the fleet report: the plant, and no faster plan sold on its homes
        from aigw_cortex import fleet
        r = fleet.report(s, 7, now=now + 86400)
        self.assertEqual(len(r["predictions"]["open"]), 1)
        self.assertIn("faults coming", fleet.text(r))


if __name__ == "__main__":
    unittest.main(verbosity=2)
