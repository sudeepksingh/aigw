#!/bin/sh
# Fast path test: the kernel flowtable for devices AIGW isn't acting on.
# The gateway runs in its own network namespace (safe next to a live AIGW):
#
#   client 10.96.0.2 ── gateway 10.96.0.1 | 198.19.0.1 ── server 198.19.0.2
#                       (namespace: agd, nft with the fast path)
#
# Checks:
#   offload    a long download is offloaded once past its first packets, and
#              its bytes are still counted for the device
#   actions    throttling the device sends its offloaded connection back through
#              the rules (and keeps it there); quarantine cuts off a connection
#              that was already offloaded; after undo, connections are offloaded
#              again
#   speed      a bulk download with and without the fast path (printed)
#
# Needs root, ip, nft, curl, python3, and a kernel with nf_flow_table
# (skipped otherwise; AIGW_REQUIRE_FLOWTABLE=1 makes that a failure, as in CI).
#
#   sudo tests/fastpath-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=aigwfg$R NC=aigwfc$R NS=aigwfs$R
LIF=lf$R WIF=wf$R
CMAC=02:00:00:00:96:02
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for n in "$NC" "$NS" "$NG"; do ip netns del "$n" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "FASTPATH TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }
skip() {
	[ "${AIGW_REQUIRE_FLOWTABLE:-0}" = 1 ] && fail "$*"
	echo "FASTPATH TEST SKIP: $*"; exit 0
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft curl python3; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
modprobe nf_flow_table_inet 2>/dev/null || true
modprobe nft_flow_offload 2>/dev/null || true

gw() { ip netns exec "$NG" "$@"; }
cl() { ip netns exec "$NC" "$@"; }
agctl() { "$BIN/agctl/agctl" "$@"; }
st() { agctl -j status | python3 -c "import json,sys; print(json.load(sys.stdin)['fastpath']['$1'])"; }
wait_st() { # wait_st <seconds> <python expression over f (status fastpath)> <message>
	i=0
	while :; do
		agctl -j status > "$T/st.json" 2>/dev/null || true
		if python3 -c "import json,sys; f=json.load(open('$T/st.json'))['fastpath']; sys.exit(0 if $2 else 1)" 2>/dev/null; then
			return 0
		fi
		i=$((i + 1))
		[ $i -ge $(($1 * 2)) ] && { cat "$T/st.json" >&2; fail "$3"; }
		sleep 0.5
	done
}

echo "== network"
for n in "$NG" "$NC" "$NS"; do ip netns add "$n"; ip -n "$n" link set lo up; done
ip link add "$LIF" netns "$NG" type veth peer name c0 netns "$NC"
ip link add "$WIF" netns "$NG" type veth peer name s0 netns "$NS"
ip -n "$NC" link set c0 address $CMAC
ip -n "$NG" addr add 10.96.0.1/24 dev "$LIF"; ip -n "$NG" link set "$LIF" up
ip -n "$NG" addr add 198.19.0.1/24 dev "$WIF"; ip -n "$NG" link set "$WIF" up
ip -n "$NC" addr add 10.96.0.2/24 dev c0; ip -n "$NC" link set c0 up
ip -n "$NC" route add default via 10.96.0.1
ip -n "$NS" addr add 198.19.0.2/24 dev s0; ip -n "$NS" link set s0 up
ip -n "$NS" route add default via 198.19.0.1
gw sysctl -qw net.ipv4.ip_forward=1
gw sysctl -qw net.netfilter.nf_conntrack_acct=1 2>/dev/null || true

gw nft -c -f - > "$T/probe.log" 2>&1 <<EOF || skip "this kernel has no flowtable support (nf_flow_table): $(tr '\n' ' ' < "$T/probe.log")"
table inet probe {
	flowtable f {
		hook ingress priority filter
		devices = { "$LIF", "$WIF" }
		counter
	}
}
EOF

mkdir -p "$T/www"
head -c 4000000 /dev/urandom > "$T/www/long"
head -c 60000000 /dev/zero > "$T/www/bulk"
ip netns exec "$NS" python3 -m http.server 8080 --bind 198.19.0.2 --directory "$T/www" \
	> "$T/web.log" 2>&1 & PIDS="$PIDS $!"

mkdir -p "$T/config"
write_config() {
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option intercept '0'

config flow 'flow'
	option warmup '0'
	option poll '1'
	option sni '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config act 'act'
	option mode 'advise'
	option dns_policy '$T/dns-policy'

config lan 'lan'
	list iface '$LIF'

config fastpath 'fastpath'
	option enabled '$1'
	option ifaces '$LIF $WIF'
EOF
}
write_config 1
echo "1999999999 $CMAC 10.96.0.2 fp-client *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT_FILE="$T/aigw.nft"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
gw "$REPO/scripts/aigw-nft" apply 2> "$T/nft.log" || fail "aigw-nft apply"
gw nft list flowtable inet ag fast > /dev/null || { cat "$T/nft.log"; fail "no flowtable after apply"; }
ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 2
long() { # a download that lasts about 20 s
	cl curl -s -o /dev/null --limit-rate 200k --max-time "${1:-40}" -w '%{size_download}' http://198.19.0.2:8080/long
}

echo "== offload: a long download goes through the flowtable, and is still counted"
long > "$T/long1.out" 2>&1 & L=$!
wait_st 20 "f['offloaded'] >= 1" "the download should be offloaded"
gw nft list chain inet ag fastpath | grep -q "flow add @fast" || fail "fast path rule missing"
if agctl -j status | python3 -c "import json,sys; sys.exit(0 if json.load(sys.stdin)['meter']['active'] else 1)"; then
	sleep 2
	r=$(agctl -j devices | python3 -c "import json,sys; print([d.get('meter', {}).get('rate_down', 0) for d in json.load(sys.stdin)['devices'] if d.get('mac') == '$CMAC'][0])")
	python3 -c "import sys; sys.exit(0 if $r > 0.8e6 else 1)" ||
		fail "the eBPF meter should still see the offloaded download (rate down $r bit/s)"
	echo "   the eBPF meter sees the offloaded download: $((r / 1000)) kbit/s"
fi
wait "$L" || fail "the offloaded download should finish"
[ "$(cat "$T/long1.out")" = 4000000 ] || fail "download incomplete: $(cat "$T/long1.out")"
i=0
while :; do
	down=$(agctl -j devices | python3 -c "import json,sys; print(sum(d.get('bytes_down', 0) for d in json.load(sys.stdin)['devices'] if d.get('mac') == '$CMAC'))" 2>/dev/null || echo 0)
	[ "$down" -ge 3800000 ] && break
	i=$((i + 1)); [ $i -ge 20 ] && fail "the device's bytes should include the offloaded download ($down)"
	sleep 0.5
done
echo "   offloaded, device counted $down bytes down"

echo "== throttle: the offloaded connection goes back through the rules and stays there"
long 60 > "$T/long2.out" 2>&1 & L=$!
wait_st 20 "f['offloaded'] >= 1" "the second download should be offloaded"
r0=$(st released)
agctl throttle fp-client 2mbit 2m > /dev/null || fail "throttle"
wait_st 5 "f['released'] > $r0" "throttling should release the device's offloaded connection"
wait_st 15 "f['offloaded'] == 0" "a throttled device's connection should stay on the slow path"
grep -q "fast path: 1 connection of 10.96.0.2 back through the rules (throttle)" "$T/agd.log" ||
	fail "agd should say why the connection went back"
id=$(agctl -j decisions | python3 -c "import json,sys; print([d['id'] for d in json.load(sys.stdin)['decisions'] if d['action'] == 'throttle' and d['state'] == 'active'][0])")
agctl undo "$id" > /dev/null || fail "undo throttle"
wait_st 15 "f['offloaded'] >= 1" "after undo, the connection should be offloaded again"
kill "$L" 2>/dev/null || true

echo "== quarantine cuts off a connection that was already offloaded"
long 30 > "$T/long3.out" 2>&1 & L=$!
wait_st 20 "f['offloaded'] >= 1" "the third download should be offloaded"
agctl quarantine fp-client 2m > /dev/null || fail "quarantine"
rc=0; wait "$L" || rc=$?
got=$(cat "$T/long3.out" 2>/dev/null || echo 0)
[ "$rc" != 0 ] && [ "${got:-0}" -lt 4000000 ] || fail "the quarantined device's offloaded download went on (rc $rc, $got bytes)"
echo "   cut off after $got bytes"
id=$(agctl -j decisions | python3 -c "import json,sys; print([d['id'] for d in json.load(sys.stdin)['decisions'] if d['action'] == 'quarantine' and d['state'] == 'active'][0])")
agctl undo "$id" > /dev/null || fail "undo quarantine"

echo "== speed: a 60 MB download with and without the fast path"
bulk() { cl curl -s -o /dev/null --max-time 60 -w '%{speed_download}' http://198.19.0.2:8080/bulk | cut -d. -f1; }
on=$(bulk)
write_config 0
gw "$REPO/scripts/aigw-nft" apply 2>> "$T/nft.log" || fail "aigw-nft apply (off)"
gw nft list flowtable inet ag fast > /dev/null 2>&1 && fail "fastpath.enabled 0 should leave no flowtable"
off=$(bulk)
echo "   with the fast path $((on / 1000000)) MB/s, without $((off / 1000000)) MB/s (veth on one host: CPU-bound, not a router's numbers)"

echo "FASTPATH TEST OK"
