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


class Dashboard(unittest.TestCase):
    """v0.38: the operator dashboard's JSON, its token, escaping, and no device
    names from homes that don't share them."""

    EVIL = "<img src=x onerror=alert(1)>"

    def fleet(self, d):
        from aigw_cortex import rollout
        now = 20000 * 86400 + 12 * 3600
        today = now // 86400
        s = Store(d)
        a, _ = s.enroll("named", "north")
        b, _ = s.enroll("anon<b>", "north")
        days = [care_day(today - k, 144, 140, WiFi=4, problems=1) for k in range(3)]
        problem = {"code": "WIFI_WEAK", "area": "WiFi", "since": now - 600, "reported": 0,
                   "summary": "Kid's tablet is far from the router " + self.EVIL}
        alert = {"id": 1, "kind": "dga", "subject": "x.example", "device": "Kid's tablet", "reason": self.EVIL,
                 "first": 1, "count": 2}
        decs = [{"id": 1, "state": "proposed", "action": "quarantine", "target": "Kid's tablet", "device": "Kid's tablet",
                 "origin": "agd", "reason": "it looked up " + self.EVIL, "created": now - 60},
                {"id": 2, "state": "active", "action": "sqm", "target": "wan", "origin": "agd", "created": now - 999},
                {"id": 3, "state": "proposed", "action": "block", "target": "bad.example", "origin": "agd",
                 "created": now - 30}]
        # devices = names: the bridge sends them with hostname and mac
        s.report(a, {"status": {"version": "0.38.0", "devices": 1, "alerts": 1},
                     "devices": [{"hostname": "Kid's tablet", "mac": "02:00:00:00:00:01"}],
                     "alerts": [alert], "decisions": decs, "care": {"days": days, "problem": problem,
                                                                    "status": "problem"}}, now)
        # devices = anonymous: ids only. A bridge that let names through anyway is not trusted for them
        s.report(b, {"status": {"version": "0.38.0", "devices": 1}, "devices": [{"id": "a1b2c3d4e5f6"}],
                     "alerts": [alert], "decisions": decs, "care": {"days": days, "problem": problem}}, now)
        s.x("UPDATE gateways SET last_seen = ?", (now - 60,))
        rollout.create(s, "r1", "0.38.0", "", waves=[50, 100], now=now)
        return s, a, b, now

    def serve(self, s, local=True):
        from http.server import ThreadingHTTPServer
        from aigw_cortex.dashboard import make_handler
        httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(s, "sekret-token", local, log=False))
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        base = f"http://127.0.0.1:{httpd.server_address[1]}"

        def req(path, tok=None, body=None, host=None, raw=None):
            h = {"Authorization": f"Bearer {tok}"} if tok else {}
            if host:
                h["Host"] = host
            data = raw if raw is not None else json.dumps(body).encode() if body is not None else None
            r = urllib.request.Request(base + path, data=data, headers=h, method="POST" if data is not None else "GET")
            try:
                with urllib.request.urlopen(r) as resp:
                    return resp.status, resp.read(), resp.headers
            except urllib.error.HTTPError as e:
                return e.code, e.read(), e.headers
        return req

    def test_endpoints(self):
        with tempfile.TemporaryDirectory() as d:
            s, a, b, _ = self.fleet(d)
            req = self.serve(s)
            code, body, h = req("/")
            self.assertEqual(code, 200)
            self.assertIn("default-src 'none'", h["Content-Security-Policy"])
            self.assertEqual(h["X-Content-Type-Options"], "nosniff")
            for path in ("/static/dashboard.js", "/static/dashboard.css"):
                self.assertEqual(req(path)[0], 200)
            # nothing from outside: no CDN, no build step
            self.assertNotRegex(body.decode(), r"(src|href)=\"(https?:)?//")
            code, body, h = req("/api/overview?days=7")
            r = json.loads(body)
            self.assertEqual((code, r["homes"]["total"], r["groups"]), (200, 2, ["north"]))
            self.assertTrue(h["Content-Type"].startswith("application/json"))
            self.assertTrue(all(x.get("words") for x in r["attention"]))
            self.assertEqual(r["queueing"]["sqm"]["on"], 2)            # the kept "sqm" decisions
            self.assertFalse(r["queueing"]["calls"]["reported"])
            ro = json.loads(req("/api/rollouts")[1])["rollouts"]
            self.assertEqual((ro[0]["name"], ro[0]["state"], ro[0]["can"]), ("r1", "running",
                                                                            {"pause": True, "abort": True}))
            self.assertNotIn("list", ro[0])                             # counts, never homes' names
            gws = json.loads(req("/api/gateways")[1])["gateways"]
            self.assertEqual({g["name"]: g["waiting"] for g in gws}, {"named": 2, "anon<b>": 2})
            g = json.loads(req(f"/api/gateways/{a}")[1])
            self.assertEqual((g["name"], g["waiting"], g["decisions"][0]["state"]), ("named", 2, "proposed"))
            self.assertEqual(req("/api/gateways/nobody")[0], 404)
            self.assertEqual(req("/api/nothing")[0], 404)
            self.assertEqual(req("/api/overview?days=x")[0], 400)

    def test_access(self):
        with tempfile.TemporaryDirectory() as d:
            s, *_ = self.fleet(d)
            req = self.serve(s, local=True)
            self.assertEqual(req("/api/rollouts")[0], 200)              # localhost: reading needs no token
            self.assertEqual(req("/api/rollouts", host="evil.example:8090")[0], 401)   # DNS rebinding
            self.assertEqual(req("/api/rollouts", tok="sekret-token", host="evil.example")[0], 200)
            # every change needs the token, even on localhost
            self.assertEqual(req("/api/rollouts/r1/pause", body={"reason": "x"})[0], 401)
            self.assertEqual(req("/api/rollouts/r1/pause", tok="wrong", body={"reason": "x"})[0], 401)
            self.assertEqual(req("/api/rollouts/r1/pause", tok="sekret-token", body={"reason": " "})[0], 400)
            self.assertEqual(req("/api/rollouts/r1/pause", tok="sekret-token", raw=b"not json")[0], 400)
            code, body, _ = req("/api/rollouts/r1/pause", tok="sekret-token", body={"reason": "calls from homes"})
            self.assertEqual(code, 200)
            r1 = json.loads(body)["rollouts"][0]
            self.assertEqual(r1["state"], "paused")
            self.assertIn("Paused by a person (dashboard) (calls from homes)", r1["log"][-1]["text"])
            # resuming stays at the command line
            self.assertEqual(req("/api/rollouts/r1/resume", tok="sekret-token", body={"reason": "x"})[0], 403)
            self.assertEqual(req("/api/rollouts/r1/pause", tok="sekret-token", body={"reason": "again"})[0], 409)
            self.assertEqual(req("/api/rollouts/r1/abort", tok="sekret-token", body={"reason": "bad"})[0], 200)
            self.assertEqual(json.loads(req("/api/rollouts")[1])["rollouts"][0]["state"], "aborted")
            self.assertEqual(req("/api/rollouts/nope/abort", tok="sekret-token", body={"reason": "x"})[0], 409)
            # bound elsewhere: every request needs the token
            req = self.serve(s, local=False)
            self.assertEqual(req("/api/overview")[0], 401)
            self.assertEqual(req("/api/gateways", tok="sekret-token")[0], 200)
            self.assertEqual(req("/")[0], 200)                          # the page itself holds no data

    def test_escaping(self):
        with tempfile.TemporaryDirectory() as d:
            s, a, b, _ = self.fleet(d)
            req = self.serve(s)
            for path in ("/api/overview", "/api/gateways", f"/api/gateways/{a}", f"/api/gateways/{b}"):
                body = req(path)[1]
                self.assertNotIn(b"<", body, path)                      # <: JSON, never HTML
                self.assertNotIn(b">", body, path)
            g = json.loads(req(f"/api/gateways/{a}")[1])
            self.assertEqual(g["alerts"][0]["reason"], self.EVIL)       # the text itself arrives intact
            self.assertIn("anon<b>", [x["name"] for x in json.loads(req("/api/gateways")[1])["gateways"]])
            # the page puts gateway text on the page as text only
            with open(os.path.join(HERE, "..", "cortex", "aigw_cortex", "static", "dashboard.js")) as f:
                js = f.read()
            for bad in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"):
                self.assertNotIn(bad, js)

    def test_privacy(self):
        from aigw_cortex import dashboard
        with tempfile.TemporaryDirectory() as d:
            s, a, b, now = self.fleet(d)
            self.assertEqual(dashboard.sharing(s.snapshot(a)), "names")
            self.assertEqual(dashboard.sharing(s.snapshot(b)), "anonymous")
            self.assertEqual(dashboard.sharing({}), "none")
            ga, gb = dashboard.gateway(s, a, now), dashboard.gateway(s, b, now)
            self.assertEqual(ga["alerts"][0]["device"], "Kid's tablet")
            self.assertIn("Kid's tablet", ga["problem"]["summary"])
            self.assertNotIn("device", gb["alerts"][0])
            self.assertNotIn("reason", gb["alerts"][0])
            self.assertEqual(gb["alerts"][0]["subject"], "x.example")    # what it looked up: alert_names
            self.assertNotIn("summary", gb["problem"])
            q = [x for x in gb["decisions"] if x["action"] == "quarantine"][0]
            self.assertEqual(q["target"], "a device")
            self.assertNotIn("reason", q)
            self.assertEqual([x["target"] for x in gb["decisions"] if x["action"] == "block"], ["bad.example"])
            self.assertNotIn("Kid", json.dumps(gb))
            o = dashboard.overview(s, 7, now=now)
            by = {x["home"]: x for x in o["attention"] if x["why"] == "problem"}
            self.assertIn("Kid's tablet", by["named"]["words"])
            self.assertNotIn("summary", by["anon<b>"])
            self.assertNotIn("Kid", by["anon<b>"]["words"])

    def test_listen(self):
        from aigw_cortex.dashboard import is_loopback, load_token, parse_listen
        self.assertEqual(parse_listen("127.0.0.1:8090"), ("127.0.0.1", 8090))
        self.assertEqual(parse_listen("[::1]:9000"), ("::1", 9000))
        self.assertEqual(parse_listen("0.0.0.0"), ("0.0.0.0", 8090))
        self.assertEqual(parse_listen(":8091"), ("127.0.0.1", 8091))
        self.assertTrue(is_loopback("127.0.0.1") and is_loopback("::1") and is_loopback("localhost"))
        self.assertFalse(is_loopback("0.0.0.0") or is_loopback("192.168.0.11"))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "dashboard.token")
            t = load_token(p)
            self.assertEqual((load_token(p), len(t) >= 40), (t, True))
            if os.name == "posix":
                self.assertEqual(os.stat(p).st_mode & 0o777, 0o600)


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


