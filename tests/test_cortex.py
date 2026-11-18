# SPDX-License-Identifier: Apache-2.0
"""Cortex unit tests: labels from people's answers, .agm stamping, usign keys
(both Ed25519 backends), the HTTP API's auth and the MCP server's guard.

    python3 tests/test_cortex.py
"""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cortex"))

from aigw_cortex import agm, usign  # noqa: E402
from aigw_cortex.mcp import Server  # noqa: E402
from aigw_cortex.store import APPROVED, REJECTED, Store, feedback  # noqa: E402

MODEL = os.path.join(HERE, "..", "models", "domain-risk.agm")


class Feedback(unittest.TestCase):
    def test_answers(self):
        self.assertEqual(feedback({"state": "rejected", "automatic": 0}), REJECTED)
        self.assertEqual(feedback({"state": "reverted", "automatic": 1}), REJECTED)
        self.assertEqual(feedback({"state": "active", "automatic": 0}), APPROVED)
        self.assertEqual(feedback({"state": "expired", "automatic": 0}), APPROVED)
        self.assertIsNone(feedback({"state": "active", "automatic": 1}))     # nobody looked
        self.assertIsNone(feedback({"state": "proposed", "automatic": 0}))
        self.assertIsNone(feedback({"state": "lapsed", "automatic": 0}))

    def test_report_labels(self):
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gid, _ = s.enroll("home")
            rep = {"status": {"version": "0.8.0", "devices": 2},
                   "alerts": [{"id": 7, "kind": "dga", "subject": "Kofamuveliza.top", "first": 1},
                              {"id": 8, "kind": "no-dns", "subject": "203.0.113.9", "first": 2}],
                   "decisions": [{"id": 1, "state": "rejected", "alert": 7, "automatic": 0, "created": 5},
                                 {"id": 2, "state": "active", "alert": 0, "automatic": 0, "created": 6},
                                 {"id": 3, "state": "garbage"}]}
            self.assertEqual(s.report(gid, rep), 1)
            labels = s.labels("dga")
            self.assertEqual([(x["subject"], x["label"]) for x in labels], [("kofamuveliza.top", 0)])
            self.assertEqual(s.history(gid)[0]["devices"], 2)
            s.report(gid, {"decisions": "not a list"})       # bad input never breaks the store


class Intel(unittest.TestCase):
    def test_list(self):
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            a, _ = s.enroll("a")
            b, _ = s.enroll("b")
            c, _ = s.enroll("c")
            s.report(a, {"alerts": [{"id": 1, "kind": "dga", "subject": "Twice.example", "first": 1},
                                    {"id": 2, "kind": "dga", "subject": "once.example", "first": 1}]})
            s.report(b, {"alerts": [{"id": 1, "kind": "dga", "subject": "twice.example", "first": 1}]})
            s.report(c, {"alerts": [{"id": 1, "kind": "dga", "subject": "twice.example", "first": 1}]})
            s.label("confirmed.example", "dga", 1)
            s.label("both.example", "dga", 1)
            s.label("both.example", "dga", 0, source="someone else")   # one false alarm keeps it off
            names = dict(s.intel(min_homes=2))
            self.assertEqual(set(names), {"twice.example", "confirmed.example"})
            self.assertIn("3 homes", names["twice.example"])
            self.assertEqual(set(dict(s.intel(min_homes=1))), {"twice.example", "once.example", "confirmed.example"})


class PluginDigest(unittest.TestCase):
    def test_digest(self):
        sys.path.insert(0, os.path.join(HERE, "..", "cortex"))
        from aigw_cortex.__main__ import plugin_digest
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "lib"))
            for name, body in (("manifest.json", b"{}"), ("run.sh", b"x"), ("lib/a.sh", b"y"), ("plugin.sig", b"z")):
                with open(os.path.join(d, name), "wb") as f:
                    f.write(body)
            text = plugin_digest(d).decode()
            lines = text.splitlines()
            self.assertEqual([ln.split("  ")[1] for ln in lines], ["lib/a.sh", "manifest.json", "run.sh"])
            self.assertTrue(all(len(ln.split("  ")[0]) == 128 for ln in lines))
            self.assertNotIn("plugin.sig", text)


