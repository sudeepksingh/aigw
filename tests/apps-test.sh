#!/bin/sh
# Per-app experience, end to end, on real links. Three network namespaces:
#
#   lan: laptop 10.198.2.2, tv 10.198.2.3
#     └─ apl ── gateway (agd, conntrack) ── apw ─┐
#              internet 10.198.3.2 (calls), 10.198.3.3 (a stream) ─┘
#
#   noticed   the laptop's video call (UDP 3478, 100 packets a second of 1000 B
#             each way, echoed back) and the TV's stream (TCP 443, about
#             3 Mbit/s) whose server the TV looked up as a Netflix name: the
#             kinds, the family (Netflix), never the name itself
#   froze     the call stops for 6 s and comes back: a bad moment, "froze for
#             about 6 s", and with the home and the line fine, the far end
#   choppy    the internet loses 40%: a choppy call, the cause beyond the home
#   said      agctl apps, the timeline, the help desk's brief (--app calls),
#             the data model and AppMoment! (no names), the scores
#   privacy   the server's name is nowhere in what per-app experience keeps or
#             says; apps.share 0: the ISP sees nothing; apps.names 0: no family
#
# Needs root, python3, ip, tc with netns, veth and netem, and nft or iptables
# (conntrack); skipped without them (a failure with AIGW_REQUIRE_APPS=1, as in CI).
#
#   sudo tests/apps-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" appqoe
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NL=aigwapl$R NG=aigwapg$R NI=aigwapi$R
WAN=apw$R
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
	echo "APPS TEST FAIL: $*" >&2
	echo "--- logs" >&2
	grep -E "apps|why" "$T/agd.log" | tail -n 40 >&2 || true
	tail -n 5 "$T/inet.log" "$T/tv.log" "$T/call.log" >&2 2>/dev/null || true
	"$BIN/agctl/agctl" apps >&2 2>/dev/null || true
	exit 1
}
skip() {
	[ "${AIGW_REQUIRE_APPS:-0}" = 1 ] && fail "required, but $*"
	echo "APPS TEST SKIPPED: $*"
	exit 0
}
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v tc >/dev/null && command -v ip >/dev/null || skip "no ip or tc"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)
modprobe nf_conntrack_netlink 2>/dev/null || true
modprobe sch_netem 2>/dev/null || true
g() { ip netns exec "$NG" "$@"; }

echo "== the home, the gateway and the internet in namespaces"
for n in "$NL" "$NG" "$NI"; do ip netns add "$n" 2>/dev/null || skip "no network namespaces"; done
ip link add "apl$R" netns "$NG" type veth peer name "aplp$R" netns "$NL" 2>/dev/null || skip "no veth"
ip link add "$WAN" netns "$NG" type veth peer name "apwp$R" netns "$NI"
g ip addr add 10.198.2.1/24 dev "apl$R"
g ip addr add 10.198.3.1/24 dev "$WAN"
g ip link set "apl$R" up; g ip link set "$WAN" up; g ip link set lo up
g ip route add default via 10.198.3.2
g sysctl -qw net.ipv4.ip_forward=1
ip -n "$NL" addr add 10.198.2.2/24 dev "aplp$R"
ip -n "$NL" addr add 10.198.2.3/24 dev "aplp$R"
ip -n "$NL" link set "aplp$R" up; ip -n "$NL" link set lo up
ip -n "$NL" route add default via 10.198.2.1
ip -n "$NI" addr add 10.198.3.2/24 dev "apwp$R"
ip -n "$NI" addr add 10.198.3.3/24 dev "apwp$R"
ip -n "$NI" link set "apwp$R" up; ip -n "$NI" link set lo up
ip -n "$NI" route add 10.198.2.0/24 via 10.198.3.1
if command -v nft >/dev/null && g nft -f - 2>/dev/null <<'EOF'
table inet apt {
	chain fwd {
		type filter hook forward priority 0; policy accept;
		ct state established,related,new accept
	}
}
EOF
then :
elif command -v iptables >/dev/null && g iptables -A FORWARD -m conntrack --ctstate NEW,ESTABLISHED -j ACCEPT 2>/dev/null; then :
else skip "neither nft nor iptables for conntrack"; fi
g sh -c 'echo 1 > /proc/sys/net/netfilter/nf_conntrack_acct' 2>/dev/null || true
ip netns exec "$NL" ping -c 1 -W 2 -I 10.198.2.2 10.198.3.2 > /dev/null || fail "the internet should answer through the gateway"
ip netns exec "$NI" tc qdisc add dev "apwp$R" root netem delay 1ms 2>/dev/null || skip "no netem"
ip netns exec "$NI" tc qdisc del dev "apwp$R" root

