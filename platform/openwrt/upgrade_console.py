#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Upgrade an older AIGW OS release to this one on OpenWRT in QEMU (used by upgrade-test.sh).

    upgrade_console.py <log> <work dir> <port> <from version> <to version> -- <qemu command...>

<work dir> holds old/ (the release's packages), new/ (this build's) and
upgrade-vm.sh; it is served to the VM on the host's <port> (10.0.2.2 inside),
which also takes the VM's snapshots (POST /up/<name>.tgz).

  1. the old release installed, a home set up on it (upgrade-vm.sh setup),
     agd restarted: snapshot "before" (only what the old agd kept)
  2. this build's packages installed over it with opkg, as a person upgrades:
     snapshot "after"
  3. a reboot: snapshot "rebooted"

Then: the settings word for word, the firewall integration, the remote phone and
the gateway's WireGuard key, the devices with their names and zone, the block
in effect, the care history, every data model path and the values set, the
files in /etc/aigw, and agd running the new version and enabled. Exit code: the
number of failed checks, or 100+ when the VM could not be set up.
"""
import http.server
import io
import json
import os
import re
import sys
import tarfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_console import VM  # noqa: E402


def serve(work, port):
    up = os.path.join(work, "up")
    os.makedirs(up, exist_ok=True)

    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=work, **k)

        def log_message(self, *a):
            pass

        def body(self):
            """The request's body: Content-Length, or chunked (uclient-fetch --post-file sends that)."""
            if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
                data = b""
                while len(data) < 8 << 20:
                    size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                    if not size:
                        self.rfile.readline()
                        return data
                    data += self.rfile.read(size)
                    self.rfile.readline()
                return b""
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if 0 < n < 8 << 20 else b""

        def do_POST(self):  # noqa: N802
            name = os.path.basename(self.path)
            data = self.body() if re.match(r"^[a-z]+\.tgz$", name) else b""
            if not data:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            with open(os.path.join(up, name), "wb") as f:
                f.write(data)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def parsed(key, text):
    if key.endswith(".json"):
        try:
            return json.loads(text)
        except ValueError:
            pass
    return text


def load(work, name):
    """A snapshot the VM posted: file name -> text (JSON files parsed)."""
    out = {}
    with tarfile.open(os.path.join(work, "up", f"{name}.tgz")) as t:
        for m in t.getmembers():
            if m.isfile():
                key = os.path.basename(m.name)
                out[key] = parsed(key, t.extractfile(m).read().decode(errors="replace"))
    return out


def load_dir(d):
    """A snapshot in a directory (the PC as AP's, tests/pcap-upgrade-test.sh)."""
    out = {}
    for key in os.listdir(d):
        with open(os.path.join(d, key), errors="replace") as f:
            out[key] = parsed(key, f.read())
    return out


def lines(text):
    return [ln.strip() for ln in (text or "").splitlines() if ln.strip()]


def dm(text):
    """path -> value; instance numbers made {i} for the paths (they may be renumbered)."""
    vals = {}
    for ln in lines(text):
        if " = " in ln:
            k, v = ln.split(" = ", 1)
            vals[k] = v
    return vals


def dm_paths(vals):
    return {re.sub(r"\.\d+\.", ".{i}.", k) for k in vals}


def checks_total(x):
    """Proactive care's history: the largest "checks" count in `agctl -j care` (its
    report over the longest period)."""
    if isinstance(x, dict):
        own = x.get("checks") if isinstance(x.get("checks"), int) else 0
        return max([own] + [checks_total(v) for v in x.values()])
    if isinstance(x, list):
        return max([0] + [checks_total(v) for v in x])
    return 0


def devices(snap):
    d = snap.get("devices.json")
    ds = d.get("devices", []) if isinstance(d, dict) else []
    return {(x.get("mac", "").lower(), x.get("hostname", ""), x.get("zone", "")) for x in ds if x.get("mac")}


def active(snap):
    d = snap.get("decisions.json")
    ds = d.get("decisions", []) if isinstance(d, dict) else []
    return {(x.get("action"), x.get("target")) for x in ds if x.get("state") == "active"}


def remote(snap):
    r = snap.get("remote.json")
    r = r if isinstance(r, dict) else {}
    return {"enabled": bool(r.get("enabled")), "running": bool(r.get("running")), "endpoint": r.get("endpoint"),
            "public_key": r.get("public_key"),
            "peers": sorted((p.get("name"), p.get("ip")) for p in r.get("peers", []))}


