#!/bin/sh
# Calls and games first, end to end, on real links. Three network namespaces:
#
#   lan: laptop 10.198.1.2, tv 10.198.1.3
#     └─ cfl ── gateway (agd, conntrack, smart queueing on its WAN cfw) ── cfw ─┐
#                                                         internet 10.198.0.2 ─┘
#
# The TV asks the internet for a 10 Mbit/s stream (a line sold at 8 Mbit/s:
# full). The laptop holds a "call": 50 small UDP packets a second each way,
# to a STUN/TURN port (3478), echoed back by the internet.
#
#   first     smart queueing on (agctl sqm --on: cake, else htb with fq_codel,
#             the download through ifb): the call is noticed, its far end
#             goes first (a flower filter to cake's voice tin or htb's
#             priority class, where its packets are counted)
#   proved    the call ends: smooth while the line was full, it went first; on
#             the timeline, agctl calls (with names), the data model and
#             CallReport! (without them); the filters gone with the call
#   no SQM    smart queueing off and bufferbloat (netem 200 ms): the next call
#             is noticed, nothing is shaped for it, and it is rough
#   share 0   the ISP sees nothing more
#
# Needs root, python3, ip, tc with netns, veth, netem, ifb, flower, skbedit
# and cake or htb with fq_codel, and nft or iptables (conntrack); skipped
# without them (a failure with AIGW_REQUIRE_CALLS=1, as in CI).
#
#   sudo tests/calls-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NL=aigwcfl$R NG=aigwcfg$R NI=aigwcfi$R
WAN=cfw$R IFB=ifb4cfw$R
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	for n in "$NL" "$NG" "$NI"; do
		ip netns pids "$n" 2>/dev/null | xargs -r kill 2>/dev/null || true
		ip netns del "$n" 2>/dev/null || true
	done
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() {
	echo "CALLS TEST FAIL: $*" >&2
	echo "--- logs" >&2
	grep -E "calls|sqm|decision|why" "$T/agd.log" | tail -n 40 >&2 || true
	tail -n 5 "$T/inet.log" "$T/tv.log" "$T/call.log" >&2 2>/dev/null || true
	g cat /proc/net/nf_conntrack 2>/dev/null | grep -E 'udp' | head -n 8 >&2 || true
	ip netns exec "$NG" tc -s qdisc show >&2 2>/dev/null || true
	ip netns exec "$NG" tc filter show dev "$IFB" parent 1: >&2 2>/dev/null || true
	exit 1
}
skip() {
	[ "${AIGW_REQUIRE_CALLS:-0}" = 1 ] && fail "required, but $*"
	echo "CALLS TEST SKIPPED: $*"
	exit 0
}
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v tc >/dev/null && command -v ip >/dev/null || skip "no ip or tc"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)
modprobe nf_conntrack_netlink 2>/dev/null || true
for m in ifb sch_netem cls_flower act_skbedit sch_cake; do modprobe "$m" 2>/dev/null || true; done
g() { ip netns exec "$NG" "$@"; }

echo "== the home, the gateway and the internet in namespaces"
for n in "$NL" "$NG" "$NI"; do ip netns add "$n" 2>/dev/null || skip "no network namespaces"; done
ip link add "cfl$R" netns "$NG" type veth peer name "cflp$R" netns "$NL" 2>/dev/null || skip "no veth"
ip link add "$WAN" netns "$NG" type veth peer name "cfwp$R" netns "$NI"
g ip addr add 10.198.1.1/24 dev "cfl$R"
g ip addr add 10.198.0.1/24 dev "$WAN"
g ip link set "cfl$R" up; g ip link set "$WAN" up; g ip link set lo up
g ip route add default via 10.198.0.2
g sysctl -qw net.ipv4.ip_forward=1
ip -n "$NL" addr add 10.198.1.2/24 dev "cflp$R"
ip -n "$NL" addr add 10.198.1.3/24 dev "cflp$R"
ip -n "$NL" link set "cflp$R" up; ip -n "$NL" link set lo up
ip -n "$NL" route add default via 10.198.1.1
ip -n "$NI" addr add 10.198.0.2/24 dev "cfwp$R"
ip -n "$NI" link set "cfwp$R" up; ip -n "$NI" link set lo up
ip -n "$NI" route add 10.198.1.0/24 via 10.198.0.1
# connection tracking in the gateway's namespace (it starts with a rule that uses it)
if command -v nft >/dev/null && g nft -f - 2>/dev/null <<'EOF'
table inet cft {
	chain fwd {
		type filter hook forward priority 0; policy accept;
		ct state established,related,new accept
	}
}
EOF
then :
elif command -v iptables >/dev/null && g iptables -A FORWARD -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT 2>/dev/null; then :
else skip "neither nft nor iptables for conntrack"; fi
# byte counts for connections that start before agd (it turns them on too, for later ones)
g sh -c 'echo 1 > /proc/sys/net/netfilter/nf_conntrack_acct' 2>/dev/null || true
ip netns exec "$NL" ping -c 1 -W 2 -I 10.198.1.2 10.198.0.2 > /dev/null || fail "the internet should answer through the gateway"
[ "$(AIGW_SQM_STATE="$T/sqm" g sh "$REPO/scripts/aigw-sqm" check)" != none ] || skip "no cake, nor htb with fq_codel"
g ip link add aigwifbt type ifb 2>/dev/null || skip "no ifb"
g ip link del aigwifbt
g ip link add aigwflt type dummy && g tc qdisc add dev aigwflt root handle 1: htb &&
	g tc filter add dev aigwflt parent 1: protocol ip prio 7 flower ip_proto udp dst_port 3478 action skbedit priority 1:4 \
	2>/dev/null || { g ip link del aigwflt 2>/dev/null; skip "no flower or skbedit"; }