def fault(gw, since, until=0, code="ISP_DOWN", uplink="exch-1"):
    from aigw_cortex import outage
    return {"gateway": gw, "uplink": uplink, "code": code, "kind": outage.kind_of(code), "since": since, "until": until}


class AreaOutageRules(unittest.TestCase):
    """v0.39: the correlation rules (outage.correlate), pure."""
    T = 1795469400

    def test_thresholds(self):
        from aigw_cortex.outage import correlate
        t = self.T
        # 20 homes on the uplink: 3 needed (min_homes), not 2
        self.assertEqual(correlate([fault("a", t), fault("b", t + 60)], {"exch-1": 20}), [])
        inc = correlate([fault("a", t), fault("b", t + 60), fault("c", t + 840)], {"exch-1": 20}, now=t + 900)
        self.assertEqual(len(inc), 1)
        i = inc[0]
        self.assertEqual((i["uplink"], i["homes"], i["homes_in_area"], i["need"], i["end"], i["still"]),
                         ("exch-1", 3, 20, 3, 0, 3))
        self.assertEqual((i["kind"], i["confidence"], i["spread_s"], i["start"]), ("DOWN", "medium", 840, t))
        # the same three, but not within the window (15 min) of the first: not one outage
        self.assertEqual(correlate([fault("a", t), fault("b", t + 600), fault("c", t + 1200)], {"exch-1": 20},
                                   {"window": 900}), [])
        # 50 homes: 10% is 5, more than min_homes
        four = [fault(f"h{i}", t + i * 30) for i in range(4)]
        self.assertEqual(correlate(four, {"exch-1": 50}), [])
        five = correlate(four + [fault("h4", t + 200)], {"exch-1": 50})
        self.assertEqual((five[0]["homes"], five[0]["need"], five[0]["confidence"]), (5, 5, "medium"))
        ten = correlate([fault(f"h{i}", t + i * 30) for i in range(10)], {"exch-1": 50})
        self.assertEqual(ten[0]["confidence"], "high")                      # twice the homes needed
        # stricter rules, from the settings
        self.assertEqual(correlate([fault(f"h{i}", t) for i in range(4)], {"exch-1": 6}, {"min_homes": 5}), [])
        # the kind most homes had (a tie goes to the worse one)
        mixed = correlate([fault("a", t, code="LINE_LOSS"), fault("b", t, code="ISP_LOSS"), fault("c", t, code="DNS_FAILING"),
                           fault("d", t, code="LINE_SLOW")], {"exch-1": 4})
        self.assertEqual((mixed[0]["kind"], mixed[0]["confidence"]), ("LOSS", "high"))   # all 4 of 4 homes

    def test_lone_home(self):
        from aigw_cortex.outage import correlate
        t = self.T
        # one home, its line flapping all evening: never an outage
        self.assertEqual(correlate([fault("a", t + k * 120, t + k * 120 + 60) for k in range(20)], {"exch-1": 30}), [])
        # a tiny uplink: both of its two homes down is still not one (two homes never are)
        self.assertEqual(correlate([fault("a", t), fault("b", t)], {"tiny": 2}), [])
        # a home with no uplink known is never grouped with anyone
        self.assertEqual(correlate([fault("a", t, uplink=""), fault("b", t, uplink=""), fault("c", t, uplink="")], {}), [])

    def test_two_areas(self):
        from aigw_cortex.outage import correlate
        t = self.T
        north = [fault(f"n{i}", t + i * 60, uplink="north") for i in range(4)]
        south = [fault(f"s{i}", t + 300 + i * 60, code="LINE_SLOW", uplink="south") for i in range(3)]
        inc = correlate(north + south, {"north": 10, "south": 8})
        self.assertEqual([(i["uplink"], i["homes"], i["kind"]) for i in inc], [("north", 4, "DOWN"), ("south", 3, "SLOW")])
        self.assertEqual(set(inc[0]["members"]), {"n0", "n1", "n2", "n3"})
        # two homes in each of two areas at the same time: not one outage of four
        two = [fault("n0", t, uplink="north"), fault("n1", t, uplink="north"),
               fault("s0", t, uplink="south"), fault("s1", t, uplink="south")]
        self.assertEqual(correlate(two, {"north": 10, "south": 10}), [])
        # the same area twice in a day: two incidents
        later = [fault(f"n{i}", t + 6 * 3600 + i * 60, t + 7 * 3600, uplink="north") for i in range(3)]
        early = [fault(f"n{i}", t + i * 60, t + 1800, uplink="north") for i in range(3)]
        two = correlate(early + later, {"north": 10})
        self.assertEqual([(i["start"], bool(i["end"])) for i in two], [(t, True), (t + 6 * 3600, True)])

    def test_recovery(self):
        from aigw_cortex.outage import correlate
        t = self.T
        homes = ["a", "b", "c", "d", "e"]
        f = [fault(h, t + i * 60) for i, h in enumerate(homes)]
        self.assertEqual(correlate(f, {"exch-1": 5})[0]["end"], 0)
        # three of five back: still on (80% must recover; one straggler may stay)
        for i in range(3):
            f[i]["until"] = t + 3600 + i * 60
        i = correlate(f, {"exch-1": 5})[0]
        self.assertEqual((i["end"], i["still"]), (0, 2))
        # the fourth: over, at the time the fourth came back; the fifth is its own problem now
        f[3]["until"] = t + 4000
        i = correlate(f, {"exch-1": 5})[0]
        self.assertEqual((i["end"], i["still"]), (t + 4000, 1))
        # three homes: all three must recover
        f3 = [fault(h, t, t + 1000 + k) for k, h in enumerate("xyz")]
        f3[2]["until"] = 0
        self.assertEqual(correlate(f3, {"exch-1": 3})[0]["end"], 0)
        f3[2]["until"] = t + 5000
        self.assertEqual(correlate(f3, {"exch-1": 3})[0]["end"], t + 5000)

    def test_later_homes_join(self):
        from aigw_cortex.outage import correlate
        t = self.T
        f = [fault(h, t + k * 60) for k, h in enumerate("abc")]
        # 40 minutes in, a fourth home's line goes too, while the outage is on: it joins
        f.append(fault("d", t + 2400, code="ISP_LOSS"))
        i = correlate(f, {"exch-1": 10}, now=t + 3000)[0]
        self.assertEqual((i["homes"], sorted(i["members"])), (4, ["a", "b", "c", "d"]))
        self.assertEqual(i["spread_s"], 120)                                # of the first homes
        # but not after it is over
        for x in f[:3]:
            x["until"] = t + 1800
        f[3]["since"] = t + 3600
        out = correlate(f, {"exch-1": 10}, now=t + 4000)
        self.assertEqual((len(out), out[0]["homes"], out[0]["end"]), (1, 3, t + 1800))
        # one home with two faults (care and the brief) counts once, from its first
        f = [fault("a", t), fault("a", t + 30, code="ISP_LOSS"), fault("b", t + 60), fault("c", t + 90)]
        i = correlate(f, {"exch-1": 10})[0]
        self.assertEqual((i["homes"], i["members"]["a"]["since"]), (3, t))

    def test_uplink(self):
        from aigw_cortex.outage import reported_uplink
        self.assertEqual(reported_uplink({"uplink_id": "OLT-7/1", "wan_gateway": "203.0.113.1"}), ("OLT-7/1", "gateway"))
        self.assertEqual(reported_uplink({"wan_gateway": "203.0.113.1"}), ("wan:203.0.113.1", "wan"))
        self.assertEqual(reported_uplink({"wan_gateway": "100.64.12.1"}), ("wan:100.64.12.1", "wan"))     # CGNAT: the ISP's
        self.assertEqual(reported_uplink({"wan_gateway": "", "bng": "198.51.100.9"}), ("bng:198.51.100.9", "bng"))
        self.assertEqual(reported_uplink({"wan_gateway": "2001:db8::1"}), ("wan:2001:db8::1", "wan"))
        for private in ("192.168.1.1", "10.0.0.1", "172.16.5.1", "fe80::1", "fd12::1", "127.0.0.1", "nonsense", None):
            self.assertEqual(reported_uplink({"wan_gateway": private}), ("", ""), private)
        self.assertEqual(reported_uplink(None), ("", ""))


