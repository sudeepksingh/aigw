# SPDX-License-Identifier: Apache-2.0
"""Staged firmware rollouts that stop themselves.

    python3 -m aigw_cortex rollout new <name> --version V --url U [--sha256 H] [--size N]
                [--waves 1,10,50,100 | --wave home,home --wave home,...] [--group G]
                [--via acs|bridge] [--max-bad 5] [--max-score-drop 2] [--min-reports 80]
                [--timeout-h 72] [--paused]
    python3 -m aigw_cortex rollout list | status <name> [--homes] [--json]
    python3 -m aigw_cortex rollout pause | resume | abort <name> [why...]
    python3 -m aigw_cortex rollout homes <name> [--json]      the homes cleared now (for an ACS)
    python3 -m aigw_cortex rollout result <name> <home> Kept|RolledBack|Worse|NotBooted|Failed
                [--score-before N] [--score-after N]           a result an ACS heard (UpgradeChecked!)

A rollout gives a firmware version to a fleet in waves: 1% of the homes, then
10%, 50% and all (or lists of homes). Every home checks its own upgrade (AIGW
OS 0.30: the devices come back, the internet, speed, the experience score;
worse, and it rolls back by itself) and reports the verdict through its
bridge. A wave passes when enough of its homes have reported and few enough
got worse; only then do the next wave's homes get the firmware. The rollout
stops by itself, and says why in words (counts, never homes' names), when:

  rollbacks    more of a wave's homes got worse (rolled back, worse, didn't
               boot, couldn't install it) than --max-bad percent of the wave;
               counted as results come in, so a bad wave stops early
  score        the experience score (the share of the time without a problem)
               fell by more than --max-score-drop points on average in the
               wave's homes that kept it
  silence      fewer than --min-reports percent of a wave's homes reported in
               --timeout-h hours (until then: wait)

It never gives the firmware to more homes without a pass. An ISP can pause
(nothing new starts; results still count, and can still stop it), resume or
abort. A stopped or aborted rollout stays so: fix the cause and start another.

How a home gets the firmware (--via):
  acs      (default) the ISP's ACS or USP controller pushes it as always
           (TR-069 Download, USP Download() with AutoActivate); it asks cortex
           which homes are cleared now: `rollout homes <name> --json`
  bridge   the home's bridge fetches the offer from cortex and hands it to agd
           (bridge setting firmware 1, capability firmware.install), which takes
           the same Download path. Either way the home checks it the same way.

The decision rules (plan, judge, advance) are pure functions, tested alone.
"""
import hashlib
import json
import math
import statistics
import time
from collections import Counter

GOOD = ("kept", "current")
BAD = ("rolled_back", "worse", "not_booted", "failed")
FINAL = GOOD + BAD
# agd's upgrade states (ag/upgrade.h up_state_name) -> a home's state in a rollout
AGD_STATE = {"Activated": "activated", "Watching": "watching", "Kept": "kept", "Worse": "worse",
             "RollingBack": "rolling_back", "RolledBack": "rolled_back", "NotBooted": "not_booted"}
# a result as an ACS hears it (Device.X_AIGW_.UpgradeChecked! Result) -> state
ACS_RESULT = {"kept": "kept", "rolledback": "rolled_back", "rollbackconfirmed": "rolled_back",
              "rollbacknotconfirmed": "rolled_back", "worse": "worse", "notbooted": "not_booted",
              "failed": "failed", "current": "current"}
WORDS = {"waiting": "waiting for their wave", "cleared": "cleared, not started", "sent": "sent",
         "downloading": "downloading", "activated": "activated", "watching": "being watched",
         "rolling_back": "rolling back", "kept": "kept it", "current": "already on it",
         "rolled_back": "rolled back", "worse": "worse (not rolled back)", "not_booted": "did not boot it",
         "failed": "could not install it"}
STATES = ("running", "paused", "stopped", "aborted", "done")