g ip link del aigwflt

echo "== the internet: an echo for calls (3478) and a stream for whoever asks (5001)"
ip netns exec "$NI" python3 - > "$T/inet.log" 2>&1 <<'EOF' &
import socket, threading, time
echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
echo.bind(("10.198.0.2", 3478))
def reflect():
    while True:
        data, a = echo.recvfrom(2048)
        echo.sendto(data, a)
threading.Thread(target=reflect, daemon=True).start()
bulk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
bulk.bind(("10.198.0.2", 5001))
bulk.setblocking(False)
to, pkt = None, b"x" * 1000
rate = 10e6 / 8 / len(pkt)
t0, n = time.time(), 0
while True:
    try:
        _, to = bulk.recvfrom(64)
    except BlockingIOError:
        pass
    if to is None:
        time.sleep(0.05)
        t0, n = time.time(), 0
        continue
    n += 1
    try:
        bulk.sendto(pkt, to)
    except BlockingIOError:
        time.sleep(0.001)
    ahead = n / rate - (time.time() - t0)
    if ahead > 0:
        time.sleep(ahead)
EOF
PIDS="$PIDS $!"

echo "== the TV asks for the stream (a keep-alive a second): the line full"
ip netns exec "$NL" python3 - > "$T/tv.log" 2>&1 <<'EOF' &
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.198.1.3", 0))
s.setblocking(False)
while True:
    s.sendto(b"more", ("10.198.0.2", 5001))
    t = time.time() + 1
    while time.time() < t:
        try:
            s.recv(2048)
        except BlockingIOError:
            time.sleep(0.002)
EOF
PIDS="$PIDS $!"

call() {   # a call from the laptop until $T/hangup exists
	rm -f "$T/hangup"
	ip netns exec "$NL" python3 - "$T/hangup" > "$T/call.log" 2>&1 <<'EOF' &
import os, socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.198.1.2", 0))
s.setblocking(False)
pkt, t0, n = b"v" * 172, time.time(), 0
while not os.path.exists(sys.argv[1]):
    n += 1
    s.sendto(pkt, ("10.198.0.2", 3478))
    while True:
        try:
            s.recv(2048)
        except BlockingIOError:
            break
    ahead = n / 50.0 - (time.time() - t0)
    if ahead > 0:
        time.sleep(ahead)
EOF
	CALL=$!
	PIDS="$PIDS $CALL"
}
hangup() { touch "$T/hangup"; wait "$CALL" 2>/dev/null || true; }