class AgmStamp(unittest.TestCase):
    def test_stamp_keeps_model(self):
        data = open(MODEL, "rb").read()
        out = agm.stamp(data, version=5)
        h1, s1 = agm.read(data)
        h2, s2 = agm.read(out)
        self.assertEqual(h1, h2)
        self.assertEqual([x for x in s1 if x[0] != agm.META], [x for x in s2 if x[0] != agm.META])
        self.assertEqual(agm.meta(out)["version"], 5)
        self.assertEqual(agm.meta(out)["schema"], agm.meta(data)["schema"])
        with self.assertRaises(ValueError):
            agm.read(b"AGM1" + b"\0" * 10)


class Usign(unittest.TestCase):
    def test_rfc8032_pure(self):
        seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        self.assertEqual(usign.public_from_seed(seed, pure=True).hex(),
                         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        self.assertTrue(usign.sign_raw(seed, b"", pure=True).hex().startswith("e5564300c360ac72"))

    def test_files(self):
        sec, pub, fp = usign.generate()
        sig = usign.sign(sec, b"model bytes")
        self.assertTrue(sig.startswith(f"untrusted comment: signed by key {fp}\n"))
        self.assertTrue(usign.verify(pub, sig, b"model bytes"))
        self.assertFalse(usign.verify(pub, sig, b"model bytes!"))
        _, other, _ = usign.generate()
        self.assertFalse(usign.verify(other, sig, b"model bytes"))
        fp2, seed = usign.read_secret(sec)
        self.assertEqual(usign.sign_raw(seed, b"x", pure=True), usign.sign_raw(seed, b"x"))
        self.assertTrue(usign.verify_raw(usign.read_public(pub)[1], b"x", usign.sign_raw(seed, b"x"), pure=True))


class Api(unittest.TestCase):
    def test_auth_and_paths(self):
        from http.server import ThreadingHTTPServer
        from aigw_cortex.server import make_handler
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            _, token = s.enroll("home")
            httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(s, log=False))
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            base = f"http://127.0.0.1:{httpd.server_address[1]}"

            def req(path, tok=token, body=None):
                r = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                                           headers={"Authorization": f"Bearer {tok}"} if tok else {})
                try:
                    with urllib.request.urlopen(r) as resp:
                        return resp.status, resp.read()
                except urllib.error.HTTPError as e:
                    return e.code, e.read()
            self.assertEqual(req("/v1/models", tok=None)[0], 401)
            self.assertEqual(req("/v1/models", tok="wrong")[0], 401)
            self.assertEqual(req("/v1/models")[0], 200)
            self.assertEqual(req("/v1/models/../cortex.db")[0], 404)
            self.assertEqual(req("/v1/models/..%2fcortex.db")[0], 404)
            self.assertEqual(req("/v1/report", body={"status": {"devices": 1}})[0], 200)
            self.assertEqual(req("/v1/report", body=[1])[0], 400)
            httpd.shutdown()