def line_down(since, code="ISP_DOWN"):
    """A report's care part with an open problem on the line (agd's care_blob)."""
    return {"status": "problem", "problem": {"code": code, "area": "Internet", "since": since, "reported": True}}


class AreaOutageStore(unittest.TestCase):
    """v0.39: incidents kept in cortex: tags, the brief, held fixes, silence, recovery."""
    T = 1795469400

    def fleet(self, d, n=8, uplink="wan:203.0.113.1"):
        s = Store(d)
        gws = [s.enroll("home%d" % i)[0] for i in range(n)]
        for g in gws:
            s.report(g, {"status": {"version": "0.39.0", "wan_gateway": uplink.split(":", 1)[1]}, "interval": 300},
                     self.T - 600)
        return s, gws

    def test_flow(self):
        from aigw_cortex import brief, outage
        t = self.T
        with tempfile.TemporaryDirectory() as d:
            s, g = self.fleet(d)
            other = s.enroll("faraway")[0]
            s.report(other, {"status": {"wan_gateway": "198.51.100.1"}, "care": line_down(t - 200)}, t)

            def fine(at):           # the other homes go on reporting (else they'd be silent: dropped)
                for gw in g[3:]:
                    s.report(gw, {"status": {"wan_gateway": "203.0.113.1"}, "care": {"status": "ok"}}, at)
            fine(t)
            # two homes down: nothing yet
            for i in range(2):
                s.report(g[i], {"status": {"wan_gateway": "203.0.113.1"}, "care": line_down(t - 300 + i * 30)}, t)
            self.assertEqual(outage.incidents(s, open_only=True), [])
            self.assertEqual(outage.notice(s, g[0]), {"incident": 0})
            # the third: an incident, its homes tagged
            s.report(g[2], {"status": {"wan_gateway": "203.0.113.1"}, "care": line_down(t - 100, "ISP_LOSS")}, t + 10)
            inc = outage.incidents(s, open_only=True)
            self.assertEqual(len(inc), 1)
            i = inc[0]
            self.assertEqual((i["uplink"], i["homes"], i["homes_in_area"], i["state"], i["uplink_from"]),
                             ("wan:203.0.113.1", 3, 8, "open", "wan"))
            self.assertEqual(sorted(h["home"] for h in i["affected"]), ["home0", "home1", "home2"])
            n = outage.notice(s, g[1])
            self.assertEqual((n["incident"], n["kind"], n["homes"], n["of"], n["hold_s"]), (i["id"], "DOWN", 3, 8, 1800))
            self.assertEqual(outage.notice(s, g[5]), {"incident": 0})              # fine: not part of it
            self.assertEqual(outage.notice(s, other), {"incident": 0})             # alone on another uplink
            # the help desk: a known outage in the area, no technician, nothing to fix in the home
            s.report(g[0], {"status": {"wan_gateway": "203.0.113.1"}, "care": line_down(t - 300),
                            "brief": a_brief("LINE", "ISP_DOWN", "IfPersists", made=t + 20,
                                             fix={"possible": True, "action": "sqm", "target": "eth0:1:1", "text": "x"})},
                     t + 20)
            b = brief.answer(s, "home0", propose=True, now=t + 30)
            self.assertEqual((b["fault"], b["visit"], b["confidence"], b["fix"]["possible"]), ("LINE", "No", "high", False))
            self.assertIn("Known outage in the area", b["verdict"])
            self.assertIn("known outage in your area", b["tell"])
            self.assertIn("Held", b["proposal"]["note"])
            self.assertEqual(b["area_outage"]["id"], i["id"])
            self.assertEqual(b["evidence"][0]["source"], "area")
            self.assertIn("KNOWN AREA OUTAGE", brief.text(b))
            self.assertEqual(s.proposals(), [])                                    # nothing proposed
            nb = brief.answer(s, "home5", now=t + 30)                               # a neighbour that is fine
            self.assertIn("this home hasn't shown it", nb["area_note"])
            self.assertNotIn("area_outage", nb)
            # fixes held for its homes: queued, not sent; a quarantine still goes
            s.propose(g[0], "sqm", "eth0:45000:0", 0, "x")
            s.propose(g[0], "quarantine", "tv", 0, "x")
            s.propose(g[5], "stay24", "aa:bb", 0, "x")
            self.assertEqual([p["action"] for p in s.take_proposals(g[0], outage.HELD_ACTIONS)], ["quarantine"])
            self.assertEqual([p["action"] for p in s.take_proposals(g[5])], ["stay24"])
            # in the fleet report: once, with its homes, and the homes to call first say so
            r = __import__("aigw_cortex.fleet", fromlist=["x"]).report(s, 7, now=t + 60)
            self.assertEqual([(o["id"], o["homes"], o["still_open"]) for o in r["outages"]], [(i["id"], 3, 3)])
            self.assertEqual({a["home"]: a.get("area_outage") for a in r["attention"] if a["why"] == "problem"},
                             {"home0": i["id"], "home1": i["id"], "home2": i["id"], "faraway": None})
            # recovery: 3 homes, all must come back; the incident keeps its id
            for at in (t + 600, t + 1200, t + 1800):
                fine(at)
            for k in range(2):
                s.report(g[k], {"status": {"wan_gateway": "203.0.113.1"}, "care": {"status": "ok"}}, t + 1800 + k)
            self.assertEqual(outage.incidents(s, open_only=True)[0]["still_affected"], 1)
            s.report(g[2], {"status": {"wan_gateway": "203.0.113.1"}, "care": {"status": "ok"}}, t + 1900)
            self.assertEqual(outage.incidents(s, open_only=True), [])
            done = outage.incidents(s, since=t - 86400)
            self.assertEqual([(x["id"], x["state"], x["end"]) for x in done], [(i["id"], "closed", t + 1900)])
            self.assertEqual(outage.notice(s, g[0]), {"incident": 0})
            self.assertEqual([p["action"] for p in s.take_proposals(g[0], ())], ["sqm"])   # sent now that it's over
            self.assertNotIn("area_outage", brief.answer(s, "home0", now=t + 2000))

    def test_operator_uplink(self):
        """The operator's id in cortex wins over what the gateway reports; private next hops group nothing."""
        from aigw_cortex import outage
        t = self.T
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            g = [s.enroll("h%d" % i)[0] for i in range(4)]
            for k, gw in enumerate(g):      # every home behind its own modem: 192.168.1.1 tells nothing
                s.report(gw, {"status": {"wan_gateway": "192.168.1.1"}, "care": line_down(t + k * 10)}, t + 60)
            self.assertEqual(outage.update(s, t + 60), [])
            for gw in g[:3]:
                s.set_group(gw, "OLT-7")
            s.report(g[3], {"status": {"uplink_id": "OLT-7"}, "care": line_down(t + 30)}, t + 90)  # the ACS's id
            inc = outage.update(s, t + 90)
            self.assertEqual((inc[0]["uplink"], inc[0]["homes"], inc[0]["homes_in_area"]), ("OLT-7", 4, 4))

    def test_silence(self):
        """Homes that stop reporting together: dropped. All of them at once: cortex is cut off, not an area."""
        from aigw_cortex import outage
        t = self.T
        with tempfile.TemporaryDirectory() as d:
            s, g = self.fleet(d, n=6)
            far = s.enroll("far")[0]
            for gw in g + [far]:
                s.report(gw, {"status": {"wan_gateway": "203.0.113.1" if gw != far else "198.51.100.1"},
                              "interval": 300}, t)
            # four homes stop reporting; the rest go on
            for k in range(1, 6):
                for gw in g[4:] + [far]:
                    s.report(gw, {"status": {"wan_gateway": "203.0.113.1" if gw != far else "198.51.100.1"},
                                  "interval": 300}, t + k * 300)
            inc = outage.update(s, t + 1500)
            self.assertEqual(len(inc), 1, inc)
            self.assertEqual((inc[0]["kind"], inc[0]["homes"], inc[0]["codes"]), ("DOWN", 4, {"SILENT": 4}))
            self.assertEqual(inc[0]["start"], t + 300)                       # when their next report was due
            # they come back: the gap is kept as their fault, and it is over
            for k, gw in enumerate(g[:4]):
                s.report(gw, {"status": {"wan_gateway": "203.0.113.1"}, "interval": 300}, t + 1600 + k)
            self.assertEqual(outage.incidents(s, open_only=True), [])
            done = outage.incidents(s, since=t)
            self.assertEqual((done[0]["state"], done[0]["end"]), ("closed", t + 1603))
            # nobody at all reports (cortex itself was cut off): nothing
        with tempfile.TemporaryDirectory() as d:
            s, g = self.fleet(d, n=6)
            self.assertEqual(outage.update(s, t + 7200), [])

    def test_settings(self):
        from aigw_cortex import outage
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            self.assertEqual(outage.settings(s)["min_homes"], 3)
            r = outage.set_settings(s, min_homes=5, share=0.2, window=None)
            self.assertEqual((r["min_homes"], r["share"], r["window"]), (5, 0.2, 900))
            with self.assertRaises(ValueError):
                outage.set_settings(s, min_homes=1)                 # never a lone home
            with self.assertRaises(ValueError):
                outage.set_settings(s, recovered=0.1)


