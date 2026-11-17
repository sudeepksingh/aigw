# SPDX-License-Identifier: Apache-2.0
"""Staged firmware rollouts (cortex/aigw_cortex/rollout.py): the pure rules
(waves, pass, wait, stop) and a rollout in the store, fed with the homes'
upgrade checks as their bridges report them.

    python3 tests/test_rollout.py
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cortex"))

from aigw_cortex import fleet, rollout as ro  # noqa: E402
from aigw_cortex.mcp import Server  # noqa: E402
from aigw_cortex.store import Store  # noqa: E402

T0 = 1_800_000_000
H = 3600


def home(state, before=None, after=None):
    return {"state": state, "score_before": before, "score_after": after}


class Plan(unittest.TestCase):
    def test_waves(self):
        self.assertEqual(ro.plan(200, [1, 10, 50, 100]), [2, 18, 80, 100])
        self.assertEqual(ro.plan(10, [1, 10, 50, 100]), [1, 1, 3, 5])    # each wave at least one more home
        self.assertEqual(ro.plan(3, [1, 10, 50, 100]), [1, 1, 1])
        self.assertEqual(ro.plan(1, [1, 10, 50, 100]), [1])
        self.assertEqual(ro.plan(100, [5, 20]), [5, 15])                  # not to 100%: only that many
        self.assertEqual(ro.plan(0, [1, 100]), [])
        for bad in ([10, 5], [0, 10], [50, 120], []):
            with self.assertRaises(ValueError):
                ro.plan(10, bad)

    def test_order_spread(self):
        ids = [f"g{i}" for i in range(20)]
        a, b = ro.order("spring", ids), ro.order("autumn", ids)
        self.assertEqual(sorted(a), sorted(ids))
        self.assertNotEqual(a, b)                      # not always the same homes first
        self.assertEqual(a, ro.order("spring", list(reversed(ids))))


class Judge(unittest.TestCase):
    R = ro.Rules(max_bad=10, max_score_drop=2, min_reports=80, timeout_s=72 * H)

    def test_pass(self):
        hs = [home("kept", 97, 96)] * 9 + [home("rolled_back", 97, 60)]
        j = ro.judge(hs, self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "pass", j)
        self.assertIn("9 of the 10 homes kept it", j["reason"])
        self.assertIn("1 got worse (within 10%)", j["reason"])
        self.assertEqual((j["score_before"], j["score_after"]), (97, 96))

    def test_stop_on_rollbacks_early(self):
        # 2 of 10 rolled back while 8 are still being watched: over 10% already, no need to wait
        hs = [home("rolled_back"), home("not_booted")] + [home("watching")] * 8
        j = ro.judge(hs, self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "stop")
        self.assertIn("2 of the 10 homes got worse on it", j["reason"])
        self.assertIn("1 rolled back", j["reason"])
        self.assertIn("1 did not boot it", j["reason"])
        self.assertIn("more than the 10% allowed (1 of 10)", j["reason"])

    def test_one_home_canary(self):
        j = ro.judge([home("worse")], self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "stop")              # a canary of one: one bad home is too many
        self.assertEqual(ro.judge([home("kept")], self.R, T0, T0 + H)["verdict"], "pass")

    def test_stop_on_score_drop(self):
        hs = [home("kept", 98, 94), home("kept", 96, 93), home("kept", 99, 97), home("kept", -1, 50)]
        j = ro.judge(hs, self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "stop", j)
        self.assertIn("fell by 3.0 points in the 3 homes that kept it", j["reason"])   # -1: not known, not counted
        self.assertIn("97.7% of the time without a problem before, 94.7% after", j["reason"])
        hs = [home("kept", 98, 97), home("kept", 96, 95)]
        self.assertEqual(ro.judge(hs, self.R, T0, T0 + H)["verdict"], "pass")       # 1 point: within 2

    def test_too_few_reports_wait(self):
        hs = [home("kept")] * 7 + [home("watching")] * 2 + [home("sent")]
        j = ro.judge(hs, self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "wait")
        self.assertEqual(j["reason"], "7 of the 10 homes reported, 8 needed; 3 on their way.")
        # ... and the scores aren't judged until enough homes reported
        hs = [home("kept", 99, 80)] + [home("watching")] * 9
        self.assertEqual(ro.judge(hs, self.R, T0, T0 + H)["verdict"], "wait")

    def test_silence_stops_after_timeout(self):
        hs = [home("kept")] * 3 + [home("sent")] * 7
        self.assertEqual(ro.judge(hs, self.R, T0, T0 + 71 * H)["verdict"], "wait")
        j = ro.judge(hs, self.R, T0, T0 + 72 * H)
        self.assertEqual(j["verdict"], "stop")
        self.assertIn("only 3 of the 10 homes reported in 72 h; 8 needed", j["reason"])

    def test_current_and_failed(self):
        hs = [home("current")] * 4 + [home("kept")] * 5 + [home("failed")]
        j = ro.judge(hs, self.R, T0, T0 + H)
        self.assertEqual(j["verdict"], "pass")           # already on it: fine; could not install it: bad
        hs = [home("failed")] * 2 + [home("kept")] * 8
        self.assertIn("2 could not install it", ro.judge(hs, self.R, T0, T0 + H)["reason"])


class Advance(unittest.TestCase):
    P, W, S = {"verdict": "pass", "reason": "ok."}, {"verdict": "wait", "reason": "w"}, {"verdict": "stop",
                                                                                          "reason": "bad."}

    def test_rules(self):
        self.assertEqual(ro.advance("running", 1, 3, [self.P]), ("running", 2, "Wave 1 of 3 passed: ok. Wave 2 starts."))
        self.assertEqual(ro.advance("running", 2, 3, [self.P, self.W]), ("running", 2, None))
        self.assertEqual(ro.advance("running", 3, 3, [self.P, self.P, self.P])[0:2], ("done", 3))
        # a stop in any opened wave, also an earlier one whose late homes got worse
        self.assertEqual(ro.advance("running", 2, 3, [self.S, self.W]),
                         ("stopped", 2, "Stopped in wave 1 of 3: bad."))
        # paused: never on to the next wave, but it can still stop
        self.assertEqual(ro.advance("paused", 1, 3, [self.P]), ("paused", 1, None))
        self.assertEqual(ro.advance("paused", 1, 3, [self.S])[0], "stopped")
        for st in ("stopped", "aborted", "done"):
            self.assertEqual(ro.advance(st, 1, 3, [self.P]), (st, 1, None))


CLOCK = [T0]


def report(s, gw, state, to="2.0", activated=None, before=97, after=97):
    """A bridge's report of the home's upgrade check, a minute after the last one."""
    CLOCK[0] += 60
    s.report(gw, {"upgrade": {"state": state, "from": "1.0", "to": to,
                              "activated": CLOCK[0] - 5 if activated is None else activated,
                              "score_before": before, "score_after": after, "summary": "words"}}, now=CLOCK[0])


class InStore(unittest.TestCase):
    def setUp(self):
        CLOCK[0] = T0
        self.tmp = tempfile.TemporaryDirectory()
        self.s = Store(self.tmp.name)
        self.ids = {}
        for i in range(10):
            gid, _ = self.s.enroll(f"home-{i}", "north" if i < 8 else "south")
            self.ids[f"home-{i}"] = gid

    def tearDown(self):
        self.tmp.cleanup()

    def wave_ids(self, name, k):
        r = ro.status(self.s, name, homes=True)
        return [self.ids[h["home"]] for h in r["list"] if h["wave"] == k]

    def test_waves_pass_to_done(self):
        rid = ro.create(self.s, "spring", "2.0", "https://fw.example/2.0.img", waves=[10, 50, 100],
                        rules=ro.Rules(max_bad=20), now=T0)
        st = ro.status(self.s, rid, T0)
        self.assertEqual([w["homes"] for w in st["per_wave"]], [1, 4, 5])
        self.assertEqual((st["state"], st["wave"], st["homes"]["states"]), ("running", 1, {"waiting": 9, "cleared": 1}))
        self.assertEqual(len(ro.cleared(self.s, "spring")), 1)               # the ACS may push to one home
        first = self.wave_ids("spring", 1)
        report(self.s, first[0], "Watching")
        self.assertEqual(ro.status(self.s, rid)["wave"], 1)
        report(self.s, first[0], "Kept", after=98)
        st = ro.status(self.s, rid)
        self.assertEqual(st["wave"], 2, st)
        self.assertIn("Wave 1 of 3 passed: 1 of the 1 homes kept it", st["reason"])
        self.assertEqual(len(ro.cleared(self.s, "spring")), 4)
        # an older upgrade, or another version, does not count for this rollout
        w2 = self.wave_ids("spring", 2)
        report(self.s, w2[0], "Kept", activated=T0 - 100)
        report(self.s, w2[1], "Kept", to="1.9")
        self.assertEqual(ro.status(self.s, rid)["homes"]["states"].get("kept"), 1)
        for g in w2:
            report(self.s, g, "Kept")
        self.assertEqual(ro.status(self.s, rid)["wave"], 3)
        w3 = self.wave_ids("spring", 3)
        report(self.s, w3[4], "RolledBack", before=97, after=40)            # 1 of 5: within 20%
        for g in w3[:3]:
            report(self.s, g, "Kept")
        st = ro.status(self.s, rid)
        self.assertEqual(st["state"], "done", st)                            # 4 of 5 reported: enough (80%)
        self.assertIn("Done: wave 3 of 3 passed: 3 of the 5 homes kept it, 1 got worse (within 20%)", st["reason"])
        self.assertEqual(ro.cleared(self.s, "spring"), [])

    def test_stop_on_rollbacks_and_no_names(self):
        rid = ro.create(self.s, "bad", "2.0", "", waves=[50, 100], rules=ro.Rules(max_bad=25), now=T0)
        w1 = self.wave_ids("bad", 1)
        report(self.s, w1[0], "RollingBack")
        report(self.s, w1[1], "Kept")
        self.assertEqual(ro.status(self.s, rid)["state"], "running")         # 1 of 5 worse: within 25%
        report(self.s, w1[0], "RolledBack")
        report(self.s, w1[2], "NotBooted")
        st = ro.status(self.s, rid)
        self.assertEqual(st["state"], "stopped", st)
        self.assertIn("Stopped in wave 1 of 2: 2 of the 5 homes got worse on it (1 rolled back, 1 did not boot it)",
                      st["reason"])
        self.assertEqual(ro.cleared(self.s, "bad"), [])
        self.assertEqual(st["homes"]["states"]["waiting"], 5)                # wave 2 never got it
        everything = json.dumps({k: v for k, v in st.items() if k != "list"}) + fleet.text(fleet.report(self.s))
        for n in self.ids:
            self.assertNotIn(n, everything)                                   # counts and words, no names
        with self.assertRaises(ValueError):
            ro.control(self.s, "bad", "resume")                              # a stopped rollout stays stopped

    def test_stop_on_score_drop(self):
        rid = ro.create(self.s, "slow", "2.0", "", lists=[["home-0", "home-1"], ["home-2"]], now=T0)
        report(self.s, self.ids["home-0"], "Kept", before=98, after=93)
        report(self.s, self.ids["home-1"], "Kept", before=96, after=95)
        st = ro.status(self.s, rid)
        self.assertEqual(st["state"], "stopped")
        self.assertIn("fell by 3.0 points in the 2 homes that kept it", st["reason"])

    def test_pause_resume_abort(self):
        rid = ro.create(self.s, "p", "2.0", "", lists=[["home-0"], ["home-1", "home-2"]], now=T0)
        ro.control(self.s, "p", "pause", "the help desk is busy", now=T0 + 1)
        self.assertEqual(ro.cleared(self.s, "p"), [])                        # paused: the ACS pushes to nobody
        report(self.s, self.ids["home-0"], "Kept")
        st = ro.status(self.s, rid)
        self.assertEqual((st["state"], st["wave"]), ("paused", 1))           # passed, but paused: wave 2 waits
        self.assertIn("Paused by you (the help desk is busy)", st["log"][-1]["text"])
        st = ro.control(self.s, "p", "resume", now=T0 + 2)
        self.assertEqual((st["state"], st["wave"]), ("running", 2))
        self.assertEqual([r["name"] for r in ro.cleared(self.s, "p")], ["home-1", "home-2"])
        st = ro.control(self.s, "p", "abort", "found a bug", now=T0 + 3)
        self.assertEqual(st["state"], "aborted")
        self.assertEqual(ro.cleared(self.s, "p"), [])
        for what in ("pause", "resume", "abort"):
            with self.assertRaises(ValueError):
                ro.control(self.s, "p", what)

    def test_too_few_reports_wait_then_timeout(self):
        rid = ro.create(self.s, "quiet", "2.0", "", waves=[50, 100], rules=ro.Rules(timeout_s=24 * H), now=T0)
        w1 = self.wave_ids("quiet", 1)
        for g in w1[:3]:
            report(self.s, g, "Kept")
        st = ro.step(self.s, rid, T0 + H)
        self.assertEqual((st["state"], st["wave"]), ("running", 1))
        self.assertEqual(st["per_wave"][0]["reason"], "3 of the 5 homes reported, 4 needed; 2 on their way.")
        st = ro.step(self.s, rid, T0 + 24 * H)
        self.assertEqual(st["state"], "stopped")
        self.assertIn("only 3 of the 5 homes reported in 24 h", st["reason"])

    def test_bridge_offer_once(self):
        rid = ro.create(self.s, "b", "2.0", "https://fw.example/2.0.img", sha256="ab" * 32, size=1234,
                        via="bridge", lists=[["home-0"], ["home-1"]], now=T0)
        g0, g1 = self.ids["home-0"], self.ids["home-1"]
        self.assertIsNone(ro.offer(self.s, g1))                              # not cleared yet
        o = ro.offer(self.s, g0)
        self.assertEqual(o, {"rollout": rid, "version": "2.0", "url": "https://fw.example/2.0.img",
                             "checksum": "ab" * 32, "size": "1234"})
        self.assertIsNone(ro.offer(self.s, g0))                              # sent once
        self.assertTrue(ro.offer_result(self.s, g0, rid, True))
        self.assertFalse(ro.offer_result(self.s, g0, rid, True))             # only for what was sent
        # the image names itself otherwise (OpenWRT): the home it was sent to still counts
        report(self.s, g0, "Kept", to="2.0-r123")
        st = ro.status(self.s, rid)
        self.assertEqual(st["wave"], 2)
        o = ro.offer(self.s, g1)
        ro.offer_result(self.s, g1, rid, False, error="checksum mismatch")
        st = ro.status(self.s, rid, homes=True)
        self.assertEqual(st["state"], "stopped")
        self.assertIn("1 could not install it", st["reason"])
        self.assertNotIn("checksum", st["reason"])
        self.assertEqual([h["error"] for h in st["list"] if h["state"] == "failed"], ["checksum mismatch"])
        # an ACS rollout is not offered through bridges
        ro.create(self.s, "a", "3.0", "https://x/3.img", lists=[["home-5"]], now=T0)
        self.assertIsNone(ro.offer(self.s, self.ids["home-5"]))

    def test_acs_result_and_group(self):
        rid = ro.create(self.s, "south", "2.0", "", waves=[100], group="south", now=T0)
        self.assertEqual(sorted(r["name"] for r in ro.cleared(self.s, "south")), ["home-8", "home-9"])
        with self.assertRaises(ValueError):
            ro.acs_result(self.s, "south", "home-0", "Kept")                 # not in this rollout
        with self.assertRaises(ValueError):
            ro.acs_result(self.s, "south", "home-8", "Maybe")
        ro.acs_result(self.s, "south", "home-8", "Kept", 97, 97)
        st = ro.acs_result(self.s, "south", "home-9", "RollbackConfirmed")
        self.assertEqual(st["state"], "stopped")                             # 1 of 2 rolled back: over 5%
        with self.assertRaises(ValueError):
            ro.create(self.s, "south", "2.1", "")                            # names are unique
        with self.assertRaises(ValueError):
            ro.create(self.s, "none", "2.1", "", group="nowhere")
        with self.assertRaises(ValueError):
            ro.create(self.s, "via", "2.1", "", via="bridge")                # the bridge needs a URL
        with self.assertRaises(ValueError):
            ro.create(self.s, "twice", "2.1", "", lists=[["home-0"], ["home-0"]])

    def test_fleet_report_and_mcp(self):
        ro.create(self.s, "spring", "2.0", "", waves=[10, 100], now=T0)
        r = fleet.report(self.s, now=T0 + H)
        self.assertEqual([x["name"] for x in r["rollouts"]], ["spring"])
        self.assertIn("rollout      spring (2.0, 10 homes): running, wave 1 of 2", fleet.text(r))
        self.assertEqual(fleet.report(self.s, group="south", now=T0 + H)["rollouts"], [])
        srv = Server(self.s)

        def call(name, args):
            res = srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                              "params": {"name": name, "arguments": args}})["result"]
            return res["isError"], res["content"][0]["text"]

        err, out = call("rollout_status", {})
        self.assertFalse(err)
        self.assertEqual(json.loads(out)[0]["state"], "running")
        self.assertTrue(call("rollout_hold", {"name": "spring", "action": "resume", "reason": "x"})[0])
        self.assertTrue(call("rollout_hold", {"name": "spring", "action": "pause", "reason": ""})[0])
        err, out = call("rollout_hold", {"name": "spring", "action": "pause", "reason": "odd reports"})
        self.assertFalse(err, out)
        self.assertEqual(json.loads(out)["state"], "paused")
        self.assertIn("Paused by an AI assistant (odd reports)", json.loads(out)["log"][-1]["text"])
        tools = {t["name"]: t for t in srv.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})["result"]["tools"]}
        self.assertTrue(tools["rollout_status"]["annotations"]["readOnlyHint"])
        self.assertFalse(tools["rollout_hold"]["annotations"]["readOnlyHint"])


if __name__ == "__main__":
    unittest.main()