class Rules:
    """When a wave passes or stops the rollout."""

    def __init__(self, max_bad=5.0, max_score_drop=2.0, min_reports=80.0, timeout_s=72 * 3600):
        self.max_bad = float(max_bad)                # % of a wave's homes that may get worse
        self.max_score_drop = float(max_score_drop)  # points the mean score may fall in a wave
        self.min_reports = float(min_reports)        # % of a wave's homes that must report
        self.timeout_s = int(timeout_s)              # ... within this long

    def as_dict(self):
        return dict(self.__dict__)

    @classmethod
    def from_dict(cls, d):
        return cls(**{k: v for k, v in (d or {}).items() if k in cls().__dict__})


def _n(k, what):
    return f"{k} {what}" if k == 1 else f"{k} {what}s"


def plan(n, waves):
    """Wave sizes for n homes. waves: cumulative percentages (1, 10, 50, 100); each
    wave gets at least one home while homes are left, and a last wave at 100% takes
    them all. Returns the sizes, e.g. plan(200, [1, 10, 50, 100]) = [2, 18, 80, 100]."""
    if n <= 0:
        return []
    pcts = [float(p) for p in waves]
    if not pcts or any(p <= 0 or p > 100 for p in pcts) or pcts != sorted(pcts):
        raise ValueError("waves are rising percentages of the homes, over 0 and at most 100 (e.g. 1,10,50,100)")
    sizes, done = [], 0
    for p in pcts:
        upto = min(n, max(done + 1, math.ceil(n * p / 100.0 - 1e-9)))
        if upto > done:
            sizes.append(upto - done)
            done = upto
    return sizes


def order(name, ids):
    """The homes in a fixed but spread order (not by name, not always the same ones first)."""
    return sorted(ids, key=lambda g: hashlib.sha256(f"{name}|{g}".encode()).hexdigest())


def counts(homes):
    c = Counter(h["state"] for h in homes)
    return {"homes": len(homes), "good": sum(c[s] for s in GOOD), "bad": sum(c[s] for s in BAD),
            "states": {s: c[s] for s in WORDS if c[s]}}


def _bad_words(c):
    return ", ".join(f"{c['states'][s]} {WORDS[s]}" for s in BAD if c["states"].get(s))


def judge(homes, rules, opened_at, now):
    """One wave: pass, wait or stop, with the reason in words (counts, no names).
    homes: dicts with state, score_before, score_after (-1 or None: not known)."""
    c = counts(homes)
    n, bad, good = c["homes"], c["bad"], c["good"]
    reported = good + bad
    out = {"verdict": "wait", "reported": reported, **c}
    if not n:
        return dict(out, verdict="pass", reason="no homes in this wave")
    allowed = n * rules.max_bad / 100.0
    if bad > allowed:
        return dict(out, verdict="stop", reason=(
            f"{bad} of the {n} homes got worse on it ({_bad_words(c)}): more than the {rules.max_bad:g}% allowed"
            f" ({math.floor(allowed + 1e-9)} of {n})."))
    need = max(1, math.ceil(n * rules.min_reports / 100.0 - 1e-9))
    if reported < need:
        if now - opened_at >= rules.timeout_s:
            return dict(out, verdict="stop", reason=(
                f"only {reported} of the {n} homes reported in {rules.timeout_s // 3600} h; {need} needed."))
        busy = n - reported - c["states"].get("waiting", 0)
        return dict(out, verdict="wait", reason=(
            f"{reported} of the {n} homes reported, {need} needed" + (f"; {busy} on their way" if busy else "") + "."))
    pairs = [(h["score_before"], h["score_after"]) for h in homes if h["state"] == "kept" and
             h.get("score_before") is not None and h.get("score_after") is not None and
             h["score_before"] >= 0 and h["score_after"] >= 0]
    if pairs:
        b = statistics.mean(p[0] for p in pairs)
        a = statistics.mean(p[1] for p in pairs)
        out["score_before"], out["score_after"] = round(b, 1), round(a, 1)
        if b - a > rules.max_score_drop:
            return dict(out, verdict="stop", reason=(
                f"the experience score fell by {b - a:.1f} points in the {_n(len(pairs), 'home')} that kept it"
                f" ({b:.1f}% of the time without a problem before, {a:.1f}% after): more than the"
                f" {rules.max_score_drop:g} allowed."))
    score = (f"; score {out['score_before']:.1f}% before, {out['score_after']:.1f}% after"
             if pairs else "")
    worse = f", {bad} got worse (within {rules.max_bad:g}%)" if bad else ""
    return dict(out, verdict="pass", reason=f"{good} of the {n} homes kept it{worse}{score}.")


