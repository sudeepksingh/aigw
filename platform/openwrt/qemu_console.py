#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive an OpenWRT VM over its serial console (used by qemu-test.sh).

    qemu_console.py <log file> <packages dir> <port> <luci port> [--keep] -- <qemu command...>

Boots, waits for the console, brings packages over HTTP from the host
(10.0.2.2 inside QEMU's user network), installs them and LuCI with opkg,
starts AIGW, runs `aigw-selftest --full`, then checks LuCI from the host
the way the AIGW pages use it. Exit code: the number of failed checks, or
100+ when the VM itself could not be set up. --keep leaves the VM running
afterwards with LuCI on http://127.0.0.1:<luci port> (root, no password)
until interrupted.
"""
import json
import os
import re
import select
import subprocess
import sys
import time
import urllib.request


class VM:
    def __init__(self, cmd, log):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  bufsize=0)
        self.buf = b""
        self.log = open(log, "ab", buffering=0)
        self.n = 0

    def read(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if r:
            data = os.read(self.p.stdout.fileno(), 65536)
            if not data:
                raise EOFError("QEMU exited")
            self.log.write(data)
            self.buf += data

    def expect(self, pattern, timeout):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(f"waiting for {pattern!r}")
            self.read(min(left, 1.0))

    def send(self, s):
        self.p.stdin.write(s.encode())
        self.p.stdin.flush()

    def run(self, cmd, timeout=120):
        """Run a shell command; returns (exit code, output)."""
        self.n += 1
        tag = f"__AIGW_{self.n}__"
        self.buf = b""
        # the marker's digits only appear in the output, never in the echoed command line
        self.send(f"{cmd}; echo {tag}RC=$?{tag}\n")
        m = self.expect(rf"{tag}RC=(\d+){tag}", timeout)
        return int(m.group(1)), m.string[:m.start()].decode(errors="replace")


def luci_check(base):
    """LuCI as the AIGW pages use it: log in, call through rpcd with the app's ACL, fetch views and menu."""
    def rpc(sid, obj, method, args=None):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "call", "params": [sid, obj, method, args or {}]})
        req = urllib.request.Request(base + "/ubus", data=body.encode(), headers={"Content-Type": "application/json"})
        return json.load(urllib.request.urlopen(req, timeout=30))

    def get(path, sid=None):
        req = urllib.request.Request(base + path, headers={"Cookie": f"sysauth_http={sid}"} if sid else {})
        r = urllib.request.urlopen(req, timeout=30)
        return r.status, r.read().decode(errors="replace")

    out = []
    sid = rpc("0" * 32, "session", "login", {"username": "root", "password": ""})["result"][1]["ubus_rpc_session"]
    for obj, method in [("ag", "status"), ("ag.device", "list"), ("ag.alert", "list"), ("ag.decision", "list"),
                        ("ag.plugin", "list"), ("ag.model", "list"), ("ag.remote", "list"), ("ag.wifi", "keys"),
                        ("ag.dnscache", "stats"), ("ag.wifi", "radios")]:
        res = rpc(sid, obj, method).get("result", [-1])
        out.append((res[0] == 0, f"luci-{obj}-{method}", f"rpcd answers for the AIGW pages (code {res[0]})"))
    err = rpc(sid, "ag.model", "install", {"file": "/etc/passwd"}).get("error")
    out.append((bool(err), "luci-acl-deny", "a call outside the ACL is refused"))
    # the remote-access page: turn on, add a phone, get the QR matrix the page draws, undo it all
    on = rpc(sid, "ag.remote", "on", {"endpoint": "192.0.2.1"}).get("result", [-1, {}])
    add = rpc(sid, "ag.remote", "add", {"name": "luci-check"}).get("result", [-1, {}])
    rows = add[1].get("qr_rows", []) if len(add) > 1 else []
    rpc(sid, "ag.remote", "revoke", {"name": "luci-check"})
    rpc(sid, "ag.remote", "off")
    out.append((len(on) > 1 and on[1].get("ok") and len(rows) >= 21 and all(len(r) == len(rows) for r in rows),
                "luci-remote-qr", f"remote on, a phone added from LuCI, QR code {len(rows)}x{len(rows)}"))
    views = []
    for v in ("overview", "devices", "decisions", "access", "plugins"):
        st, body = get(f"/luci-static/resources/view/aigw/{v}.js")
        if st == 200 and "view.extend" in body:
            views.append(v)
    out.append((len(views) == 5, "luci-views", f"{len(views)} of 5 pages served"))
    menu = json.loads(get("/cgi-bin/luci/admin/menu", sid)[1])
    kids = menu.get("children", {}).get("admin", {}).get("children", {}).get("aigw", {}).get("children", {})
    out.append((len(kids) == 5, "luci-menu", "AIGW menu: " + ", ".join(sorted(kids))))
    return out


