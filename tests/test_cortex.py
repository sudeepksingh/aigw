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


if __name__ == "__main__":
    unittest.main()