config() {   # config <calls.share>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15362'

config flow 'flow'
	option warmup '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config diag 'diag'
	option targets '10.198.0.2'
	option name ''

config care 'care'
	option enabled '0'
	option speed_mbps '8'

config why 'why'
	option interval '2'
	option hours '1'
	option wan '$WAN'
	option file '$T/why.bin'

config sqm 'sqm'
	option handler '$T/aigw-sqm'
	option detect '60'

config calls 'calls'
	option poll '1'
	option min_s '6'
	option share '$1'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'

config lan 'lan'
	list iface 'cfl$R'
EOF
}
printf '#!/bin/sh\nAIGW_SQM_STATE=%s/sqm exec sh %s/scripts/aigw-sqm "$@"\n' "$T" "$REPO" > "$T/aigw-sqm"
chmod +x "$T/aigw-sqm"
printf '1999999999 02:00:00:00:c1:02 10.198.1.2 laptop *\n1999999999 02:00:00:00:c1:03 10.198.1.3 tv *\n' > "$T/leases"
config 1
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
dmget() { agctl dm get "$1" | sed 's/^[^=]*= //'; }
wait_for() {   # wait_for <seconds> <what> <shell condition>
	n=$1; what=$2; shift 2
	i=0
	until eval "$*"; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "$what"; sleep 0.5
	done
}
shaped() { g tc qdisc show dev "$WAN" | grep -q 'qdisc ingress' && g tc qdisc show dev "$IFB" 2>/dev/null | grep -q -E 'cake|htb'; }
live() { agctl -j calls | js "l = d.get('live', []); print(l[0]['kind'] + ' ' + str(l[0].get('first')).lower() if l else 'none')"; }
reports() {   # CallReport! events so far, one a line: Kind Verdict Prioritized | Summary
	python3 - "$T/events.log" <<'EOF'
import json, sys
for line in open(sys.argv[1]):
    try:
        e = json.loads(line).get("ag.dm.event", {})
    except ValueError:
        continue
    if e.get("event") == "Device.X_AIGW_.CallReport!":
        a = e["args"]
        print(a["Kind"], a["Verdict"], a["Prioritized"], a["FullLineSeconds"], "|", a["Summary"])
EOF
}
first_packets() {   # packets that went first on the download: htb's class 1:5, or cake's voice tin
	if g tc qdisc show dev "$IFB" | grep -q 'qdisc cake'; then
		# cake's table: the "pkts" row, diffserv4's columns Bulk, Best Effort, Video, Voice
		g tc -s qdisc show dev "$IFB" | awk '$1 == "pkts" && NF == 5 { n = $5 } END { print n + 0 }'
	else
		g tc -s class show dev "$IFB" classid 1:5 | awk '$1 == "Sent" { n = $4 } END { print n + 0 }'
	fi
}

start_agd
grep -q "calls: calls and games first" "$T/agd.log" || fail "calls and games first didn't start"
grep -q "flow sensor on" "$T/agd.log" || fail "the flow sensor didn't start"

echo "== smart queueing on the WAN (asked for: 7.2 Mbit/s of 8)"
wait_for 20 "agctl sqm --on: $(agctl sqm --on 2>&1)" 'agctl sqm --on > /dev/null 2>&1 || [ "$(dmget Device.X_AIGW_.SQM.Status)" = On ]'
wait_for 10 "the WAN shaped: $(g tc qdisc show dev "$WAN")" shaped
g tc qdisc show dev "$IFB" | head -1

echo "== a call while the TV streams: noticed, first in the queue"
call
wait_for 30 "the call noticed and first: $(agctl -j calls)" '[ "$(live)" = "Call true" ]'
agctl calls
wait_for 10 "a filter for the call's far end: $(g tc filter show dev "$IFB" parent 1:)" \
	'g tc filter show dev "$IFB" parent 1: | grep -q "src_port 3478"'
[ "$(dmget Device.X_AIGW_.CallsFirst.Status)" = Prioritized ] || fail "CallsFirst.Status: $(agctl dm get Device.X_AIGW_.CallsFirst.)"
[ "$(dmget Device.X_AIGW_.CallsFirst.ActiveCalls)" = 1 ] || fail "CallsFirst.ActiveCalls"
agctl -j calls | js "sys.exit(0 if d['live'][0]['device'] == 'laptop' and '10.198.0.2:3478' in d['live'][0]['far_ends'] else 1)" ||
	fail "the call on the laptop, to 10.198.0.2:3478: $(agctl -j calls)"
wait_for 10 "the call's packets counted where they go first ($(first_packets))" '[ "$(first_packets)" -gt 100 ]'
echo "   $(first_packets) of the call's packets went first on the download"
sleep 8

echo "== the call ends: smooth while the line was full, it went first"
hangup
wait_for 60 "the report: $(agctl -j calls)" 'agctl -j calls | js "sys.exit(0 if d.get(\"last\") else 1)"'
agctl calls
agctl -j calls | js "
l = d['last']
ok = l['verdict'] == 'Smooth' and l['prioritized'] in (1, True) and l['full_samples'] >= 2 and l['line_kbps'] >= 6400
ok = ok and 'laptop' in l['summary'] and 'went first' in l['summary'] and 'tv downloading' in l['summary']
ok = ok and 'laptop' not in l['isp_summary'] and 'tv ' not in l['isp_summary'].lower()
sys.exit(0 if ok else 1)" || fail "the report: $(agctl -j calls)"
[ "$(dmget Device.X_AIGW_.CallsFirst.Last.Verdict)" = Smooth ] && [ "$(dmget Device.X_AIGW_.CallsFirst.Last.Prioritized)" = true ] &&
	[ "$(dmget Device.X_AIGW_.CallsFirst.Total)" = 1 ] && [ "$(dmget Device.X_AIGW_.CallsFirst.SmoothOnFullLine)" = 1 ] ||
	fail "CallsFirst. after the call: $(agctl dm get Device.X_AIGW_.CallsFirst.)"