class ACS:
    """The host side of tests/cwmp/acs.py (serving in DIR): queue jobs, read what the CPE sent."""

    def __init__(self, d, vm):
        self.d, self.vm = d, vm
        for sub in ("jobs", "done"):
            os.makedirs(os.path.join(d, sub), exist_ok=True)

    def _lines(self, name):
        try:
            with open(os.path.join(self.d, name)) as f:
                return [json.loads(x) for x in f.read().splitlines() if x.strip()]
        except FileNotFoundError:
            return []

    def informs(self):
        return self._lines("informs.jsonl")

    def transfers(self):
        return self._lines("transfers.jsonl")

    def wait(self, pred, timeout):
        """Wait for pred() to be true, reading the console meanwhile (a full pipe would stop QEMU)."""
        end = time.time() + timeout
        while time.time() < end:
            r = pred()
            if r:
                return r
            self.vm.read(1.0)
            self.vm.buf = self.vm.buf[-65536:]
        return None

    def job(self, rpcs, timeout):
        """Queue a job for the next session (the CPE's periodic Inform); its results, or None."""
        name = "%d.json" % int(time.time() * 1000)
        tmp = os.path.join(self.d, "jobs", name + ".tmp")
        with open(tmp, "w") as f:
            json.dump({"rpcs": rpcs}, f)
        os.rename(tmp, os.path.join(self.d, "jobs", name))
        done = os.path.join(self.d, "done", name)
        if not self.wait(lambda: os.path.exists(done), timeout):
            return None
        with open(done) as f:
            return json.load(f)["results"]


