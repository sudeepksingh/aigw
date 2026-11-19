# SPDX-License-Identifier: Apache-2.0
"""ISP ticketing (v0.42, cortex/aigw_cortex/tickets.py) against a local mock
TMF621 server (tests/tmf621_mock.py, on 127.0.0.1): create, update, resolve,
retry after the server was down, no duplicates after a lost reply or a restart,
the ISP's hub events, v5 bodies, dry run, and signed webhooks. No network calls
leave this machine.

    python3 tests/test_tickets.py
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cortex"))
sys.path.insert(0, HERE)

from aigw_cortex import outage, tickets  # noqa: E402
from aigw_cortex.store import Store  # noqa: E402
from tmf621_mock import TOKEN, V4, V5, Mock  # noqa: E402

T = 1795469400
UPLINK = "203.0.113.1"
CLIENT_SECRET = "s3cret-client"
HOOK_SECRET = "hook-s3cret-value"
LISTENER_TOKEN = "listener-s3cret-token"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def line_down(since, code="ISP_DOWN"):
    return {"status": "problem", "problem": {"code": code, "area": "Internet", "since": since, "reported": True}}


def pro_ticket(tid, status="Raised", is_open=True, opened=T - 300, told=True, **kw):
    t = {"id": tid, "status": status, "open": 1 if is_open else 0, "code": "WIFI_COVERAGE", "fault": "HOME_WIFI",
         "severity": "problem", "points": 4, "devices": 2, "since": opened, "opened": opened, "told_isp": 1 if told else 0,
         "summary": "Two devices keep losing the Wi-Fi in the far room", "evidence": "RSSI -84 dBm, 31 drops in 1 h",
         "next_step": "Offer a mesh point for the far room", "customer_note": "Your Wi-Fi is weak in one room; we "
                                                                             "can help."}
    t.update(kw)
    return t


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        with open(os.path.join(self.dir, "client.secret"), "w") as f:
            f.write(CLIENT_SECRET + "\n")
        with open(os.path.join(self.dir, "hook.secret"), "w") as f:
            f.write(HOOK_SECRET)
        for n in ("client.secret", "hook.secret"):          # as docs/isp-ticketing.md says (refused otherwise)
            os.chmod(os.path.join(self.dir, n), 0o600)
        os.environ["AIGW_TEST_LISTENER_TOKEN"] = LISTENER_TOKEN
        tickets._tokens.clear()
        self.mock = None

    def tearDown(self):
        if self.mock:
            self.mock.stop()
        self.tmp.cleanup()

    def cfg(self, port=None, version=4, **kw):
        base = V5 if version == 5 else V4
        c = {"enabled": True, "events": ["outage", "proactive"], "retry": {"base_s": 10, "max_s": 600},
             "tmf621": {"url": f"http://127.0.0.1:{port or self.mock.port}{base}", "version": version, "timeout": 3,
                        "auth": {"type": "oauth2", "token_url": f"http://127.0.0.1:{port or self.mock.port}{TOKEN}",
                                 "client_id": "aigw", "client_secret_file": os.path.join(self.dir, "client.secret")},
                        "listener": {"token_env": "AIGW_TEST_LISTENER_TOKEN"}}}
        c.update(kw)
        return tickets.check_config(c)

    def fleet(self, s, n=6):
        gws = [s.enroll("home%d" % i)[0] for i in range(n)]
        for g in gws:
            s.report(g, {"status": {"version": "0.42.0", "wan_gateway": UPLINK}, "interval": 300}, T - 600)
        return gws

    def outage_of(self, s, gws, k=3, at=T):
        for g in gws[k:]:
            s.report(g, {"status": {"wan_gateway": UPLINK}, "care": {"status": "ok"}}, at)
        for i in range(k):
            s.report(gws[i], {"status": {"wan_gateway": UPLINK}, "care": line_down(T - 300 + i * 30)}, at)
        inc = outage.incidents(s, open_only=True)
        self.assertEqual(len(inc), 1)
        return inc[0]

    def recover(self, s, gws, at):
        for g in gws:
            s.report(g, {"status": {"wan_gateway": UPLINK}, "care": {"status": "ok"}}, at)


class Config(unittest.TestCase):
    def test_off_by_default_and_checked(self):
        with tempfile.TemporaryDirectory() as d:
            c = tickets.load_config(d)
            self.assertFalse(c["enabled"])
            self.assertEqual(tickets.scan(Store(d), c), {})
        bad = [
            {"tmf621": {"url": "https://tt.example/v4", "auth": {"type": "bearer", "token": "inline"}}},
            {"tmf621": {"url": "https://tt.example/v4", "auth": {"type": "bearer"}}},
            {"tmf621": {"url": "http://tt.example/v4"}},                        # http beyond this machine
            {"tmf621": {"url": "https://user:pw@tt.example/v4"}},
            {"tmf621": {"url": "https://tt.example/v4", "auth": {"type": "oauth2", "token_url": "https://t.example",
                                                                 "client_id": "x", "client_secret": "inline"}}},
            {"webhooks": [{"url": "https://hooks.example/a"}]},                 # unsigned
            {"webhooks": [{"url": "https://hooks.example/a", "secret": "inline"}]},
            {"events": ["everything"]},
            {"tmf621": {"url": "https://tt.example/v4", "version": 3}},
        ]
        for c in bad:
            with self.assertRaises(ValueError, msg=c):
                tickets.check_config(c)
        ok = tickets.check_config({"enabled": True, "tmf621": {"url": "https://tt.example/tmf-api/troubleTicket/v4/"}})
        self.assertEqual(ok["tmf621"]["url"], "https://tt.example/tmf-api/troubleTicket/v4")
        self.assertEqual(ok["tmf621"]["version"], 4)


class Signatures(unittest.TestCase):
    def test_sign_and_verify(self):
        body = b'{"type":"outage.opened"}'
        sig = tickets.sign("k", 1700000000, body)
        self.assertTrue(sig.startswith("v1=") and len(sig) == 67)
        self.assertTrue(tickets.verify("k", "1700000000", body, sig, now=1700000100))
        self.assertFalse(tickets.verify("k", 1700000000, body + b" ", sig, now=1700000100))      # body changed
        self.assertFalse(tickets.verify("other", 1700000000, body, sig, now=1700000100))         # wrong secret
        self.assertFalse(tickets.verify("k", 1700000001, body, sig, now=1700000100))             # timestamp changed
        self.assertFalse(tickets.verify("k", 1700000000, body, sig, now=1700000000 + 301))       # too old: a replay
        self.assertFalse(tickets.verify("k", "x", body, sig))


class Tmf621(Base):
    def test_outage_create_update_resolve(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        gws = self.fleet(s, 8)
        tickets.scan(s, cfg, T - 60)                        # on before the outage
        inc = self.outage_of(s, gws, 3)
        r = tickets.sync(s, cfg, T + 10)
        self.assertEqual(r["found"], {"outage.opened": 1})
        self.assertEqual(r["sent"], {"sent": 1})
        self.assertEqual(len(self.mock.tickets), 1)
        tt = list(self.mock.tickets.values())[0]
        ext = f"aigw:{tickets.instance(s)}:outage:{inc['id']}"
        self.assertEqual(tt["externalId"], ext)
        self.assertEqual((tt["severity"], tt["ticketType"], tt["@type"]), ("Critical", "AreaOutage", "TroubleTicket"))
        self.assertIn(f"3 of 8 homes", tt["name"])
        ents = tt["relatedEntity"]
        self.assertEqual(ents[0], {"id": "wan:" + UPLINK, "name": "wan:" + UPLINK, "role": "uplink",
                                   "@referredType": "Resource"})
        self.assertEqual(sorted(e["name"] for e in ents[1:]), ["home0", "home1", "home2"])
        self.assertTrue(all(e["role"] == "affectedGateway" for e in ents[1:]))
        self.assertEqual(tt["relatedParty"][0]["role"], "originator")
        self.assertEqual([n["id"] for n in tt["note"]], ["aigw-brief", "aigw-evidence", "aigw-next-step"])
        self.assertTrue(all(n["author"] == "AIGW OS cortex" and n["date"].endswith("Z") for n in tt["note"]))
        # the OAuth2 client credentials: one token, used as a bearer
        self.assertEqual(len(self.mock.requests("POST")), 2)
        self.assertTrue(self.mock.log[1][2]["Authorization"].startswith("Bearer tok-"))
        st = tickets.listing(s)[0]
        self.assertEqual((st["sync"], st["remote_id"], st["remote_status"]), ("synced", tt["id"], "acknowledged"))
        # nothing new: nothing sent
        self.assertEqual(tickets.sync(s, cfg, T + 20), {"found": {}, "sent": {}})
        # a fourth home goes down while it is on: one PATCH with the new count
        s.report(gws[3], {"status": {"wan_gateway": UPLINK}, "care": line_down(T + 400)}, T + 420)
        r = tickets.sync(s, cfg, T + 430)
        self.assertEqual(r["found"], {"outage.updated": 1})
        self.assertEqual(tt["patches"], 1)
        self.assertIn("4 of 8 homes", tt["name"])
        self.assertEqual(len(tt["relatedEntity"]), 5)
        # the homes recover: resolved
        self.recover(s, gws, T + 3600)
        self.assertEqual(outage.incidents(s, open_only=True), [])
        r = tickets.sync(s, cfg, T + 3610)
        self.assertEqual(r["found"], {"outage.resolved": 1})
        self.assertEqual(tt["status"], "resolved")
        self.assertTrue(tt["statusChangeReason"].startswith("Over: "))
        self.assertTrue(tt["resolutionDate"].endswith("Z"))
        self.assertEqual(tt["note"][-1]["id"], "aigw-resolution")
        self.assertEqual(len(self.mock.tickets), 1)
        self.assertEqual(len(self.mock.requests("POST")), 2)            # one token, one ticket: ever
        self.assertEqual(tickets.summary(s, cfg)["tickets"], {"open": 0, "resolved": 1, "isp_closed": 0})

    def test_proactive_tickets(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        g = s.enroll("flat-12", "riverside")[0]
        tickets.scan(s, cfg, T - 600)
        raised = pro_ticket(4)
        fixed = pro_ticket(3, "Fixed", is_open=False, told=False, code="WIFI_HOPPING",      # AIGW's own: not the ISP's
                           opened=T - 3600)
        s.report(g, {"status": {"version": "0.42.0"}, "proactive": {"tickets": [raised, fixed]}}, T)
        r = tickets.sync(s, cfg, T + 5)
        self.assertEqual(r["found"], {"proactive.opened": 1})
        tt = list(self.mock.tickets.values())[0]
        self.assertEqual(tt["externalId"], f"aigw:{tickets.instance(s)}:proactive:{g}:4:{T - 300}")
        self.assertEqual((tt["severity"], tt["ticketType"]), ("Major", "ProactiveCare"))
        self.assertEqual(tt["name"], "WIFI_COVERAGE at flat-12: Two devices keep losing the Wi-Fi in the far room")
        self.assertEqual(tt["relatedEntity"][0]["name"], "flat-12")
        self.assertEqual(tt["relatedEntity"][1], {"id": "riverside", "name": "riverside", "role": "uplink",
                                                  "@referredType": "Resource"})
        notes = {n["id"]: n["text"] for n in tt["note"]}
        self.assertEqual(notes["aigw-next-step"], "Next step: Offer a mesh point for the far room")
        self.assertTrue(notes["aigw-customer-note"].startswith("For the customer: Your Wi-Fi is weak"))
        self.assertIn("Raised", notes["aigw-status"])
        # the gateway's news: worse, then fixed
        raised2 = dict(raised, devices=3, evidence="RSSI -86 dBm, 45 drops in 1 h")
        s.report(g, {"proactive": {"tickets": [raised2, fixed]}}, T + 600)
        self.assertEqual(tickets.sync(s, cfg, T + 605)["found"], {"proactive.updated": 1})
        self.assertIn("45 drops", {n["id"]: n["text"] for n in tt["note"]}["aigw-evidence"])
        done = dict(raised2, open=0, status="Resolved")
        s.report(g, {"proactive": {"tickets": [done, fixed]}}, T + 1200)
        self.assertEqual(tickets.sync(s, cfg, T + 1205)["found"], {"proactive.resolved": 1})
        self.assertEqual(tt["status"], "resolved")
        self.assertEqual(len(self.mock.tickets), 1)
        # "all": AIGW's own fixes become tickets too (opened and resolved at once)
        cfg_all = self.cfg(proactive="all")
        late = pro_ticket(5, "Fixed", is_open=False, told=False, opened=T + 1300)
        s.report(g, {"proactive": {"tickets": [done, fixed, late]}}, T + 1400)
        r = tickets.sync(s, cfg_all, T + 1405)
        self.assertEqual(r["found"], {"proactive.opened+resolved": 1})            # #3 was over before ticketing: not sent
        self.assertEqual(len(self.mock.tickets), 2)
        self.assertEqual(self.mock.tickets["1001"]["status"], "resolved")

    def test_retry_after_the_server_was_down(self):
        port = free_port()                              # nobody listens there yet
        s = Store(self.dir)
        cfg = self.cfg(port)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        r = tickets.sync(s, cfg, T + 10)
        self.assertEqual(r["sent"], {"retrying": 1})
        box = tickets.outbox(s)[0]
        self.assertEqual((box["state"], box["attempts"]), ("queued", 1))
        self.assertTrue(T + 10 + 8 <= box["next_try"] <= T + 10 + 12, box)          # 10 s, +-20%
        self.assertEqual(tickets.listing(s)[0]["sync"], "retrying")
        self.assertIn("127.0.0.1", tickets.summary(s, cfg)["last_error"]["text"])
        # not due yet: not tried
        self.assertEqual(tickets.flush(s, cfg, T + 11), {})
        self.assertEqual(tickets.flush(s, cfg, T + 30), {"retrying": 1})
        self.assertGreater(tickets.outbox(s)[0]["next_try"], T + 30 + 15)          # 20 s, then 40 s...
        # the server is back (and briefly busy: 503, then 429 with Retry-After)
        self.mock = Mock(port).start()
        self.mock.fail = [503]
        self.assertEqual(tickets.flush(s, cfg, T + 3600), {"retrying": 1})
        self.mock.fail = [429]
        self.assertEqual(tickets.flush(s, cfg, T + 7200), {"retrying": 1})
        self.assertEqual(tickets.flush(s, cfg, T + 10800), {"sent": 1})
        self.assertEqual(len(self.mock.tickets), 1)
        self.assertEqual(tickets.listing(s)[0]["sync"], "synced")
        self.assertEqual(tickets.outbox(s)[0]["attempts"], 5)

    def test_no_duplicate_after_a_lost_reply_and_a_restart(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        self.mock.lose_create_reply = True              # the ISP keeps it, its answer is lost
        self.assertEqual(tickets.sync(s, cfg, T + 10)["sent"], {"retrying": 1})
        self.assertEqual(len(self.mock.tickets), 1)
        # cortex restarts: a new Store on the same data, a new token, the same configuration
        s.db.close()
        tickets._tokens.clear()
        s = Store(self.dir)
        self.assertEqual(tickets.sync(s, cfg, T + 3600), {"found": {}, "sent": {"sent": 1}})
        self.assertEqual(len(self.mock.tickets), 1)                     # found by externalId, not posted again
        gets = [x for x in self.mock.requests("GET") if "/troubleTicket" in x[1]]
        self.assertEqual(len(gets), 1)
        self.assertEqual(tickets.listing(s)[0]["remote_id"], "1000")
        # and once more, after everything was sent: still nothing new
        s.db.close()
        s = Store(self.dir)
        self.assertEqual(tickets.sync(s, cfg, T + 3700), {"found": {}, "sent": {}})
        self.assertEqual(len(self.mock.tickets), 1)
        # a crash between the POST and keeping the ISP's id: the next try looks first
        s.report(gws[3], {"status": {"wan_gateway": UPLINK}, "care": line_down(T + 3000)}, T + 3710)
        s.x("UPDATE isp_tickets SET remote_id = ''")
        tickets.set_meta(s, "tried:" + tickets.listing(s)[0]["key"], T)
        tickets.sync(s, cfg, T + 3720)
        self.assertEqual(len(self.mock.tickets), 1)
        self.assertEqual(self.mock.tickets["1000"]["patches"], 1)

    def test_refused_is_kept_and_retried_by_hand(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        c = self.cfg()
        c["tmf621"]["ticket_type"]["outage"] = "Refused"           # the mock answers 400
        gws = self.fleet(s)
        tickets.scan(s, c, T - 60)
        self.outage_of(s, gws)
        self.assertEqual(tickets.sync(s, c, T + 10)["sent"], {"failed": 1})
        self.assertEqual(tickets.outbox(s)[0]["state"], "failed")
        self.assertIn("HTTP 400: refused", tickets.listing(s)[0]["error"])
        self.assertEqual(tickets.flush(s, c, T + 99999), {})          # not retried by itself
        self.assertEqual(tickets.retry(s), 1)
        self.assertEqual(tickets.flush(s, self.cfg(), T + 100000), {"sent": 1})
        self.assertEqual(len(self.mock.tickets), 1)

    def test_token_refreshed_once_on_401(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        tickets.sync(s, cfg, T + 10)
        self.mock.tokens.clear()                        # the ISP revoked it
        s.report(gws[3], {"status": {"wan_gateway": UPLINK}, "care": line_down(T + 400)}, T + 420)
        self.assertEqual(tickets.sync(s, cfg, T + 430)["sent"], {"sent": 1})
        self.assertEqual(len([x for x in self.mock.log if x[1] == TOKEN]), 2)

    def test_v5_bodies(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg(version=5)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        self.mock.lose_create_reply = True
        tickets.sync(s, cfg, T + 10)
        tickets.flush(s, cfg, T + 3600)
        self.assertEqual(len(self.mock.tickets), 1)                     # found by externalIdentifier.id
        tt = self.mock.tickets["1000"]
        self.assertNotIn("externalId", tt)
        self.assertEqual(tt["externalIdentifier"][0]["id"], tickets.listing(s)[0]["external_id"])
        e = tt["relatedEntity"][0]
        self.assertEqual((e["role"], e["@type"], e["entity"]["@type"], e["entity"]["id"]),
                         ("uplink", "RelatedEntity", "EntityRef", "wan:" + UPLINK))
        p = tt["relatedParty"][0]
        self.assertEqual((p["role"], p["partyOrPartyRole"]["id"]), ("originator", "aigw-cortex"))
        self.assertTrue(all(n["@type"] == "Note" for n in tt["note"]))

    def test_dry_run_sends_nothing(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg(dry_run=True)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        self.assertEqual(tickets.sync(s, cfg, T + 10)["sent"], {"dry-run": 1})
        self.assertEqual(self.mock.log, [])
        box = tickets.outbox(s)
        self.assertEqual(box[0]["state"], "dry-run")
        body = json.loads(s.q("SELECT body FROM ticket_outbox")[0]["body"])
        self.assertEqual(body["ticketType"], "AreaOutage")               # what would have gone
        self.assertEqual(tickets.listing(s)[0]["sync"], "dry-run")
        # dry run off: the ticket is created with its next news
        self.recover(s, gws, T + 3600)
        tickets.sync(s, self.cfg(), T + 3610)
        self.assertEqual(len(self.mock.tickets), 1)
        self.assertEqual(self.mock.tickets["1000"]["status"], "resolved")

    def test_secrets_never_shown(self):
        port = free_port()
        s = Store(self.dir)
        cfg = self.cfg(port)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        tickets.sync(s, cfg, T + 10)
        self.mock = Mock(port, client=("aigw", "another")).start()    # the token endpoint refuses our secret
        tickets.flush(s, cfg, T + 3600)
        seen = json.dumps([tickets.summary(s, cfg), tickets.outbox(s), tickets.listing(s), cfg,
                           s.q("SELECT * FROM ticket_meta"), s.q("SELECT * FROM ticket_outbox")])
        for secret in (CLIENT_SECRET, HOOK_SECRET, LISTENER_TOKEN):
            self.assertNotIn(secret, seen)
        self.assertIn("OAuth2 token", tickets.listing(s)[0]["error"])


class Hub(Base):
    def serve(self, s, cfg):
        from http.server import ThreadingHTTPServer
        from aigw_cortex.server import make_handler
        httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(s, log=False, tickets_cfg=cfg))
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        return f"http://127.0.0.1:{httpd.server_address[1]}"

    def post(self, url, body, token=None):
        h = {"Content-Type": "application/json"}
        if token:
            h["Authorization"] = "Bearer " + token
        req = urllib.request.Request(url, json.dumps(body).encode(), h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null")

    def test_register_and_events(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        cfg["tmf621"]["hub_callback"] = "https://cortex.isp.example/tmf621/listener"
        self.assertEqual(tickets.hub_register(s, cfg), "1")
        self.assertEqual(self.mock.hubs["1"]["callback"], "https://cortex.isp.example/tmf621/listener")
        api = self.serve(s, cfg)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        inc = outage.declare(s, "wan:" + UPLINK, "DOWN", "fibre cut at the exchange", now=T)
        tickets.sync(s, cfg, T + 10)
        tt = self.mock.tickets["1000"]
        self.assertIn("fibre cut", json.dumps(tt["note"]))
        ev = {"eventId": "e1", "eventType": "TroubleTicketStatusChangeEvent", "eventTime": "2026-11-24T10:00:00Z",
              "event": {"troubleTicket": {"id": "1000", "status": "inProgress"}}}
        self.assertEqual(self.post(api + "/tmf621/listener", ev)[0], 401)
        self.assertEqual(self.post(api + "/tmf621/listener", ev, "wrong")[0], 401)
        self.assertEqual(self.post(api + "/tmf621/listener/troubleTicketStatusChangeEvent", ev, LISTENER_TOKEN)[0], 201)
        self.assertEqual(tickets.listing(s)[0]["remote_status"], "inProgress")
        # the ISP fixed the fibre and resolved its ticket: the declared outage is over in cortex too
        ev["event"]["troubleTicket"]["status"] = "resolved"
        code, ans = self.post(api + "/tmf621/listener", ev, LISTENER_TOKEN)
        self.assertEqual((code, ans["ended_outage"]), (201, inc["id"]))
        self.assertEqual(outage.incidents(s, open_only=True), [])
        row = tickets.listing(s)[0]
        self.assertTrue(row["isp_closed"])
        # cortex says nothing more on a ticket the ISP closed
        n = len(self.mock.log)
        tickets.sync(s, cfg, T + 600)
        self.assertEqual([x for x in self.mock.log[n:] if x[0] == "PATCH"], [])
        # not one of ours: fine
        other = {"event": {"troubleTicket": {"id": "77", "externalId": "someone-else", "status": "closed"}}}
        self.assertEqual(self.post(api + "/tmf621/listener", other, LISTENER_TOKEN), (202, {"ok": True, "known": False}))
        self.assertEqual(self.post(api + "/tmf621/listener", {"x": 1}, LISTENER_TOKEN)[0], 400)
        self.assertEqual(tickets.hub_unregister(s, cfg), "1")
        self.assertEqual(self.mock.hubs, {})

    def test_listener_off_without_a_token(self):
        s = Store(self.dir)
        cfg = tickets.check_config({"enabled": True, "tmf621": {"url": "https://tt.example/v4"}})
        api = self.serve(s, cfg)
        self.assertEqual(self.post(api + "/tmf621/listener", {}, "anything")[0], 404)
        api2 = self.serve(s, tickets.check_config({}))
        self.assertEqual(self.post(api2 + "/tmf621/listener", {}, "anything")[0], 404)


class Webhooks(Base):
    def test_signed_events(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = tickets.check_config({"enabled": True, "events": ["outage"],
                                    "webhooks": [{"url": self.mock.url + "/hook",
                                                  "secret_file": os.path.join(self.dir, "hook.secret")},
                                                 {"url": self.mock.url + "/hook?resolved-only",
                                                  "secret_file": os.path.join(self.dir, "hook.secret"),
                                                  "events": ["outage.resolved"]}]})
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        self.assertEqual(tickets.sync(s, cfg, T + 10)["sent"], {"sent": 1})
        self.recover(s, gws, T + 3600)
        self.assertEqual(tickets.sync(s, cfg, T + 3610)["sent"], {"sent": 2})
        self.assertEqual(len(self.mock.hooks), 3)
        types = []
        for h in self.mock.hooks:
            hd, body = h["headers"], h["body"]
            self.assertTrue(tickets.verify(HOOK_SECRET, hd["x-aigw-timestamp"], body, hd["x-aigw-signature"]))
            self.assertFalse(tickets.verify("not-the-secret", hd["x-aigw-timestamp"], body, hd["x-aigw-signature"]))
            self.assertEqual(hd["content-type"], "application/json")
            j = json.loads(body)
            self.assertEqual((j["type"], j["id"]), (hd["x-aigw-event"], hd["x-aigw-delivery"]))
            self.assertEqual(j["kind"], "outage")
            types.append(j["type"])
        self.assertEqual(sorted(types), ["outage.opened", "outage.resolved", "outage.resolved"])
        self.assertEqual(len({h["headers"]["x-aigw-delivery"] for h in self.mock.hooks}), 3)
        self.assertEqual(json.loads(self.mock.hooks[0]["body"])["data"]["homes"], 3)
        # no TMF621 here: the tickets have no TMF sync state
        self.assertEqual(tickets.listing(s)[0]["sync"], "none")

    def test_a_webhook_down_is_retried(self):
        port = free_port()
        s = Store(self.dir)
        cfg = tickets.check_config({"enabled": True, "webhooks": [{"url": f"http://127.0.0.1:{port}/hook",
                                                                   "secret_env": "AIGW_TEST_LISTENER_TOKEN"}]})
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        self.assertEqual(tickets.sync(s, cfg, T + 10)["sent"], {"retrying": 1})
        self.mock = Mock(port).start()
        self.assertEqual(tickets.flush(s, cfg, T + 3600), {"sent": 1})
        self.assertEqual(len(self.mock.hooks), 1)


class Views(Base):
    def test_dashboard_fleet_and_mcp(self):
        from aigw_cortex import dashboard, fleet
        from aigw_cortex.mcp import Server
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        with open(os.path.join(self.dir, "tickets.json"), "w") as f:
            raw = {"enabled": True, "tmf621": {"url": cfg["tmf621"]["url"], "auth": cfg["tmf621"]["auth"]}}
            json.dump(raw, f)
        os.chmod(os.path.join(self.dir, "tickets.json"), 0o644)
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        inc = self.outage_of(s, gws, at=int(time.time()))
        tickets.sync(s, cfg)
        r = fleet.report(s, 7)
        self.assertEqual(r["isp_tickets"]["enabled"], True)
        self.assertEqual(r["isp_tickets"]["targets"][0]["kind"], "TMF621 v4")
        o = [x for x in r["outages"] if x["id"] == inc["id"]][0]
        self.assertEqual((o["isp_ticket"]["sync"], o["isp_ticket"]["remote_id"]), ("synced", "1000"))
        self.assertIn("ISP tickets  1 open", fleet.text(r))
        ov = dashboard.overview(s)
        self.assertEqual(ov["isp_tickets"]["tickets"]["open"], 1)
        mcp = Server(s)
        out = mcp.call("isp_tickets", {})
        self.assertEqual((out["enabled"], out["list"][0]["remote_id"]), (True, "1000"))
        self.assertIn("isp_tickets", [t["name"] for t in __import__("aigw_cortex.mcp", fromlist=["TOOLS"]).TOOLS])
        self.assertEqual(mcp.call("area_outages", {})[0]["isp_ticket"]["remote_id"], "1000")
        self.assertNotIn(CLIENT_SECRET, json.dumps([ov, out]))
        # a broken configuration is said, not hidden
        with open(os.path.join(self.dir, "tickets.json"), "w") as f:
            f.write("{not json")
        self.assertIn("not JSON", tickets.state(s)["config_error"])


class Security(Base):
    """v0.43 (docs/security.md): secrets and the configuration only their owner's; a
    webhook receiver's whole check (replays, headers the signature doesn't cover); the
    outbox at rest holds no secret."""

    def test_secret_files_must_be_private(self):
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg()
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        secret = os.path.join(self.dir, "client.secret")
        os.chmod(secret, 0o644)                             # anyone on this machine could read it
        self.assertEqual(tickets.sync(s, cfg, T + 10)["sent"], {"failed": 1})
        self.assertEqual(self.mock.tickets, {})
        self.assertFalse([x for x in self.mock.log if TOKEN in x[1]])  # the secret was never sent
        err = tickets.listing(s)[0]["error"]
        self.assertIn("chmod 600", err)
        self.assertNotIn(CLIENT_SECRET, err)
        os.chmod(secret, 0o640)                             # a group may read it (root:cortex)
        tickets.retry(s)
        self.assertEqual(tickets.flush(s, cfg, T + 20), {"sent": 1})
        os.chmod(secret, 0o660)                             # never write it
        with self.assertRaises(tickets.Permanent):
            tickets._auth_header(cfg["tmf621"], fresh=True)

    def test_configuration_others_can_change_is_refused(self):
        p = os.path.join(self.dir, "tickets.json")
        with open(p, "w") as f:
            json.dump({"enabled": True, "webhooks": [{"url": "https://hooks.example/a", "secret_env": "X"}]}, f)
        for mode in (0o666, 0o664, 0o646):
            os.chmod(p, mode)
            with self.assertRaises(ValueError, msg=oct(mode)) as e:
                tickets.load_config(self.dir)
            self.assertIn("could change it", str(e.exception))
            self.assertIn("could change it", tickets.state(Store(self.dir))["config_error"])
        os.chmod(p, 0o644)
        self.assertTrue(tickets.load_config(self.dir)["enabled"])

    def test_listener_token_file_readable_by_others(self):
        tok = os.path.join(self.dir, "listener.token")
        with open(tok, "w") as f:
            f.write(LISTENER_TOKEN)
        cfg = tickets.check_config({"enabled": True, "tmf621": {"url": "https://tt.example/v4",
                                                                 "listener": {"token_file": tok}}})
        os.chmod(tok, 0o644)
        self.assertFalse(tickets.listener_token_ok(cfg, "Bearer " + LISTENER_TOKEN))   # 401, not a crash
        os.chmod(tok, 0o600)
        self.assertTrue(tickets.listener_token_ok(cfg, "Bearer " + LISTENER_TOKEN))
        self.assertFalse(tickets.listener_token_ok(cfg, "Bearer " + LISTENER_TOKEN[:-1]))

    def test_a_receivers_check(self):
        body = json.dumps({"id": "d1", "type": "outage.opened", "data": {}}).encode()
        now = 1700000000

        def hdrs(**kw):
            h = {"X-AIGW-Timestamp": str(now), "X-AIGW-Signature": tickets.sign("k", now, body),
                 "X-AIGW-Delivery": "d1", "X-AIGW-Event": "outage.opened"}
            h.update(kw)
            return h
        seen = set()
        self.assertEqual(tickets.verify_delivery("k", hdrs(), body, seen, now + 10), (True, "outage.opened"))
        self.assertEqual(tickets.verify_delivery("k", hdrs(), body, seen, now + 20)[0], False)    # replayed
        # the signature doesn't cover these headers: a replay with a new delivery id is still refused
        ok, why = tickets.verify_delivery("k", hdrs(**{"X-AIGW-Delivery": "d2"}), body, set(), now + 10)
        self.assertEqual((ok, why), (False, "headers do not match the signed body"))
        self.assertFalse(tickets.verify_delivery("k", hdrs(**{"X-AIGW-Event": "outage.resolved"}), body, set(),
                                                 now + 10)[0])
        self.assertFalse(tickets.verify_delivery("other", hdrs(), body, set(), now + 10)[0])
        self.assertFalse(tickets.verify_delivery("k", hdrs(), body, set(), now + 301)[0])         # too old
        self.assertFalse(tickets.verify_delivery("k", hdrs(), body + b" ", set(), now + 10)[0])

    def test_the_outbox_at_rest(self):
        import stat
        self.mock = Mock().start()
        s = Store(self.dir)
        cfg = self.cfg(webhooks=[{"url": self.mock.url + "/hook", "secret_file": os.path.join(self.dir, "hook.secret")}])
        gws = self.fleet(s)
        tickets.scan(s, cfg, T - 60)
        self.outage_of(s, gws)
        tickets.sync(s, cfg, T + 10)
        self.assertEqual(len(self.mock.hooks), 1)
        db = os.path.join(self.dir, "cortex.db")
        self.assertEqual(stat.S_IMODE(os.stat(db).st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(os.stat(self.dir).st_mode) & 0o077, 0)
        s.db.commit()
        raw = b""
        for f in (db, db + "-wal", db + "-journal"):
            if os.path.exists(f):
                with open(f, "rb") as fh:
                    raw += fh.read()
        for secret in [CLIENT_SECRET, HOOK_SECRET, LISTENER_TOKEN] + sorted(self.mock.tokens):
            self.assertNotIn(secret.encode(), raw, "a secret in cortex.db")


if __name__ == "__main__":
    unittest.main(verbosity=int(os.environ.get("VERBOSE", "1")))