def compare(before, after, to_version, what, pc=False):
    """The checks, one line each: (ok, name, message). pc: a PC as AP (no fw4)."""
    out = []
    st = after.get("status.json") if isinstance(after.get("status.json"), dict) else {}
    out.append((st.get("version") == to_version, "version", f"agd runs {st.get('version')} (want {to_version})"))
    out.append((after.get("service.txt", "").strip() == "enabled", "service", "aigw enabled at boot"))
    ub, ua = lines(before.get("uci.txt")), lines(after.get("uci.txt"))
    gone, new = sorted(set(ub) - set(ua)), sorted(set(ua) - set(ub))
    out.append((not gone and not new, "settings",
                "kept word for word" if not gone and not new else f"lost {gone[:6]}, new {new[:6]}"))
    for key in ("aigw.my_iot.zone='iot'", "aigw.bridge.token='upgrade-test-token-0123456789'",
                "aigw.act.mode='enforce'"):
        out.append((key in ua, "setting " + key.split("=")[0], "kept" if key in ua else "missing"))
    out.append((not any(ln.startswith("aigw.wifi_busy") for ln in ua), "removed policy",
                "the policy removed stays removed"))
    if not pc:
        fb, fa = set(lines(before.get("firewall.txt"))), set(lines(after.get("firewall.txt")))
        out.append((bool(fb) and fb <= fa, "firewall",
                    "fw4 include and remote access rules kept" if fb <= fa else f"lost {sorted(fb - fa)}"))
    rb, ra = remote(before), remote(after)
    out.append((rb["enabled"] and rb["peers"] and ra == rb, "remote",
                f"on, {len(ra['peers'])} phone(s), same key" if ra == rb else f"before {rb}, after {ra}"))
    db, da = devices(before), devices(after)
    out.append((bool(db) and db <= da and any(z == "iot" for _, _, z in da), "devices",
                f"{len(da)} devices, names and zone kept" if db <= da else f"lost {sorted(db - da)}"))
    ab, aa = active(before), active(after)
    out.append((("block", "blocked.example.com") in ab and ab <= aa, "decisions",
                "the block is still in effect" if ab <= aa else f"before {sorted(ab)}, after {sorted(aa)}"))
    cb, ca = checks_total(before.get("care.json")), checks_total(after.get("care.json"))
    out.append((cb > 0 and ca >= cb, "care history", f"{ca} checks kept (before: {cb})"))
    vb, va = dm(before.get("dm.txt")), dm(after.get("dm.txt"))
    pb, pa = dm_paths(vb), dm_paths(va)
    out.append((len(pb) > 50 and pb <= pa, "data model",
                f"{len(pa)} paths, all {len(pb)} of before kept" if pb <= pa else f"lost {sorted(pb - pa)[:8]}"))
    for k in ("Device.X_EXAMPLE-COM_.Mode", "Device.DeviceInfo.Manufacturer"):
        out.append((vb.get(k) and va.get(k) == vb.get(k), "dm " + k, f"{vb.get(k)!r} -> {va.get(k)!r}"))
    fb, fa = set(lines(before.get("files.txt"))), set(lines(after.get("files.txt")))
    out.append((fb <= fa, "files", "every file in /etc/aigw kept (keys, peers, records)" if fb <= fa
                else f"lost or changed {sorted(fb - fa)}"))
    return [(ok, f"{what}-{name}", msg) for ok, name, msg in out]


def compare_main(args):
    """compare <before dir> <after dir> <version> [--pc]: the same checks, from directories."""
    failed = 0
    for ok, name, msg in compare(load_dir(args[0]), load_dir(args[1]), args[2], "upgrade", "--pc" in args):
        print(f"    {'PASS' if ok else 'FAIL'} {name}: {msg}", flush=True)
        failed += 0 if ok else 1
    sys.exit(min(failed, 99))


