# SPDX-License-Identifier: Apache-2.0
"""The help desk: one short answer about one home, for the ISP's support agent.

    python3 -m aigw_cortex brief <home> [--period "tonight 20:00-22:00"] [--propose] [--json]
    MCP tool: support_brief

A home is found by its gateway's name, its id, or the serial number the ISP's
ACS knows it by (TR-069's Device.DeviceInfo.SerialNumber).

The answer is the gateway's own brief (agd's rules, core/libag/brief.c): the
fault in one word (LINE, HOME_WIFI, DEVICE, OUTSIDE, NONE, UNKNOWN), one
sentence, the evidence with times and numbers, what AIGW already did, what to
tell the customer, a remote fix if there is one, and whether a technician
should go out. Each bridge sends the brief of the last hour with its report,
with device kinds ("a laptop"), not names, unless the home allows names.

The bridge reports every few minutes and takes no requests, so cortex cannot
ask the home again: it answers from the last report and says how old it is.
For a fresh one, or another period, the ISP asks the gateway itself:
Device.X_AIGW_.SupportBrief() over USP or TR-069 (it checks the home first).

A fix is only proposed (--propose, or propose in the MCP tool): it travels to
the gateway with the next check-in and waits there for a person's yes. Every
answer is logged (the fault, whether a visit was advised), for the fleet
report's proof: how many calls the help desk answered, and how few needed a
technician.
"""
import json
import time
from collections import Counter

FAULTS = ("NONE", "LINE", "HOME_WIFI", "DEVICE", "OUTSIDE", "UNKNOWN")
# the same split as the gateway's rules (core/libag/brief.c), for gateways that send no brief (before 0.37)
FAULT_OF = {
    "OK": "NONE", "LINE_FULL": "NONE",
    "NO_ROUTE": "LINE", "LINE_DOWN": "LINE", "ISP_DOWN": "LINE", "LINE_LOSS": "LINE", "ISP_LOSS": "LINE",
    "LINE_SLOW": "LINE", "ISP_SLOW": "LINE", "DNS_FAILING": "LINE", "DNS_SLOW": "LINE", "LINE_SLOW_SPEED": "LINE",
    "WIFI_COVERAGE": "HOME_WIFI", "WIFI_CONGESTION": "HOME_WIFI", "WIFI_HOPPING": "HOME_WIFI",
    "WIFI_RADIO": "HOME_WIFI", "WIFI_PROBLEM": "HOME_WIFI",
    "WIFI_DEVICE": "DEVICE", "WIFI_DROPS": "DEVICE", "DEVICE_OFFLINE": "DEVICE", "DEVICE_BLOCKED": "DEVICE",
    "DEVICE_LIMITED": "DEVICE",
}
FIX_ACTIONS = ("sqm", "stay24")         # what a brief may propose (it waits on the gateway for a yes)
STALE_S = 900                           # older than this: say so, and where to get a fresh one
SAME_HOUR = ("", "now", "last hour", "the last hour", "1h", "last 1h", "60m", "last 60 min")

SCHEMA = """CREATE TABLE IF NOT EXISTS briefs (
    gateway TEXT, ts INTEGER, fault TEXT, code TEXT, visit TEXT, remote_fix INTEGER, proposed INTEGER,
    age_s INTEGER, who TEXT)"""


def _table(store):
    store.x(SCHEMA)


def words(s):
    s = max(0, int(s))
    return f"{s} s" if s < 120 else f"{s // 60} min" if s < 7200 else f"{s // 3600} h" if s < 172800 else f"{s // 86400} days"


def find(store, ref=None):
    """A gateway by name or id, else by the TR-069 serial number its last report gave; None."""
    g = store.gateway(ref) if ref else store.gateway()
    if g or not ref:
        return g
    for row in store.q("SELECT id, snapshot FROM gateways"):
        try:
            b = (json.loads(row["snapshot"] or "{}") or {}).get("brief") or {}
        except ValueError:
            continue
        if isinstance(b, dict) and str(b.get("serial", "")).lower() == str(ref).lower():
            return store.gateway(row["id"])
    return None


