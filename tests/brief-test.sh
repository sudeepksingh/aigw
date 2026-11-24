#!/bin/sh
# The help-desk assistant, end to end: agd in a gateway network namespace
# whose WAN (a veth) leads to an "ISP" namespace, a fake hostapd, the timeline
# sampled every 2 s, and the support agent asking.
#
#   all fine       agctl brief: a fresh check first, NONE, no visit
#   a weak laptop  the laptop leaves and comes back at -82 dBm: HOME_WIFI;
#                  at home by name, for the ISP (SupportBrief() over the data
#                  model) as "a laptop", without its name or MAC
#   a full line    the ISP sends 4 Mbit/s down a 2 Mbit/s line: LINE_FULL,
#                  not a fault; smart queueing is the fix, and --propose only
#                  proposes it: a decision that waits, nothing shaped
#   the internet   the internet stops answering (the ISP's side): LINE, now,
#                  the fresh check and the timeline agree, and no technician
#   counts         every brief counted by fault (the fleet report's proof),
#                  kept over a restart
#
# Needs root (network namespaces, a private ubusd, raw ICMP), python3, ip, tc.
#
#   sudo tests/brief-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" brief
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
GW=aigwbrief-gw
ISP=aigwbrief-isp
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	ip netns del "$GW" 2>/dev/null || true
	ip netns del "$ISP" 2>/dev/null || true
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "BRIEF TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v ip >/dev/null || fail "missing ip"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

echo "== the gateway's namespace, its WAN to the ISP's, the internet behind it (10.98.0.1)"
ip netns del "$GW" 2>/dev/null || true
ip netns del "$ISP" 2>/dev/null || true
ip netns add "$GW"
ip netns add "$ISP"
ip link add brfw0 netns "$GW" type veth peer name brfw1 netns "$ISP"
ip -n "$GW" link set lo up
ip -n "$ISP" link set lo up
ip -n "$GW" addr add 10.97.0.1/24 dev brfw0
ip -n "$ISP" addr add 10.97.0.2/24 dev brfw1
ip -n "$ISP" addr add 10.98.0.1/32 dev lo
ip -n "$GW" link set brfw0 up
ip -n "$ISP" link set brfw1 up
ip -n "$GW" route add default via 10.97.0.2

LAPTOP=02:00:00:00:89:02
PHONE=02:00:00:00:89:01
printf '%s -50\n%s -55\n' "$PHONE" "$LAPTOP" > "$T/stations"

echo "== a fake hostapd: the devices in $T/stations (MAC, signal) are connected"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, socket, sys, time
d, stations = sys.argv[1:3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(os.path.join(d, "wlan0"))
since = {}
def present():
    p = dict(l.split() for l in open(stations) if l.strip())
    for m in list(since):
        if m not in p:
            del since[m]
    for m in p:
        since.setdefault(m, time.time())
    return p
def sta(m, sig):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%s\nconnected_time=%d\ntx_rate_info=1440\n" % (
        m, sig, int(time.time() - since[m]))
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    w = cmd.split()
    if cmd == "STATUS":
        rep = "state=ENABLED\nfreq=5180\nchannel=36\nbssid[0]=02:aa:00:00:00:06\nssid[0]=AIGW-TEST\n"
    elif cmd == "STA-FIRST":
        p = present(); k = sorted(p); rep = sta(k[0], p[k[0]]) if k else ""
    elif w[0] == "STA-NEXT":
        p = present(); k = sorted(p); i = k.index(w[1]) + 1 if w[1] in k else len(k)
        rep = sta(k[i], p[k[i]]) if i < len(k) else ""
    else:
        rep = "UNKNOWN COMMAND\n"
    if addr:
        s.sendto(rep.encode(), addr)
EOF
PIDS="$PIDS $!"

