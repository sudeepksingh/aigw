#!/bin/sh
# Phase 5 remote access test: a real WireGuard tunnel between two network
# namespaces. The gateway namespace runs agd, agdns and aigw-remote; the
# "phone" reaches it over a stand-in for the internet:
#
#   phone 203.0.113.2 ── internet ── 203.0.113.1 gateway (agd, agdns, agwg 10.77.0.1)
#     wg0 10.77.0.2 ════════════ tunnel ════════════╝
#
# Checks: off by default and refuses peers; "remote on" brings up the
# interface, rules and keys (private key mode 600) and saves the setting;
# "remote add" gives a WireGuard config whose private key is not kept; the
# phone pings the gateway and its lookups go through agdns (redirect from
# port 53), appearing in AIGW under the peer's name; the tunnel survives an
# agd restart; revoke cuts the peer off; "remote off" removes everything.
# Needs root, ip, nft, wg (wireguard-tools), dnsmasq, dig, ping, python3, and
# WireGuard in the kernel.
#
#   sudo tests/remote-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=aigwrg$R NC=aigwrc$R
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -n "${AGD:-}" ] && kill "$AGD" 2>/dev/null || true
	for n in "$NC" "$NG"; do ip netns del "$n" 2>/dev/null || true; done
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() { echo "REMOTE TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft wg dnsmasq dig ping python3; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

agctl() { "$BIN/agctl/agctl" "$@"; }
gw() { ip netns exec "$NG" "$@"; }
ph() { ip netns exec "$NC" "$@"; }
jcheck() {
	args=""
	while [ "$1" != "--" ]; do args="$args $1"; shift; done
	shift
	# shellcheck disable=SC2086
	agctl -j $args > "$T/j.json" 2>&1 || true
	python3 - "$T/j.json" "$1" <<'EOF' || { cat "$T/j.json" >&2; fail "$2"; }
import json, sys
j = json.load(open(sys.argv[1]))
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
wait_for() {
	n=$1; shift
	i=0
	while :; do
		if ( jcheck "$@" ) > /dev/null 2>&1; then return 0; fi
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && jcheck "$@"
		sleep 0.5
	done
}

echo "== namespaces: gateway and phone, joined by a stand-in internet"
for n in "$NG" "$NC"; do ip netns add "$n"; ip -n "$n" link set lo up; done
ip link add gi0 netns "$NG" type veth peer name ci0 netns "$NC"
ip -n "$NG" addr add 203.0.113.1/24 dev gi0; ip -n "$NG" link set gi0 up
ip -n "$NC" addr add 203.0.113.2/24 dev ci0; ip -n "$NC" link set ci0 up
gw ip link add probe0 type wireguard 2>/dev/null || fail "no WireGuard in this kernel"
gw ip link del probe0

# upstream DNS for agdns, inside the gateway namespace
gw dnsmasq --conf-file=/dev/null --no-resolv --no-hosts --bind-interfaces --listen-address=127.0.0.1 \
	--port=5399 --address=/remote.test/198.18.0.9 --pid-file="$T/dns.pid" --log-facility="$T/dnsmasq.log"
PIDS="$PIDS $(cat "$T/dns.pid")"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '0.0.0.0'
	option port '15353'
	option upstream '127.0.0.1#5399'

config lan 'lan'
	option iface 'aigw-none0'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config remote 'remote'
	option dir '$T/remote'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true AIGW_REMOTE="$REPO/scripts/aigw-remote"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; sleep 1.5; }
stop_agd() { kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done; AGD=""; }
start_agd
ip netns exec "$NG" "$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5

echo "== off by default"
jcheck remote -- "not j['enabled'] and not j['peers']" "remote access should start off"
agctl remote add phone > "$T/out" 2>&1 && fail "a peer was added while remote access is off"
grep -q "remote on" "$T/out" || { cat "$T/out"; fail "the refusal should say how to turn it on"; }
agctl remote on > "$T/out" 2>&1 && fail "turned on without an endpoint"

echo "== remote on: interface, rules, keys"
agctl remote on 203.0.113.1 > "$T/out" 2>&1 || { cat "$T/out"; fail "remote on"; }
gw ip link show agwg > /dev/null 2>&1 || fail "no agwg interface"
[ "$(gw wg show agwg listen-port)" = 51820 ] || fail "not listening on 51820"
gw nft list table inet ag_remote > /dev/null 2>&1 || fail "no ag_remote table"
[ "$(stat -c '%a' "$T/remote/server.key")" = 600 ] || fail "server.key should be mode 600"
[ "$(stat -c '%a' "$T/remote/wg.conf")" = 600 ] || fail "wg.conf should be mode 600"
grep -q "option enabled '1'" "$T/config/aigw" && grep -q "203.0.113.1" "$T/config/aigw" || fail "setting not saved"
jcheck status -- "j['remote']['enabled']" "status should show remote access on"

echo "== a peer: WireGuard config, private key not kept"
agctl -j remote add phone > "$T/add.json" || { cat "$T/add.json"; fail "remote add"; }
python3 - "$T/add.json" "$T" <<'EOF' || fail "peer config"
import json, sys
j = json.load(open(sys.argv[1]))
assert j["ok"] and j["ip"] == "10.77.0.2", j
c = dict(l.split("=", 1) for l in j["config"].splitlines() if "=" in l)
assert c["Endpoint"] == "203.0.113.1:51820" and c["DNS"] == "10.77.0.1" and c["AllowedIPs"] == "0.0.0.0/0", c
assert c["Address"] == "10.77.0.2/32" and len(c["PrivateKey"]) == 44 and len(c["PublicKey"]) == 44
open(sys.argv[2] + "/phone.key", "w").write(c["PrivateKey"] + "\n")
open(sys.argv[2] + "/server.pub", "w").write(c["PublicKey"] + "\n")
EOF
grep -rq "$(cat "$T/phone.key")" "$T/remote" && fail "the phone's private key was kept on the gateway"
agctl remote add laptop > "$T/qr.out" 2>&1 || { cat "$T/qr.out"; fail "remote add with QR"; }
grep -q "too long" "$T/qr.out" && fail "the config does not fit a QR code"
grep -q "WireGuard app" "$T/qr.out" || fail "no QR instructions"
agctl remote add phone > "$T/out" 2>&1 && fail "the same name was added twice"

echo "== the phone connects: ping and DNS through AIGW"
ph ip link add wg0 type wireguard
ph wg set wg0 private-key "$T/phone.key" peer "$(cat "$T/server.pub")" endpoint 203.0.113.1:51820 \
	allowed-ips 10.77.0.0/24 persistent-keepalive 25
ph ip addr add 10.77.0.2/32 dev wg0
ph ip link set wg0 up
ph ip route add 10.77.0.0/24 dev wg0
ph ping -c 2 -W 2 10.77.0.1 > /dev/null || fail "phone cannot reach the gateway through the tunnel"
ans=$(ph dig +short +time=2 +tries=2 @10.77.0.1 remote.test A)
[ "$ans" = 198.18.0.9 ] || fail "DNS through the tunnel answered '$ans'"
wait_for 10 dns 20 -- "any(e['client'] == '10.77.0.2' and e['qname'] == 'remote.test' for e in j['log'])" \
	"the phone's lookup is not in AIGW's DNS log"
wait_for 25 remote -- "any(p['name'] == 'phone' and p['online'] and p['endpoint'].startswith('203.0.113.2') \
	for p in j['peers'])" "the phone should show as connected"
wait_for 10 devices -- "any(d['ip'] == '10.77.0.2' and d['hostname'] == 'phone' for d in j['devices'])" \
	"the tunnel client should be a device named after its peer"

echo "== the tunnel survives an agd restart"
stop_agd
start_agd
ph ping -c 2 -W 2 10.77.0.1 > /dev/null || fail "tunnel broken after an agd restart"
jcheck remote -- "len(j['peers']) == 2 and j['running']" "peers lost across a restart"

echo "== revoke cuts the peer off"
agctl remote revoke phone > /dev/null || fail "revoke"
ph ping -c 2 -W 2 10.77.0.1 > /dev/null 2>&1 && fail "a revoked peer still gets through"
gw wg show agwg peers | grep -q . || fail "the other peer should remain"

echo "== remote off removes it all"
agctl remote off > /dev/null || fail "remote off"
gw ip link show agwg > /dev/null 2>&1 && fail "agwg still there"
gw nft list table inet ag_remote > /dev/null 2>&1 && fail "ag_remote table still there"
grep -q "option enabled '0'" "$T/config/aigw" || fail "off not saved"

echo "REMOTE TEST OK"