class Fed(unittest.TestCase):
    def test_round(self):
        from aigw_cortex import fed
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            st = fed.Settings(min_homes=2, gap=0, noise=1.0)
            g = [s.enroll(n)[0] for n in ("a", "b", "c")]
            r = fed.round_for_gateway(s, st)
            self.assertEqual((r["round"], r["weights"]), (1, fed.IDENTITY))
            up = lambda gw, delta, rnd=1, n=3: fed.accept(s, st, gw, {"round": rnd, "delta": delta, "n": n,
                                                                    "noise": 1.0, "clip": 1.0})
            z = [0.0] * fed.N_W
            self.assertEqual(up(g[0], [1, 2])[0], 400)
            self.assertEqual(up(g[0], z, n=0)[0], 400)
            self.assertEqual(up(g[0], z[:-1] + [float("nan")])[0], 400)
            self.assertEqual(up(g[0], z, rnd=7)[0], 409)
            self.assertEqual(up(g[0], [0.0] * 13 + [-0.4])[0], 200)
            self.assertEqual(up(g[0], z)[0], 409)                     # one change per home per round
            code, ans = up(g[1], [0.0] * 13 + [-0.2])                 # second home: the round closes
            self.assertEqual((code, ans["published"]), (200, 1))
            self.assertAlmostEqual(fed.round_for_gateway(s, st)["weights"][-1], -0.3)   # mean, not sum
            self.assertEqual(fed.round_for_gateway(s, st)["round"], 2)
            m = [x for x in s.latest_models() if x["name"] == "domain-adapter"][0]
            with open(os.path.join(s.models_dir(), m["file"]), "rb") as f:
                data = f.read()
            self.assertTrue(data.startswith(b"# aigw-adapter domain-adapter\n# version 1\n"))
            self.assertIn(b"weights 1 0 0 0 0 0 0 0 0 0 0 0 0 -0.3", data)
            self.assertTrue(os.path.exists(os.path.join(s.models_dir(), m["file"] + ".sig")))
            # closing early with one home (fed aggregate) works; an average that would
            # wreck the trees' weight is not published and the round's layer stays
            up(g[0], [-0.4] + z[1:], rnd=2)
            r = fed.aggregate(s, st, force=True)
            self.assertEqual((r["homes"], r["version"]), (1, 2))
            for gw in g[:2]:
                up(gw, [-0.9] + z[1:], rnd=3)
            last = fed.status(s, st)["rounds"]
            self.assertEqual((last[1]["round"], last[1]["state"]), (3, "refused"))
            self.assertIn("weight of the trees", last[1]["note"])
            self.assertAlmostEqual(fed.round_for_gateway(s, st)["weights"][0], 0.6)
            self.assertAlmostEqual(fed.epsilon(1.0), 5.30, places=2)          # zCDP, delta 1e-5
            self.assertAlmostEqual(fed.epsilon(1.0, rounds=4), 11.6, places=1)  # adds up over rounds


def care_day(day, checks, good, **kw):
    """A day's row as a bridge sends it (agd's care_days_blob)."""
    d = {"day": day, "checks": checks, "good": good, "Internet": 0, "DNS": 0, "WiFi": 0, "Device": 0, "AIGW": 0,
         "problems": 0, "reported": 0, "fixed_by_aigw": 0, "problem_s": 0, "early_warnings": 0, "warned_of": 0,
         "warning_lead_s": 0, "speed_tests": 0, "speed_kbps_sum": 0, "speed_kbps_min": 0, "speed_low": 0}
    d.update(kw)
    return d