def _from_care(snap):
    """An answer from proactive care alone (a gateway that sends no brief): careful, and says so."""
    care = snap.get("care") if isinstance(snap.get("care"), dict) else {}
    p = care.get("problem") if isinstance(care.get("problem"), dict) else None
    if p and p.get("code"):
        code = str(p["code"])
        fault = FAULT_OF.get(code, "UNKNOWN")
        return {"fault": fault, "code": code, "confidence": "low", "current": True,
                "verdict": f"From proactive care only: a problem open now ({code}, {p.get('area', '')}).",
                "evidence": [{"source": "care", "time": p.get("since"), "text": str(p.get("summary", "") or code)}],
                "done": [{"source": "care", "text": "Told the ISP (ProblemDetected!)."}] if p.get("reported") else [],
                "tell": "", "fix": {"possible": False, "text": "Ask the gateway for its brief (SupportBrief())."},
                "visit": "No", "visit_reason": "No: not without the gateway's brief; ask it (SupportBrief()) first."}
    if care.get("status") == "ok":
        return {"fault": "NONE", "code": "OK", "confidence": "low", "current": True,
                "verdict": "From proactive care only: no problem open now.", "evidence": [], "done": [],
                "tell": "", "fix": {"possible": False, "text": "Nothing to fix remotely."},
                "visit": "No", "visit_reason": "No: nothing wrong found."}
    return {"fault": "UNKNOWN", "code": "NO_DATA", "confidence": "low", "current": False,
            "verdict": "Not known: the gateway reported neither a brief nor proactive care.", "evidence": [], "done": [],
            "tell": "", "fix": {"possible": False, "text": "Nothing to fix remotely."},
            "visit": "No", "visit_reason": "No: not enough evidence."}


def answer(store, ref=None, period=None, propose=False, now=None, who="help desk"):
    """The brief for one home, from its last report, with how old it is; a fix proposed only if asked."""
    now = int(now or time.time())
    g = find(store, ref)
    if not g:
        raise ValueError(f"no home called {ref} (a gateway name, id or TR-069 serial number)" if ref
                         else "no gateway has reported yet")
    snap = store.snapshot(g["id"]) or {}
    b = snap.get("brief")
    out = {"home": g["name"], "gateway": g["id"], "group": g.get("grp") or ""}
    if isinstance(b, dict) and b.get("fault") in FAULTS:
        made = int(b.get("made") or g["last_seen"] or 0)
        for k in ("fault", "code", "confidence", "current", "verdict", "evidence", "done", "tell", "fix", "visit",
                  "visit_reason", "from", "to", "serial"):
            if k in b:
                out[k] = b[k]
        out["source"] = "the gateway's brief of the last hour, in its last report"
    else:
        made = int(g["last_seen"] or 0)
        out.update(_from_care(snap))
        out["source"] = "proactive care in the gateway's last report (it sends no brief: AIGW OS before 0.37)"
    age = now - made if made else None
    out["made"] = made
    out["age_s"] = age
    if age is None:
        out["freshness"] = "The gateway has never reported."
    elif age > STALE_S:
        out["freshness"] = (f"This is {words(age)} old. For a fresh one, ask the gateway itself: "
                            "Device.X_AIGW_.SupportBrief() over USP or TR-069 checks the home first.")
    else:
        out["freshness"] = f"As of {words(age)} ago."
    if period is not None and str(period).strip().lower() not in SAME_HOUR:
        out["period_note"] = (f"cortex has the last hour, as reported. For \"{period}\", ask the gateway: "
                              f"SupportBrief() with Period \"{period}\" (or agctl brief {period} at home).")
    out["refresh"] = ("Not through the bridge: it reports every few minutes and takes no requests. "
                      "The gateway answers SupportBrief() over USP or TR-069 at once.")
    fix = out.get("fix") if isinstance(out.get("fix"), dict) else {}
    proposed = 0
    if propose:
        if fix.get("possible") and fix.get("action") in FIX_ACTIONS and fix.get("target"):
            pid = store.propose(g["id"], fix["action"], str(fix["target"])[:200], 86400 if fix["action"] == "stay24" else 0,
                                ("help desk: " + str(out.get("verdict", "")))[:300], who)
            proposed = 1
            out["proposal"] = {"id": pid, "state": "queued",
                               "note": f"Sent to {g['name']} with its next check-in. It waits there for a person's "
                                       "yes; nothing changes until then."}
        elif fix.get("action") == "approve" and fix.get("decision"):
            out["proposal"] = {"decision": fix["decision"],
                               "note": "It already waits on the gateway for the customer's yes."}
        elif fix.get("possible"):
            out["proposal"] = {"note": "The device's address stays home: propose it from the gateway "
                                       "(SupportBrief() with Propose true)."}
        else:
            out["proposal"] = {"note": "Nothing to propose: " + str(fix.get("text") or "no remote fix") }
    _table(store)
    store.x("INSERT INTO briefs VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (g["id"], now, str(out.get("fault", "UNKNOWN")), str(out.get("code", "")), str(out.get("visit", "No")),
             int(bool(fix.get("possible"))), proposed, -1 if age is None else int(age), str(who)[:40]))
    return out