class AreaOutageEndToEnd(unittest.TestCase):
    """v0.39, end to end: 14 simulated homes on two uplinks report to cortex over HTTP. One
    uplink's line goes; cortex opens one incident, tells those gateways in its answer, holds
    their fixes, the help desk and the dashboard and the MCP tool say so; the homes recover
    and it closes. The other uplink and a home alone stay out of it."""

    def test_homes(self):
        import time as _time
        from http.server import ThreadingHTTPServer
        from aigw_cortex import dashboard, server
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            api = ThreadingHTTPServer(("127.0.0.1", 0), server.make_handler(s, log=False))
            dash = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.make_handler(s, "tok", True, log=False))
            for h in (api, dash):
                threading.Thread(target=h.serve_forever, daemon=True).start()
                self.addCleanup(h.server_close)
                self.addCleanup(h.shutdown)

            def call(base, path, tok=None, body=None):
                r = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                                           headers={"Authorization": f"Bearer {tok}"} if tok else {})
                with urllib.request.urlopen(r) as resp:
                    return json.loads(resp.read())
            a = f"http://127.0.0.1:{api.server_address[1]}"
            b = f"http://127.0.0.1:{dash.server_address[1]}"
            # homes: 8 behind one BNG (PPPoE: no next hop), 5 behind an OLT the ACS named, 1 behind its own modem
            homes = {}
            for i in range(8):
                homes[f"bng-{i}"] = ({"bng": "198.51.100.7", "wan_gateway": ""}, s.enroll(f"bng-{i}")[1])
            for i in range(5):
                homes[f"olt-{i}"] = ({"uplink_id": "OLT-3/2", "wan_gateway": "192.168.100.1"}, s.enroll(f"olt-{i}")[1])
            homes["lone"] = ({"wan_gateway": "192.168.1.1"}, s.enroll("lone")[1])

            def report(name, care=None, brief=None):
                st, tok = homes[name]
                body = {"bridge": "0.39.0", "interval": 300, "status": dict(st, version="0.39.0", host=name)}
                if care:
                    body["care"] = care
                if brief:
                    body["brief"] = brief
                return call(a, "/v1/report", tok, body)
            for name in homes:
                self.assertEqual(report(name)["area"], {"incident": 0})
            now = int(_time.time())
            # the BNG's line goes for 5 of its 8 homes within a few minutes; one OLT home and the lone home too
            answers = {}
            for k in range(5):
                answers[k] = report(f"bng-{k}", care=line_down(now - 240 + k * 40, "ISP_DOWN" if k % 2 else "ISP_LOSS"))
            report("olt-0", care=line_down(now - 200))
            report("lone", care=line_down(now - 220, "LINE_DOWN"))
            self.assertEqual([answers[k]["area"]["incident"] for k in range(3)], [0, 0, answers[2]["area"]["incident"]])
            n = report("bng-0", care=line_down(now - 240, "ISP_LOSS"))["area"]
            self.assertGreater(n["incident"], 0)
            self.assertEqual((n["homes"], n["of"], n["uplink"], n["confidence"]), (5, 8, "bng:198.51.100.7", "high"))
            self.assertEqual(report("bng-6")["area"], {"incident": 0})             # its neighbour is fine
            self.assertEqual(report("olt-1")["area"], {"incident": 0})
            self.assertEqual(report("lone", care=line_down(now - 220, "LINE_DOWN"))["area"], {"incident": 0})
            # cortex holds fixes for those homes: a proposal waits, firmware isn't offered
            gid = s.gateway("bng-1")["id"]
            s.propose(gid, "sqm", "pppoe-wan:40000:0", 0, "help desk")
            self.assertEqual(call(a, "/v1/proposals", homes["bng-1"][1])["proposals"], [])
            self.assertIsNone(call(a, "/v1/firmware", homes["bng-1"][1])["offer"])
            # the dashboard: the incident, its homes; a home's page says so
            o = call(b, "/api/outages?state=open")
            self.assertEqual(len(o["outages"]), 1)
            inc = o["outages"][0]
            self.assertEqual((inc["id"], inc["kind"], inc["homes"]), (n["incident"], "LOSS", 5))
            self.assertEqual(sorted(h["home"] for h in inc["affected"]), [f"bng-{k}" for k in range(5)])
            self.assertEqual(o["rules"]["min_homes"], 3)
            ov = call(b, "/api/overview")
            self.assertEqual([x["id"] for x in ov["outages"]], [n["incident"]])
            self.assertEqual(call(b, f"/api/gateways/{gid}")["area_outage"]["id"], n["incident"])
            self.assertIsNone(call(b, "/api/gateways/lone")["area_outage"])
            # the MCP tools: area_outages, and the help desk's brief
            srv = Server(s)
            r = srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "area_outages",
                                                                                     "arguments": {}}})
            self.assertEqual([x["id"] for x in json.loads(r["result"]["content"][0]["text"])], [n["incident"]])
            r = srv.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                            "params": {"name": "support_brief", "arguments": {"gateway": "bng-3"}}})
            self.assertIn("Known outage in the area", json.loads(r["result"]["content"][0]["text"])["verdict"])
            r = srv.handle({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                            "params": {"name": "support_brief", "arguments": {"gateway": "lone"}}})
            self.assertNotIn("area_outage", json.loads(r["result"]["content"][0]["text"]))
            # the homes recover (80%: 4 of 5): over; the fix that waited is sent
            for k in range(4):
                report(f"bng-{k}", care={"status": "ok"})
            self.assertEqual(call(b, "/api/outages?state=open")["outages"], [])
            self.assertEqual(report("bng-4", care=line_down(now - 80))["area"], {"incident": 0})   # a straggler: its own
            self.assertEqual([p["action"] for p in call(a, "/v1/proposals", homes["bng-1"][1])["proposals"]], ["sqm"])
            closed = call(b, "/api/outages")["outages"]
            self.assertEqual([(x["id"], x["state"]) for x in closed], [(n["incident"], "closed")])