def advance(state, wave, n_waves, judgements):
    """The rollout's next state: (state, wave, reason or None). judgements: judge() of
    waves 1..wave. Any opened wave that says stop stops it (also while paused); only a
    running rollout whose open wave passed goes on, to the next wave or done."""
    if state not in ("running", "paused"):
        return state, wave, None
    for k, j in enumerate(judgements, 1):
        if j["verdict"] == "stop":
            return "stopped", wave, f"Stopped in wave {k} of {n_waves}: {j['reason']}"
    if state == "paused" or not judgements or judgements[wave - 1]["verdict"] != "pass":
        return state, wave, None
    j = judgements[wave - 1]
    if wave >= n_waves:
        return "done", wave, f"Done: wave {wave} of {n_waves} passed: {j['reason']}"
    return state, wave + 1, f"Wave {wave} of {n_waves} passed: {j['reason']} Wave {wave + 1} starts."


# ---------- the rollout in the store ----------

def _row(store, ref):
    r = store.q("SELECT * FROM rollouts WHERE name = ? OR CAST(id AS TEXT) = ?", (str(ref), str(ref)))
    if not r:
        raise ValueError(f"no rollout {ref}")
    return r[0]


def _homes(store, rid):
    return store.q("SELECT * FROM rollout_homes WHERE rollout = ? ORDER BY wave, gateway", (rid,))


def log(store, rid, text, now=None):
    store.x("INSERT INTO rollout_log (rollout, ts, text) VALUES (?, ?, ?)", (rid, int(now or time.time()), text))


def create(store, name, version, url, sha256="", size="", waves=(1, 10, 50, 100), lists=None, group=None,
           via="acs", rules=None, paused=False, now=None):
    """A new rollout; its first wave is cleared at once (unless paused)."""
    now = int(now or time.time())
    rules = rules or Rules()
    if via not in ("acs", "bridge"):
        raise ValueError("via is acs or bridge")
    if not name or not version or (via == "bridge" and not url):
        raise ValueError("a rollout needs a name and a version (and with --via bridge an image URL)")
    if url and not url.startswith(("https://", "http://", "file://")):
        raise ValueError("the image URL is https://, http:// or file://")
    if store.q("SELECT 1 FROM rollouts WHERE name = ?", (name,)):
        raise ValueError(f"there is already a rollout {name}")
    gws = {g["id"]: g for g in store.gateways()}
    if lists:
        groups, seen = [], set()
        for wave in lists:
            ids = []
            for ref in wave:
                g = store.gateway(ref)
                if not g:
                    raise ValueError(f"no gateway {ref}")
                if g["id"] in seen:
                    raise ValueError(f"{ref} is in two waves")
                seen.add(g["id"])
                ids.append(g["id"])
            if ids:
                groups.append(ids)
    else:
        ids = order(name, [g for g, v in gws.items() if group is None or (v["grp"] or "") == group])
        groups, at = [], 0
        for size_k in plan(len(ids), waves):
            groups.append(ids[at:at + size_k])
            at += size_k
    if not groups:
        raise ValueError("no homes to roll out to" + (f" in group {group}" if group else ""))
    cur = store.x("INSERT INTO rollouts (name, version, url, sha256, size, via, grp, waves, rules, state, wave, "
                  "opened, reason, created, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, '', ?, ?)",
                  (name, version, url or "", sha256 or "", str(size or ""), via, group or "",
                   json.dumps([len(g) for g in groups]), json.dumps(rules.as_dict()),
                   "paused" if paused else "running", json.dumps([now]), now, now))
    rid = cur.lastrowid
    for k, ids in enumerate(groups, 1):
        for g in ids:
            store.x("INSERT INTO rollout_homes (rollout, gateway, wave, state, updated) VALUES (?, ?, ?, ?, ?)",
                    (rid, g, k, "cleared" if k == 1 else "waiting", now))
    total = sum(len(g) for g in groups)
    log(store, rid, f"Planned: {version} to {_n(total, 'home')} in {_n(len(groups), 'wave')} of "
                    f"{', '.join(str(len(g)) for g in groups)}" + (f" (group {group})" if group else "") +
                    f", through the {'bridge' if via == 'bridge' else 'ACS'}; stops when more than {rules.max_bad:g}%"
                    f" of a wave get worse or its score falls by more than {rules.max_score_drop:g} points." +
                    (" Paused." if paused else " Wave 1 starts."), now)
    return rid


