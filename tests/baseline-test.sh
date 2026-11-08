#!/bin/sh
# Behaviour-baseline integration test, on a fast clock: one "hour" is one
# second, so a day of history takes 24 seconds.
#
#   client 10.96.0.2 ── host (agd, agdns) ── server 198.18.1.2 (DNS), 198.18.1.3 (upload sink)
#
# Checks: a steady device using a new site -> new-destination; an upload far
# above the usual -> upload-spike; activity in a never-active slot ->
# unusual-hour; the baseline (known sites) survives an agd restart.
# Needs root, ip, dnsmasq, python3, dig.
#
#   sudo tests/baseline-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NC=aigbc$R NS=aigbs$R
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -f "$T/dns.pid" ] && kill "$(cat "$T/dns.pid")" 2>/dev/null || true
	ip netns del "$NC" 2>/dev/null || true
	ip netns del "$NS" 2>/dev/null || true
	if command -v iptables >/dev/null; then
		iptables -D FORWARD -i "abc$R" -j ACCEPT 2>/dev/null || true
		iptables -D FORWARD -i "abs$R" -j ACCEPT 2>/dev/null || true
	fi
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "BASELINE TEST FAIL: $*" >&2; echo "--- logs" >&2; cat "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip dnsmasq python3 dig; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
modprobe nf_conntrack_netlink 2>/dev/null || true

echo "== network"
ip netns add "$NC"
ip netns add "$NS"
ip link add "abc$R" type veth peer name "abc${R}p"
ip link add "abs$R" type veth peer name "abs${R}p"
ip link set "abc${R}p" netns "$NC"
ip link set "abs${R}p" netns "$NS"
ip addr add 10.96.0.1/24 dev "abc$R"; ip link set "abc$R" up
ip addr add 198.18.1.1/24 dev "abs$R"; ip link set "abs$R" up
ip -n "$NC" addr add 10.96.0.2/24 dev "abc${R}p"
ip -n "$NC" link set "abc${R}p" up; ip -n "$NC" link set lo up
ip -n "$NC" route add default via 10.96.0.1
ip -n "$NS" addr add 198.18.1.2/24 dev "abs${R}p"
ip -n "$NS" addr add 198.18.1.3/24 dev "abs${R}p"
ip -n "$NS" link set "abs${R}p" up; ip -n "$NS" link set lo up
ip -n "$NS" route add default via 198.18.1.1
sysctl -qw net.ipv4.ip_forward=1
if command -v iptables >/dev/null; then
	iptables -I FORWARD -i "abc$R" -j ACCEPT
	iptables -I FORWARD -i "abs$R" -j ACCEPT
fi

ip netns exec "$NS" dnsmasq --conf-file=/dev/null --no-resolv --no-hosts --bind-interfaces \
	--listen-address=198.18.1.2 --address=/#/198.18.1.2 --pid-file="$T/dns.pid"
cat > "$T/sink.py" <<'EOF'
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9000))
s.listen(5)
while True:
    c, _ = s.accept()
    while c.recv(65536):
        pass
    c.close()
EOF
ip netns exec "$NS" python3 "$T/sink.py" > "$T/sink.log" 2>&1 & PIDS="$PIDS $!"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '10.96.0.1'
	option port '15354'
	option upstream '198.18.1.2'

config flow 'flow'
	option warmup '0'
	option poll '1'

config baseline 'baseline'
	option bucket_seconds '1'
	option learn_minutes '0'
	option min_known '2'
	option novelty_max '1000000000'
	option spike_min_mb '0.2'
	option unusual_min_days '1'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	# this test is about alerts; acting has its own test (act-test.sh)
	option mode 'off'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config lan 'lan'
	# only the test client: a live AIGW's clients on this host stay out of the test
	list iface 'abc$R'
EOF
echo "1999999999 02:00:00:00:96:02 10.96.0.2 smart-plug *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 2

agctl() { "$BIN/agctl/agctl" "$@"; }
lookup() { ip netns exec "$NC" dig +short +time=2 +tries=2 @10.96.0.1 -p 15354 "$1" A > /dev/null; }
has_alert() { agctl -j alerts 100 | python3 -c "
import json, sys
a = json.load(sys.stdin)['alerts']
sys.exit(0 if any(x['kind'] == '$1' and '$2' in x['subject'] for x in a) else 1)"; }

echo "== learning: the plug uses its two usual sites"
lookup api.plugvendor.com
lookup time.plugvendor-ntp.com
sleep 1
has_alert new-destination plugvendor && fail "alert while still learning"

echo "== steady device uses a new site"
lookup collector.unknown-endpoint.net
sleep 1
has_alert new-destination unknown-endpoint.net || fail "no new-destination alert"
lookup api.plugvendor.com
sleep 1
has_alert new-destination plugvendor.com && fail "alert for a site the device already knows"

echo "== a day of quiet history (25 one-second buckets)"
sleep 26

echo "== sudden upload, in a slot where the device was never active"
ip netns exec "$NC" python3 -c "
import socket
s = socket.create_connection(('198.18.1.3', 9000))
s.sendall(b'x' * 450000)
s.close()"
sleep 4
agctl alerts 10
has_alert upload-spike upload || fail "no upload-spike alert"
has_alert unusual-hour slot || fail "no unusual-hour alert"
agctl device smart-plug | grep -E 'baseline|active hours' || true

echo "== the baseline survives an agd restart"
kill "$AGD"; sleep 1
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
sleep 2
n=$(agctl -j alerts 100 | python3 -c "import json,sys; print(sum(1 for a in json.load(sys.stdin)['alerts'] if a['kind']=='new-destination'))")
lookup api.plugvendor.com        # known before the restart: no alert
lookup cdn.another-new-site.org  # new: alert
sleep 1
agctl -j alerts 100 | python3 -c "
import json, sys
a = [x for x in json.load(sys.stdin)['alerts'] if x['kind'] == 'new-destination']
subjects = [x['subject'] for x in a]
print('   new-destination alerts:', subjects)
ok = 'another-new-site.org' in subjects and subjects.count('plugvendor.com') == 0 and len(a) == $n + 1
sys.exit(0 if ok else 1)" || fail "baseline not restored correctly"

echo "BASELINE TEST OK"
