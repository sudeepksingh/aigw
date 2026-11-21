#!/bin/sh
# Coverage placement advice, end to end: agd with a fake hostapd (wlan0 5 GHz,
# wlan1 2.4 GHz) and a simulated home, a look at each device every second, the
# local hour set by the test (AIGW_TEST_HOUR_FILE).
#
#   the home      by day (10:00): the phone in the living room (-55 dBm), the
#                 laptop near the gateway (-50), the TV in the far room (-86,
#                 6 Mbit/s to it), a smart plug on 2.4 GHz (-80, yet 65 Mbit/s
#                 to it: it hears the gateway well, only its own radio is weak)
#   at night      (22:00) the phone goes to the far room too (-84)
#   learning      before a device has enough looks: no advice (callers keep
#                 the generic line)
#   the verdict   the phone weak only sometimes (it moves), the TV all the time
#                 (it lives there), the plug a weak radio of its own (compared
#                 with the others), the laptop fine; one spot the phone and the
#                 TV share: the gateway's place, an extender about halfway
#   everywhere    agctl coverage, the data model (Device.X_AIGW_.Coverage. and
#                 Hosts' X_AIGW_Coverage: kinds, never names), the timeline's
#                 and the brief's advice for the phone
#   a ticket      the phone keeps dropping off Wi-Fi at night: the proactive
#                 ticket says where and what fixes it; the customer's note is
#                 plain (an extender about halfway), without names
#   kept          agd restarts: the history is still there (the state file)
#
# Needs root (a private ubusd) and python3.
#
#   sudo tests/coverage-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "COVERAGE TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a -E 'coverage|proactive' "$T"/agd.log | tail -n 20 >&2 2>/dev/null || true; tail -n 15 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

PHONE=02:00:00:00:8c:01
LAPTOP=02:00:00:00:8c:02
TV=02:00:00:00:8c:03
PLUG=02:00:00:00:8c:04
stations() { printf '%s\n' "$@" > "$T/stations.new"; mv "$T/stations.new" "$T/stations"; }
hour() { echo "$1 20000" > "$T/hour.new"; mv "$T/hour.new" "$T/hour"; }
day() { stations "$PHONE -55 wlan0 400" "$LAPTOP -50 wlan0 433" "$TV -86 wlan0 6" "$PLUG -80 wlan1 65"; hour 10; }
night() { stations "$PHONE -84 wlan0 6" "$LAPTOP -50 wlan0 433" "$TV -86 wlan0 6" "$PLUG -80 wlan1 65"; hour 22; }
day

echo "== a fake hostapd: wlan0 (5 GHz) and wlan1 (2.4 GHz); the stations in $T/stations"
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
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%s\nconnected_time=%d\ntx_rate_info=%d\n" % (
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

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15377'

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
	option enabled '1'
	option names '0'

config proactive 'proactive'
	option enabled '1'
	option interval '2'
	option threshold '1'
	option min_duration '2'
	option clear '6'
	option cooldown '600'
	option max_per_day '5'

config lan 'lan'
	list iface 'lo'
EOF
printf '1999999999 %s 10.96.0.51 pixel-phone *\n1999999999 %s 10.96.0.52 sara-laptop *\n1999999999 %s 10.96.0.53 tv-lounge *\n1999999999 %s 10.96.0.54 smart-plug *\n' \
	"$PHONE" "$LAPTOP" "$TV" "$PLUG" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_TEST_HOUR_FILE="$T/hour"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
cov() { agctl -j coverage; }
dev() { echo "[x for x in d['list'] if x['mac'] == '$1'][0]"; }
wait_cov() {   # wait up to $1 s until a condition on `agctl -j coverage` (d) holds
	n=$1; c=$2
	i=0
	until cov | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "waited for $c: $(agctl coverage)"; sleep 0.5
	done
}
NAMES="('pixel-phone', 'sara-laptop', 'tv-lounge', 'smart-plug', '$PHONE', '$TV', '$PLUG', '$LAPTOP', '10.96.0.5')"
dm() { agctl dm get "$1" | sed 's/^[^=]*= //'; }

agctl zone smart-plug iot > /dev/null || fail "the plug's zone"

echo "== learning: no advice yet, the generic line stays"
cov | js "sys.exit(0 if d['enabled'] in (1, True) and d['verdict'] == 'Learning' and d['advice'] == '' else 1)" ||
	fail "learning: $(agctl coverage)"

echo "== by day (10:00), then at night (22:00): the phone goes to the far room"
wait_cov 30 "all(x['samples'] >= 14 for x in d['list']) and len(d['list']) == 4"
night
wait_cov 40 "$(dev "$PHONE")['kind'] == 'Sometimes' and $(dev "$PHONE")['weak_pct'] >= 40 and d['verdict'] == 'Extender'"
agctl coverage
cov | js "
p, t, l, g = $(dev "$PHONE"), $(dev "$TV"), $(dev "$LAPTOP"), $(dev "$PLUG")
ok = p['kind'] == 'Sometimes' and p.get('hours') == '22:00-23:00' and t['kind'] == 'Always' and l['kind'] == 'Fine'
ok = ok and g['kind'] == 'WeakTransmitter' and g['on_24ghz_pct'] == 100
s = [x for x in d['spots'] if x['shared'] in (1, True)]
ok = ok and len(s) == 1 and s[0]['devices'] == 2 and 'tv-lounge' in s[0]['where'] and 'pixel-phone' in s[0]['where']
ok = ok and 'smart-plug' not in s[0]['where'] and d['shared_spots'] == 1 and d['weak_transmitters'] == 1
ok = ok and 'Move the gateway toward that spot' in d['advice'] and 'about halfway' in d['advice']
ok = ok and 'own radio is weak' in g['advice'] and 'only some of the time' not in t['advice']
sys.exit(0 if ok else 1)" || fail "the verdict: $(agctl -j coverage)"
agctl coverage | grep -q "spot 1    where tv-lounge and pixel-phone are" || fail "agctl coverage: the spot by its devices"