class Fleet(unittest.TestCase):
    def test_report(self):
        from aigw_cortex import fleet
        now = 20000 * 86400 + 12 * 3600
        today = now // 86400
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gws = {}
            for i in range(12):
                gid, _ = s.enroll("home%02d" % i, "north" if i < 10 else "south")
                gws[i] = gid
                s.x("UPDATE gateways SET last_seen = ? WHERE id = ?", (now - 60, gid))
            # every home: a week before at 99%, this week by its number
            for i, gid in gws.items():
                days = [care_day(today - 7 - k, 144, 143) for k in range(7)]
                good = 144 if i % 3 else 100                       # homes 0, 3, 6, 9: poor (69%)
                days += [care_day(today - k, 144, good, Internet=144 - good, problems=1 if good < 144 else 0,
                                  reported=1 if good < 144 else 0) for k in range(7)]
                if i == 1:
                    days[-1].update(WiFi=4, good=140, problems=2, fixed_by_aigw=2, early_warnings=1, warned_of=1,
                                    warning_lead_s=7200, speed_tests=1, speed_kbps_sum=95000, speed_kbps_min=95000)
                care = {"days": days}
                # six homes in the north lose the internet within 10 minutes: an outage
                if 2 <= i <= 7:
                    care["problem"] = {"code": "ISP_DOWN", "area": "Internet", "since": now - 3600 + i * 100,
                                       "reported": 1, "summary": "the internet doesn't answer"}
                if i == 11:     # one home in the south, alone: not an outage
                    care["problem"] = {"code": "LINE_DOWN", "area": "Internet", "since": now - 3000, "reported": 1}
                if i == 8:
                    care["early_warning"] = {"status": "warning", "metric": "InternetLatency", "area": "Internet",
                                             "since": now - 7200, "predicted": 1, "predicted_fault_days": 5,
                                             "summary": "rising"}
                s.care(gid, care, now)
            r = fleet.report(s, 7, now=now)
            self.assertEqual(r["homes"]["total"], 12)
            self.assertEqual(r["homes"]["reporting"], 12)
            self.assertEqual(r["experience"]["poor"], 4)
            self.assertEqual(r["experience"]["mean_before"], 99.3)
            self.assertLess(r["experience"]["mean"], r["experience"]["mean_before"])
            self.assertEqual(r["problems"]["fixed_by_aigw"], 2)
            self.assertEqual(r["problems"]["causes"]["Internet"], 4 * 7 * 44)
            self.assertEqual(r["early_warnings"]["problems_seen_coming"], 1)
            self.assertEqual(r["early_warnings"]["mean_lead_hours"], 2.0)
            self.assertEqual(r["speed"]["median_mbps"], 95.0)
            # the outage: the north's six homes, not the south's one
            self.assertEqual(len(r["outages"]), 1, r["outages"])
            o = r["outages"][0]
            self.assertEqual((o["group"], o["homes"], o["homes_in_group"], o["still_open"]), ("north", 6, 10, 6))
            self.assertEqual(o["codes"], {"ISP_DOWN": 6})
            # attention: the open problems (oldest first), then the warning, then poor scores
            a = r["attention"]
            self.assertEqual([x["why"] for x in a[:7]], ["problem"] * 7)
            self.assertEqual(a[0]["home"], "home02")
            self.assertEqual(a[7]["why"], "early warning")
            self.assertEqual(a[7]["predicted_fault_days"], 5)
            self.assertTrue(all(x["why"] == "poor score" for x in a[8:12]))
            t = fleet.text(r, now)
            self.assertIn("OUTAGE       6 of 10 homes in north", t)
            self.assertIn("2 fixed by AIGW itself", t)
            # three homes come back: closed, the rest still open
            for i in (2, 3, 4):
                s.care(gws[i], {"days": []}, now + 600)
            o = fleet.report(s, 7, now=now + 600)["outages"][0]
            self.assertEqual(o["still_open"], 3)
            # one group only
            south = fleet.report(s, 7, group="south", now=now)
            self.assertEqual((south["homes"]["total"], south["outages"]), (2, []))
            # the AI assistant asks for it (read only)
            out = Server(s).call("fleet_report", {"days": 7, "group": "north"})
            self.assertEqual(out["homes"]["total"], 10)

    def test_score_of_time(self):
        """v0.29.1: the score is of the time watched, not of the checks (bad
        minutes are rechecked every minute, fine ones every ten)."""
        from aigw_cortex import fleet
        now = 20000 * 86400 + 12 * 3600
        today = now // 86400
        with tempfile.TemporaryDirectory() as d:
            # a database from v0.29.0: care_days without the times
            s = Store(d)
            s.db.executescript("DROP TABLE care_days; CREATE TABLE care_days (gateway TEXT, day INTEGER, "
                               "checks INTEGER, good INTEGER, internet INTEGER, dns INTEGER, wifi INTEGER, "
                               "device INTEGER, aigw INTEGER, problems INTEGER, reported INTEGER, fixed INTEGER, "
                               "problem_s INTEGER, warnings INTEGER, warned INTEGER, lead_s INTEGER, "
                               "speed_tests INTEGER, speed_kbps_sum INTEGER, speed_kbps_min INTEGER, "
                               "speed_low INTEGER, PRIMARY KEY (gateway, day));")
            s = Store(d)
            gid, _ = s.enroll("home")
            # the PC access point's day: 147 checks, 97 of them bad, 7 h 30 m watched, 1 h 39 m of problems
            # v0.29.2: of its 7 problems, 6 were over though AIGW changed nothing (advise mode)
            s.care(gid, {"days": [care_day(today, 147, 50, WiFi=97, problems=7, problem_s=5951, reported=1,
                                           watched_s=27000, ok_s=27000 - 5951, cleared_by_itself=6)]}, now)
            r = fleet.report(s, 1, now=now)
            h = r["per_home"][0]
            self.assertEqual(h["score"], 78.0)
            self.assertEqual(h["watched_h"], 7.5)
            self.assertEqual((h["fixed_by_aigw"], h["cleared_by_itself"]), (0, 6))
            self.assertEqual((r["problems"]["fixed_by_aigw"], r["problems"]["cleared_by_itself"]), (0, 6))
            self.assertIn("0 fixed by AIGW itself (no call needed), 6 cleared by themselves", fleet.text(r, now))

    def test_signals(self):
        """v0.32: each gateway's signals for the ISP, in the order a team calls."""
        from aigw_cortex import fleet
        now = 20000 * 86400 + 12 * 3600
        today = now // 86400

        def sig(code, kind, product, conf, days=5):
            return {"code": code, "kind": kind, "product": product, "confidence": conf, "days": days, "of_days": 7,
                    "since": (today - days + 1) * 86400, "evidence": f"{code} evidence"}

        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gids = [s.enroll("home%d" % i, "east")[0] for i in range(4)]
            days = [care_day(today - k, 144, 144) for k in range(7)]
            s.care(gids[0], {"days": days, "offers": [sig("PLAN_UPGRADE", "upsell", "faster plan", "medium", 3),
                                                      sig("MESH_WIFI", "upsell", "mesh Wi-Fi", "high")]}, now)
            s.care(gids[1], {"days": days, "offers": [sig("PLAN_UPGRADE", "upsell", "faster plan", "high")]}, now)
            s.care(gids[2], {"days": days, "offers": [sig("LINE_REPAIR", "repair", "a line repair", "high", 2),
                                                      sig("RETENTION_RISK", "retention", "a call from the ISP",
                                                          "medium", 7)]}, now)
            s.care(gids[3], {"days": days}, now)        # an older gateway, or offers.share 0: nothing
            r = fleet.report(s, 7, now=now)["signals"]
            self.assertEqual(r["homes"], 3)
            self.assertEqual(r["by_product"]["faster plan"], 2)
            self.assertEqual(r["high"]["faster plan"], 1)
            # repairs first, then who may leave, then sales by confidence
            self.assertEqual([(o["home"], o["code"]) for o in r["list"]],
                             [("home2", "LINE_REPAIR"), ("home2", "RETENTION_RISK"), ("home0", "MESH_WIFI"),
                              ("home1", "PLAN_UPGRADE"), ("home0", "PLAN_UPGRADE")])
            t = fleet.text(fleet.report(s, 7, now=now), now)
            self.assertIn("signals      3 homes: faster plan 2 (1 high)", t)
            self.assertIn("home2 (east): LINE_REPAIR, a line repair (high, 2 of 7 days): LINE_REPAIR evidence", t)
            # a signal that is gone from the next report is closed; a report without the key changes nothing
            s.care(gids[0], {"days": days, "offers": [sig("MESH_WIFI", "upsell", "mesh Wi-Fi", "high")]}, now + 3600)
            s.care(gids[1], {"days": days}, now + 3600)
            r = fleet.report(s, 7, now=now + 3600)["signals"]
            self.assertEqual(r["by_product"].get("faster plan"), 1)
            self.assertEqual(r["codes"]["MESH_WIFI"], 1)
            # the bridge's report carries them inside care
            s.report(gids[3], {"status": {"version": "0.32.0"}, "care": {"days": days, "offers": [
                sig("SMART_QUEUE", "upsell", "smart queueing", "medium", 4)]}})
            self.assertEqual(fleet.report(s, 7)["signals"]["codes"].get("SMART_QUEUE"), 1)

    def test_old_reports(self):
        from aigw_cortex import fleet
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gid, _ = s.enroll("old")
            s.report(gid, {"status": {"version": "0.28.0"}})     # no care in it
            r = fleet.report(s, 7)
            self.assertEqual((r["homes"]["total"], r["homes"]["reporting"], r["experience"]["mean"]), (1, 0, None))
            self.assertIn("no care data yet", fleet.text(r))


