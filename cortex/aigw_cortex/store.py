# SPDX-License-Identifier: Apache-2.0
"""Fleet history in SQLite: gateways, their reports, labels for training,
proposals on their way to a gateway, published models, federated
learning rounds (only noisy changes of the shared layer, never names), and
staged firmware rollouts (rollout.py: waves, each home's result).

One file (cortex.db) in the cortex data directory. Gateways send features
and decisions, never packets or DNS logs; the only names that arrive are
the subjects of alerts, which is what the training loop learns from.
"""
import hashlib
import json
import os
import secrets
import sqlite3
import sys
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS gateways (
    id TEXT PRIMARY KEY, name TEXT UNIQUE, token_hash TEXT UNIQUE, created INTEGER,
    last_seen INTEGER DEFAULT 0, aigw TEXT DEFAULT '', host TEXT DEFAULT '', snapshot TEXT DEFAULT '{}');
CREATE TABLE IF NOT EXISTS history (
    gateway TEXT, ts INTEGER, devices INTEGER, alerts INTEGER, waiting INTEGER, active INTEGER);
CREATE TABLE IF NOT EXISTS alerts (
    gateway TEXT, id INTEGER, first INTEGER, kind TEXT, subject TEXT, device TEXT, score REAL,
    last INTEGER, count INTEGER, PRIMARY KEY (gateway, id, first));
CREATE TABLE IF NOT EXISTS decisions (
    gateway TEXT, id INTEGER, created INTEGER, action TEXT, target TEXT, state TEXT, origin TEXT,
    alert INTEGER, automatic INTEGER, PRIMARY KEY (gateway, id, created));
CREATE TABLE IF NOT EXISTS labels (
    subject TEXT, kind TEXT, label INTEGER, source TEXT, gateway TEXT, ts INTEGER,
    PRIMARY KEY (subject, kind, source, gateway));
CREATE TABLE IF NOT EXISTS proposals (
    id INTEGER PRIMARY KEY AUTOINCREMENT, gateway TEXT, action TEXT, target TEXT, ttl INTEGER,
    reason TEXT, origin TEXT, state TEXT, decision INTEGER DEFAULT 0, error TEXT DEFAULT '',
    created INTEGER, updated INTEGER);
CREATE TABLE IF NOT EXISTS models (
    name TEXT, version INTEGER, file TEXT, sha256 TEXT, size INTEGER, published INTEGER, notes TEXT,
    PRIMARY KEY (name, version));
