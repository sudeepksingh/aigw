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
                        ("ag.plugin", "list"), ("ag.model", "list"), ("ag.remote", "list"), ("ag.wifi", "keys")]:
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


def main():
    args = sys.argv[1:]
    sep = args.index("--")
    log, pkgdir, port, luci_port = args[0], args[1], args[2], args[3]
    keep = "--keep" in args[4:sep]
    qemu = args[sep + 1:]
    vm = VM(qemu, log)
    say = lambda s: print(f"==> {s}", flush=True)  # noqa: E731

    def must(cmd, what, timeout=180):
        rc, _ = vm.run(cmd, timeout)
        if rc:
            say(f"FAILED: {what} (exit {rc}); see {log}")
            vm.p.kill()
            sys.exit(101)

    try:
        say("booting OpenWRT")
        vm.expect(r"Please press Enter to activate this console", 600)
        time.sleep(2)
        vm.send("\n")
        vm.expect(r"root@[^:]+:[^#]*# ", 60)
        vm.run("dmesg -n 1; export TERM=dumb")
        say("waiting for the network (wan via QEMU)")
        # loops run in a subshell: an "exit" must never end the console's own shell
        must("(i=0; until ping -c1 -W1 10.0.2.2 >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 90 ] && exit 1; sleep 1; done)",
             "network", 120)
        say("fetching packages from the host")
        names = sorted(f for f in os.listdir(pkgdir) if f.endswith((".ipk", ".apk")))
        for f in names:
            must(f"wget -q -O /tmp/{f} http://10.0.2.2:{port}/{f}", f"download {f}", 120)
        apk = names and names[0].endswith(".apk")
        say("installing with dependencies from the OpenWRT feeds: " + " ".join(names))
        # QEMU's user network now and then drops a download: retry
        retry = "(n=0; until {}; do n=$((n+1)); [ $n -ge 3 ] && exit 1; sleep 5; done)"
        pkgs = " ".join(f"/tmp/{f}" for f in names)
        luci = "luci-base luci-mod-admin-full luci-theme-bootstrap uhttpd rpcd-mod-luci"
        if apk:
            must(retry.format("apk update"), "apk update", 600)
            must(retry.format(f"apk add --allow-untrusted {pkgs}"), "apk add", 900)
            must(retry.format(f"apk add {luci}"), "LuCI", 900)
        else:
            must(retry.format("opkg update"), "opkg update", 600)
            must(retry.format(f"opkg install {pkgs}"), "opkg install", 900)
            must(retry.format(f"opkg install {luci}"), "LuCI", 900)
        # LuCI reachable from the host (QEMU forwards a port to the wan side): open it in this test VM only
        must("uci set firewall.aigwtest=rule; uci set firewall.aigwtest.src=wan; uci set firewall.aigwtest.proto=tcp;"
             " uci set firewall.aigwtest.dest_port=80; uci set firewall.aigwtest.target=ACCEPT;"
             " uci commit firewall; fw4 reload; /etc/init.d/rpcd restart; /etc/init.d/uhttpd restart", "LuCI port", 120)
        say("starting AIGW")
        vm.run("/etc/init.d/aigw enable; /etc/init.d/aigw restart")
        must("(i=0; until agctl status >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 60 ] && exit 1; sleep 1; done)",
             "agd answering", 90)
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
        if keep:
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