echo "== the internet: an echo for calls (3478), a stream for whoever connects (443)"
ip netns exec "$NI" python3 - > "$T/inet.log" 2>&1 <<'EOF' &
import socket, threading, time
echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
echo.bind(("10.198.3.2", 3478))
def reflect():
    while True:
        data, a = echo.recvfrom(2048)
        echo.sendto(data, a)
threading.Thread(target=reflect, daemon=True).start()
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("10.198.3.3", 443))
srv.listen(4)
def stream(c):
    chunk, t0, n = b"s" * 16384, time.time(), 0
    rate = 3e6 / 8
    try:
        while True:
            c.sendall(chunk)
            n += len(chunk)
            ahead = n / rate - (time.time() - t0)
            if ahead > 0:
                time.sleep(ahead)
    except OSError:
        pass
while True:
    c, _ = srv.accept()
    threading.Thread(target=stream, args=(c,), daemon=True).start()
EOF
PIDS="$PIDS $!"

config() {   # config <apps.share> <apps.names>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15363'

config flow 'flow'
	option warmup '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config diag 'diag'
	option targets '10.198.3.2'
	option name ''

config care 'care'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option wan '$WAN'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '0'

config calls 'calls'
	option enabled '0'

config apps 'apps'
	option poll '1'
	option min_s '4'
	option share '$1'
	option names '$2'
	option rules '$T/no-such-file'

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
	list iface 'apl$R'
EOF
}
printf '1999999999 02:00:00:00:a1:02 10.198.2.2 laptop *\n1999999999 02:00:00:00:a1:03 10.198.2.3 tv *\n' > "$T/leases"
config 1 1
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
live() {   # the live sessions: "<kind>:<device>:<app>" sorted, one line
	agctl -j apps | js "print(' '.join(sorted(s['kind'] + ':' + s['device'] + ':' + s.get('app', '') for s in d.get('live', []))))"
}
moment() {   # the newest moment's <field> (empty: none)
	agctl -j apps | js "m = d.get('moments', []); print(m[0].get('$1', '') if m else '')"
}
appevents() {   # AppMoment! events: Kind What Cause | Summary
	python3 - "$T/events.log" <<'EOF'
import json, sys
for line in open(sys.argv[1]):
    try:
        e = json.loads(line).get("ag.dm.event", {})
    except ValueError:
        continue
    if e.get("event") == "Device.X_AIGW_.AppMoment!":
        a = e["args"]
        print(a["Kind"], a["What"], a["Cause"], "|", a["Summary"])
EOF
}

call() {   # a video call from the laptop until $T/hangup exists; silent while $T/pause exists
	rm -f "$T/hangup" "$T/pause"
	ip netns exec "$NL" python3 - "$T/hangup" "$T/pause" > "$T/call.log" 2>&1 <<'EOF' &
import os, socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.198.2.2", 0))
s.setblocking(False)
pkt, t0, n = b"v" * 1000, time.time(), 0
while not os.path.exists(sys.argv[1]):
    if os.path.exists(sys.argv[2]):
        time.sleep(0.05)
        t0, n = time.time(), 0
        continue
    n += 1
    s.sendto(pkt, ("10.198.3.2", 3478))
    while True:
        try:
            s.recv(2048)
        except BlockingIOError:
            break
    ahead = n / 100.0 - (time.time() - t0)
    if ahead > 0:
        time.sleep(ahead)
EOF
	CALL=$!
	PIDS="$PIDS $CALL"
}
hangup() { touch "$T/hangup"; wait "$CALL" 2>/dev/null || true; }
tv() {   # the TV's stream: it looked the server up first (a Netflix name)
	"$UBUS" -s "$AIGW_UBUS" send ag.dns \
		'{"client":"10.198.2.3","qname":"secret-show-4711.nflxvideo.net","qtype":1,"rcode":0,"ttl":300,"answers":["10.198.3.3"]}'
	sleep 0.5
	ip netns exec "$NL" python3 - > "$T/tv.log" 2>&1 <<'EOF' &
import socket
s = socket.create_connection(("10.198.3.3", 443), source_address=("10.198.2.3", 0))
while s.recv(65536):
    pass
EOF
	TV=$!
	PIDS="$PIDS $TV"
}