def a_ticket(tid, status, code="ISP_DOWN", fault="LINE", is_open=True, told=True, called=False, opened=0, since=0):
    return {"id": tid, "status": status, "open": is_open, "code": code, "fault": fault, "severity": "problem",
            "points": 3, "reason": "a problem +2, 3 devices +1", "devices": 3, "episodes": 1, "flaps": 0,
            "since": since, "opened": opened, "told_isp": told, "called": called, "acted": status == "Fixed",
            "summary": "The ISP's line or network, now: no answer (ISP_DOWN).", "evidence": "Worth acting on.",
            "next_step": "Check the ISP's network.", "customer_note": "We noticed your internet stopped working tonight."}


def a_proactive(lst, **counts):
    c = {"tickets": 0, "told_isp": 0, "fixed_by_aigw": 0, "proposed": 0, "suppressed": 0, "deduplicated": 0,
         "flaps": 0, "called_anyway": 0, "calls_likely_avoided": 0, "since": 0}
    c.update(counts)
    return {"enabled": True, "open": sum(1 for t in lst if t["open"]), "counts": c, "tickets": lst, "names": 0}


class Proactive(unittest.TestCase):
    """v0.40: proactive care before the call across the fleet (proactive.py), the MCP tool, the dashboard."""

    def fleet(self, d, now):
        s = Store(d)
        a, _ = s.enroll("home-a", "east")
        b, _ = s.enroll("home-b", "west")
        c, _ = s.enroll("old")
        s.report(a, {"status": {"version": "0.40.0"}, "proactive": a_proactive(
            [a_ticket(2, "Raised", opened=now - 300, since=now - 400),
             a_ticket(1, "Fixed", "WIFI_HOPPING", "HOME_WIFI", False, False, opened=now - 9000, since=now - 9100)],
            tickets=2, told_isp=2, fixed_by_aigw=1, calls_likely_avoided=1, deduplicated=3)}, now)
        s.report(b, {"status": {"version": "0.40.0"}, "proactive": a_proactive(
            [a_ticket(5, "Proposed", "LINE_FULL", "NONE", opened=now - 60, since=now - 600),
             a_ticket(4, "Resolved", is_open=False, called=True, opened=now - 7000)],
            tickets=4, told_isp=3, proposed=1, suppressed=1, called_anyway=1, calls_likely_avoided=2)}, now)
        s.report(c, {"status": {"version": "0.39.0"}}, now)
        return s

    def test_fleet(self):
        from aigw_cortex import fleet, proactive
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = self.fleet(d, now)
            p = fleet.report(s, 7, now=now)["proactive"]
            self.assertEqual((p["homes"], p["open"], p["homes_with_open"]), (2, 2, 2))
            self.assertEqual(p["calls_likely_avoided"], 3)
            self.assertEqual((p["counts"]["tickets"], p["counts"]["fixed_by_aigw"], p["counts"]["suppressed"]), (6, 1, 1))
            self.assertEqual([t["status"] for t in p["open_tickets"]], ["Raised", "Proposed"])     # the ISP's first
            self.assertEqual(p["in_period"]["tickets"], 4)
            self.assertEqual(p["in_period"]["calls_likely_avoided"], 1)   # the fixed one; the resolved one was called about
            t = fleet.text(fleet.report(s, 7, now=now))
            self.assertIn("before call  2 homes (AIGW OS 0.40 on): 6 faults worth acting on, 1 fixed by AIGW itself", t)
            self.assertIn("calls likely avoided 3 (called anyway 1)", t)
            self.assertEqual(fleet.report(s, 7, "west", now=now)["proactive"]["calls_likely_avoided"], 2)
            one = proactive.summary(s, "home-a", now=now)
            self.assertEqual([x["id"] for x in one["tickets"]], [2, 1])
            self.assertEqual([x["id"] for x in proactive.summary(s, "home-a", True, now)["tickets"]], [2])
            self.assertIn("home-a #2 Raised ISP_DOWN (LINE), open", proactive.text(one, now))
            with self.assertRaises(ValueError):
                proactive.summary(s, "nobody")
            self.assertEqual(proactive.summary(s, "old", now=now)["tickets"], [])

    def test_mcp(self):
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = self.fleet(d, now)
            srv = Server(s)
            names = [t["name"] for t in srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})["result"]["tools"]]
            self.assertIn("proactive_tickets", names)
            r = srv.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                            "params": {"name": "proactive_tickets", "arguments": {"open": True}}})
            out = json.loads(r["result"]["content"][0]["text"])
            self.assertEqual(sorted(t["home"] for t in out["tickets"]), ["home-a", "home-b"])
            self.assertEqual(out["calls_likely_avoided"], 3)

    def test_dashboard(self):
        from http.server import ThreadingHTTPServer
        from aigw_cortex.dashboard import gateway, make_handler, overview
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = self.fleet(d, now)
            o = overview(s, now=now)
            self.assertEqual(o["proactive"]["calls_likely_avoided"], 3)
            g = gateway(s, "home-a", now)
            self.assertEqual(g["proactive"]["counts"]["fixed_by_aigw"], 1)
            self.assertEqual(len(g["proactive"]["tickets"]), 2)
            httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(s, "tok", True, log=False))
            threading.Thread(target=httpd.serve_forever, daemon=True).start()
            self.addCleanup(httpd.server_close)
            self.addCleanup(httpd.shutdown)
            with urllib.request.urlopen(f"http://127.0.0.1:{httpd.server_address[1]}/api/proactive?open=1") as resp:
                body = json.loads(resp.read())
            self.assertEqual(len(body["tickets"]), 2)

