# SPDX-License-Identifier: Apache-2.0
"""Line health: which way each of a line's numbers is going, and when it would be
a fault at that rate. The same rules as the gateway's (core/libag/line.c, see
include/ag/line.h); cortex uses them to simulate fleets (areasim.py), and
tests/test_areapredict.py checks both give the same answers on the same days.

analyse(days, today, metric) -> {state, ndays, level, base, slope, fault, onset, days_left, conf}
    days: [(day number, value), ...]; only the `window` complete days before today count.
"""
import math

METRICS = ("sync", "margin", "optical", "fec", "crc", "drops", "latency", "loss", "speed")
NAMES = ("SyncDown", "NoiseMargin", "OpticalPower", "FECErrors", "CRCErrors", "Drops", "NextHopLatency",
         "NextHopLoss", "DownloadSpeed")
UNITS = ("kbit/s", "dB", "dBm", "/h", "/h", "/day", "ms", "%", "Mbit/s")
WORDS = ("the sync rate", "the noise margin", "the optical level", "FEC errors", "CRC errors", "retrains and drops",
         "the next hop's delay", "loss to the next hop", "the speed test")
# lower is worse; fault = base * rel + add; the least change worth saying: max(floor_abs, floor_rel * |base|)
DEF = {
    "sync": (True, 0.7, 0.0, 0.0, 0.05), "margin": (True, 0.0, 3.0, 1.0, 0.0),
    "optical": (True, 0.0, -27.0, 1.0, 0.0), "fec": (False, 10.0, 1000.0, 100.0, 0.5),
    "crc": (False, 5.0, 60.0, 5.0, 0.5), "drops": (False, 1.0, 4.0, 2.0, 0.0),
    "latency": (False, 1.0, 30.0, 3.0, 0.2), "loss": (False, 1.0, 2.0, 0.3, 0.0),
    "speed": (True, 0.7, 0.0, 1.0, 0.05),
}
DEGRADED = ("Stepped", "Drifting", "Fault")
PARAMS = {"min_days": 5, "window": 14, "cp_z": 4.0, "tau": 0.5, "horizon": 30}


def median(v):
    v = sorted(v)
    n = len(v)
    if not n:
        return 0.0
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def theil_sen(x, z):
    s, c = [], 0
    for i in range(len(x)):
        for j in range(i + 1, len(x)):
            dz = z[j] - z[i]
            s.append(dz / (x[j] - x[i]))
            c += 1 if dz > 0 else -1 if dz < 0 else 0
    return median(s), (c / len(s) if s else 0.0)


def _r(v):
    """Round half away from zero, as C's lroundf."""
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


def conf_name(c):
    return "high" if c >= 70 else "medium" if c >= 40 else "low"