dmget Device.X_AIGW_.CallsFirst.Last.Summary | grep -q '^A call stayed smooth' || fail "Last.Summary"
agctl dm get Device.X_AIGW_.CallsFirst. | grep -q -i -E 'laptop|tv down' && fail "a name in the data model"
wait_for 10 "CallReport!: $(cat "$T/events.log")" '[ "$(reports | grep -c "^Call Smooth true")" = 1 ]'
reports
reports | grep -q -i -E 'laptop|\btv\b' && fail "a name in CallReport!"
sleep 1   # a stretch ends before the second it is asked in
agctl -j why --last 1h --raw > "$T/why.json"
python3 -c "
import json, sys
d = json.load(open('$T/why.json'))
ev = [e for e in d.get('events', []) if e.get('kind') == 'call']
st = [s for s in d.get('story', []) if s['text'].startswith('Call on laptop')]
sys.exit(0 if ev and st and 'smooth' in st[0]['text'] else 1)" ||
	fail "the call on the timeline (now $(date +%s)): $(python3 -c "import json; d=json.load(open('$T/why.json')); print(d.get('from'), d.get('to'), d.get('events'), d.get('story'))")"
wait_for 10 "the filters gone with the call: $(g tc filter show dev "$IFB" parent 1:)" \
	'! g tc filter show dev "$IFB" parent 1: | grep -q "src_port 3478"'
[ "$(dmget Device.X_AIGW_.CallsFirst.Status)" = Idle ] || fail "CallsFirst.Status Idle"

echo "== no smart queueing, bufferbloat: the next call is noticed, nothing shaped for it, rough"
agctl sqm --off > /dev/null || fail "agctl sqm --off"
wait_for 10 "the WAN back as it was: $(g tc qdisc show dev "$WAN")" '! shaped'
ip netns exec "$NI" tc qdisc replace dev "cfwp$R" root netem delay 200ms 60ms limit 10000 || skip "no netem"
call
wait_for 30 "the call noticed, not first: $(agctl -j calls)" '[ "$(live)" = "Call false" ]'
agctl calls
[ "$(dmget Device.X_AIGW_.CallsFirst.Status)" = Active ] || fail "CallsFirst.Status Active"
g tc qdisc show dev "$WAN" | grep -q -E 'cake|htb|ingress' && fail "shaped without smart queueing"
g ip link show "$IFB" > /dev/null 2>&1 && fail "an ifb without smart queueing"
sleep 8
hangup
wait_for 60 "the second report: $(agctl -j calls)" '[ "$(dmget Device.X_AIGW_.CallsFirst.Total)" = 2 ]'
agctl calls
agctl -j calls | js "
l = d['last']
sys.exit(0 if l['verdict'] == 'Rough' and l['prioritized'] in (0, False) and 'Smart queueing was off' in l['summary'] else 1)" ||
	fail "rough, not first: $(agctl -j calls)"
wait_for 10 "CallReport! rough: $(reports)" '[ "$(reports | grep -c "^Call Rough false")" = 1 ]'
ip netns exec "$NI" tc qdisc del dev "cfwp$R" root 2>/dev/null || true

echo "== kept over a restart; share 0: the ISP sees nothing more"
stop_agd
config 0
start_agd
[ "$(dmget Device.X_AIGW_.CallsFirst.Shared)" = false ] && [ "$(dmget Device.X_AIGW_.CallsFirst.Status)" = NotShared ] &&
	[ "$(dmget Device.X_AIGW_.CallsFirst.Total)" = 0 ] && [ -z "$(dmget Device.X_AIGW_.CallsFirst.Last.Summary)" ] ||
	fail "share 0: $(agctl dm get Device.X_AIGW_.CallsFirst.)"
agctl -j calls | js "sys.exit(0 if d['stats']['total'] == 2 and 'laptop' in d['last']['summary'] else 1)" ||
	fail "kept at home over a restart: $(agctl -j calls)"
echo "CALLS TEST OK"