echo "== the gateway's own DNS (where Diagnose asks): a stand-in that answers every name"
ip netns exec "$GW" python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 15373))
while True:
    q, a = s.recvfrom(1500)
    i = 12
    while i < len(q) and q[i]:
        i += q[i] + 1
    qe = i + 5
    s.sendto(q[:2] + b'\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00' + q[12:qe] +
             b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04' + bytes([10, 98, 0, 1]), a)
" > "$T/dns.log" 2>&1 &
PIDS="$PIDS $!"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15373'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'

config diag 'diag'
	option targets '10.98.0.1'
	option name 'example.com'

config care 'care'
	option enabled '0'
	option speed_mbps '2'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '1'
	option handler '$T/aigw-sqm'

config act 'act'
	option mode 'advise'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '0'

config brief 'brief'
	option enabled '1'
	option names '0'

config lan 'lan'
	list iface 'lo'
EOF
printf '1999999999 %s 10.96.0.31 dads-phone *\n1999999999 %s 10.96.0.32 sara-laptop *\n' "$PHONE" "$LAPTOP" > "$T/leases"
# smart queueing's program: says htb is there, and writes down anything else it is asked
printf '#!/bin/sh\n[ "$1" = check ] && { echo htb; exit 0; }\necho "$*" >> %s/sqm.calls\n' "$T" > "$T/aigw-sqm"
chmod +x "$T/aigw-sqm"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { ip netns exec "$GW" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
brief() { agctl -j brief "$@"; }
isp() {     # isp <origin> <args json>: SupportBrief() as a management system asks it
	"$UBUS" -s "$AIGW_UBUS" -t 60 call ag.dm operate "{\"command\":\"Device.X_AIGW_.SupportBrief()\",\"args\":$2,\"origin\":\"$1\"}"
}
wait_why() {   # wait up to $1 s until a condition on `agctl -j why $3...` (d) holds
	n=$1; c=$2; shift 2
	i=0
	until agctl -j why "$@" | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "agctl why $*: $(agctl why "$@")"; sleep 0.5
	done
}

echo "== all fine: the brief checks the home first"
wait_why 20 "d['recorded'] >= 3 and d['code'] == 'OK'" --last 1m
agctl brief
brief | js "
ok = d['fault'] == 'NONE' and d['code'] == 'OK' and d['checked'] in (1, True) and d['visit'] == 'No'
ok = ok and any(e['source'] == 'now' for e in d['evidence']) and any(e['source'] == 'timeline' for e in d['evidence'])
sys.exit(0 if ok else 1)" || fail "all fine: $(agctl brief)"
brief --no-check | js "sys.exit(0 if d['checked'] in (0, False) and d['fault'] == 'NONE' else 1)" ||
	fail "--no-check: $(agctl brief --no-check)"
brief --period "next week" | js "sys.exit(0 if 'a period is' in d.get('error', '') else 1)" || fail "a period that isn't one"
brief nosuchdevice | js "sys.exit(0 if 'no device' in d.get('error', '') else 1)" || fail "an unknown device"

echo "== the laptop leaves Wi-Fi and comes back with a weak signal (-82 dBm)"
printf '%s -50\n' "$PHONE" > "$T/stations"
wait_why 15 "any('sara-laptop left Wi-Fi' in s['text'] for s in d['story'])" sara-laptop --last 2m
printf '%s -50\n%s -82\n' "$PHONE" "$LAPTOP" > "$T/stations"
wait_why 20 "any(f['code'] == 'WIFI_COVERAGE' for f in d['findings'])" sara-laptop --last 2m
sleep 1
agctl brief sara-laptop last 2m
brief sara-laptop last 2m | js "
ok = d['fault'] == 'HOME_WIFI' and d['code'] == 'WIFI_COVERAGE' and d['visit'] == 'No' and d['names'] in (1, True)
ok = ok and 'sara-laptop' in d['verdict'] + ' '.join(e['text'] for e in d['evidence'])
sys.exit(0 if ok else 1)" || fail "the laptop, at home: $(agctl brief sara-laptop last 2m)"
isp usp '{"Host":"sara-laptop","Period":"last 2m"}' > "$T/isp.json"
python3 - "$T/isp.json" "$LAPTOP" <<'EOF' || fail "the laptop, for the ISP: $(cat "$T/isp.json")"
import json, sys
d = json.load(open(sys.argv[1]))
o = d["out"]
text = json.dumps(o)
ok = d["ok"] and o["Fault"] == "HOME_WIFI" and o["Code"] == "WIFI_COVERAGE" and o["Visit"] == "No"
ok = ok and "a laptop" in o["Verdict"] + o["Evidence"] and "sara-laptop" not in text and sys.argv[2] not in text
ok = ok and "10.96.0.32" not in text and "dads-phone" not in text and o["Checked"] == "true" and o["Start"].endswith("Z")
sys.exit(0 if ok else 1)
EOF
python3 -c "import json; o=json.load(open('$T/isp.json'))['out']; print('   for the ISP: ' + o['Verdict'])"

echo "== the ISP fills the 2 Mbit/s line with 4 Mbit/s for 16 s"
ip netns exec "$ISP" python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
p = b'x' * 1000
end = time.time() + 16
while time.time() < end:
    t = time.time()
    for _ in range(50):
        s.sendto(p, ('10.97.0.1', 9))
    time.sleep(max(0, 0.1 - (time.time() - t)))
" &
FLOOD=$!
PIDS="$PIDS $FLOOD"
wait_why 30 "d['code'] == 'LINE_FULL'" --last 30s
wait "$FLOOD" 2>/dev/null || true
sleep 1
agctl brief last 1m
brief last 1m | js "
f = d['fix']
ok = d['fault'] == 'NONE' and d['code'] == 'LINE_FULL' and d['visit'] == 'No' and f['possible'] in (1, True)
ok = ok and f['action'] in ('sqm', 'approve') and 'smart queueing' in f['text'].lower()
sys.exit(0 if ok else 1)" || fail "a full line: $(agctl brief last 1m)"
brief --no-check --propose last 1m > "$T/p.json"
js "f = d['fix']; sys.exit(0 if f.get('decision') and f['proposed'] in (1, True, 0, False) else 1)" < "$T/p.json" ||
	fail "--propose: $(cat "$T/p.json")"
DEC=$(js "print(d['fix']['decision'])" < "$T/p.json")
agctl -j decisions | js "
x = [k for k in d['decisions'] if k['id'] == $DEC]
sys.exit(0 if x and x[0]['action'] == 'sqm' and x[0]['state'] == 'proposed' else 1)" ||
	fail "the fix must wait for a yes (decision $DEC): $(agctl decisions)"
grep -q apply "$T/sqm.calls" 2>/dev/null && fail "nothing may be shaped before a yes: $(cat "$T/sqm.calls")"
echo "   smart queueing proposed as decision $DEC: it waits for a yes, nothing shaped"
brief --no-check --propose last 1m | js "sys.exit(0 if d['fix']['decision'] == $DEC else 1)" ||
	fail "asking again proposes nothing new"

echo "== the internet stops answering (the ISP's side)"
ip -n "$ISP" addr del 10.98.0.1/32 dev lo
wait_why 30 "d['code'] in ('ISP_DOWN', 'LINE_DOWN')" --last 12s
sleep 1
agctl brief last 12s
brief "last 12s" | js "
ok = d['fault'] == 'LINE' and d['code'] in ('ISP_DOWN', 'LINE_DOWN') and d['current'] in (1, True)
ok = ok and d['confidence'] == 'high' and d['visit'] in ('No', 'IfPersists')
ok = ok and (d['visit'] == 'No') == (d['code'] == 'ISP_DOWN')
sys.exit(0 if ok else 1)" || fail "the internet down: $(agctl brief last 12s)"
isp acs '{"Period":"last 12s"}' > "$T/acs.json"
js "o = d['out']; sys.exit(0 if d['ok'] and o['Fault'] == 'LINE' and o['Visit'] != 'Yes' and o['RemoteFix'] == 'false' else 1)" \
	< "$T/acs.json" || fail "the internet down, for the ACS: $(cat "$T/acs.json")"
isp usp '{"Period":"next week"}' | js "sys.exit(0 if not d['ok'] and d['code'] == 'invalid_arguments' else 1)" ||
	fail "SupportBrief() with a period that isn't one"

echo "== every brief counted by fault, kept over a restart"
stop_agd
python3 - "$T/state.json" <<'EOF' || fail "the counts: $(python3 -c "import json; print(json.load(open('$T/state.json')).get('brief'))")"
import json, sys
b = json.load(open(sys.argv[1]))["brief"]
f = b["fault"]
ok = b["total"] >= 9 and f["NONE"] >= 3 and f["HOME_WIFI"] >= 2 and f["LINE"] >= 2 and b["remote_fix"] >= 2 and b["proposed"] >= 1
print("   %d briefs: %s" % (b["total"], ", ".join("%s %d" % (k, v) for k, v in f.items() if v)))
sys.exit(0 if ok else 1)
EOF
start_agd
brief --no-check > /dev/null
stop_agd
python3 -c "import json,sys; b=json.load(open('$T/state.json'))['brief']; sys.exit(0 if b['total'] >= 10 else 1)" ||
	fail "the counts must survive a restart"
echo "BRIEF TEST OK"
