#!/bin/sh
# The home Wi-Fi model, end to end: agd with a fake hostapd (wlan0 5 GHz,
# wlan1 2.4 GHz), a look at each device every second (coverage.interval 1).
#
#   the home      the laptop in the living room near the gateway (-50 dBm),
#                 the TV in the bedroom far away (-77 on 5 GHz, 6 Mbit/s); the
#                 phone walks living room -> hallway (-62) -> bedroom and back
#   learning      before home.learn looks: Learning, no advice
#   the model     places (3 or more), the bedroom's path loss, the hallway
#                 next to the bedroom, the TV's problems
#   what if       2.4 GHz there fixes the bedroom's problems and costs nothing:
#                 it comes first ("try this before buying an extender"), and an
#                 extender is not worth selling (extender_helps no)
#   zones         agctl home name tv-bedroom bedroom: the advice says "in the
#                 bedroom" at home; the ISP (the data model) sees place numbers
#                 and kinds, never the name or the devices' names
#   coverage      coverage advice carries the home model's line
#   events        a new best option: ag.home and HomeAdvice! (no names)
#   kept          agd restarts: the model and the name are still there
#   off           home.enabled 0: agctl home says so, the data model too
#
# Needs root (a private ubusd) and python3.
#
#   sudo tests/home-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" homemodel
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "HOME TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a -E 'home model|coverage' "$T"/agd.log | tail -n 20 >&2 2>/dev/null || true; tail -n 15 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

PHONE=02:00:00:00:9e:01
LAPTOP=02:00:00:00:9e:02
TV=02:00:00:00:9e:03
stations() { printf '%s\n' "$@" > "$T/stations.new"; mv "$T/stations.new" "$T/stations"; }
echo "10 20000" > "$T/hour"

echo "== a fake hostapd: wlan0 (5 GHz) and wlan1 (2.4 GHz); the stations in $T/stations"
stations "$PHONE -50 wlan0 400" "$LAPTOP -50 wlan0 433" "$TV -77 wlan0 6"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, select, socket, sys, time
d, stations = sys.argv[1:3]
aps = {"wlan0": ("5180", "36", "02:aa:00:00:00:01"), "wlan1": ("2437", "6", "02:aa:00:00:00:02")}
socks = {}
for name in aps:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(os.path.join(d, name))
    socks[s] = name
since = {}
def present(ap):
    out = {}
    for l in open(stations):
        w = l.split()
        if len(w) == 4 and w[2] == ap:
            out[w[0]] = (w[1], w[3])
    for k in list(since):
        if k[0] == ap and k[1] not in out:
            del since[k]
    for m in out:
        since.setdefault((ap, m), time.time())
    return out
def sta(ap, m, v):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT][VHT]\nsignal=%s\nconnected_time=%d\ntx_rate_info=%d\n" % (
        m, v[0], int(time.time() - since[(ap, m)]), int(v[1]) * 10)
while True:
    r, _, _ = select.select(list(socks), [], [])
    for s in r:
        ap = socks[s]
        data, addr = s.recvfrom(4096)
        cmd = data.decode().strip()
        w = cmd.split()
        if cmd == "STATUS":
            f, ch, bssid = aps[ap]
            rep = "state=ENABLED\nfreq=%s\nchannel=%s\nbssid[0]=%s\nssid[0]=AIGW-TEST\n" % (f, ch, bssid)
        elif cmd == "STA-FIRST":
            p = present(ap); k = sorted(p); rep = sta(ap, k[0], p[k[0]]) if k else ""
        elif w and w[0] == "STA-NEXT":
            p = present(ap); k = sorted(p); i = k.index(w[1]) + 1 if w[1] in k else len(k)
            rep = sta(ap, k[i], p[k[i]]) if i < len(k) else ""
        else:
            rep = "UNKNOWN COMMAND\n"
        if addr:
            s.sendto(rep.encode(), addr)
EOF
PIDS="$PIDS $!"

# the phone's walk: living room, hallway, bedroom, back (a step every 2 s)
( while :; do
	for s in "-50" "-50" "-62" "-62" "-77" "-77" "-62" "-50"; do
		stations "$PHONE $s wlan0 400" "$LAPTOP -50 wlan0 433" "$TV -77 wlan0 6"; sleep 2
	done
  done ) & WALK=$!
PIDS="$PIDS $WALK"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15378'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'
	option heal '0'

config diag 'diag'
	option targets '127.0.0.1'
	option name ''

config care 'care'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '0'

config mesh 'mesh'
	option enabled '0'

config coverage 'coverage'
	option interval '1'
	option learn '12'

config home 'home'
	option learn '40'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'

config brief 'brief'
	option names '0'

config lan 'lan'
	list iface 'lo'
EOF
printf '1999999999 %s 10.97.0.51 pixel-phone *\n1999999999 %s 10.97.0.52 sara-laptop *\n1999999999 %s 10.97.0.53 tv-bedroom *\n' \
	"$PHONE" "$LAPTOP" "$TV" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_TEST_HOUR_FILE="$T/hour" AIGW_TEST_POLL_MS=500