class Mcp(unittest.TestCase):
    def test_guard(self):
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            srv = Server(s)
            r = srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                            "params": {"name": "list_devices", "arguments": {}}})
            self.assertTrue(r["result"]["isError"])            # nobody reported yet
            gid, _ = s.enroll("home")
            for args in ({"action": "quarantine", "target": "", "reason": "x"},
                         {"action": "rm -rf", "target": "tv", "reason": "x"},
                         {"action": "block", "target": "x.com", "reason": ""}):
                r = srv.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                "params": {"name": "propose_action", "arguments": args}})
                self.assertTrue(r["result"]["isError"], args)
            self.assertEqual(s.proposals(), [])
            self.assertIsNone(srv.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))
            self.assertEqual(srv.handle({"jsonrpc": "2.0", "id": 3, "method": "nope"})["error"]["code"], -32601)


def a_brief(fault, code, visit="No", fix=None, made=0, serial="", asked=None):
    """A gateway's brief, as its bridge reports it (agd's brief_report_blob)."""
    return {"fault": fault, "code": code, "confidence": "high", "current": True,
            "verdict": f"{fault}: {code} (a laptop)", "evidence": [{"source": "now", "clock": "20:02", "text": code}],
            "done": [], "tell": "Tell them.", "fix": fix or {"possible": False, "proposed": False, "text": "Nothing."},
            "visit": visit, "visit_reason": visit + ": because.", "from": made - 3600, "to": made, "checked": False,
            "names": False, "made": made, "serial": serial,
            "asked": asked or {"total": 0, "fault": {}, "visits": 0, "remote_fix": 0, "proposed": 0, "since": 0}}