CREATE TABLE IF NOT EXISTS fed_rounds (
    round INTEGER PRIMARY KEY, started INTEGER, weights TEXT, state TEXT, closed INTEGER DEFAULT 0,
    homes INTEGER DEFAULT 0, version INTEGER DEFAULT 0, note TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS fed_updates (
    round INTEGER, gateway TEXT, delta TEXT, n INTEGER, noise REAL, clip REAL, ts INTEGER,
    PRIMARY KEY (round, gateway));
CREATE TABLE IF NOT EXISTS care_days (
    gateway TEXT, day INTEGER, checks INTEGER, good INTEGER, internet INTEGER, dns INTEGER, wifi INTEGER,
    device INTEGER, aigw INTEGER, problems INTEGER, reported INTEGER, fixed INTEGER, problem_s INTEGER,
    warnings INTEGER, warned INTEGER, lead_s INTEGER, speed_tests INTEGER, speed_kbps_sum INTEGER,
    speed_kbps_min INTEGER, speed_low INTEGER, watched_s INTEGER DEFAULT 0, ok_s INTEGER DEFAULT 0,
    cleared INTEGER DEFAULT 0, PRIMARY KEY (gateway, day));
CREATE TABLE IF NOT EXISTS care_events (
    gateway TEXT, kind TEXT, code TEXT, area TEXT, since INTEGER, until INTEGER DEFAULT 0,
    reported INTEGER DEFAULT 0, summary TEXT DEFAULT '', predict INTEGER DEFAULT -1,
    PRIMARY KEY (gateway, kind, code, since));
-- v0.32: signals for the ISP (a faster plan, mesh Wi-Fi, a customer at risk...), one row per signal while it lasts
CREATE TABLE IF NOT EXISTS offers (
    gateway TEXT, code TEXT, since INTEGER, kind TEXT DEFAULT '', product TEXT DEFAULT '', confidence TEXT DEFAULT '',
    days INTEGER DEFAULT 0, of_days INTEGER DEFAULT 0, evidence TEXT DEFAULT '', seen INTEGER DEFAULT 0,
    until INTEGER DEFAULT 0, PRIMARY KEY (gateway, code, since));
-- v0.34: staged firmware rollouts (rollout.py): waves of homes, each home's result, what happened in words
CREATE TABLE IF NOT EXISTS rollouts (
    id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, version TEXT, url TEXT DEFAULT '',
    sha256 TEXT DEFAULT '', size TEXT DEFAULT '', via TEXT DEFAULT 'acs', grp TEXT DEFAULT '', waves TEXT,
    rules TEXT, state TEXT, wave INTEGER DEFAULT 1, opened TEXT DEFAULT '[]', reason TEXT DEFAULT '',
    created INTEGER, updated INTEGER);
CREATE TABLE IF NOT EXISTS rollout_homes (
    rollout INTEGER, gateway TEXT, wave INTEGER, state TEXT, score_before INTEGER, score_after INTEGER,
    error TEXT DEFAULT '', updated INTEGER, PRIMARY KEY (rollout, gateway));
CREATE TABLE IF NOT EXISTS rollout_log (rollout INTEGER, ts INTEGER, text TEXT);
"""

# a day's row from a gateway (agd's care_days_blob) -> care_days' columns
CARE_COLUMNS = (("checks", "checks"), ("good", "good"), ("Internet", "internet"), ("DNS", "dns"), ("WiFi", "wifi"),
                ("Device", "device"), ("AIGW", "aigw"), ("problems", "problems"), ("reported", "reported"),
                ("fixed_by_aigw", "fixed"), ("problem_s", "problem_s"), ("early_warnings", "warnings"),
                ("warned_of", "warned"), ("warning_lead_s", "lead_s"), ("speed_tests", "speed_tests"),
                ("speed_kbps_sum", "speed_kbps_sum"), ("speed_kbps_min", "speed_kbps_min"), ("speed_low", "speed_low"),
                # v0.29.1: the time watched, and of it without a problem (the score); 0 from older gateways
                ("watched_s", "watched_s"), ("ok_s", "ok_s"),
                # v0.29.2: Wi-Fi problems over though AIGW changed nothing (not its fix)
                ("cleared_by_itself", "cleared"))

# what a person's answer to a decision says about the alert behind it
APPROVED = 1      # the alert was right
REJECTED = 0      # the alert was wrong: a false alarm


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def private(path, mode=0o700):
    """Make a directory (mode 0700) or tighten a file to mode: what cortex keeps is about
    homes, and its tokens. Cortex before 0.43 left the data directory and cortex.db
    readable by every user; group or other access is taken away (with a note)."""
    if mode == 0o700:
        os.makedirs(path, mode=0o700, exist_ok=True)
    try:
        st = os.stat(path)
    except FileNotFoundError:
        return
    if st.st_mode & 0o077 and st.st_uid == os.getuid():
        os.chmod(path, st.st_mode & 0o700)
        sys.stderr.write(f"cortex: {path} was readable by other users; now only by its owner\n")


class Store:
    def __init__(self, data_dir):
        self.dir = data_dir
        private(data_dir)
        os.makedirs(os.path.join(data_dir, "models"), exist_ok=True)
        db = os.path.join(data_dir, "cortex.db")
        # the homes' reports (device names, addresses), gateways' token hashes: the owner's alone
        os.close(os.open(db, os.O_RDWR | os.O_CREAT, 0o600))
        for f in (db, db + "-wal", db + "-shm", db + "-journal"):
            private(f, 0o600)
        self.db = sqlite3.connect(db, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock:
            self.db.executescript(SCHEMA)
            # v0.29: a gateway's group (an area, an exchange: outages are looked for within one)
            cols = {r[1] for r in self.db.execute("PRAGMA table_info(gateways)").fetchall()}
            if "grp" not in cols:
                self.db.execute("ALTER TABLE gateways ADD COLUMN grp TEXT DEFAULT ''")
                self.db.commit()
            # v0.29.1: the time a gateway watched the home
            cols = {r[1] for r in self.db.execute("PRAGMA table_info(care_days)").fetchall()}
            for c in ("watched_s", "ok_s", "cleared"):
                if c not in cols:
                    self.db.execute(f"ALTER TABLE care_days ADD COLUMN {c} INTEGER DEFAULT 0")
            self.db.commit()

    def q(self, sql, args=()):
        with self.lock:
            return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    def x(self, sql, args=()):
        with self.lock, self.db:
            return self.db.execute(sql, args)

    # ---------- gateways ----------

    def enroll(self, name, group=""):
        token = secrets.token_urlsafe(32)
        gid = secrets.token_hex(6)
        self.x("INSERT INTO gateways (id, name, token_hash, created, grp) VALUES (?, ?, ?, ?, ?)",
               (gid, name, token_hash(token), int(time.time()), group or ""))
        return gid, token

    def set_group(self, ref, group):
        g = self.gateway(ref)
        if g:
            self.x("UPDATE gateways SET grp = ? WHERE id = ?", (group or "", g["id"]))
        return g

    def gateway_by_token(self, token):
        r = self.q("SELECT * FROM gateways WHERE token_hash = ?", (token_hash(token),))
        return r[0] if r else None

    def gateway(self, ref=None):
        """By id or name; with no ref, the only gateway (or the one seen last)."""
        if ref:
            r = self.q("SELECT * FROM gateways WHERE id = ? OR name = ?", (ref, ref))
        else:
            r = self.q("SELECT * FROM gateways ORDER BY last_seen DESC LIMIT 1")
        return r[0] if r else None

    def gateways(self):
        return self.q("SELECT id, name, created, last_seen, aigw, host, grp FROM gateways ORDER BY name")

    def remove(self, ref):
        g = self.gateway(ref)
        if g:
            self.x("DELETE FROM gateways WHERE id = ?", (g["id"],))
        return g

    # ---------- reports ----------

    def report(self, gw, rep, now=None):
        now = int(now or time.time())
        st = rep.get("status") or {}
        prev = self.q("SELECT last_seen FROM gateways WHERE id = ?", (gw,))
        prev_seen = prev[0]["last_seen"] if prev else 0
        self.x("UPDATE gateways SET last_seen = ?, aigw = ?, host = ?, snapshot = ? WHERE id = ?",
               (now, str(st.get("version", "")), str(st.get("host", "")), json.dumps(rep), gw))
        self.x("INSERT INTO history VALUES (?, ?, ?, ?, ?, ?)",
               (gw, now, int(st.get("devices", 0)), int(st.get("alerts", 0)),
                int(st.get("waiting", 0)), int(st.get("active", 0))))
        alerts = {}
        for a in rep.get("alerts") or []:
            try:
                alerts[int(a["id"])] = a
                self.x("INSERT OR REPLACE INTO alerts VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       (gw, int(a["id"]), int(a.get("first", 0)), str(a.get("kind", "")),
                        str(a.get("subject", "")), str(a.get("device", "")), float(a.get("score", 0)),
                        int(a.get("last", 0)), int(a.get("count", 1))))
            except (KeyError, TypeError, ValueError):
                continue
        learned = 0
        for d in rep.get("decisions") or []:
            try:
                self.x("INSERT OR REPLACE INTO decisions VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       (gw, int(d["id"]), int(d.get("created", 0)), str(d.get("action", "")),
                        str(d.get("target", "")), str(d.get("state", "")), str(d.get("origin", "")),
                        int(d.get("alert", 0)), int(d.get("automatic", 0))))
            except (KeyError, TypeError, ValueError):
                continue
            label = feedback(d)
            a = alerts.get(int(d.get("alert", 0)))
            if label is None or not a or not a.get("subject"):
                continue
            self.x("INSERT OR REPLACE INTO labels VALUES (?, ?, ?, 'decision', ?, ?)",
                   (str(a["subject"]).lower(), str(a.get("kind", "")), label, gw, now))
            learned += 1
        self.care(gw, rep.get("care"), now)
        if rep.get("upgrade") is not None:
            from . import rollout             # a staged rollout's wave may wait for this home's verdict
            rollout.on_report(self, gw, rep.get("upgrade"), now)
        from . import outage                  # v0.39: many homes of one uplink at once: the ISP's network
        outage.on_report(self, gw, rep, prev_seen, now)
        up = self.q("SELECT g.grp, u.reported FROM gateways g LEFT JOIN uplinks u ON u.gateway = g.id WHERE g.id = ?",
                    (gw,))
        if up and (up[0]["grp"] or up[0]["reported"]):
            outage.update(self, now, up[0]["grp"] or up[0]["reported"])
        return learned

    # ---------- proactive care (the fleet report) ----------

    def care(self, gw, c, now=None):
        """A gateway's care: its days (replaced as they fill), and its open problem and
        early warning as events, closed when a report no longer has them."""
        if not isinstance(c, dict):
            return
        now = int(now or time.time())
        for d in c.get("days") or []:
            try:
                vals = [int(d.get(k, 0) or 0) for k, _ in CARE_COLUMNS]
                cols = ", ".join(c for _, c in CARE_COLUMNS)
                self.x(f"INSERT OR REPLACE INTO care_days (gateway, day, {cols}) VALUES (?, ?" + ", ?" * len(vals) + ")",
                       [gw, int(d["day"])] + vals)
            except (KeyError, TypeError, ValueError):
                continue
        open_now = set()
        p = c.get("problem")
        if isinstance(p, dict) and p.get("code") and p.get("since"):
            key = ("problem", str(p["code"]), int(p["since"]))
            open_now.add(key)
            self.x("INSERT OR IGNORE INTO care_events (gateway, kind, code, area, since) VALUES (?, ?, ?, ?, ?)",
                   (gw, key[0], key[1], str(p.get("area", "")), key[2]))
            self.x("UPDATE care_events SET reported = ?, summary = ?, until = 0 WHERE gateway = ? AND kind = ? "
                   "AND code = ? AND since = ?", (int(bool(p.get("reported"))), str(p.get("summary", ""))[:400],
                                                gw, *key))
        w = c.get("early_warning")
        if isinstance(w, dict) and w.get("status") == "warning" and w.get("metric") and w.get("since"):
            key = ("warning", str(w["metric"]), int(w["since"]))
            open_now.add(key)
            self.x("INSERT OR IGNORE INTO care_events (gateway, kind, code, area, since) VALUES (?, ?, ?, ?, ?)",
                   (gw, key[0], key[1], str(w.get("area", "")), key[2]))
            self.x("UPDATE care_events SET summary = ?, predict = ?, until = 0 WHERE gateway = ? AND kind = ? "
                   "AND code = ? AND since = ?",
                   (str(w.get("summary", ""))[:400],
                    int(w.get("predicted_fault_days", 0)) if w.get("predicted") else -1, gw, *key))
        for r in self.q("SELECT kind, code, since FROM care_events WHERE gateway = ? AND until = 0", (gw,)):
            if (r["kind"], r["code"], r["since"]) not in open_now:
                self.x("UPDATE care_events SET until = ? WHERE gateway = ? AND kind = ? AND code = ? AND since = ?",
                       (now, gw, r["kind"], r["code"], r["since"]))
        self.offers(gw, c.get("offers"), now)

    def offers(self, gw, offers, now=None):
        """A gateway's signals for the ISP (v0.32): one row per signal while it lasts,
        closed when a report no longer has it. Reports without the key (older
        gateways, offers.share 0) leave what is there alone."""
        if not isinstance(offers, list):
            return
        now = int(now or time.time())
        open_now = set()
        for o in offers:
            try:
                key = (str(o["code"]), int(o.get("since") or 0))
            except (KeyError, TypeError, ValueError):
                continue
            open_now.add(key)
            self.x("INSERT OR IGNORE INTO offers (gateway, code, since) VALUES (?, ?, ?)", (gw, *key))
            self.x("UPDATE offers SET kind = ?, product = ?, confidence = ?, days = ?, of_days = ?, evidence = ?, "
                   "seen = ?, until = 0 WHERE gateway = ? AND code = ? AND since = ?",
                   (str(o.get("kind", "")), str(o.get("product", "")), str(o.get("confidence", "")),
                    int(o.get("days", 0) or 0), int(o.get("of_days", 0) or 0), str(o.get("evidence", ""))[:400],
                    now, gw, *key))
        for r in self.q("SELECT code, since FROM offers WHERE gateway = ? AND until = 0", (gw,)):
            if (r["code"], r["since"]) not in open_now:
                self.x("UPDATE offers SET until = ? WHERE gateway = ? AND code = ? AND since = ?",
                       (now, gw, r["code"], r["since"]))

    def offers_open(self, gws=None):
        rows = self.q("SELECT * FROM offers WHERE until = 0 ORDER BY since")
        return [r for r in rows if gws is None or r["gateway"] in gws]
    def care_days(self, first_day, last_day, gws=None):
        rows = self.q("SELECT * FROM care_days WHERE day >= ? AND day <= ?", (first_day, last_day))
        return [r for r in rows if gws is None or r["gateway"] in gws]

    def care_events(self, since, gws=None):
        rows = self.q("SELECT * FROM care_events WHERE until = 0 OR until >= ? ORDER BY since", (since,))
        return [r for r in rows if gws is None or r["gateway"] in gws]

    def snapshot(self, gw):
        g = self.gateway(gw)
        return json.loads(g["snapshot"] or "{}") if g else None

    def history(self, gw, since=0):
        return self.q("SELECT ts, devices, alerts, waiting, active FROM history WHERE gateway = ? AND ts >= ? "
                      "ORDER BY ts", (gw, since))

    # ---------- labels ----------

    def label(self, subject, kind, label, source="you"):
        self.x("INSERT OR REPLACE INTO labels VALUES (?, ?, ?, ?, '', ?)",
               (subject.lower(), kind, int(label), source, int(time.time())))

    def labels(self, kind=None):
        if kind:
            return self.q("SELECT * FROM labels WHERE kind = ? ORDER BY ts", (kind,))
        return self.q("SELECT * FROM labels ORDER BY ts")

    # ---------- shared threat intelligence ----------

    def intel(self, min_homes=2):
        """Names for the shared threat list: confirmed by a person anywhere, or
        flagged as machine-generated in at least min_homes homes; never a name
        anyone called a false alarm. Returns [(name, reason)]."""
        benign = {r["subject"] for r in self.q("SELECT DISTINCT subject FROM labels WHERE label = 0")}
        out = {}
        for r in self.q("SELECT subject, COUNT(DISTINCT gateway) homes FROM labels WHERE label = 1 "
                        "GROUP BY subject"):
            n = r["homes"]
            out[r["subject"]] = f"confirmed by people in {n} home{'s' if n != 1 else ''}"
        for r in self.q("SELECT LOWER(subject) subject, COUNT(DISTINCT gateway) homes FROM alerts "
                        "WHERE kind = 'dga' GROUP BY LOWER(subject) HAVING homes >= ?", (int(min_homes),)):
            out.setdefault(r["subject"], f"looked machine-generated in {r['homes']} homes")
        return sorted((k, v) for k, v in out.items() if k not in benign and "." in k)

    # ---------- proposals ----------

    def propose(self, gw, action, target, ttl=0, reason="", origin="cortex"):
        now = int(time.time())
        cur = self.x("INSERT INTO proposals (gateway, action, target, ttl, reason, origin, state, created, updated) "
                     "VALUES (?, ?, ?, ?, ?, ?, 'queued', ?, ?)",
                     (gw, action, target, int(ttl), reason, origin, now, now))
        return cur.lastrowid

    def take_proposals(self, gw, hold=()):
        """The queued proposals, now sent; those whose action is in hold stay queued (v0.39: an
        area outage holds fixes in the home until it is over)."""
        rows = [r for r in self.q("SELECT id, action, target, ttl, reason, origin FROM proposals "
                                  "WHERE gateway = ? AND state = 'queued' ORDER BY id", (gw,))
                if r["action"] not in hold]
        for r in rows:
            self.x("UPDATE proposals SET state = 'sent', updated = ? WHERE id = ?", (int(time.time()), r["id"]))
        return rows

    def proposal_result(self, gw, pid, ok, decision=0, error=""):
        cur = self.x("UPDATE proposals SET state = ?, decision = ?, error = ?, updated = ? "
                     "WHERE id = ? AND gateway = ?",
                     ("waiting for approval" if ok else "refused", int(decision), str(error)[:300],
                      int(time.time()), int(pid), gw))
        return cur.rowcount

    def proposals(self, gw=None):
        if gw:
            return self.q("SELECT * FROM proposals WHERE gateway = ? ORDER BY id DESC LIMIT 50", (gw,))
        return self.q("SELECT * FROM proposals ORDER BY id DESC LIMIT 50")

    # ---------- models ----------

    def models_dir(self):
        return os.path.join(self.dir, "models")

    def add_model(self, name, version, file, sha, size, notes=""):
        self.x("INSERT OR REPLACE INTO models VALUES (?, ?, ?, ?, ?, ?, ?)",
               (name, version, file, sha, size, int(time.time()), notes))

    def latest_models(self):
        return self.q("SELECT m.* FROM models m JOIN (SELECT name, MAX(version) v FROM models GROUP BY name) l "
                      "ON m.name = l.name AND m.version = l.v ORDER BY m.name")

    def next_version(self, name):
        r = self.q("SELECT MAX(version) v FROM models WHERE name = ?", (name,))
        return (r[0]["v"] or 0) + 1


def feedback(d):
    """What a decision's outcome says about its alert, or None if nothing."""
    state, auto = d.get("state"), int(d.get("automatic", 0))
    if state == "rejected":
        return REJECTED
    if auto and state == "reverted":
        return REJECTED                # applied on its own, and a person undid it
    if not auto and state in ("active", "expired", "reverted"):
        return APPROVED                # a person approved it
    return None