def fleet(store, since, ids):
    """For the fleet report: the briefs cortex answered in the period, by fault, and those the
    gateways answered themselves (over USP, TR-069 or agctl; their own counts)."""
    _table(store)
    rows = [r for r in store.q("SELECT * FROM briefs WHERE ts >= ?", (int(since),)) if r["gateway"] in ids]
    gw = Counter()
    gw_total = gw_visits = gw_remote = 0
    for row in store.q("SELECT id, snapshot FROM gateways"):
        if row["id"] not in ids:
            continue
        try:
            asked = ((json.loads(row["snapshot"] or "{}") or {}).get("brief") or {}).get("asked") or {}
        except (ValueError, AttributeError):
            continue
        gw_total += int(asked.get("total", 0) or 0)
        gw_visits += int(asked.get("visits", 0) or 0)
        gw_remote += int(asked.get("remote_fix", 0) or 0)
        for k, v in (asked.get("fault") or {}).items():
            if k in FAULTS and v:
                gw[k] += int(v)
    return {"briefs": len(rows), "homes": len({r["gateway"] for r in rows}),
            "fault": {k: v for k, v in Counter(r["fault"] for r in rows).most_common()},
            "visit_advised": sum(1 for r in rows if r["visit"] == "Yes"),
            "visit_if_persists": sum(1 for r in rows if r["visit"] == "IfPersists"),
            "no_visit": sum(1 for r in rows if r["visit"] == "No"),
            "remote_fix": sum(1 for r in rows if r["remote_fix"]), "proposed": sum(1 for r in rows if r["proposed"]),
            "on_gateways": {"briefs": gw_total, "fault": dict(gw.most_common()), "visit_advised": gw_visits,
                            "remote_fix": gw_remote}}


def fleet_lines(r):
    """The help desk's lines of the fleet report, in words."""
    h = r.get("help_desk") or {}
    out = []
    if h.get("briefs"):
        f = ", ".join(f"{k} {v}" for k, v in h["fault"].items())
        out.append(f"  help desk    {h['briefs']} brief{'' if h['briefs'] == 1 else 's'} for {h['homes']} "
                   f"home{'' if h['homes'] == 1 else 's'}: {f}; a technician advised for {h['visit_advised']}, "
                   f"only if it persists for {h['visit_if_persists']}, not needed for {h['no_visit']}; "
                   f"a remote fix for {h['remote_fix']} ({h['proposed']} proposed)")
    g = h.get("on_gateways") or {}
    if g.get("briefs"):
        f = ", ".join(f"{k} {v}" for k, v in g["fault"].items())
        out.append(f"               asked of the gateways themselves (USP, TR-069, agctl): {g['briefs']}: {f}; "
                   f"a technician advised for {g['visit_advised']}")
    return out


def text(b):
    """One brief in words, for the support agent."""
    out = [f"{b['home']}: {b.get('fault', 'UNKNOWN')} ({b.get('code', '')}, {b.get('confidence', 'low')} confidence)",
           f"  verdict   {b.get('verdict', '')}"]
    for i, e in enumerate(b.get("evidence") or []):
        out.append(("  evidence  " if not i else "            ") + (f"{e['clock']} " if e.get("clock") else "") + e.get("text", ""))
    for i, e in enumerate(b.get("done") or []):
        out.append(("  AIGW did  " if not i else "            ") + (f"{e['clock']} " if e.get("clock") else "") + e.get("text", ""))
    if b.get("tell"):
        out.append(f"  tell      {b['tell']}")
    fix = b.get("fix") or {}
    out.append(f"  fix       {'remote: ' if fix.get('possible') else ''}{fix.get('text', '')}")
    if b.get("proposal"):
        p = b["proposal"]
        out.append("            " + (f"proposal {p['id']}: " if p.get("id") else "") + p.get("note", ""))
    out.append(f"  visit     {b.get('visit_reason') or b.get('visit', 'No')}")
    out.append(f"  data      {b.get('source', '')}. {b.get('freshness', '')}")
    if b.get("period_note"):
        out.append(f"            {b['period_note']}")
    return "\n".join(out)
