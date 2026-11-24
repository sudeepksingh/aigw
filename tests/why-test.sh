#!/bin/sh
# "Why was it slow?", end to end: agd keeps the home's timeline (a sample
# every 2 s here instead of every minute) with a fake hostapd, and explains it
# afterwards.
#
#   the line      each sample sends echo requests to the internet (127.0.0.1
#                 here): answered, and kept
#   a device      the laptop leaves Wi-Fi and comes back with a weak signal:
#                 asked about it, the answer is its weak signal, and the story
#                 says what happened, in order
#   kept          agd restarts: the timeline is still there (why.file), with
#                 the restart in it
#   no internet   the internet stops answering (diag.targets unreachable): the
#                 stretch is explained as the internet down
#   the ISP       the same over the data model: Device.X_AIGW_.WhySlow() and
#                 Device.X_AIGW_.Timeline.
#
# Needs root (private ubusd, raw ICMP) and python3.
#
#   sudo tests/why-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" why
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "WHY TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
grep -q '^[^ ]*[[:space:]]00000000' /proc/net/route || { echo "WHY TEST SKIPPED: no default route"; exit 0; }
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

PHONE=02:00:00:00:88:01
LAPTOP=02:00:00:00:88:02
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
        rep = "state=ENABLED\nfreq=5180\nchannel=36\nbssid[0]=02:aa:00:00:00:05\nssid[0]=AIGW-TEST\n"
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

config() {   # config <internet target>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15358'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'

config diag 'diag'
	option targets '$1'
	option name ''

config care 'care'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

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
	list iface 'lo'
EOF
}
config 127.0.0.1
printf '1999999999 %s 10.96.0.21 phone *\n1999999999 %s 10.96.0.22 laptop *\n' "$PHONE" "$LAPTOP" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
why() { agctl -j why "$@"; }
# wait up to $1 s until a condition on `agctl -j why $3...` (d) holds
wait_why() {
	n=$1; c=$2; shift 2
	i=0
	until why "$@" | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "agctl why $*: $(why "$@")"; sleep 0.5
	done
}

echo "== samples: the line answers (echo requests to 127.0.0.1, every 2 s)"
wait_why 15 "d['recorded'] >= 3 and any(s['in_sent'] == 3 and s['in_lost'] == 0 and 'in_ms' in s for s in d['samples'])" \
	--last 1m --raw
why --last 1m | js "sys.exit(0 if d['code'] in ('OK', 'LINE_FULL') else 1)" ||
	fail "an ordinary minute should be fine: $(agctl why --last 1m)"
agctl why --last 1m

echo "== the laptop leaves Wi-Fi and comes back with a weak signal (-82 dBm)"
printf '%s -50\n' "$PHONE" > "$T/stations"
wait_why 15 "any('laptop left Wi-Fi' in s['text'] for s in d['story'])" laptop --last 2m
printf '%s -50\n%s -82\n' "$PHONE" "$LAPTOP" > "$T/stations"
wait_why 20 "any(f['code'] == 'WIFI_COVERAGE' for f in d['findings'])" laptop --last 2m
agctl why laptop --last 2m
why laptop --last 2m | js "
st = [s['text'] for s in d['story']]
need = ['laptop left Wi-Fi', 'laptop joined Wi-Fi on 5 GHz (-82 dBm)', 'signal turned weak']
ok = d['host'] == 'laptop' and all(any(n in s for s in st) for n in need)
ok = ok and [i for n in need for i, s in enumerate(st) if n in s] == sorted(i for n in need for i, s in enumerate(st) if n in s)
ok = ok and d['code'] == 'WIFI_COVERAGE' and '-82 dBm' in d['summary']
sys.exit(0 if ok else 1)" || fail "the laptop's story: $(agctl why laptop --last 2m)"
why --last 2m | js "sys.exit(0 if not any('laptop' in s['text'] for s in d['story']) else 1)" ||
	fail "the home's story leaves one device's comings and goings out: $(agctl why --last 2m)"
why nosuchdevice | js "sys.exit(0 if 'no device' in d.get('error', '') else 1)" || fail "an unknown device"

echo "== kept over a restart of agd"
before=$(agctl dm get Device.X_AIGW_.Timeline.Samples | sed 's/.*= //')
since=$(agctl dm get Device.X_AIGW_.Timeline.RecordedSince | sed 's/.*= //')
stop_agd
[ -s "$T/why.bin" ] || fail "the timeline should be saved when agd stops"
config 192.0.2.1                                        # and from now on the internet doesn't answer
start_agd
after=$(agctl dm get Device.X_AIGW_.Timeline.Samples | sed 's/.*= //')
[ "$after" -ge "$before" ] && [ "$before" -ge 3 ] || fail "samples kept: $before before, $after after"
[ "$(agctl dm get Device.X_AIGW_.Timeline.RecordedSince | sed 's/.*= //')" = "$since" ] || fail "RecordedSince kept"
why --last 2m | js "sys.exit(0 if any('AIGW restarted' in s['text'] for s in d['story']) else 1)" ||
	fail "the restart is in the story: $(agctl why --last 2m)"
echo "   $before samples before, $after after; RecordedSince $since"

echo "== the internet stops answering"
wait_why 20 "d['code'] in ('LINE_DOWN', 'ISP_DOWN')" --last 12s
agctl why --last 12s
why --last 12s | js "sys.exit(0 if 'The internet didn' in d['summary'] and d['severity'] == 'problem' else 1)" ||
	fail "the internet down: $(agctl why --last 12s)"

echo "== the ISP: Device.X_AIGW_.WhySlow() and Device.X_AIGW_.Timeline."
sleep 2
"$UBUS" -s "$AIGW_UBUS" call ag.dm operate '{"command":"Device.X_AIGW_.WhySlow()","args":{"Host":"laptop"}}' > "$T/op.json" ||
	fail "WhySlow()"
js "
o = d.get('out', d)
ok = d.get('ok', True) and o['Code'] in ('LINE_DOWN', 'ISP_DOWN', 'WIFI_COVERAGE') and 'laptop' in o['Story']
ok = ok and o['Start'].endswith('Z') and int(o['RecordedPercent']) > 0 and 'WIFI_COVERAGE' in o['Findings']
sys.exit(0 if ok else 1)" < "$T/op.json" || fail "WhySlow(): $(cat "$T/op.json")"
"$UBUS" -s "$AIGW_UBUS" call ag.dm operate '{"command":"Device.X_AIGW_.WhySlow()","args":{"Start":"yesterday"}}' > "$T/bad.json" || true
js "sys.exit(0 if not d.get('ok', True) else 1)" < "$T/bad.json" || fail "a Start that isn't a dateTime: $(cat "$T/bad.json")"
agctl dm get Device.X_AIGW_.Timeline. > "$T/tl.txt"
grep -q 'Enable = true' "$T/tl.txt" && grep -q 'Hours = 1' "$T/tl.txt" || fail "Timeline.: $(cat "$T/tl.txt")"
python3 -c "import json; o=json.load(open('$T/op.json')); print('   WhySlow(): ' + o.get('out', o)['Code'])"
echo "WHY TEST OK"
