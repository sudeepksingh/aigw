#!/bin/sh
# Traffic meter test: the eBPF program agd attaches to the LAN interface
# (tcx) counts each device's traffic in the kernel. The gateway runs in its
# own network namespace (safe next to a live AIGW):
#
#   client 10.95.0.2 ── gateway 10.95.0.1 | 198.20.0.1 ── server 198.20.0.2
#
# Checks: a download at a known speed shows as that rate (down) on the
# device while it runs, an upload as a rate up; the kernel's byte counts
# cover the whole transfer; the rates fall to zero when it ends; traffic is
# untouched (the program only counts); with meter.enabled 0 nothing is
# attached. Needs root, ip, curl, python3, and Linux 6.6+ with eBPF (skipped
# otherwise; AIGW_REQUIRE_METER=1 makes that a failure).
#
#   sudo tests/meter-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=aigwmg$R NC=aigwmc$R NS=aigwms$R
LIF=lm$R WIF=wm$R
CMAC=02:00:00:00:95:02
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for n in "$NC" "$NS" "$NG"; do ip netns del "$n" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "METER TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip curl python3; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

cl() { ip netns exec "$NC" "$@"; }
agctl() { "$BIN/agctl/agctl" "$@"; }
dev() { # dev <python expression over m (the client's meter table)>
	agctl -j devices > "$T/d.json" 2>/dev/null || true
	python3 - "$T/d.json" "$1" <<'EOF'
import json, sys
d = [x for x in json.load(open(sys.argv[1]))["devices"] if x.get("mac") == "02:00:00:00:95:02"]
m = d[0].get("meter", {}) if d else {}
print(eval(sys.argv[2]))
EOF
}

echo "== network"
for n in "$NG" "$NC" "$NS"; do ip netns add "$n"; ip -n "$n" link set lo up; done
ip link add "$LIF" netns "$NG" type veth peer name c0 netns "$NC"
ip link add "$WIF" netns "$NG" type veth peer name s0 netns "$NS"
ip -n "$NC" link set c0 address $CMAC
ip -n "$NG" addr add 10.95.0.1/24 dev "$LIF"; ip -n "$NG" link set "$LIF" up
ip -n "$NG" addr add 198.20.0.1/24 dev "$WIF"; ip -n "$NG" link set "$WIF" up
ip -n "$NC" addr add 10.95.0.2/24 dev c0; ip -n "$NC" link set c0 up
ip -n "$NC" route add default via 10.95.0.1
ip -n "$NS" addr add 198.20.0.2/24 dev s0; ip -n "$NS" link set s0 up
ip -n "$NS" route add default via 198.20.0.1
ip netns exec "$NG" sysctl -qw net.ipv4.ip_forward=1

mkdir -p "$T/www"
head -c 3000000 /dev/urandom > "$T/www/file"
head -c 2000000 /dev/urandom > "$T/up"
cat > "$T/server.py" <<'EOF'
import http.server, sys, os, time
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/paced":
            return super().do_GET()
        # 40 chunks of 64 KB, one every 0.2 s: a steady 320 KB/s (2.62 Mbit/s) for 8 s
        self.send_response(200); self.send_header("Content-Length", str(40 * 65536)); self.end_headers()
        chunk, t0 = os.urandom(65536), time.time()
        for i in range(40):
            self.wfile.write(chunk); self.wfile.flush()
            time.sleep(max(0, t0 + (i + 1) * 0.2 - time.time()))
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        while n > 0:
            n -= len(self.rfile.read(min(n, 65536)))
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    def log_message(self, *a): pass
os.chdir(sys.argv[1])
http.server.ThreadingHTTPServer(("198.20.0.2", 8080), H).serve_forever()
EOF
ip netns exec "$NS" python3 "$T/server.py" "$T/www" > "$T/web.log" 2>&1 & PIDS="$PIDS $!"

mkdir -p "$T/config"
write_config() {
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config act 'act'
	option dns_policy '$T/dns-policy'

config lan 'lan'
	list iface '$LIF'

config meter 'meter'
	option enabled '$1'
EOF
}
write_config 1
echo "1999999999 $CMAC 10.95.0.2 meter-client *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 2; }
start_agd

active=$(agctl -j status | python3 -c "import json,sys; m=json.load(sys.stdin)['meter']; print(int(bool(m['active'])), m.get('why', ''))")
case "$active" in
1*) ;;
*)	[ "${AIGW_REQUIRE_METER:-0}" = 1 ] && fail "meter not active: $active"
	echo "METER TEST SKIP: ${active#0 }"; exit 0 ;;
esac
agctl -j status | python3 -c "import json,sys; assert json.load(sys.stdin)['meter']['ifaces'] == ['$LIF']" ||
	fail "the meter should be on the LAN interface"
cl ping -c 2 -W 1 198.20.0.2 > /dev/null || fail "traffic should pass"

echo "== a download the server paces at 320 KB/s shows as about 2.6 Mbit/s down"
cl curl -s -o /dev/null --max-time 30 -w '%{size_download}' http://198.20.0.2:8080/paced > "$T/dl.out" &
C=$!
sleep 3
r1=$(dev "m.get('rate_down', 0)"); sleep 2; r2=$(dev "m.get('rate_down', 0)")
wait "$C" || fail "the download should finish"
[ "$(cat "$T/dl.out")" = 2621440 ] || fail "download incomplete"
python3 -c "import sys; a = ($r1 + $r2) / 2; sys.exit(0 if 0.8 * 2.62e6 <= a <= 1.3 * 2.62e6 else 1)" ||
	fail "rate down ($r1, $r2 bit/s) should be about 2.62 Mbit/s"
echo "   rate down $(((r1 + r2) / 2000)) kbit/s (paced at 2621)"
cl curl -s -o /dev/null --max-time 30 http://198.20.0.2:8080/file || fail "a plain download"
sleep 2
b=$(dev "m.get('bytes_down', 0)")
[ "$b" -ge 3000000 ] || fail "the kernel's byte count should cover the download ($b)"
r=$(dev "m.get('rate_down', 0)")
[ "$r" -lt 100000 ] || fail "rate down should fall when the download ends ($r)"

echo "== an upload at 300 KB/s shows as about 2.4 Mbit/s up"
cl curl -s -o /dev/null --limit-rate 300k --max-time 30 --data-binary @"$T/up" http://198.20.0.2:8080/ &
C=$!
sleep 4
r=$(dev "m.get('rate_up', 0)")
python3 -c "import sys; sys.exit(0 if 1.6e6 <= $r <= 3.4e6 else 1)" || fail "rate up should be about 2.4 Mbit/s, is $r"
echo "   rate up $((r / 1000)) kbit/s while uploading"
wait "$C" || fail "the upload should finish"
sleep 2
b=$(dev "m.get('bytes_up', 0)")
[ "$b" -ge 2000000 ] || fail "the kernel's byte count should cover the upload ($b)"

echo "== meter.enabled 0: nothing attached"
kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done
write_config 0
start_agd
agctl -j status | python3 -c "import json,sys; m=json.load(sys.stdin)['meter']; assert not m['active'] and 'config' in m['why']" ||
	fail "the meter should be off"
dev "'meter' in str(m) or m == {}" > /dev/null

echo "METER TEST OK"