class Demo(unittest.TestCase):
    """v0.41: the simulated fleet (python3 -m aigw_cortex demo, the demo image): what the
    dashboard shows, labelled as demo data, and never mixed into a real fleet's directory."""

    def test_fleet(self):
        from aigw_cortex import dashboard, demo, fleet
        with tempfile.TemporaryDirectory() as d:
            s, token = demo.seed(d, home="This demo VM")
            self.assertTrue(demo.is_demo(d) and token)
            self.assertEqual(s.gateway_by_token(token)["name"], "This demo VM")
            r = dashboard.overview(s)
            self.assertTrue(r["demo"])
            self.assertEqual(r["homes"]["reporting"], 24)
            self.assertEqual(r["groups"], ["hillcrest", "riverside"])
            self.assertEqual(len(r["outages"]), 1)                       # four homes of riverside, minutes apart
            o = r["outages"][0]
            self.assertEqual((o["uplink"], o["kind"], o["state"], o["homes"]), ("riverside", "DOWN", "open", 4))
            p = r["proactive"]                                          # v0.40: tickets before the call
            self.assertEqual(sorted(t["status"] for t in p["open_tickets"]), ["Proposed", "Raised"])
            self.assertGreater(p["calls_likely_avoided"], 0)
            self.assertTrue(any(a["why"] == "early warning" for a in r["attention"]))
            self.assertGreater(r["experience"]["mean"], r["experience"]["mean_before"])   # better than before
            self.assertGreater(r["problems"]["fixed_by_aigw"], 0)
            q = r["queueing"]
            self.assertTrue(q["calls"]["reported"] and q["calls"]["prioritized"] > 0 and q["sqm"]["on"] > 0)
            self.assertEqual({x["state"] for x in dashboard.rollouts(s)}, {"running", "stopped"})
            self.assertGreaterEqual(fleet.report(s)["signals"]["homes"], 5)

    def test_never_a_real_fleet(self):
        from aigw_cortex import dashboard, demo
        with tempfile.TemporaryDirectory() as d:
            Store(d).enroll("a real home")
            with self.assertRaises(ValueError):
                demo.seed(d)
            self.assertFalse(demo.is_demo(d))
            self.assertFalse(dashboard.overview(Store(d))["demo"])
        with tempfile.TemporaryDirectory() as d:
            demo.seed(d)
            with self.assertRaises(ValueError):                         # once: a second fleet would double it
                demo.seed(d)


class Security(unittest.TestCase):
    """v0.43 (docs/security.md): what cortex keeps is its owner's alone; a fleet API beyond
    this host without TLS says so; odd requests are refused, not crashed on."""

    def test_private_files(self):
        import contextlib
        import io
        import stat
        with tempfile.TemporaryDirectory() as top:
            d = os.path.join(top, "new")
            Store(d)
            self.assertEqual(stat.S_IMODE(os.stat(d).st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(d, "cortex.db")).st_mode), 0o600)
            # a data directory made by cortex before 0.43 (umask 022): tightened, with a note
            old = os.path.join(top, "old")
            os.makedirs(old, mode=0o755)
            os.chmod(old, 0o755)
            Store(old).enroll("home")
            os.chmod(os.path.join(old, "cortex.db"), 0o644)
            os.chmod(old, 0o755)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                s = Store(old)
            self.assertEqual(stat.S_IMODE(os.stat(old).st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(old, "cortex.db")).st_mode), 0o600)
            self.assertIn("readable by other users", err.getvalue())
            self.assertEqual(len(s.gateways()), 1)                      # nothing lost

    def test_exposure_note(self):
        from aigw_cortex.server import exposure_note
        for listen in ("127.0.0.1", "localhost", "::1"):
            self.assertEqual(exposure_note(listen, None), "", listen)
        self.assertEqual(exposure_note("0.0.0.0", "cert.pem"), "")
        for listen in ("0.0.0.0", "", "192.0.2.10", "::"):
            self.assertIn("without TLS", exposure_note(listen, None), listen)

    def test_odd_requests(self):
        import http.client
        from http.server import ThreadingHTTPServer
        from aigw_cortex import dashboard, server
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            _, token = s.enroll("home")
            api = ThreadingHTTPServer(("127.0.0.1", 0), server.make_handler(s, log=False))
            dash = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.make_handler(s, "tok", True, log=False))
            for h in (api, dash):
                threading.Thread(target=h.serve_forever, daemon=True).start()
            try:
                def post(port, path, tok):
                    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                    c.putrequest("POST", path)
                    c.putheader("Authorization", f"Bearer {tok}")
                    c.putheader("Content-Length", "lots")
                    c.endheaders()
                    r = c.getresponse()
                    c.close()
                    return r.status
                self.assertEqual(post(api.server_address[1], "/v1/report", token), 400)
                self.assertEqual(post(dash.server_address[1], "/api/rollouts/x/pause", "tok"), 400)
                # changes need the token on loopback too, and no cookie or form can stand in for it
                self.assertEqual(post(dash.server_address[1], "/api/rollouts/x/pause", "wrong"), 401)
            finally:
                api.shutdown()
                dash.shutdown()