def step(store, ref, now=None):
    """Judge a rollout's opened waves and move it on (or stop it). Returns its status."""
    now = int(now or time.time())
    r = _row(store, ref)
    if r["state"] in ("running", "paused"):
        rules = Rules.from_dict(json.loads(r["rules"] or "{}"))
        sizes, opened = json.loads(r["waves"]), json.loads(r["opened"] or "[]")
        homes = _homes(store, r["id"])
        js = [judge([h for h in homes if h["wave"] == k], rules, opened[k - 1] if k <= len(opened) else now, now)
              for k in range(1, r["wave"] + 1)]
        state, wave, reason = advance(r["state"], r["wave"], len(sizes), js)
        if reason:
            if wave > r["wave"]:
                opened.append(now)
                store.x("UPDATE rollout_homes SET state = 'cleared', updated = ? WHERE rollout = ? AND wave = ? "
                        "AND state = 'waiting'", (now, r["id"], wave))
            store.x("UPDATE rollouts SET state = ?, wave = ?, opened = ?, reason = ?, updated = ? WHERE id = ?",
                    (state, wave, json.dumps(opened), reason, now, r["id"]))
            log(store, r["id"], reason, now)
    return status(store, r["id"], now)


def step_all(store, now=None):
    for r in store.q("SELECT id FROM rollouts WHERE state IN ('running', 'paused')"):
        step(store, r["id"], now)


def control(store, ref, what, why="", who="you", now=None):
    """pause, resume or abort; returns the status."""
    now = int(now or time.time())
    r = _row(store, ref)
    why = f" ({why})" if why else ""
    if what == "pause":
        if r["state"] != "running":
            raise ValueError(f"rollout {r['name']} is {r['state']}: only a running one can be paused")
        text = f"Paused by {who}{why}: no new home starts; results still count."
        state = "paused"
    elif what == "resume":
        if r["state"] != "paused":
            raise ValueError(f"rollout {r['name']} is {r['state']}: only a paused one can be resumed" +
                             (" (a stopped rollout stays stopped: fix the cause and start another)"
                              if r["state"] == "stopped" else ""))
        text = f"Resumed by {who}{why}."
        state = "running"
    elif what == "abort":
        if r["state"] not in ("running", "paused"):
            raise ValueError(f"rollout {r['name']} is already {r['state']}")
        text = f"Aborted by {who}{why}: no more homes get it."
        state = "aborted"
        store.x("UPDATE rollout_homes SET state = 'waiting', updated = ? WHERE rollout = ? AND state = 'cleared'",
                (now, r["id"]))
    else:
        raise ValueError("pause, resume or abort")
    store.x("UPDATE rollouts SET state = ?, reason = ?, updated = ? WHERE id = ?", (state, text, now, r["id"]))
    log(store, r["id"], text, now)
    return step(store, r["id"], now) if state == "running" else status(store, r["id"], now)


# ---------- the homes' results ----------

