#!/bin/sh
# Flow sensor integration test. A "client" and a "server" live in their own
# network namespaces and this host routes between them, like a gateway:
#
#   client 10.97.0.2 ── host 10.97.0.1 | 198.18.0.1 ── server 198.18.0.2, 198.19.0.3
#                        (agd, agdns)                  (DNS + web server)
#
# Checks: DNS -> flow join (connection labelled with the looked-up name),
# byte counting, "no DNS lookup" detection, and device history surviving an
# agd restart. Needs root, ip, dnsmasq, curl, python3, dig.
#
#   sudo tests/flow-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NC=aigwc$R NS=aigws$R
PIDS=""
TW=/proc/sys/net/netfilter/nf_conntrack_tcp_timeout_time_wait
TW_OLD=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -f "$T/dns.pid" ] && kill "$(cat "$T/dns.pid")" 2>/dev/null || true
	ip netns del "$NC" 2>/dev/null || true
	ip netns del "$NS" 2>/dev/null || true
	if command -v iptables >/dev/null; then
		iptables -D FORWARD -i "agc$R" -j ACCEPT 2>/dev/null || true
		iptables -D FORWARD -i "ags$R" -j ACCEPT 2>/dev/null || true
	fi
	[ -n "$TW_OLD" ] && echo "$TW_OLD" > "$TW" 2>/dev/null || true
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() { echo "FLOW TEST FAIL: $*" >&2; echo "--- logs" >&2; cat "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip dnsmasq curl python3 dig; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
modprobe nf_conntrack_netlink 2>/dev/null || true

echo "== network: client and server namespaces"
ip netns add "$NC"
ip netns add "$NS"
ip link add "agc$R" type veth peer name "agc${R}p"
ip link add "ags$R" type veth peer name "ags${R}p"
ip link set "agc${R}p" netns "$NC"
ip link set "ags${R}p" netns "$NS"
ip addr add 10.97.0.1/24 dev "agc$R"; ip link set "agc$R" up
ip addr add 198.18.0.1/24 dev "ags$R"; ip link set "ags$R" up
ip -n "$NC" addr add 10.97.0.2/24 dev "agc${R}p"
ip -n "$NC" link set "agc${R}p" up; ip -n "$NC" link set lo up
ip -n "$NC" route add default via 10.97.0.1
ip -n "$NS" addr add 198.18.0.2/24 dev "ags${R}p"
ip -n "$NS" addr add 198.19.0.3/24 dev "ags${R}p"
ip -n "$NS" addr add 198.18.0.5/24 dev "ags${R}p"
ip -n "$NS" link set "ags${R}p" up; ip -n "$NS" link set lo up
ip -n "$NS" route add default via 198.18.0.1
ip route add 198.19.0.0/24 via 198.18.0.2   # a second network behind the server
sysctl -qw net.ipv4.ip_forward=1
if command -v iptables >/dev/null; then   # e.g. Docker sets FORWARD to DROP
	iptables -I FORWARD -i "agc$R" -j ACCEPT
	iptables -I FORWARD -i "ags$R" -j ACCEPT
fi
# finished TCP connections leave conntrack after TIME_WAIT; shorten it for the test
if [ -w "$TW" ]; then TW_OLD=$(cat "$TW"); echo 2 > "$TW"; fi

echo "== server: DNS for shop.aigw.test and a web server"
ip netns exec "$NS" dnsmasq --conf-file=/dev/null --no-resolv --no-hosts --bind-interfaces \
	--listen-address=198.18.0.2 --address=/shop.aigw.test/198.18.0.2 \
	--address=/xkqjzvbwptmr.com/198.18.0.4 --pid-file="$T/dns.pid"
mkdir -p "$T/www"
head -c 300000 /dev/urandom > "$T/www/file"
ip netns exec "$NS" python3 -m http.server 8080 --bind 0.0.0.0 --directory "$T/www" \
	> "$T/web.log" 2>&1 & PIDS="$PIDS $!"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '10.97.0.1'
	option port '15353'
	option upstream '198.18.0.2'

config flow 'flow'
	option warmup '0'
	option poll '1'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
EOF
echo "1999999999 02:00:00:00:97:02 10.97.0.2 test-client *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 2
grep -q "flow sensor on" "$T/agd.log" || fail "flow sensor did not start"

echo "== client looks up shop.aigw.test, then downloads from it"
ans=$(ip netns exec "$NC" dig +short +time=3 +tries=2 @10.97.0.1 -p 15353 shop.aigw.test A)
[ "$ans" = 198.18.0.2 ] || fail "lookup through agdns returned '$ans'"
ip netns exec "$NC" curl -s -o /dev/null http://198.18.0.2:8080/file || fail "download 1"

echo "== client connects to 198.19.0.3 without looking anything up"
ip netns exec "$NC" curl -s -o /dev/null http://198.19.0.3:8080/file || fail "download 2"

echo "== client connects to 198.18.0.5, next to shop.aigw.test, without a lookup"
ip netns exec "$NC" curl -s -o /dev/null http://198.18.0.5:8080/file || fail "download nearby"

echo "== client looks up a machine-generated name, and tries DNS over TLS"
ip netns exec "$NC" dig +short +time=3 +tries=2 @10.97.0.1 -p 15353 xkqjzvbwptmr.com A > /dev/null
ip netns exec "$NC" curl -s --connect-timeout 1 -o /dev/null http://198.19.0.3:853/ || true

sleep 5   # connections close, leave TIME_WAIT, counters refresh

agctl() { "$BIN/agctl/agctl" "$@"; }
agctl device test-client
agctl flows 5
agctl -j device test-client > "$T/dev.json"
agctl -j flows 20 > "$T/flows.json"

python3 - "$T/dev.json" "$T/flows.json" <<'EOF' || fail "flow checks"
import json, sys
dev = json.load(open(sys.argv[1]))
flows = json.load(open(sys.argv[2]))["flows"]
dst = {d["name"]: d["bytes"] for d in dev["top_destinations"]}
problems = []
if dev["flows"] < 2: problems.append("expected >= 2 connections, got %d" % dev["flows"])
if dev["flows_no_dns"] < 1: problems.append("connection to 198.19.0.3 not flagged as no-DNS")
if dev["bytes_down"] < 2 * 300000: problems.append("bytes_down too small: %d" % dev["bytes_down"])
if dst.get("shop.aigw.test", 0) < 300000: problems.append("traffic not attributed to shop.aigw.test: %s" % dst)
if dst.get("198.19.0.3", 0) < 300000: problems.append("traffic to 198.19.0.3 missing: %s" % dst)
if not any(f["domain"] == "shop.aigw.test" and not f["no_dns"] for f in flows): problems.append("no labelled flow")
if not any(f["dst"] == "198.19.0.3" and f["no_dns"] for f in flows): problems.append("no no-DNS flow")
if not any(f["dst"] == "198.18.0.5" and f["domain"] == "shop.aigw.test (nearby)" and not f["no_dns"] for f in flows):
    problems.append("connection next to a looked-up name was not labelled nearby")
for p in problems: print("  -", p)
sys.exit(1 if problems else 0)
EOF

echo "== alerts"
agctl alerts
agctl -j alerts > "$T/alerts.json"
python3 - "$T/alerts.json" <<'EOF' || fail "alert checks"
import json, sys
alerts = json.load(open(sys.argv[1]))["alerts"]
kinds = {(a["kind"], a["subject"]) for a in alerts}
problems = []
if ("dga", "xkqjzvbwptmr.com") not in kinds: problems.append("no DGA alert for xkqjzvbwptmr.com")
if ("no-dns", "198.19.0.3") not in kinds: problems.append("no no-DNS alert for 198.19.0.3")
if ("dns-bypass", "198.19.0.3") not in kinds: problems.append("no DNS-bypass alert for port 853")
if any(a["kind"] == "dga" and a["subject"] == "shop.aigw.test" for a in alerts): problems.append("false DGA alert")
if not all(a["reason"] and a["device"] == "test-client" for a in alerts): problems.append("alert without reason or device")
for p in problems: print("  -", p)
sys.exit(1 if problems else 0)
EOF
agctl score xkqjzvbwptmr.com
agctl score www.wikipedia.org

echo "== device history survives an agd restart"
kill "$AGD"; sleep 1
grep -q test-client "$T/state.json" || fail "state file has no device"
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
sleep 2
agctl -j device test-client > "$T/dev2.json"
python3 - "$T/dev2.json" <<'EOF' || fail "history not restored"
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if d["bytes_down"] >= 600000 and d["flows"] >= 2 else 1)
EOF
agctl -j alerts > "$T/alerts2.json"
grep -q xkqjzvbwptmr "$T/alerts2.json" || fail "alerts not restored"

echo "== after the restart, a cached DNS answer is still recognised"
ip netns exec "$NC" curl -s -o /dev/null http://198.18.0.2:8080/file || fail "download 3"
sleep 4
agctl -j flows 1 > "$T/flows2.json"
python3 - "$T/flows2.json" <<'EOF' || fail "connection after restart lost its name (false no-DNS)"
import json, sys
f = json.load(open(sys.argv[1]))["flows"][0]
print("  ", f["dst"], "->", f["domain"] or "(no name)", "no_dns =", f["no_dns"])
sys.exit(0 if f["domain"] == "shop.aigw.test" and not f["no_dns"] else 1)
EOF

echo "FLOW TEST OK"