def a_mesh(extenders=1, wireless=1, weak=0, hurting=0, findings=(), **counts):
    c = {"steers": 0, "kept": 0, "undone": 0, "misses": 0, "proposed": 0, "channel_changes": 0, "channels_kept": 0,
         "channels_undone": 0, "roams": 0}
    c.update(counts)
    return {"agents": extenders + 1, "extenders": extenders, "stations": 7, "wireless_backhauls": wireless,
            "weak_backhauls": weak, "hurting": hurting, "findings": list(findings), "counts": c}


class Mesh(unittest.TestCase):
    """v0.44: mesh Wi-Fi across the fleet (mesh.py): homes whose extenders hurt more than they help."""

    def test_fleet(self):
        from aigw_cortex import fleet, mesh
        from aigw_cortex.dashboard import overview
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            a, _ = s.enroll("home-a", "east")
            b, _ = s.enroll("home-b", "west")
            c, _ = s.enroll("home-c")
            old, _ = s.enroll("old")
            s.report(a, {"status": {"version": "0.44.0"}, "mesh": a_mesh(
                weak=1, hurting=1, findings=[{"kind": "Weak", "stations": 3, "hurts": True, "signal": -79,
                                              "rate_kbps": 48000}], steers=4, kept=3, undone=1)}, now)
            s.report(b, {"status": {"version": "0.44.0"}, "mesh": a_mesh(
                extenders=2, wireless=1, findings=[{"kind": "Try6", "stations": 2, "hurts": False, "signal": -58}],
                steers=2, kept=1, misses=1, channel_changes=1, channels_kept=1)}, now)
            s.report(c, {"status": {"version": "0.44.0"}, "mesh": a_mesh(extenders=0, wireless=0)}, now)
            s.report(old, {"status": {"version": "0.43.1"}}, now)
            m = fleet.report(s, 7, now=now)["mesh"]
            self.assertEqual((m["homes"], m["homes_with_extenders"], m["extenders"], m["homes_hurting"]), (3, 2, 3, 1))
            self.assertEqual((m["wireless_backhauls"], m["weak_backhauls"]), (2, 1))
            self.assertEqual((m["counts"]["steers"], m["counts"]["kept"], m["kept_pct"]), (6, 4, 67))
            self.assertEqual(m["findings"], {"Weak": 1, "Try6": 1})
            self.assertEqual(m["hurting"], [{"home": "home-a", "group": "east", "extenders": 1, "hurting": 1,
                                             "stations": 3, "kind": "Weak", "signal": -79, "rate_mbps": 48}])
            t = fleet.text(fleet.report(s, 7, now=now))
            self.assertIn("mesh        3 homes with an EasyMesh controller (AIGW OS 0.44 on), 3 extenders (2 wireless, 1 weak); "
                          "1 home whose extenders hurt more than they help", t)
            self.assertIn("home-a (east): an extender's backhaul weak, -79 dBm, 48 Mbit/s, 3 devices behind it", t)
            self.assertEqual(fleet.report(s, 7, "west", now=now)["mesh"]["homes_hurting"], 0)
            self.assertEqual(overview(s, now=now)["mesh"]["homes_hurting"], 1)
            self.assertEqual(mesh.of(s.snapshot(old)), {})


class Channels(unittest.TestCase):
    """v0.45: neighbour-aware channels across the fleet (channels.py): homes with a clearly better channel."""

    def test_fleet(self):
        from aigw_cortex import channels, fleet
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            a, _ = s.enroll("home-a", "east")
            b, _ = s.enroll("home-b")
            old, _ = s.enroll("old")
            s.report(a, {"status": {"version": "0.45.0"}, "channels": {"radios": [
                {"band": "2.4", "channel": 11, "status": "Planned", "best": 6, "gain_pct": 89, "metric": "neighbours",
                 "proposed": 1, "kept": 0, "tried": 1},
                {"band": "5", "channel": 36, "status": "Stay", "best": 0, "proposed": 0, "kept": 1, "tried": 0}]}}, now)
            s.report(b, {"status": {"version": "0.45.0"}, "channels": {"radios": [
                {"band": "2.4", "channel": 1, "status": "Learning", "best": 0}]}}, now)
            s.report(old, {"status": {"version": "0.44.0"}}, now)
            m = fleet.report(s, 7, now=now)["channels"]
            self.assertEqual((m["homes"], m["radios"], m["homes_better"]), (2, 3, 1))
            self.assertEqual(m["counts"], {"proposed": 1, "kept": 1, "tried": 1})
            self.assertEqual(m["better"], [{"home": "home-a", "group": "east", "band": "2.4", "channel": 11, "best": 6,
                                            "gain_pct": 89, "metric": "neighbours", "status": "Planned"}])
            t = fleet.text(fleet.report(s, 7, now=now))
            self.assertIn("channels    2 homes learning their Wi-Fi channels by the hour (AIGW OS 0.45 on): 1 with a "
                          "clearly better channel in their busy hours; AIGW moved 2 (kept 1, moved back 1)", t)
            self.assertIn("home-a (east): 2.4 GHz channel 11 -> 6, 89% less load in the busy hours (neighbours), planned", t)
            self.assertEqual(channels.of(s.snapshot(old)), [])


def a_coverage(verdict, suffering=0, where="", aps=1, **kw):
    spots = [{"where": where, "when": "in the evening", "devices": suffering, "suffering": suffering, "shared": True,
              "samples": 120, "signal": -85}] if suffering else []
    c = {"enabled": True, "names": False, "verdict": verdict, "judged": 6, "weak_devices": suffering, "to_move": suffering,
         "weak_transmitters": 0, "shared_spots": 1 if suffering >= 2 else 0, "access_points": aps, "spots": spots,
         "advice": f"{suffering} devices are weak in the same spot ({where})" if suffering else ""}
    c.update(kw)
    return c