def main():
    args = sys.argv[1:]
    if args and args[0] == "compare":
        return compare_main(args[1:])
    sep = args.index("--")
    log, work, port, from_v, to_v = args[:5]
    vm = VM(args[sep + 1:], log)
    httpd = serve(work, port)
    say = lambda s: print(f"==> {s}", flush=True)  # noqa: E731
    host = f"http://10.0.2.2:{port}"
    old = sorted(os.listdir(os.path.join(work, "old")))
    new = sorted(os.listdir(os.path.join(work, "new")))

    def must(cmd, what, timeout=180):
        rc, out = vm.run(cmd, timeout)
        if rc:
            say(f"FAILED: {what} (exit {rc}); see {log}")
            for ln in out.splitlines()[-30:]:
                print("    | " + ln.rstrip("\r"), flush=True)
            vm.p.kill()
            sys.exit(101)
        return out

    def shell():
        vm.expect(r"Please press Enter to activate this console", 600)
        time.sleep(2)
        vm.send("\n")
        vm.expect(r"root@[^:]+:[^#]*# ", 60)
        vm.run("dmesg -n 1; export TERM=dumb")
        must("(i=0; until ping -c1 -W1 10.0.2.2 >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 90 ] && exit 1; sleep 1; done)",
             "network", 120)

    def snap(name):
        # /tmp is RAM: fetched again after a reboot
        must(f"wget -q -O /tmp/upgrade-vm.sh {host}/upgrade-vm.sh", "fetch upgrade-vm.sh", 60)
        out = must(f"sh /tmp/upgrade-vm.sh snap {name} {host}/up/{name}.tgz", f"snapshot {name}", 300)
        if not os.path.exists(os.path.join(work, "up", f"{name}.tgz")):
            say(f"FAILED: snapshot {name} did not arrive: {' '.join(out.split())[-300:]}")
            vm.p.kill()
            sys.exit(104)
        return load(work, name)

    retry = "(n=0; until {}; do n=$((n+1)); [ $n -ge 3 ] && exit 1; sleep 5; done)"
    try:
        say(f"booting OpenWRT; AIGW OS {from_v} from its release: {' '.join(old)}")
        shell()
        must("mkdir -p /tmp/old /tmp/new", "dirs", 30)
        for d, files in (("old", old), ("new", new)):
            for f in files:
                must(f"wget -q -O /tmp/{d}/{f} {host}/{d}/{f}", f"download {d}/{f}", 120)
        must(retry.format("opkg update"), "opkg update", 600)
        must(retry.format("opkg install /tmp/old/*.ipk"), f"install {from_v}", 1200)
        must("/etc/init.d/aigw enable; /etc/init.d/aigw restart; (i=0; until agctl status >/dev/null 2>&1;"
             " do i=$((i+1)); [ $i -gt 120 ] && exit 1; sleep 1; done)", "agd answering", 240)
        say(f"a home on {from_v}: settings, a policy of its own, devices and a zone, a block, care, a remote phone")
        out = must(f"wget -q -O /tmp/upgrade-vm.sh {host}/upgrade-vm.sh && sh /tmp/upgrade-vm.sh setup", "setup", 600)
        for ln in out.splitlines()[-8:]:
            print("    | " + ln.rstrip("\r"), flush=True)
        before = snap("before")
        say(f"upgrading to {to_v} with opkg: {' '.join(new)}")
        out = must(retry.format("opkg install /tmp/new/*.ipk"), f"install {to_v}", 1200)
        print("    " + " | ".join(ln.strip() for ln in out.splitlines() if ln.strip().startswith(("Upgrading", "Configuring",
                                                                                                    "Collected")))[:600])
        after = snap("after")
        failed = 0

        def report(checks):
            nonlocal failed
            for ok, name, msg in checks:
                print(f"    {'PASS' if ok else 'FAIL'} {name}: {msg}", flush=True)
                failed += 0 if ok else 1
        report(compare(before, after, to_v, "upgrade"))
        say("rebooting")
        vm.send("reboot\n")
        shell()
        rc, out = vm.run(f"wget -q -O /tmp/upgrade-vm.sh {host}/upgrade-vm.sh && "
                         f"sh /tmp/upgrade-vm.sh snap rebooted {host}/up/rebooted.tgz", 400)
        if rc or not os.path.exists(os.path.join(work, "up", "rebooted.tgz")):
            report([(False, "reboot-agd", "after the reboot: " + " | ".join(
                ln.strip() for ln in out.splitlines()[-12:] if ln.strip()))])
        else:
            report(compare(before, load(work, "rebooted"), to_v, "reboot"))
        vm.send("poweroff\n")
        try:
            vm.p.wait(30)
        except Exception:  # noqa: BLE001
            vm.p.kill()
        httpd.shutdown()
        sys.exit(min(failed, 99))
    except (TimeoutError, EOFError) as e:
        say(f"FAILED: {e}; see {log}")
        vm.p.kill()
        sys.exit(103)


if __name__ == "__main__":
    main()