def analyse(days, today, metric, fault=None, p=None):
    p = dict(PARAMS, **(p or {}))
    lower, rel, add, fl_abs, fl_rel = DEF[metric]
    sg = -1.0 if lower else 1.0
    o = {"state": "", "ndays": 0, "level": 0.0, "base": 0.0, "slope": 0.0, "fault": None, "onset": 0,
         "days_left": -1, "conf": 0, "tau7": 0.0, "slope7": 0.0}
    w = sorted((int(d), float(v)) for d, v in days
               if d and today - p["window"] <= int(d) < today and v is not None and math.isfinite(float(v)))
    k = len(w)
    o["ndays"] = k
    if not k:
        return o
    x = [float(d - today) for d, _ in w]
    z = [sg * v for _, v in w]
    if k < p["min_days"]:
        return dict(o, state="Learning", level=w[-1][1], base=sg * median(z))
    base = sg * median(z[:max(3, k // 3)])
    floor_ = max(fl_abs, fl_rel * abs(base))
    sig = max(1.4826 * median([abs(z[i] - z[i - 1]) for i in range(1, k)]) / 1.4142, 0.25 * floor_)
    if k >= 7:                  # the last 7 days on their own, proven or not (cortex pools them)
        s7, t7 = theil_sen(x[-7:], z[-7:])
        o["slope7"], o["tau7"] = sg * s7, t7
    best, step, frm = 0.0, 0.0, 0
    for i in range(3, k - 2):
        a, b = median(z[:i]), median(z[i:])
        mid = (a + b) / 2
        lo = sum(1 for j in range(i) if z[j] < mid)
        hi = sum(1 for j in range(i, k) if z[j] > mid)
        if 4 * lo < 3 * i or 4 * hi < 3 * (k - i):
            continue
        st = (b - a) / sig * math.sqrt(i * (k - i) / k)
        if st > best:
            best, step, frm = st, b - a, i
    cp = best >= p["cp_z"] and step >= floor_
    if cp:
        base = sg * median(z[:frm])
    else:
        frm = 0
    slope = tau = 0.0
    drift = False
    seg = frm
    # the trend after the change point; a ramp's median split comes late: if too short to prove, start earlier
    for i in range(frm, (2 if cp else 0) - 1, -1):
        nseg = k - i
        if nseg < 4:
            continue
        slope, tau = theil_sen(x[i:], z[i:])
        drift = (slope > 0 and tau >= p["tau"] and slope * (x[-1] - x[i]) >= floor_ and
                 3 * tau * math.sqrt(nseg * (nseg - 1)) / math.sqrt(2 * (2 * nseg + 5)) >= 2.33)
        if drift:
            seg = i
        if drift or not cp:
            break
    nseg = k - seg
    if drift:
        lz = median([z[i] - slope * x[i] for i in range(seg, k)]) + slope * x[-1]
    else:
        slope = 0.0
        lz = median(z[frm:])
    flt = fault if fault else (base * rel + add if rel else add)
    fz = sg * flt
    if sg * base >= fz:
        flt = None
    o.update(level=sg * lz, base=base, slope=sg * slope, fault=flt)
    o["onset"] = w[frm][0] if cp else 0
    if drift:
        at = min(max(x[-1] - (lz - sg * base) / slope, x[0]), x[-1])
        o["onset"] = today + _r(at)
    cd = 100 * tau * min(1.0, nseg / 10) if drift else 0.0
    cs = 100 * min(1.0, best / (2 * p["cp_z"])) * min(1.0, k / 10) if cp else 0.0
    if flt is not None and lz >= fz:
        o.update(state="Fault", days_left=0, conf=_r((max(60.0, cd, cs))))
        o["onset"] = o["onset"] or w[-1][0]
        return o
    if drift and flt is not None:
        left = math.ceil((fz - lz) / slope)
        o["days_left"] = int(left) if left <= p["horizon"] else -1
    o["state"] = "Drifting" if drift else "Stepped" if cp else "OK"
    o["conf"] = _r((max(cd, cs))) if drift else _r((cs)) if cp else 0
    return o


def words(metric, t):
    """The trend in words, as the gateway says it."""
    i = METRICS.index(metric)
    what = WORDS[i][0].upper() + WORDS[i][1:]
    u = UNITS[i]

    def val(v):
        a = abs(v)
        if a >= 100 or metric in ("sync", "drops"):
            return f"{v:.0f} {u}"
        return f"{v:.1f} {u}" if a >= 10 else f"{v:.2g} {u}"
    if t["state"] in ("", "Learning"):
        return f"{what}: not enough days yet ({t['ndays']} of 5)."
    if t["state"] == "OK":
        return f"{what} is steady ({val(t['level'])})."
    lower = DEF[metric][0]
    verb = ("stepped down" if lower else "stepped up") if t["state"] == "Stepped" else "fell" if lower else "rose"
    s = f"{what} {verb} from {val(t['base'])} to {val(t['level'])}"
    if t["state"] == "Drifting":
        s += f", {val(abs(t['slope']))} a day"
    if t["days_left"] == 0:
        return s + f": a fault now ({val(t['fault'])})."
    if t["days_left"] > 0:
        return s + f": at this rate a fault ({val(t['fault'])}) in about {t['days_left']} day" + \
            ("" if t["days_left"] == 1 else "s") + "."
    return s + (", and stays there." if t["state"] == "Stepped" else ".")


def vector(series, today, tech="dsl"):
    """A gateway's line health vector (as the bridge sends it) from its days:
    series {metric: [(day, value), ...]}. For simulations and tests."""
    ms = []
    worst = None
    for m in METRICS:
        if m not in series:
            continue
        t = analyse(series[m], today, m)
        ms.append({"metric": m, "state": t["state"], "days": t["ndays"], "level": t["level"], "base": t["base"],
                   "slope": t["slope"], "fault": t["fault"], "onset": t["onset"], "days_left": t["days_left"],
                   "conf": t["conf"], "tau7": t["tau7"], "slope7": t["slope7"]})
    for e in ms:
        if e["state"] not in DEGRADED or (e["conf"] < 40 and e["state"] != "Fault"):
            continue
        if worst is None or _worse(e, worst):
            worst = e
    status = worst["state"] if worst else ("Learning" if any(e["state"] == "Learning" for e in ms) else "OK")
    return {"v": 1, "tech": tech, "day": today, "status": status, "worst": worst["metric"] if worst else "",
            "days_left": worst["days_left"] if worst else -1, "conf": worst["conf"] if worst else 0, "metrics": ms}


RANK = {"": 0, "Learning": 1, "OK": 2, "Stepped": 3, "Drifting": 4, "Fault": 5}


def _worse(a, b):
    if RANK[a["state"]] != RANK[b["state"]]:
        return RANK[a["state"]] > RANK[b["state"]]
    if (a["days_left"] >= 0) != (b["days_left"] >= 0):
        return a["days_left"] >= 0
    if a["days_left"] != b["days_left"]:
        return a["days_left"] < b["days_left"]
    return a["conf"] > b["conf"]