class Coverage(unittest.TestCase):
    """v0.46: Wi-Fi coverage across the fleet (coverage.py): the homes that need an extender, and those that don't."""

    def test_fleet(self):
        from aigw_cortex import coverage, fleet
        from aigw_cortex.dashboard import overview
        now = 20000 * 86400 + 12 * 3600
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            ids = {n: s.enroll(n, g)[0] for n, g in (("flat-1", "east"), ("flat-2", "east"), ("flat-3", "west"),
                                                     ("flat-4", None), ("flat-5", None), ("old", None))}
            s.report(ids["flat-1"], {"status": {"version": "0.46.0"}, "coverage": a_coverage(
                "Extender", 3, "where a TV, a laptop and 1 more are, in the evening (about -86 dBm at the gateway)")}, now)
            s.report(ids["flat-2"], {"status": {"version": "0.46.0"}, "coverage": a_coverage(
                "Extender", 2, "where a TV and an Android phone are, at night (about -84 dBm at the gateway)")}, now)
            s.report(ids["flat-3"], {"status": {"version": "0.46.0"}, "coverage": a_coverage(
                "UseExtender", 2, "where two phones are", aps=2)}, now)
            s.report(ids["flat-4"], {"status": {"version": "0.46.0"}, "coverage": a_coverage("MoveDevice", to_move=1)}, now)
            s.report(ids["flat-5"], {"status": {"version": "0.46.0"}, "coverage": a_coverage(
                "WeakDevice", weak_transmitters=1)}, now)
            s.report(ids["old"], {"status": {"version": "0.45.0"}}, now)
            c = fleet.report(s, 7, now=now)["coverage"]
            self.assertEqual((c["homes"], c["need_extender"], c["use_extender"], c["move_device"], c["weak_device_only"]),
                             (5, 2, 1, 1, 1))
            self.assertEqual([h["home"] for h in c["leads"]], ["flat-1", "flat-2"])    # most devices first
            self.assertEqual((c["leads"][0]["devices"], c["leads"][0]["group"], c["leads"][0]["mesh"]), (3, "east", False))
            t = fleet.text(fleet.report(s, 7, now=now))
            self.assertIn("coverage    5 homes judged (AIGW OS 0.46 on): 2 need an extender (a weak spot two or more "
                          "devices share), 1 should use the extender they have, 1 only a device to move", t)
            self.assertIn("flat-2 (east): 2 devices weak in one spot where a TV and an Android phone are, at night", t)
            self.assertNotIn("02:00", t)
            self.assertEqual(fleet.report(s, 7, "west", now=now)["coverage"]["need_extender"], 0)
            self.assertEqual(overview(s, now=now)["coverage"]["need_extender"], 2)
            self.assertEqual(coverage.of(s.snapshot(ids["old"])), {})


class FleetLearning(unittest.TestCase):
    """v0.50 the reference aggregator: merge rules, k-anonymity, the cap, what is refused, the signed file."""
    KEY = "5/80+/high/phone/evening/fibre"

    def summary(self, outcomes, normals=(), line="fibre"):
        return {"v": 1, "line": line, "outcomes": [list(o) for o in outcomes], "normals": [list(n) for n in normals]}

    def test_check_refuses_what_is_not_coarse(self):
        from aigw_cortex import fleetlearn as fl
        ok, _ = fl.check(self.summary([("WIFI_HOPPING", "stay24", self.KEY, 4, 1, 1, 0)], [(1, 3, 22.0, 3.0)]))
        self.assertTrue(ok)
        bad = [
            self.summary([("WIFI_HOPPING", "stay24", "5/80+/high/02:00:00:00:00:01/evening/fibre", 4, 1, 0, 0)]),
            self.summary([("WIFI_HOPPING", "stay24", "5/80+/high/192.168.1.20/evening/fibre", 4, 1, 0, 0)]),
            self.summary([("WIFI_HOPPING", "stay24", "5/80+/high/phone/evening", 4, 1, 0, 0)]),
            self.summary([("WIFI_HOPPING", "stay24", self.KEY, 4, 3, 2, 0)]),      # more helped+worse than tried
            self.summary([("wifi hopping", "stay24", self.KEY, 4, 1, 0, 0)]),
            self.summary([("WIFI_HOPPING", "stay24", self.KEY, 4, 1.5, 0, 0)]),
            self.summary([], [(9, 3, 22.0, 3.0)]),
            self.summary([], line="my-street"),
            dict(self.summary([]), hostname="laptop-anna"),
            {"v": 2, "outcomes": []},
        ]
        for b in bad:
            self.assertIsNone(fl.check(b)[0], b)

    def test_merge_k_cap_and_publish(self):
        from aigw_cortex import fleetlearn as fl
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            st = fl.Settings(k=3, cap=50, gap=0)
            g = [s.enroll(f"h{i}")[0] for i in range(4)]
            # three homes: keeping a phone on 2.4 GHz rarely helps, waiting often does
            for gw in g[:3]:
                code, ans = fl.accept(s, st, gw, self.summary(
                    [("WIFI_HOPPING", "stay24", self.KEY, 10, 2, 1, 0), ("WIFI_HOPPING", "wait", self.KEY, 10, 7, 0, 0)],
                    [(1, 3, 20.0, 3.0)]))
                self.assertEqual(code, 200)
            # one home only: LINE_FULL (below k), and a huge count (capped at 50)
            fl.accept(s, st, g[3], self.summary([("LINE_FULL", "sqm", self.KEY, 90, 9, 0, 0),
                                                  ("WIFI_HOPPING", "wait", self.KEY, 1000, 1000, 0, 0)],
                                                 [(1, 3, 80.0, 3.0)]))
            agg = fl.aggregate(s, st)
            by = {(c, a, k): (h, t, he, w, u) for c, a, k, h, t, he, w, u in agg["buckets"]}
            self.assertEqual(by[("WIFI_HOPPING", "stay24", self.KEY)], (3, 30, 6, 3, 0))
            self.assertEqual(by[("WIFI_HOPPING", "stay24", "5/*/high/*/*/fibre")], (3, 30, 6, 3, 0))
            self.assertEqual(by[("WIFI_HOPPING", "stay24", "*/*/*/*/*/*")], (3, 30, 6, 3, 0))
            self.assertEqual(by[("WIFI_HOPPING", "wait", self.KEY)], (4, 80, 71, 0, 0))     # 30 + capped 50
            self.assertFalse(any(c == "LINE_FULL" for c, *_ in agg["buckets"]), "one home's bucket is left out")
            n = {(line, m, b): (h, mean) for line, m, b, h, mean, _ in agg["normals"]}
            self.assertEqual(n[("fibre", 1, 3)], (4, 20.0))       # the median: one odd home doesn't move it
            self.assertIn(("*", 1, 3), n)
            # published once, signed, and the gateway's format
            m = [x for x in s.latest_models() if x["name"] == "fleet-priors"]
            self.assertTrue(m)
            with open(os.path.join(s.models_dir(), m[0]["file"]), "rb") as f:
                data = f.read().decode()
            self.assertTrue(data.startswith("# aigw-priors fleet-priors\n# version "))
            self.assertIn("# k 3\n", data)
            self.assertIn(f"o WIFI_HOPPING stay24 {self.KEY} 3 30 6 3 0\n", data)
            self.assertNotIn("LINE_FULL", data)
            for gw in g:
                self.assertNotIn(gw, data)
            self.assertTrue(os.path.exists(os.path.join(s.models_dir(), m[0]["file"] + ".sig")))
            v = m[0]["version"]
            self.assertIsNone(fl.maybe_publish(s, st), "nothing changed: not published again")
            fl.accept(s, st, g[0], self.summary([("WIFI_HOPPING", "stay24", self.KEY, 12, 2, 1, 0)]))
            self.assertGreater([x for x in s.latest_models() if x["name"] == "fleet-priors"][0]["version"], v)
            # a home that leaves takes its share with it
            s.remove("h3")
            agg = fl.aggregate(s, st)
            self.assertNotIn(("WIFI_HOPPING", "wait", self.KEY),
                             {(c, a, k) for c, a, k, *_ in agg["buckets"]}, "two homes left: below k")

    def test_off(self):
        from aigw_cortex import fleetlearn as fl
        with tempfile.TemporaryDirectory() as d:
            s = Store(d)
            self.assertEqual(fl.accept(s, fl.Settings(enabled=False), "x", self.summary([]))[0], 409)


if __name__ == "__main__":
    unittest.main()