start_agd
grep -q "apps: per-app experience" "$T/agd.log" || fail "per-app experience didn't start"
grep -q "the built-in rules" "$T/agd.log" || fail "no rules file: the built-in rules"
grep -q "flow sensor on" "$T/agd.log" || fail "the flow sensor didn't start"

echo "== a video call on the laptop, a stream on the TV: noticed, the stream's family from its name"
tv
call
wait_for 30 "both sessions: $(live)" '[ "$(live)" = "VideoCall:laptop: VideoStreaming:tv:Netflix" ]'
agctl apps
sleep 8                                   # its usual rate learned, good minutes scored

echo "== the call stops for 6 s and comes back: it froze; nothing wrong in the home or on the line"
touch "$T/pause"; sleep 6; rm -f "$T/pause"
wait_for 30 "the moment: $(agctl -j apps | js "print(d.get('moments'))")" '[ "$(moment what)" = Freeze ]'
agctl apps
[ "$(moment kind)" = VideoCall ] && [ "$(moment device)" = laptop ] && [ "$(moment cause)" = FarEnd ] ||
	fail "the moment: $(agctl -j apps | js "print(d['moments'][0])")"
S=$(moment seconds)
[ "$S" -ge 3 ] && [ "$S" -le 9 ] || fail "froze about 6 s, not $S"
moment summary | grep -q "^The video call on laptop at .* froze for about $S s: nothing in the home or on the line was wrong" ||
	fail "in words: $(moment summary)"
moment isp_summary | grep -q "^A video call at " || fail "for the ISP: $(moment isp_summary)"
moment isp_summary | grep -q -i laptop && fail "a name for the ISP: $(moment isp_summary)"
wait_for 10 "AppMoment!: $(cat "$T/events.log")" '[ "$(appevents | grep -c "^VideoCall Freeze FarEnd")" = 1 ]'
appevents
appevents | grep -q -i -E 'laptop|netflix|\btv\b' && fail "a name in AppMoment!"
[ "$(dmget Device.X_AIGW_.AppExperience.Status)" = Active ] && [ "$(dmget Device.X_AIGW_.AppExperience.BadMoments)" = 1 ] &&
	[ "$(dmget Device.X_AIGW_.AppExperience.MomentNumberOfEntries)" = 1 ] &&
	[ "$(dmget Device.X_AIGW_.AppExperience.Moment.1.Cause)" = FarEnd ] &&
	[ "$(dmget Device.X_AIGW_.AppExperience.Moment.1.What)" = Freeze ] ||
	fail "the data model: $(agctl dm get Device.X_AIGW_.AppExperience.)"
agctl dm get Device.X_AIGW_.AppExperience. | grep -q -i -E 'laptop|netflix|secret' && fail "a name in the data model"
sleep 1   # a stretch ends before the second it is asked in
agctl -j why --last 1h --raw > "$T/why.json"
python3 -c "
import json, sys
d = json.load(open('$T/why.json'))
ev = [e for e in d.get('events', []) if e.get('kind') == 'app' and e['text'].startswith('Video call on laptop froze')]
sys.exit(0 if ev else 1)" || fail "the moment on the timeline: $(python3 -c "import json; print(json.load(open('$T/why.json')).get('events'))")"

