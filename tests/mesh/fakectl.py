#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A fake EasyMesh controller for AIGW's mesh tests: a simulated home whose
controller has a standard Data Elements northbound (TR-181
Device.WiFi.DataElements.). Run by these names (a link to this file):

    ubus      the controller's objects on ubus, as the mesh-dataelements handler
              reaches them:
                ubus -S list <object>
                ubus call Device.WiFi.DataElements.Network _get '{"depth":"10"}'
                ubus call <...BSS.{i}.STA.{i}> ClientSteer '{"TargetBSSID": ..., ...}'
                ubus call <...Device.{i}.Radio.{i}> ChannelSelectionRequest '{"Class.1.OpClass": ..., ...}'
    obuspa    the same through a local USP agent: obuspa -c get <path>,
              obuspa -c operate '<path>.<Command>(name=value,...)'
    anything else   a snapshot: fakectl.py get <file>

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

_get answers with path-keyed objects, one key a line, as ubus prints them.
"get <file>" writes the same; with a file "lines" in the directory, as
"path=value" lines. Each station is heard (UnassociatedSTA) by every radio it
has a link to. Requests go to requests.log as "steer <station> <bssid>
<RequestMode> imminent=<BTMDisassociationImminent>" and "channel <ruid>
<opclass> <channel at preference 15> others=<channels at lower preference>";
"obey": false stations ignore steers (a miss). Files in the directory: "refuse"
makes every command answer Error_Other; "nobus" takes the objects off ubus;
"noagent" takes them off the USP agent.
"""
import json
import os
import re
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
    o = model_read()
    with open(path, "w") as f:
        if os.path.exists(os.path.join(DIR, "lines")):
            for obj, params in o.items():
                for k, v in params.items():
                    v = ("true" if v else "false") if isinstance(v, bool) else v
                    f.write('%s%s="%s"\n' % (obj, k, v) if isinstance(v, str) else "%s%s=%s\n" % (obj, k, v))
        else:
            json.dump(o, f, indent=1)


def flag(name):
    return os.path.exists(os.path.join(DIR, name))


def command(obj, cmd, args):
    """A TR-181 command on an instance: its Status."""
    obj = obj.rstrip(".") + "."
    w = load()
    o = model(w).get(obj)
    if o is None:
        return "Error_Invalid_Input"
    if cmd == "ClientSteer" and "MACAddress" in o:
        mac, bssid = o["MACAddress"].lower(), str(args.get("TargetBSSID", "")).lower()
        log("steer %s %s %s imminent=%s" % (mac, bssid, args.get("RequestMode"),
                                            str(args.get("BTMDisassociationImminent")).lower()))
        if flag("refuse"):
            return "Error_Other"
        s = w["stations"].get(mac)
        if not s or args.get("RequestMode") not in ("Steering_Mandate", "Steering_Opportunity"):
            return "Error_Invalid_Input"
        if s.get("obey", True):
            s["at"] = bssid
            s["joined"] = time.time()
            s["ctl_steered"] = time.time()
        save(w)
        return "Success"
    if cmd == "ChannelSelectionRequest" and "Utilization" in o:
        opclass = int(args.get("Class.1.OpClass", 0))
        prefs = {}
        i = 1
        while "Class.1.Channel.%d.Channel" % i in args:
            prefs[int(args["Class.1.Channel.%d.Channel" % i])] = int(args.get("Class.1.Channel.%d.Preference" % i, 0))
            i += 1
        best = [c for c, p in prefs.items() if p == 15]
        others = sorted(c for c, p in prefs.items() if p < 15)
        log("channel %s %d %s others=%s" % (o["ID"].lower(), opclass, ",".join(map(str, best)),
                                            ",".join(map(str, others))))
        if flag("refuse"):
            return "Error_Other"
        if len(best) != 1 or not opclass:
            return "Error_Invalid_Input"
        for ag in w["agents"]:
            for r in ag["radios"]:
                if r["id"].lower() == o["ID"].lower():
                    r["class"], r["ch"] = opclass, best[0]
        save(w)
        return "Success"
    return "Error_Invalid_Input"


def fake_ubus(a):
    """ubus [-S] [-t <s>] [-s <socket>] list|call ..."""
    while a and a[0].startswith("-"):
        a = a[2:] if a[0] in ("-t", "-s") else a[1:]
    if flag("nobus"):
        sys.exit(4)    # ubus: not found
    if a[:1] == ["list"]:
        names = [k.rstrip(".") for k in model(load())]
        for n in names:
            if len(a) < 2 or n == a[1]:
                print(n)
        return
    if a[:1] == ["call"] and len(a) >= 3:
        obj, method = a[1], a[2]
        args = json.loads(a[3]) if len(a) > 3 else {}
        if method == "_get":
            if obj.rstrip(".") != DE.rstrip("."):
                sys.exit(4)
            print(json.dumps(model_read(), indent="\t"))
            return
        if obj.rstrip(".") + "." not in model(load()):
            sys.exit(4)
        print(json.dumps({"Status": command(obj, method, args)}, indent="\t"))
        return
    sys.exit(2)


def fake_obuspa(a):
    """obuspa -c get <path> | obuspa -c operate '<path>.<Command>(name=value,...)'"""
    if a[:1] != ["-c"] or len(a) < 3:
        sys.exit(2)
    if flag("noagent"):
        print("ERROR: Unable to connect to the agent")
        sys.exit(1)
    if a[1] == "get":
        for obj, params in model_read().items():
            for k, v in params.items():
                if (obj + k).startswith(a[2]):
                    print("%s%s => %s" % (obj, k, ("true" if v else "false") if isinstance(v, bool) else v))
        return
    if a[1] == "operate":
        m = re.match(r"^(.*)\.(\w+)\((.*)\)$", a[2])
        if not m:
            print("Missing opening bracket in the argument")
            sys.exit(1)
        args = dict(kv.split("=", 1) for kv in m.group(3).split(",") if "=" in kv)
        st = command(m.group(1), m.group(2), args)
        print("Synchronous Operation (%s.%s()) completed successfully.\nOutput Arguments:-\n   Status => %s"
              % (m.group(1), m.group(2), st))
        return
    sys.exit(2)


def model_read():
    """The model as a read sees it: the counters move on."""
    cf = os.path.join(DIR, "counters.json")
    try:
        with open(cf) as f:
            COUNTERS.update(json.load(f))
    except (OSError, ValueError):
        pass
    o = model(load())
    with open(cf + ".tmp", "w") as f:
        json.dump(COUNTERS, f)
    os.replace(cf + ".tmp", cf)
    return o


def main():
    name = os.path.basename(sys.argv[0])
    a = sys.argv[1:]
    if name == "ubus":
        return fake_ubus(a)
    if name == "obuspa":
        return fake_obuspa(a)
    if a[:1] == ["get"] and len(a) == 2:
        get(a[1])
        return
    sys.exit(2)


if __name__ == "__main__":
    main()
