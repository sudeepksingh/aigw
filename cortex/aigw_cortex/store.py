# SPDX-License-Identifier: Apache-2.0
"""Fleet history in SQLite: gateways, their reports, labels for training,
proposals on their way to a gateway, and published models.

One file (cortex.db) in the cortex data directory. Gateways send features
and decisions, never packets or DNS logs; the only names that arrive are
the subjects of alerts, which is what the training loop learns from.
"""
import hashlib
import json
import os
import secrets
import sqlite3
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
"""

# what a person's answer to a decision says about the alert behind it
APPROVED = 1      # the alert was right
REJECTED = 0      # the alert was wrong: a false alarm


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


class Store:
    def __init__(self, data_dir):
        self.dir = data_dir
        os.makedirs(os.path.join(data_dir, "models"), exist_ok=True)
        self.db = sqlite3.connect(os.path.join(data_dir, "cortex.db"), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock:
            self.db.executescript(SCHEMA)

    def q(self, sql, args=()):
        with self.lock:
            return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    def x(self, sql, args=()):
        with self.lock, self.db:
            return self.db.execute(sql, args)

    # ---------- gateways ----------

    def enroll(self, name):
        token = secrets.token_urlsafe(32)
        gid = secrets.token_hex(6)
        self.x("INSERT INTO gateways (id, name, token_hash, created) VALUES (?, ?, ?, ?)",
               (gid, name, token_hash(token), int(time.time())))
        return gid, token

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
        return self.q("SELECT id, name, created, last_seen, aigw, host FROM gateways ORDER BY name")

    def remove(self, ref):
        g = self.gateway(ref)
        if g:
            self.x("DELETE FROM gateways WHERE id = ?", (g["id"],))
        return g

    # ---------- reports ----------

    def report(self, gw, rep):
        now = int(time.time())
        st = rep.get("status") or {}
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
        return learned

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

    def take_proposals(self, gw):
        rows = self.q("SELECT id, action, target, ttl, reason, origin FROM proposals "
                      "WHERE gateway = ? AND state = 'queued' ORDER BY id", (gw,))
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
