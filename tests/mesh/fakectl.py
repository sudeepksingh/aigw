#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A fake EasyMesh controller for AIGW's mesh tests: a mesh handler (agd's
mesh.handler) over a simulated home.

    fakectl.py get <file>                               the Data Elements model
    fakectl.py steer <station> <bssid> <btm|mandate>    move a station
    fakectl.py channel <ruid> <ch> <opclass> <centre> <width>

The home is a JSON "world" next to this program's state ($FAKECTL_DIR, default
the directory of the world file given in $FAKECTL_WORLD):

    {"network": "home-1", "controller": "<AL MAC of the gateway>",
     "agents": [{"id": AL MAC, "model": "...",
                 "backhaul": null | {"type": "Wi-Fi", "mac": parent BSSID, "parent": AL MAC,
                                     "dbm": -60, "kbps": 600000, "class": 128, "ch": 42},
                 "radios": [{"id": RUID, "class": 81, "ch": 1, "util": 40,
                             "bss": [{"bssid": ..., "ssid": "home", "backhaul": false, "fronthaul": true}]}]}],
     "stations": {MAC: {"at": BSSID, "btm": true, "obey": true,
                        "links": {BSSID: [dBm, kbit/s, % sent again]}, "ctl_steered": 0}}}

"get" writes it the way prplMesh's controller answers `ubus call
Device.WiFi.DataElements.Network _get '{"depth":"10"}'` (path-keyed objects);
with a file "lines" in the directory, as "path=value" lines (ubus-cli, ba-cli).
Each station is heard (UnassociatedSTA) by every radio it has a link to.
Requests go to requests.log; "obey": false stations ignore steers (a miss);
a file "refuse" makes every request fail.
"""
import json
import os
import sys
import time

WORLD = os.environ.get("FAKECTL_WORLD") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "world.json")
DIR = os.environ.get("FAKECTL_DIR") or os.path.dirname(os.path.abspath(WORLD))
DE = "Device.WiFi.DataElements.Network."
CAPS_BTM = "31040a000004686f6d657f08000008000000004046057200000000"
CAPS_LEGACY = "31040a000004686f6d65"


def load():
    with open(WORLD) as f:
        return json.load(f)


def save(w):
    tmp = WORLD + ".tmp"
    with open(tmp, "w") as f:
        json.dump(w, f, indent=1)
    os.replace(tmp, WORLD)


def rcpi(dbm):
    return max(0, min(220, (int(dbm) + 110) * 2))


def log(line):
    with open(os.path.join(DIR, "requests.log"), "a") as f:
        f.write(line + "\n")


COUNTERS = {}


def counters(w, mac, retry):
    """Packets sent grow by 1000 a read; resends by the station's rate of them (kept apart from the world,
    which the test edits)."""
    c = COUNTERS.setdefault(mac, [1000, 0])
    c[0] += 1000
    c[1] += int(retry) * 10
    return c


def model(w):
    now = time.time()
    o = {DE: {"ID": w.get("network", "home"), "ControllerID": w["controller"],
              "DeviceNumberOfEntries": len(w["agents"])}}
    radio_of = {}
    for di, a in enumerate(w["agents"], 1):
        dp = "%sDevice.%d." % (DE, di)
        o[dp] = {"ID": a["id"], "Manufacturer": "fake", "ManufacturerModel": a.get("model", "fake-ap")}
        bh = a.get("backhaul")
        o[dp + "MultiAPDevice.Backhaul."] = {
            "LinkType": bh["type"] if bh else "None", "BackhaulMACAddress": bh.get("mac", "") if bh else "",
            "BackhaulDeviceID": bh.get("parent", "") if bh else "", "MACAddress": bh.get("own", "") if bh else ""}
        if bh:
            o[dp + "MultiAPDevice.Backhaul.Stats."] = {"SignalStrength": rcpi(bh.get("dbm", -60)),
                                                       "LastDataDownlinkRate": bh.get("kbps", 0),
                                                       "LastDataUplinkRate": bh.get("kbps", 0),
                                                       "LinkUtilization": bh.get("util", 20)}
            if bh.get("ch"):
                o[dp + "MultiAPDevice.Backhaul.CurrentOperatingClassProfile.1."] = {"Class": bh.get("class", 0),
                                                                                   "Channel": bh["ch"]}
        for ri, r in enumerate(a["radios"], 1):
            rp = "%sRadio.%d." % (dp, ri)
            o[rp] = {"ID": r["id"], "Utilization": int(r.get("util", 30) * 255 / 100), "Noise": rcpi(-95)}
            o[rp + "CurrentOperatingClassProfile.1."] = {"Class": r["class"], "Channel": r["ch"]}
            for bi, b in enumerate(r["bss"], 1):
                bp = "%sBSS.%d." % (rp, bi)
                o[bp] = {"BSSID": b["bssid"], "SSID": b.get("ssid", "home"), "Enabled": True,
                         "BackhaulUse": b.get("backhaul", False), "FronthaulUse": b.get("fronthaul", True)}
                radio_of[b["bssid"].lower()] = (rp, bp)
                si = 0
                for mac, s in sorted(w["stations"].items()):
                    if s["at"].lower() != b["bssid"].lower():
                        continue
                    si += 1
                    dbm, kbps, retry = s["links"].get(s["at"], [-90, 1000, 50])
                    c = counters(w, mac, retry)
                    sp = "%sSTA.%d." % (bp, si)
                    o[sp] = {"MACAddress": mac.upper(), "SignalStrength": rcpi(dbm), "LastDataDownlinkRate": kbps,
                             "LastDataUplinkRate": kbps // 2, "EstMACDataRateDownlink": kbps // 1000,
                             "PacketsSent": c[0], "RetransCount": c[1], "ErrorsSent": 0,
                             "LastConnectTime": int(now - s.get("joined", now - 3600)),
                             "ClientCapabilities": CAPS_BTM if s.get("btm", True) else CAPS_LEGACY}
                    steered = s.get("ctl_steered", 0)
                    o[sp + "MultiAPSTA.SteeringSummaryStats."] = {
                        "LastSteerTime": max(1, int(now - steered)) if steered else 0,
                        "BTMAttempts": 0, "BTMFailures": 0}
    # each station heard by the radios it has a link to (unassociated station link metrics)
    un = {}
    for mac, s in sorted(w["stations"].items()):
        for bssid, (dbm, _, _) in s["links"].items():
            if bssid.lower() == s["at"].lower() or bssid.lower() not in radio_of:
                continue
            rp = radio_of[bssid.lower()][0]
            un[rp] = un.get(rp, 0) + 1
            o["%sUnassociatedSTA.%d." % (rp, un[rp])] = {"MACAddress": mac, "SignalStrength": rcpi(dbm)}
    return o


def get(path):
    cf = os.path.join(DIR, "counters.json")
    w = load()
    try:
        with open(cf) as f:
            COUNTERS.update(json.load(f))
    except (OSError, ValueError):
        pass
    o = model(w)
    with open(cf + ".tmp", "w") as f:
        json.dump(COUNTERS, f)
    os.replace(cf + ".tmp", cf)
    with open(path, "w") as f:
        if os.path.exists(os.path.join(DIR, "lines")):
            for obj, params in o.items():
                for k, v in params.items():
                    v = ("true" if v else "false") if isinstance(v, bool) else v
                    f.write('%s%s="%s"\n' % (obj, k, v) if isinstance(v, str) else "%s%s=%s\n" % (obj, k, v))
        else:
            json.dump(o, f, indent=1)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(2)
    if a[0] == "get" and len(a) == 2:
        get(a[1])
        return
    log(" ".join(a))
    if os.path.exists(os.path.join(DIR, "refuse")):
        print("refused (the test asked the controller to say no)")
        sys.exit(1)
    w = load()
    if a[0] == "steer" and len(a) == 4:
        s = w["stations"].get(a[1].lower())
        if not s:
            print("no such station")
            sys.exit(1)
        if s.get("obey", True):
            s["at"] = a[2].lower()
            s["joined"] = time.time()
            s["ctl_steered"] = time.time()
        save(w)
        print("steering %s to %s" % (a[1], a[2]))
        return
    if a[0] == "channel" and len(a) == 6:
        for ag in w["agents"]:
            for r in ag["radios"]:
                if r["id"].lower() == a[1].lower():
                    width = int(a[5])
                    r["class"] = int(a[3])
                    r["ch"] = int(a[4]) if width > 20 else int(a[2])
                    save(w)
                    print("radio %s on channel %s" % (a[1], a[2]))
                    return
        print("no such radio")
        sys.exit(1)
    sys.exit(2)


if __name__ == "__main__":
    main()
