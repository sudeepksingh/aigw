#!/bin/sh
# Self-tuning smart queueing, end to end, on a real link: a network namespace
# plays the internet behind a veth "WAN" (why.wan), a sender fills the line
# (10 Mbit/s of a line sold at 8), and netem makes the internet's answers wait
# 200 ms meanwhile: bufferbloat, as the timeline sees it (a sample every 2 s).
#
#   advise    the bloat is seen: an "sqm" decision proposed; approved, the real
#             aigw-sqm shapes the WAN (cake, else htb with fq_codel; the
#             download through ifb). The delay goes (netem off): kept, with the
#             before and after; restored after agd restarts; agctl sqm --off
#             takes it away
#   enforce   the bloat again, and SQM doesn't help (netem stays): tried by
#             itself, judged no better, undone: the WAN back as it was
#   the ISP   Device.X_AIGW_.SQM. says so at each step
#
# Needs root, python3, ip and tc with netns, veth, netem, ifb, and cake or htb
# with fq_codel; skipped without them.
#
#   sudo tests/sqm-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
NS=aigwsqm$$
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	ip netns pids "$NS" 2>/dev/null | xargs -r kill 2>/dev/null || true
	AIGW_SQM_STATE="$T/sqm" sh "$REPO/scripts/aigw-sqm" remove sqw0 > /dev/null 2>&1 || true
	ip link del sqw0 2>/dev/null || true
	ip netns del "$NS" 2>/dev/null || true
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "SQM TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -E "sqm|decision|bufferbloat" "$T/agd.log" | tail -n 30 >&2 || true; exit 1; }
skip() { echo "SQM TEST SKIPPED: $*"; exit 0; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v tc >/dev/null && command -v ip >/dev/null || skip "no ip or tc"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

echo "== the internet in a namespace, behind the WAN sqw0"
ip netns add "$NS" 2>/dev/null || skip "no network namespaces"
ip link add sqw0 type veth peer name sqw1 2>/dev/null || skip "no veth"
ip link set sqw1 netns "$NS"
ip addr add 10.199.0.1/24 dev sqw0
ip link set sqw0 up
ip netns exec "$NS" ip addr add 10.199.0.2/24 dev sqw1
ip netns exec "$NS" ip link set sqw1 up
ip netns exec "$NS" ip link set lo up
[ "$(AIGW_SQM_STATE="$T/sqm" sh "$REPO/scripts/aigw-sqm" check)" != none ] || skip "no cake, nor htb with fq_codel"
ip link add aigwifbt type ifb 2>/dev/null || skip "no ifb"
ip link del aigwifbt
bloat() { ip netns exec "$NS" tc qdisc replace dev sqw1 root netem delay 200ms limit 10000 || skip "no netem"; }
unbloat() { ip netns exec "$NS" tc qdisc del dev sqw1 root 2>/dev/null || true; }
ping -c 1 -W 2 10.199.0.2 > /dev/null || fail "the namespace should answer"

echo "== a sender fills the line: 10 Mbit/s towards the gateway"
ip netns exec "$NS" python3 - > /dev/null 2>&1 <<'EOF' &
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pkt = b"x" * 1000
rate = 10e6 / 8 / len(pkt)              # packets a second
t0, n = time.time(), 0
while True:
    n += 1
    s.sendto(pkt, ("10.199.0.1", 9))
    ahead = n / rate - (time.time() - t0)
    if ahead > 0:
        time.sleep(ahead)
EOF
PIDS="$PIDS $!"

config() {   # config <act mode> <sqm hold>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15360'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config diag 'diag'
	option targets '10.199.0.2'
	option name ''

config care 'care'
	option enabled '0'
	option speed_mbps '8'

config why 'why'
	option interval '2'
	option hours '1'
	option wan 'sqw0'
	option file '$T/why.bin'

config sqm 'sqm'
	option handler '$T/aigw-sqm'
	option detect '4'
	option trial '6'
	option hold '$2'

config act 'act'
	option mode '$1'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'

config lan 'lan'
	list iface 'lo'
EOF
}
# the real program, with its state in the test's folder
printf '#!/bin/sh\nAIGW_SQM_STATE=%s/sqm exec sh %s/scripts/aigw-sqm "$@"\n' "$T" "$REPO" > "$T/aigw-sqm"
chmod +x "$T/aigw-sqm"
: > "$T/leases"
config advise 86400
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
dmget() { agctl dm get "$1" | sed 's/^[^=]*= //'; }
sqmdec() {   # the sqm decision's id and state: "<id> <state>" (the newest)
	agctl -j decisions 50 | js "
ds = [x for x in d.get('decisions', []) if x.get('action') == 'sqm']
ds.sort(key=lambda x: x['id'])
print('%d %s' % (ds[-1]['id'], ds[-1]['state']) if ds else '0 none')"
}
wait_for() {   # wait_for <seconds> <what> <shell condition>
	n=$1; what=$2; shift 2
	i=0
	until eval "$*"; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "$what"; sleep 0.5
	done
}
shaped() { tc qdisc show dev sqw0 | grep -q 'qdisc ingress' && tc qdisc show dev ifb4sqw0 2>/dev/null | grep -q -E 'cake|htb'; }

echo "== advise: the bloat seen, smart queueing proposed"
bloat
start_agd
wait_for 60 "a proposal: $(agctl -j decisions 10)" '[ "$(sqmdec | cut -d" " -f2)" = proposed ]'
ID=$(sqmdec | cut -d' ' -f1)
[ "$(dmget Device.X_AIGW_.SQM.Status)" = Proposed ] || fail "SQM.Status Proposed"
agctl -j decisions 10 | js "
x = [x for x in d['decisions'] if x['id'] == $ID][0]
sys.exit(0 if x['target'] == 'sqw0:7200:' + x['target'].split(':')[2] and 'Bufferbloat' in x['reason'] else 1)" ||
	fail "the proposal: sqw0 at 7200 kbit/s (90% of 8 Mbit/s), why: $(agctl -j decisions 10)"
shaped && fail "nothing shaped before it is approved"
agctl approve "$ID" > /dev/null || fail "approve $ID"
wait_for 10 "the WAN shaped: $(tc qdisc show dev sqw0)" shaped
tc qdisc show dev ifb4sqw0 | head -1
[ "$(dmget Device.X_AIGW_.SQM.Status)" = Trial ] && [ "$(dmget Device.X_AIGW_.SQM.DownloadKbps)" = 7200 ] ||
	fail "SQM. on trial: $(agctl dm get Device.X_AIGW_.SQM.)"

echo "== the delay goes: kept"
unbloat
wait_for 60 "kept: $(agctl sqm)" '[ "$(sqmdec | cut -d" " -f2)" = kept ]'
agctl sqm
agctl -j sqm | js "
t = d['trial']
ok = d['status'] == 'On' and t['verdict'] == 'kept' and t['before_ms'] >= 150 and t['after_ms'] < 70 and t['before_full'] >= 4
sys.exit(0 if ok else 1)" || fail "kept, with before and after: $(agctl -j sqm)"
shaped || fail "still shaped once kept"
[ "$(dmget Device.X_AIGW_.SQM.Status)" = On ] && [ "$(dmget Device.X_AIGW_.SQM.LatencyBeforeMs)" -ge 150 ] ||
	fail "SQM. kept: $(agctl dm get Device.X_AIGW_.SQM.)"
dmget Device.X_AIGW_.SQM.Summary | grep -q '^kept' || fail "SQM.Summary"

echo "== kept over a restart of agd; agctl sqm --off takes it away"
stop_agd
start_agd
shaped || fail "restored after a restart"
[ "$(sqmdec | cut -d' ' -f2)" = kept ] || fail "still kept"
agctl sqm --off > /dev/null || fail "agctl sqm --off"
wait_for 10 "the WAN back as it was: $(tc qdisc show dev sqw0)" '! shaped'
[ "$(dmget Device.X_AIGW_.SQM.Status)" != On ] || fail "SQM.Status after --off"

echo "== enforce: the bloat again, and SQM doesn't help: tried, undone by itself"
stop_agd
config enforce 0
bloat
start_agd
wait_for 150 "a trial on its own: $(agctl -j decisions 10)" '[ "$(sqmdec | cut -d" " -f2)" = active ]'
wait_for 10 "shaped on its own" shaped
wait_for 60 "tried: $(agctl sqm)" '[ "$(sqmdec | cut -d" " -f2)" = tried ]'
agctl sqm
wait_for 10 "undone: $(tc qdisc show dev sqw0)" '! shaped'
agctl -j sqm | js "sys.exit(0 if d['status'] == 'Tried' and d['trial']['verdict'] == 'tried' and 'still' in d['trial']['summary'] else 1)" ||
	fail "tried: $(agctl -j sqm)"
[ "$(dmget Device.X_AIGW_.SQM.Status)" = Tried ] || fail "SQM.Status Tried"
grep -q "bufferbloat" "$T/agd.log" || fail "the log"
echo "SQM TEST OK"