class HelpDesk(unittest.TestCase):
    """v0.37: the help desk's answer about one home (brief.py), the MCP tool, the fleet report's counts."""

    def test_answer(self):
        from aigw_cortex import brief
        now = 1795469400
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gid, _ = s.enroll("home-a", "east")
            s.report(gid, {"status": {"version": "0.37.0"},
                           "brief": a_brief("LINE", "LINE_LOSS", "Yes", made=now - 120, serial="A1B2C3D4E5F6")}, now - 120)
            # by name, id, or the serial number the ACS knows
            for ref in ("home-a", gid, "A1B2C3D4E5F6", "a1b2c3d4e5f6"):
                b = brief.answer(s, ref, now=now)
                self.assertEqual((b["home"], b["fault"], b["code"], b["visit"]), ("home-a", "LINE", "LINE_LOSS", "Yes"), ref)
            self.assertEqual(b["age_s"], 120)
            self.assertIn("As of 2 min ago", b["freshness"])
            self.assertNotIn("period_note", b)
            with self.assertRaises(ValueError):
                brief.answer(s, "nobody", now=now)
            # old data: says so, and where to get a fresh one; another period: where to ask
            b = brief.answer(s, "home-a", "tonight 20:00-22:00", now=now + 3600)
            self.assertIn("62 min old", b["freshness"])
            self.assertIn("SupportBrief()", b["freshness"])
            self.assertIn("tonight 20:00-22:00", b["period_note"])
            self.assertIn("Not through the bridge", b["refresh"])
            t = brief.text(b)
            self.assertIn("home-a: LINE (LINE_LOSS, high confidence)", t)
            self.assertIn("visit     Yes: because.", t)

    def test_propose_only(self):
        from aigw_cortex import brief
        now = 1795469400
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gid, _ = s.enroll("home-b")
            fix = {"possible": True, "action": "sqm", "target": "eth0:45000:0", "text": "Turn on smart queueing."}
            s.report(gid, {"status": {}, "brief": a_brief("NONE", "LINE_FULL", fix=fix, made=now)}, now)
            b = brief.answer(s, "home-b", now=now)
            self.assertNotIn("proposal", b)
            self.assertEqual(s.proposals(), [])                 # asking changes nothing
            b = brief.answer(s, "home-b", propose=True, now=now)
            p = s.proposals()
            self.assertEqual([(x["action"], x["target"], x["state"]) for x in p], [("sqm", "eth0:45000:0", "queued")])
            self.assertIn("waits there for a person's yes", b["proposal"]["note"])
            # a fix that waits already is not proposed again; one without a target (a device's MAC stays home) isn't either
            for fx in ({"possible": True, "action": "approve", "decision": 7, "text": "Approve 7."},
                       {"possible": True, "action": "stay24", "text": "Keep a laptop on 2.4 GHz."},
                       {"possible": True, "action": "quarantine", "target": "tv", "text": "never"}):
                s.report(gid, {"status": {}, "brief": a_brief("HOME_WIFI", "WIFI_HOPPING", fix=fx, made=now)}, now)
                brief.answer(s, "home-b", propose=True, now=now)
            self.assertEqual(len(s.proposals()), 1)

    def test_old_gateway(self):
        """A gateway before 0.37 sends no brief: an answer from proactive care alone, careful."""
        from aigw_cortex import brief
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            gid, _ = s.enroll("old")
            s.report(gid, {"status": {"version": "0.36.0"}, "care": {"status": "problem", "problem": {
                "code": "ISP_DOWN", "area": "Internet", "since": 100, "reported": True, "summary": "down"}}})
            b = brief.answer(s, "old")
            self.assertEqual((b["fault"], b["code"], b["confidence"], b["visit"]), ("LINE", "ISP_DOWN", "low", "No"))
            self.assertIn("before 0.37", b["source"])
            s.report(gid, {"status": {"version": "0.36.0"}})
            self.assertEqual(brief.answer(s, "old")["fault"], "UNKNOWN")

    def test_fleet_and_mcp(self):
        from aigw_cortex import fleet
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            srv = Server(s)
            gids = [s.enroll("home%d" % i)[0] for i in range(3)]
            s.report(gids[0], {"status": {}, "brief": a_brief("LINE", "LINE_LOSS", "Yes", made=now, asked={
                "total": 5, "fault": {"LINE": 2, "HOME_WIFI": 3}, "visits": 1, "remote_fix": 2, "proposed": 1})}, now)
            s.report(gids[1], {"status": {}, "brief": a_brief("HOME_WIFI", "WIFI_COVERAGE", made=now)}, now)
            s.report(gids[2], {"status": {}, "brief": a_brief("NONE", "OK", made=now)}, now)
            for i, g in enumerate(("home0", "home1", "home2", "home1")):
                r = srv.handle({"jsonrpc": "2.0", "id": i, "method": "tools/call",
                                "params": {"name": "support_brief", "arguments": {"gateway": g}}})
                self.assertFalse(r["result"]["isError"], r)
            out = json.loads(r["result"]["content"][0]["text"])
            self.assertEqual((out["home"], out["fault"]), ("home1", "HOME_WIFI"))
            r = srv.handle({"jsonrpc": "2.0", "id": 9, "method": "tools/call",
                            "params": {"name": "support_brief", "arguments": {"gateway": "nobody"}}})
            self.assertTrue(r["result"]["isError"])
            names = [t["name"] for t in srv.handle({"jsonrpc": "2.0", "id": 10, "method": "tools/list"})["result"]["tools"]]
            self.assertIn("support_brief", names)
            h = fleet.report(s, 7)["help_desk"]
            self.assertEqual((h["briefs"], h["homes"], h["fault"]), (4, 3, {"HOME_WIFI": 2, "LINE": 1, "NONE": 1}))
            self.assertEqual((h["visit_advised"], h["no_visit"]), (1, 3))
            self.assertEqual(h["on_gateways"], {"briefs": 5, "fault": {"HOME_WIFI": 3, "LINE": 2}, "visit_advised": 1,
                                                "remote_fix": 2})
            t = fleet.text(fleet.report(s, 7))
            self.assertIn("help desk    4 briefs for 3 homes: HOME_WIFI 2, LINE 1, NONE 1; a technician advised for 1", t)
            self.assertIn("asked of the gateways themselves (USP, TR-069, agctl): 5", t)
            # a group's report counts only its homes
            s.set_group("home2", "west")
            self.assertEqual(fleet.report(s, 7, "west")["help_desk"]["briefs"], 1)


if __name__ == "__main__":
    unittest.main()