echo "== the internet loses 40%: the call is choppy, the cause beyond the home"
ip netns exec "$NI" tc qdisc replace dev "apwp$R" root netem loss 40% delay 30ms
sleep 10
ip netns exec "$NI" tc qdisc del dev "apwp$R" root
wait_for 30 "a second moment: $(agctl -j apps | js "print(d.get('moments'))")" \
	'[ "$(agctl -j apps | js "print(len(d[\"moments\"]))")" -ge 2 ]'
agctl apps
C=$(moment cause)
case "$C" in ISP|Line) ;; *) fail "choppy because of the network beyond the home, not $C: $(moment summary)" ;; esac
[ "$(moment what)" = Stutter ] || [ "$(moment what)" = Freeze ] || fail "choppy or frozen: $(moment what)"
moment mos | grep -q -E '^[1-3]\.' || fail "a low MOS: $(moment mos)"

echo "== the help desk asks about calls: the call's moments, with their causes"
agctl -j brief --no-check --app calls --period "last 1h" > "$T/brief.json" || fail "agctl brief --app calls"
python3 -c "
import json, sys
d = json.load(open('$T/brief.json'))
ev = [e['text'] for e in d.get('evidence', []) if e.get('source') == 'apps']
ok = any(t.startswith('Calls in the period') for t in ev) and any('froze for about' in t for t in ev)
sys.exit(0 if ok else 1)" || fail "the brief: $(cat "$T/brief.json")"
agctl brief --no-check --app calls --period "last 1h" | head -n 20
agctl -j brief --no-check --app nonsense 2>&1 | grep -q "app is calls" || fail "an app that isn't one is refused"

echo "== the scores, the privacy of what is kept"
wait_for 30 "a calls score: $(agctl -j apps | js "print(d['scores'])")" \
	'agctl -j apps | js "s = [x for x in d[\"scores\"] if x[\"group\"] == \"Calls\"][0]; sys.exit(0 if 0 <= s[\"score\"] <= 100 and s[\"bad_minutes\"] >= 2 else 1)"'
agctl -j apps | js "print([(s['group'], s['score'], s['minutes'], s['bad_minutes']) for s in d['scores'] if s['minutes']])"
[ "$(dmget Device.X_AIGW_.AppExperience.KindNumberOfEntries)" = 6 ] && [ "$(dmget Device.X_AIGW_.AppExperience.Kind.1.Name)" = Calls ] ||
	fail "Kind.{i}.: $(agctl dm get Device.X_AIGW_.AppExperience.Kind.)"
agctl -j apps | grep -q secret-show && fail "the server's name in agctl apps"
hangup
kill "$TV" 2>/dev/null || true
stop_agd
python3 -c "
import json, sys
a = json.load(open('$T/state.json')).get('apps')
s = json.dumps(a)
sys.exit(0 if a and a.get('moments') and 'secret' not in s and 'nflxvideo' not in s and '10.198.3' not in s else 1)" ||
	fail "kept with the state, without a server name or address: $(python3 -c "import json; print(json.load(open('$T/state.json')).get('apps'))")"

echo "== kept over a restart; apps.share 0: the ISP sees nothing; apps.names 0: no family"
config 0 0
start_agd
agctl -j apps | js "sys.exit(0 if len(d['moments']) >= 2 and d['moments'][-1]['what'] == 'Freeze' and d['status'] == 'Active' else 1)" ||
	fail "the moments kept at home: $(agctl -j apps)"
[ "$(dmget Device.X_AIGW_.AppExperience.Shared)" = false ] && [ "$(dmget Device.X_AIGW_.AppExperience.Status)" = NotShared ] &&
	[ "$(dmget Device.X_AIGW_.AppExperience.BadMoments)" = 0 ] && [ "$(dmget Device.X_AIGW_.AppExperience.MomentNumberOfEntries)" = 0 ] ||
	fail "share 0: $(agctl dm get Device.X_AIGW_.AppExperience.)"
tv
wait_for 30 "the stream again, without its family: $(live)" '[ "$(live)" = "VideoStreaming:tv:" ]'
agctl apps
echo "APPS TEST OK"