def _set_home(store, rid, gw, state, now, score_before=None, score_after=None, error=None):
    sets, args = ["state = ?", "updated = ?"], [state, now]
    if score_before is not None:
        sets.append("score_before = ?")
        args.append(int(score_before))
    if score_after is not None:
        sets.append("score_after = ?")
        args.append(int(score_after))
    if error is not None:
        sets.append("error = ?")
        args.append(str(error)[:300])
    store.x(f"UPDATE rollout_homes SET {', '.join(sets)} WHERE rollout = ? AND gateway = ?", args + [rid, gw])


def _num(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def on_report(store, gw, up, now=None):
    """A home's upgrade check from its bridge's report (agd's upgrade_blob, no devices)."""
    if not isinstance(up, dict):
        return 0
    now = int(now or time.time())
    state = AGD_STATE.get(str(up.get("state", "")))
    if not state:
        return 0
    to, activated = str(up.get("to", "")), _num(up.get("activated")) or 0
    n = 0
    for h in store.q("SELECT h.*, r.version, r.opened FROM rollout_homes h JOIN rollouts r ON r.id = h.rollout "
                     "WHERE h.gateway = ? AND r.state IN ('running', 'paused', 'stopped') "
                     "AND h.state NOT IN ('waiting', 'kept', 'current', 'failed')", (gw,)):
        opened = json.loads(h["opened"] or "[]")
        since = opened[h["wave"] - 1] if h["wave"] <= len(opened) else 0
        # this rollout's upgrade: its version (or the image named itself otherwise), activated after the wave opened
        if activated < since or (to != h["version"] and h["state"] not in ("sent", "downloading")):
            continue
        if h["state"] == state and h["score_after"] == _num(up.get("score_after")):
            continue
        _set_home(store, h["rollout"], gw, state, now, _num(up.get("score_before")), _num(up.get("score_after")),
                  up.get("summary", "") if state in BAD else "")
        n += 1
    if n:
        step_all(store, now)
    return n


def acs_result(store, ref, home, result, score_before=None, score_after=None, now=None):
    """A result the ACS heard (UpgradeChecked!), for homes without a bridge."""
    now = int(now or time.time())
    r = _row(store, ref)
    g = store.gateway(home)
    state = ACS_RESULT.get(str(result).lower().replace("_", "").replace("-", ""))
    if not g:
        raise ValueError(f"no gateway {home}")
    if not state:
        raise ValueError("result is Kept, RolledBack, Worse, NotBooted, Failed or Current")
    h = store.q("SELECT * FROM rollout_homes WHERE rollout = ? AND gateway = ?", (r["id"], g["id"]))
    if not h or h[0]["state"] == "waiting":
        raise ValueError(f"{g['name']} is not cleared in rollout {r['name']}: it should not have it")
    _set_home(store, r["id"], g["id"], state, now, score_before, score_after)
    return step(store, r["id"], now)


def offer(store, gw, now=None):
    """The firmware for this home's bridge, if a running bridge rollout has cleared it (sent once)."""
    now = int(now or time.time())
    for h in store.q("SELECT h.rollout, r.version, r.url, r.sha256, r.size FROM rollout_homes h "
                     "JOIN rollouts r ON r.id = h.rollout WHERE h.gateway = ? AND h.state = 'cleared' "
                     "AND r.state = 'running' AND r.via = 'bridge' ORDER BY r.id", (gw,)):
        _set_home(store, h["rollout"], gw, "sent", now)
        return {"rollout": h["rollout"], "version": h["version"], "url": h["url"], "checksum": h["sha256"],
                "size": h["size"]}
    return None


def offer_result(store, gw, rid, ok, current=False, error="", now=None):
    now = int(now or time.time())
    h = store.q("SELECT state FROM rollout_homes WHERE rollout = ? AND gateway = ?", (int(rid), gw))
    if not h or h[0]["state"] != "sent":
        return False
    _set_home(store, int(rid), gw, "downloading" if ok else "current" if current else "failed", now,
              error="" if ok or current else error)
    step(store, int(rid), now)
    return True


def cleared(store, ref):
    """The homes an ACS may push the firmware to now (none unless running)."""
    r = _row(store, ref)
    if r["state"] != "running":
        return []
    return store.q("SELECT g.name, g.id FROM rollout_homes h JOIN gateways g ON g.id = h.gateway "
                   "WHERE h.rollout = ? AND h.state = 'cleared' ORDER BY g.name", (r["id"],))


# ---------- reading ----------

def status(store, ref, now=None, homes=False):
    now = int(now or time.time())
    r = _row(store, ref)
    rules = Rules.from_dict(json.loads(r["rules"] or "{}"))
    sizes, opened = json.loads(r["waves"]), json.loads(r["opened"] or "[]")
    hs = _homes(store, r["id"])
    waves = []
    for k, size in enumerate(sizes, 1):
        wh = [h for h in hs if h["wave"] == k]
        w = {"wave": k, "homes": size, "opened": opened[k - 1] if k <= len(opened) else 0}
        if k <= r["wave"] and k <= len(opened):
            j = judge(wh, rules, w["opened"], now)
            w.update({x: j[x] for x in ("verdict", "reason", "reported", "good", "bad", "states")})
            for x in ("score_before", "score_after"):
                if x in j:
                    w[x] = j[x]
        waves.append(w)
    out = {"name": r["name"], "id": r["id"], "version": r["version"], "via": r["via"], "group": r["grp"],
           "state": r["state"], "wave": r["wave"], "waves": len(sizes), "reason": r["reason"],
           "rules": rules.as_dict(), "created": r["created"], "updated": r["updated"],
           "homes": counts(hs), "per_wave": waves,
           "log": [{"ts": x["ts"], "text": x["text"]} for x in
                   store.q("SELECT ts, text FROM rollout_log WHERE rollout = ? ORDER BY rowid", (r["id"],))]}
    if homes:
        names = {g["id"]: g["name"] for g in store.gateways()}
        out["list"] = [{"home": names.get(h["gateway"], h["gateway"]), "wave": h["wave"], "state": h["state"],
                        "score_before": h["score_before"], "score_after": h["score_after"], "error": h["error"]}
                       for h in hs]
    return out


def summaries(store, since=0, now=None):
    """Every rollout still going, or changed since `since`: for the fleet report."""
    rows = store.q("SELECT id FROM rollouts WHERE state IN ('running', 'paused') OR updated >= ? ORDER BY id DESC",
                   (int(since),))
    out = []
    for r in rows:
        s = status(store, r["id"], now)
        one = {k: s[k] for k in ("name", "version", "via", "group", "state", "wave", "waves", "reason")}
        one.update({"homes": s["homes"]["homes"], "states": s["homes"]["states"],
                    "open_wave": s["per_wave"][s["wave"] - 1] if s["per_wave"] else {}})
        out.append(one)
    return out


def _when(ts):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts)) if ts else "-"


def text(s):
    """A rollout's status in words."""
    st = ", ".join(f"{v} {WORDS[k]}" for k, v in s["homes"]["states"].items())
    out = [f"Rollout {s['name']}: {s['version']} to {_n(s['homes']['homes'], 'home')}, through the "
           f"{'bridge' if s['via'] == 'bridge' else 'ACS'}: {s['state']}, wave {s['wave']} of {s['waves']}",
           f"  homes    {st}"]
    for w in s["per_wave"]:
        line = f"  wave {w['wave']}   {_n(w['homes'], 'home')}"
        if w.get("verdict"):
            line += f", opened {_when(w['opened'])}: {w['verdict']}: {w['reason']}"
        out.append(line)
    if s["reason"]:
        out.append(f"  now      {s['reason']}")
    for x in s.get("list", []):
        sc = (f", score {x['score_before']} -> {x['score_after']}"
              if x["score_before"] is not None and x["score_after"] is not None else "")
        out.append(f"    {x['home']}: wave {x['wave']}, {WORDS.get(x['state'], x['state'])}{sc}" +
                   (f": {x['error']}" if x.get("error") else ""))
    return "\n".join(out)