export AIGW_TEST_HOME_PROBLEM=3
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.home ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
home() { agctl -j home; }
wait_home() {   # wait up to $1 s until a condition on `agctl -j home` (d) holds
	n=$1; c=$2
	i=0
	until home | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "waited for $c: $(agctl home)"; sleep 0.5
	done
}
NAMES="('pixel-phone', 'sara-laptop', 'tv-bedroom', 'bedroom', '$PHONE', '$TV', '$LAPTOP', '10.97.0.5')"

echo "== learning: no advice yet"
home | js "sys.exit(0 if d['enabled'] in (1, True) and d['status'] == 'Learning' and d['advice'] == '' else 1)" ||
	fail "learning: $(agctl home)"

echo "== the model: places, the bedroom's problems, what if"
# (just after Ready AIGW is not yet sure of the free change: an extender isn't ruled out until it is)
wait_home 90 "d['status'] == 'Ready' and d['problems'] >= 1 and d['places'] >= 3 and d.get('best') == 'Band24' and d['extender_helps'] == 'no'"
agctl home
home | js "
o = {x['kind']: x for x in d['options']}
b = o['Band24']
ok = b['cost'] == 'free' and b['fixed'] == d['problems'] and d['extender_helps'] == 'no'
ok = ok and 'Try this before buying an extender' in d['advice'] and 'tv-bedroom' in d['advice']
bed = [p for p in d['list'] if p['signal'] <= -75]
hall = [p for p in d['list'] if -70 <= p['signal'] <= -55 and bed and bed[0]['place'] in p['next_to']]
ok = ok and len(bed) == 1 and bed[0]['problems'] >= 1 and bed[0]['path_loss_5'] in (99, 100, 101)
ok = ok and len(hall) >= 1 and 'tv-bedroom' in bed[0]['where']
sys.exit(0 if ok else 1)" || fail "the model: $(agctl -j home)"

echo "== zones: the bedroom named by the TV seen there"
agctl home name tv-bedroom bedroom | grep -q "named" || fail "agctl home name"
agctl home name nosuch kitchen 2>&1 | grep -q "no device 'nosuch'" || fail "an unknown device is refused"
home | js "sys.exit(0 if 'in the bedroom' in d['advice'] and [p for p in d['list'] if p['name'] == 'bedroom'] else 1)" ||
	fail "the advice by the zone's name: $(agctl home)"

echo "== the data model: Device.X_AIGW_.HomeModel. (places by number, devices by kind, no names)"
agctl dm get Device.X_AIGW_.HomeModel. > "$T/dm.txt"
cat "$T/dm.txt"
python3 - "$T/dm.txt" <<EOF || fail "HomeModel.: $(cat "$T/dm.txt")"
import sys
t = open(sys.argv[1]).read()
v = dict(l.split(' = ', 1) for l in t.splitlines() if ' = ' in l)
g = lambda k: v.get('Device.X_AIGW_.HomeModel.' + k, '')
ok = g('Status') == 'Ready' and g('BestOption') == 'Band24' and g('ExtenderHelps') == 'No'
ok = ok and int(g('PlaceNumberOfEntries')) >= 3 and g('Option.1.Kind') == 'Band24' and g('Option.1.Cost') == 'Free'
ok = ok and 'a TV' in g('Advice') and 'a TV' in g('Option.1.Summary') and g('Place.1.Name') == ''
ok = ok and not any(n in t for n in $NAMES)
sys.exit(0 if ok else 1)
EOF

echo "== coverage advice carries the home model's line"
agctl -j coverage | js "sys.exit(0 if 'The home model:' in d['advice'] and '2.4 GHz' in d['advice'] else 1)" ||
	fail "coverage advice: $(agctl coverage)"

echo "== events: ag.home and HomeAdvice! (no names in the ISP's copy)"
grep -q '"ag.home"' "$T/events.log" || fail "no ag.home event: $(cat "$T/events.log")"
python3 - "$T/events.log" <<EOF || fail "HomeAdvice!: $(cat "$T/events.log")"
import re, sys
t = open(sys.argv[1]).read()
i = t.find('HomeAdvice!')
seg = t[i:t.find('\n', i) if t.find('\n', i) > 0 else len(t)]
sys.exit(0 if i >= 0 and re.search(r'"Kind": ?"Band24"', seg) and not any(n in seg for n in $NAMES) else 1)
EOF

echo "== kept over a restart"
stop_agd
start_agd
home | js "sys.exit(0 if d['status'] == 'Ready' and [p for p in d['list'] if p['name'] == 'bedroom'] else 1)" ||
	fail "after a restart: $(agctl home)"

echo "== off: home.enabled 0"
stop_agd
printf '\nconfig home home\n\toption enabled 0\n' >> "$T/config/aigw"
start_agd
agctl home | grep -q "The home model is off" || fail "off: $(agctl home)"
[ "$(agctl dm get Device.X_AIGW_.HomeModel.Status | sed 's/^[^=]*= //')" = Disabled ] || fail "off in the data model"

echo "HOME TEST OK"