def main():
    args = sys.argv[1:]
    sep = args.index("--")
    log, pkgdir, port, luci_port = args[0], args[1], args[2], args[3]
    keep = "--keep" in args[4:sep]
    add_arch = next((a.split("=", 1)[1] for a in args[4:sep] if a.startswith("--add-arch=")), "")
    # --acs=DIR:PORT: tests/cwmp/acs.py serves DIR on the host's PORT (10.0.2.2 inside)
    acs_arg = next((a.split("=", 1)[1] for a in args[4:sep] if a.startswith("--acs=")), "")
    # --upgrade=FILE: a sysupgrade image the ACS serves (DIR/files/FILE) for a real firmware upgrade
    upgrade = next((a.split("=", 1)[1] for a in args[4:sep] if a.startswith("--upgrade=")), "")
    # --add-arch replaces opkg's list: repeat the device's own architectures, then add ours
    opkg_arch = (" $(opkg print-architecture | awk '{print \"--add-arch \" $2 \":\" $3}')"
                 f" --add-arch {add_arch}:200") if add_arch else ""
    qemu = args[sep + 1:]
    vm = VM(qemu, log)
    say = lambda s: print(f"==> {s}", flush=True)  # noqa: E731

    def must(cmd, what, timeout=180):
        rc, _ = vm.run(cmd, timeout)
        if rc:
            say(f"FAILED: {what} (exit {rc}); see {log}")
            vm.p.kill()
            sys.exit(101)

    def bring_up(luci_too):
        """Boot to a shell, the network, AIGW's packages (and LuCI), agd answering."""
        vm.expect(r"Please press Enter to activate this console", 600)
        time.sleep(2)
        vm.send("\n")
        vm.expect(r"root@[^:]+:[^#]*# ", 60)
        vm.run("dmesg -n 1; export TERM=dumb")
        say("waiting for the network (wan via QEMU)")
        # some images (malta) have no wan by default: make the second NIC a DHCP wan
        vm.run("uci -q get network.wan >/dev/null || { uci set network.wan=interface; uci set network.wan.device=eth1;"
               " uci set network.wan.proto=dhcp; uci commit network; /etc/init.d/network reload; }")
        # loops run in a subshell: an "exit" must never end the console's own shell
        must("(i=0; until ping -c1 -W1 10.0.2.2 >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 90 ] && exit 1; sleep 1; done)",
             "network", 120)
        say("fetching packages from the host")
        names = sorted(f for f in os.listdir(pkgdir) if f.endswith((".ipk", ".apk")))
        vm.run("mkdir -p /tmp/p")
        for f in names:
            must(f"wget -q -O /tmp/p/{f} http://10.0.2.2:{port}/{f}", f"download {f}", 120)
        apk = names and names[0].endswith(".apk")
        say("installing with dependencies from the OpenWRT feeds: " + " ".join(names))
        # QEMU's user network now and then drops a download: retry
        retry = "(n=0; until {}; do n=$((n+1)); [ $n -ge 3 ] && exit 1; sleep 5; done)"
        # a wildcard, not the names: the serial console cuts long command lines
        pkgs = "/tmp/p/*.apk" if apk else "/tmp/p/*.ipk"
        luci = "luci-base luci-mod-admin-full luci-theme-bootstrap uhttpd rpcd-mod-luci"
        if apk:
            must(retry.format("apk update"), "apk update", 600)
            must(retry.format(f"apk add --allow-untrusted {pkgs}"), "apk add", 900)
            if luci_too:
                must(retry.format(f"apk add {luci}"), "LuCI", 900)
        else:
            must(retry.format("opkg update"), "opkg update", 600)
            must(retry.format(f"opkg install{opkg_arch} {pkgs}"), "opkg install", 1200)
            if luci_too:
                must(retry.format(f"opkg install{opkg_arch} {luci}"), "LuCI", 1200)
        if luci_too:
            # LuCI reachable from the host (QEMU forwards a port to the wan side): open it in this test VM only
            must("uci set firewall.aigwtest=rule; uci set firewall.aigwtest.src=wan; uci set firewall.aigwtest.proto=tcp;"
                 " uci set firewall.aigwtest.dest_port=80; uci set firewall.aigwtest.target=ACCEPT;"
                 " uci commit firewall; fw4 reload; /etc/init.d/rpcd restart; /etc/init.d/uhttpd restart", "LuCI port", 120)
        say("starting AIGW")
        vm.run("/etc/init.d/aigw enable; /etc/init.d/aigw restart")
        must("(i=0; until agctl status >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 200 ] && exit 1; sleep 1; done)",
             "agd answering", 240)

    def check(good, name, msg):
        print(f"    {'PASS' if good else 'FAIL'} {name}: {msg}", flush=True)
        return 0 if good else 1

    def isp_checks(acs_dir, acs_port):
        """USP and TR-069 on this OpenWRT, and with --upgrade a real firmware upgrade from the ACS."""
        bad = 0
        acs = ACS(acs_dir, vm)
        say("USP: OB-USP-Agent with AIGW's plug-in, asked on the device (its CLI)")
        vm.run("uci set aigw.mgmt.enabled=1; uci set aigw.usp.broker=10.0.2.2; uci set aigw.usp.tls=0;"
               f" uci set aigw.cwmp.url=http://10.0.2.2:{acs_port}/; uci set aigw.cwmp.periodic_interval=30;"
               " uci commit aigw; /etc/init.d/aigw-usp enable; /etc/init.d/aigw-usp restart;"
               " /etc/init.d/aigw-cwmp enable; /etc/init.d/aigw-cwmp restart", 120)
        cli = "obuspa -s /etc/aigw/usp/cli.sock -c"
        rc, out = vm.run(f"(i=0; until {cli} get Device.DeviceInfo.SoftwareVersion 2>/dev/null | grep -q '=>'; do"
                         " i=$((i+1)); [ $i -gt 60 ] && exit 1; sleep 2; done);"
                         f" {cli} get Device.DeviceInfo.SoftwareVersion; {cli} get 'Device.DeviceInfo.FirmwareImage.*.Status';"
                         f" {cli} get Device.X_AIGW_.Mode", 240)
        bad += check(rc == 0 and "FirmwareImage.1.Status => Active" in out and "Device.X_AIGW_.Mode =>" in out,
                     "usp-get", "the agent answers from AIGW's data model: " +
                     " ".join(ln.strip() for ln in out.splitlines() if "=>" in ln)[:200])
        say("TR-069: icwmp with AIGW's data model, an ACS on the host")
        boot = acs.wait(lambda: next((i for i in acs.informs() if {"0 BOOTSTRAP", "1 BOOT"} & set(i["events"])), None), 300)
        dev = boot["device"] if boot else {}
        bad += check(bool(boot) and dev.get("ProductClass") == "AIGW OS gateway" and dev.get("SerialNumber"),
                     "cwmp-inform", f"boot Inform {boot['events'] if boot else 'missing'} {dev}")
        res = acs.job([{"method": "GetParameterValues",
                        "names": ["Device.DeviceInfo.SoftwareVersion", "Device.DeviceInfo.FirmwareImage."]}], 180)
        vals = res[0].get("values", {}) if res else {}
        before = vals.get("Device.DeviceInfo.SoftwareVersion", "")
        bad += check(vals.get("Device.DeviceInfo.FirmwareImage.1.Status") == "Active" and before,
                     "cwmp-gpv", f"SoftwareVersion {before}, image 1 {vals.get('Device.DeviceInfo.FirmwareImage.1.Status')}")
        if not upgrade:
            return bad
        say(f"TR-069: a real firmware upgrade from the ACS ({upgrade}): sysupgrade, reboot, TransferComplete")
        size = os.path.getsize(os.path.join(acs_dir, "files", upgrade))
        n = len(acs.informs())
        res = acs.job([{"method": "Download", "url": f"http://10.0.2.2:{acs_port}/{upgrade}", "key": "qemu-up",
                        "size": size}], 180)
        bad += check(bool(res) and res[0].get("status") in (0, 1), "cwmp-download", f"DownloadResponse {res}")
        # icwmp fetches it, sysupgrade -T, agd's handler puts it in image 2, icwmp has it activated: sysupgrade
        try:
            vm.expect(r"Please press Enter to activate this console", 900)
        except TimeoutError:
            return bad + check(False, "cwmp-upgrade", "the VM did not reboot into the new image")
        say("rebooted into the new image; AIGW again (an image with AIGW built in has it already)")
        vm.buf = b"Please press Enter to activate this console"
        bring_up(False)
        rc, out = vm.run("uci -q get aigw.cwmp.url; ls /etc/icwmpd/", 30)
        bad += check(f"10.0.2.2:{acs_port}" in out, "upgrade-kept-config", "the configuration survived the upgrade")
        vm.run("/etc/init.d/aigw-cwmp restart", 60)
        tc = acs.wait(lambda: next((i for i in acs.informs()[n:] if "7 TRANSFER COMPLETE" in i["events"]), None), 300)
        bad += check(bool(tc) and "1 BOOT" in tc["events"], "upgrade-inform",
                     f"after the upgrade: {tc['events'] if tc else 'no TRANSFER COMPLETE Inform'}")
        t = acs.wait(lambda: next((x for x in acs.transfers() if x["key"] == "qemu-up"), None), 120)
        bad += check(bool(t) and t["fault"] == 0, "upgrade-transfer-complete", f"TransferComplete {t}")
        return bad

    try:
        say("booting OpenWRT")
        bring_up(True)
        say("aigw-selftest --full")
        vm.buf = b""
        vm.n += 1
        tag = f"__AIGW_{vm.n}__"
        vm.send(f"aigw-selftest --full 2>&1 | sed 's/^/|| /'; echo {tag}RC=$?{tag}\n")
        m = vm.expect(rf"{tag}RC=(\d+){tag}", 600)
        text = m.string[:m.start()].decode(errors="replace")
        lines = [ln[3:].rstrip("\r") for ln in text.splitlines() if ln.startswith("|| ")]
        for ln in lines:
            print("    " + ln, flush=True)
        summary = [ln for ln in lines if ln.startswith("selftest:")]
        failed = sum(1 for ln in lines if ln.startswith("FAIL"))
        if not summary:
            say(f"FAILED: the self-test did not finish; see {log}")
            failed = 102
        say("LuCI, as the AIGW pages use it")
        try:
            for good, name, msg in luci_check(f"http://127.0.0.1:{luci_port}"):
                print(f"    {'PASS' if good else 'FAIL'} {name}: {msg}", flush=True)
                failed += 0 if good else 1
        except Exception as e:  # noqa: BLE001 - any failure here is a failed check
            print(f"    FAIL luci: {e}", flush=True)
            failed += 1
        if acs_arg:
            acs_dir, acs_port = acs_arg.rsplit(":", 1)
            failed += isp_checks(acs_dir, acs_port)
        if keep:
            # something to look at, including a hostile host name the pages must show as text
            vm.run("t=$(( $(date +%s) + 86400 )); printf '%s\\n' "
                   "\"$t 02:00:00:00:66:01 192.168.1.66 <svg/onload=alert(1)> *\" "
                   "\"$t 02:00:00:00:66:02 192.168.1.67 Living-room-TV *\" >> /tmp/dhcp.leases; sleep 6;"
                   " ubus send ag.dns '{\"client\":\"192.168.1.67\",\"qname\":\"www.youtube.com\"}';"
                   " ubus send ag.dns '{\"client\":\"192.168.1.66\",\"qname\":\"xkqjzvbwptmr.com\"}';"
                   " agctl remote on 203.0.113.10 >/dev/null; agctl remote add \"Anna's phone\" >/dev/null;"
                   " agctl key add 'Living room plug' iot >/dev/null 2>&1; true", 120)
            say(f"VM kept running: LuCI on http://127.0.0.1:{luci_port} (root, no password). Ctrl+C to stop.")
            try:
                while True:
                    vm.read(5)
                    vm.buf = vm.buf[-4096:]
            except KeyboardInterrupt:
                pass
        vm.send("poweroff\n")
        try:
            vm.p.wait(30)
        except subprocess.TimeoutExpired:
            vm.p.kill()
        sys.exit(min(failed, 99) if failed < 100 else failed)
    except (TimeoutError, EOFError) as e:
        say(f"FAILED: {e}; see {log}")
        vm.p.kill()
        sys.exit(103)


if __name__ == "__main__":
    main()