echo "== the data model: Device.X_AIGW_.Coverage. and the Hosts (kinds, never names)"
agctl dm get Device.X_AIGW_.Coverage. > "$T/dm.txt"
cat "$T/dm.txt"
python3 - "$T/dm.txt" <<EOF || fail "Coverage.: $(cat "$T/dm.txt")"
import sys
t = open(sys.argv[1]).read()
v = dict(l.split(' = ', 1) for l in t.splitlines() if ' = ' in l)
g = lambda k: v.get('Device.X_AIGW_.Coverage.' + k, '')
ok = g('Status') == 'Extender' and g('SharedSpots') == '1' and g('WeakTransmitters') == '1' and int(g('SpotNumberOfEntries')) >= 1
ok = ok and 'a TV' in g('Spot.1.Where') and g('Spot.1.Shared') == 'true' and 'about halfway' in g('Advice')
ok = ok and 'extender' in g('CustomerNote') and not any(n in t for n in $NAMES)
sys.exit(0 if ok else 1)
EOF
grep -q 'Device.X_AIGW_.Coverage.Spot.1.When = ' "$T/dm.txt" || fail "Spot.1.When"
agctl dm get Device.Hosts. > "$T/hosts.txt"
python3 - "$T/hosts.txt" <<EOF || fail "Hosts' X_AIGW_Coverage: $(grep -E 'PhysAddress|X_AIGW_Coverage|UsualSignal' "$T/hosts.txt")"
import re, sys
t = open(sys.argv[1]).read()
host = {}
for k, v in re.findall(r'(Device\.Hosts\.Host\.\d+\.[A-Za-z_]+) = (.*)', t):
    i, name = k.split('.')[3], k.split('.')[4]
    host.setdefault(i, {})[name] = v
by = {h['PhysAddress']: h for h in host.values() if 'PhysAddress' in h}
ok = by['$TV']['X_AIGW_Coverage'] == 'Always' and by['$PHONE']['X_AIGW_Coverage'] == 'Sometimes'
ok = ok and by['$PLUG']['X_AIGW_Coverage'] == 'WeakTransmitter' and int(by['$TV']['X_AIGW_UsualSignal']) <= -84
sys.exit(0 if ok else 1)
EOF

echo "== the timeline and the brief: the phone's advice names the spot, not 'move it closer'"
sleep 2
agctl -j why pixel-phone --last 1m | js "
ok = d['code'] in ('WIFI_COVERAGE', 'WIFI_DROPS') and 'other device share' in d['advice']
sys.exit(0 if ok and 'access point higher' not in d['advice'] else 1)" || fail "why: $(agctl why pixel-phone --last 1m)"
agctl -j brief pixel-phone "last 1m" --no-check > "$T/brief.json" || fail "brief"
js "
ev = ' '.join(e['text'] for e in d['evidence'])
ok = 'Coverage: pixel-phone is weak some of the time' in ev and 'about halfway' in d['fix']['text']
sys.exit(0 if ok else 1)" < "$T/brief.json" || fail "brief: $(cat "$T/brief.json")"

echo "== a ticket: the phone keeps dropping off Wi-Fi at night"
k=0
while [ $k -lt 3 ]; do
	stations "$LAPTOP -50 wlan0 433" "$TV -86 wlan0 6" "$PLUG -80 wlan1 65"; sleep 5
	night; sleep 5
	k=$((k + 1))
done
i=0
until agctl -j care proactive | js "sys.exit(0 if [t for t in d['tickets'] if t['code'] in ('WIFI_DROPS', 'WIFI_COVERAGE')] else 1)" 2>/dev/null; do
	i=$((i + 1)); [ $i -le 60 ] || fail "no ticket: $(agctl care proactive)"; sleep 0.5
done
agctl -j care proactive > "$T/pro.json"
agctl care proactive
python3 - "$T/pro.json" <<EOF || fail "the ticket: $(agctl care proactive)"
import json, re, sys
d = json.load(open(sys.argv[1]))
t = [t for t in d['tickets'] if t['code'] in ('WIFI_DROPS', 'WIFI_COVERAGE')][0]
text = t['summary'] + ' ' + t['next_step'] + ' ' + t['evidence']
note = t['customer_note']
plain = not re.search(r'[A-Z]{3,}|_|\\b(DNS|MAC|ISP|IP|GHz|dBm|ms|Mbit)\\b|packet|channel|router|band', note)
checks = {'advice': 'about halfway' in text and ('same spot' in text or 'other device' in text),
          'note': plain and 'extender' in note and 'one part of your home' in note,
          'no names': not any(n in text + note for n in $NAMES)}
print('   ', checks, [n for n in $NAMES if n in text + note])
sys.exit(0 if all(checks.values()) else 1)
EOF

echo "== kept over a restart"
stop_agd
start_agd
cov | js "sys.exit(0 if d['verdict'] == 'Extender' and $(dev "$TV")['kind'] == 'Always' else 1)" ||
	fail "after a restart: $(agctl coverage)"

echo "COVERAGE TEST OK"
