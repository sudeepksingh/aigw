#!/bin/sh
# Wi-Fi sensing, end to end: agd with a fake hostapd whose stations' signals
# are quiet or "moving" (spread out), and frame counters that grow. Polls
# every 0.2 s, a window every 3 polls, links learned in 5 quiet windows.
#
#   off           by default: nothing runs, nothing is kept (the state file
#                 has no sensing), the data model says Enable false and
#                 nothing else; "agctl sense on" without --i-agree is refused
#   on            agctl sense on --i-agree: the consent is kept in the config
#   zones         the TV in the living room, the smart plug in the kitchen
#                 (sensing.zone): quiet, both still; the TV's link spread out:
#                 the living room moves, the kitchen doesn't; quiet again: still
#   presence      the phone (someone's own) leaves and nothing moves: nobody
#                 home (ag.sense event, local); no PresenceChanged! for the ISP
#                 (share_events 0), the data model shows no presence
#   share         sensing.share_events 1: PresenceChanged! when someone is
#                 back (Home), and never a zone or a device in it
#   CSI           a vendor handler's motion score moves the kitchen
#   off again     agctl sense off: forgotten (no sensing in the state file,
#                 the consent cleared)
#   budget        agd --sense-bench
#
# Needs root (a private ubusd) and python3.
#
#   sudo tests/sense-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" sensing
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "SENSE TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a -E 'sensing' "$T"/agd.log | tail -n 20 >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

PHONE=02:00:00:00:5e:01
TV=02:00:00:00:5e:02
PLUG=02:00:00:00:5e:03
# "<MAC> <dBm> <interface> <quiet|moving>"
stations() { printf '%s\n' "$@" > "$T/stations.new"; mv "$T/stations.new" "$T/stations"; }
quiet() { stations "$PHONE -55 wlan0 quiet" "$TV -60 wlan0 quiet" "$PLUG -66 wlan1 quiet"; }
quiet
echo "10 20000" > "$T/hour"

echo "== a fake hostapd: signals quiet or moving, frame counters that grow"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, random, select, socket, sys, time
d, stations = sys.argv[1:3]
aps = {"wlan0": ("5180", "36", "02:aa:00:00:00:01"), "wlan1": ("2437", "6", "02:aa:00:00:00:02")}
socks = {}
for name in aps:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(os.path.join(d, name))
    socks[s] = name
rx = {}
def present(ap):
    out = {}
    for l in open(stations):
        w = l.split()
        if len(w) == 4 and w[2] == ap:
            out[w[0]] = (int(w[1]), w[3])
    return out
def sta(m, v):
    rx[m] = rx.get(m, 0) + random.randint(1, 30)
    sig = v[0] + (random.choice((0, 0, 0, 1, -1)) if v[1] == "quiet" else int(random.gauss(0, 5)))
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%d\nconnected_time=100\nrx_packets=%d\ntx_rate_info=1000\n" % (
        m, sig, rx[m])
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
            p = present(ap); k = sorted(p); rep = sta(k[0], p[k[0]]) if k else ""
        elif w and w[0] == "STA-NEXT":
            p = present(ap); k = sorted(p); i = k.index(w[1]) + 1 if w[1] in k else len(k)
            rep = sta(k[i], p[k[i]]) if i < len(k) else ""
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
	option port '15379'

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

config sensing 'sensing'
	option window '3'
	list zone '$TV=living'
	list zone '$PLUG=kitchen'

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

config lan 'lan'
	list iface 'lo'
EOF
printf '1999999999 %s 10.98.0.51 pixel-phone *\n1999999999 %s 10.98.0.52 tv-lounge *\n1999999999 %s 10.98.0.53 smart-plug *\n' \
	"$PHONE" "$TV" "$PLUG" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_TEST_HOUR_FILE="$T/hour" AIGW_TEST_POLL_MS=200
export AIGW_TEST_SENSE_LEARN=5
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.sense ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
sense() { agctl -j sense; }
wait_sense() {   # wait up to $1 s until a condition on `agctl -j sense` (d) holds
	n=$1; c=$2
	i=0
	until sense | js "z = {x['zone']: x for x in d.get('zones', [])}; sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 4)) ] || fail "waited for $c: $(agctl sense)"; sleep 0.25
	done
}
dm() { agctl dm get "$1" | sed 's/^[^=]*= //'; }
NAMES="('living', 'kitchen', 'pixel-phone', 'tv-lounge', 'smart-plug', '$PHONE', '$TV', '$PLUG')"

echo "== off by default: nothing runs, nothing is kept"
sleep 2
sense | js "sys.exit(0 if d['enabled'] in (0, False) and 'presence' not in d and 'zones' not in d and d['sensed'] else 1)" ||
	fail "off: $(agctl -j sense)"
agctl sense | grep -q "Wi-Fi sensing is off (opt-in)" || fail "agctl sense: the 'what is sensed' page"
[ "$(dm Device.X_AIGW_.Sensing.Enable)" = false ] && [ "$(dm Device.X_AIGW_.Sensing.Presence)" = "" ] || fail "the data model, off"
[ "$(dm Device.X_AIGW_.Sensing.Status)" = Off ] || fail "Sensing.Status, off"
grep -q 'sensing: off (opt-in' "$T/agd.log" || fail "the log, off"
stop_agd
grep -q '"sensing"' "$T/state.json" && fail "off, yet the state file has sensing"
start_agd
if agctl sense on > "$T/on.txt" 2>&1; then fail "on without --i-agree was not refused"; fi
grep -q -- "--i-agree" "$T/on.txt" || fail "the consent text: $(cat "$T/on.txt")"
sense | js "sys.exit(0 if d['enabled'] in (0, False) else 1)" || fail "still off after a refused on"

echo "== on, with the home's consent"
agctl sense on --i-agree > /dev/null || fail "agctl sense on --i-agree"
grep -q "agreed with agctl on" "$T/config/aigw" || fail "the consent is not in the config"
grep -q "option enabled '1'" "$T/config/aigw" || fail "sensing.enabled is not 1"

echo "== zones: quiet, then the living room moves, then quiet again"
wait_sense 30 "d['enabled'] in (1, True) and d['windows'] >= 12 and not z['living']['moving'] and not z['kitchen']['moving'] and d['presence'] == 'home'"
stations "$PHONE -55 wlan0 quiet" "$TV -60 wlan0 moving" "$PLUG -66 wlan1 quiet"
wait_sense 20 "z['living']['moving'] in (1, True) and not z['kitchen']['moving']"
agctl sense
quiet
wait_sense 30 "not z['living']['moving']"

echo "== presence: the phone leaves, nothing moves: nobody home (local only)"
stations "$TV -60 wlan0 quiet" "$PLUG -66 wlan1 quiet"
wait_sense 60 "d['presence'] == 'away'"
grep -q '"presence":"away"' "$T/events.log" || fail "no ag.sense presence event: $(cat "$T/events.log")"
grep -q 'PresenceChanged!' "$T/events.log" && fail "PresenceChanged! went to the ISP with share_events 0"
[ "$(dm Device.X_AIGW_.Sensing.Presence)" = "" ] || fail "the data model shows presence with share_events 0"

echo "== share_events 1: coarse events for the ISP, never a zone"
stop_agd
sed -i "s/config sensing 'sensing'/config sensing 'sensing'\n\toption share_events '1'/" "$T/config/aigw"
start_agd
sense | js "sys.exit(0 if d['enabled'] in (1, True) and d['windows'] >= 12 else 1)" || fail "kept over a restart: $(agctl -j sense)"
stations "$PHONE -55 wlan0 moving" "$TV -60 wlan0 moving" "$PLUG -66 wlan1 quiet"
wait_sense 40 "d['presence'] == 'home'"
i=0
until grep -q 'PresenceChanged!' "$T/events.log"; do i=$((i + 1)); [ $i -le 20 ] || fail "no PresenceChanged!"; sleep 0.25; done
python3 - "$T/events.log" <<EOF || fail "PresenceChanged!: $(grep PresenceChanged "$T/events.log")"
import re, sys
l = [x for x in open(sys.argv[1]) if 'PresenceChanged!' in x][-1]
sys.exit(0 if re.search(r'"Presence": ?"Home"', l) and not any(n in l for n in $NAMES) else 1)
EOF
[ "$(dm Device.X_AIGW_.Sensing.Presence)" = Home ] || fail "Sensing.Presence with share_events"
agctl dm get Device.X_AIGW_.Sensing. > "$T/dm.txt"
python3 -c "import sys; t = open('$T/dm.txt').read(); sys.exit(0 if not any(n in t for n in $NAMES) else 1)" ||
	fail "names in Sensing.: $(cat "$T/dm.txt")"
quiet

echo "== CSI: a vendor handler's motion score"
cat > "$T/csi" <<EOF
#!/bin/sh
[ "\$1" = motion ] && echo "$PLUG 90"
EOF
chmod +x "$T/csi"
stop_agd
sed -i "s|config sensing 'sensing'|config sensing 'sensing'\n\toption csi_handler '$T/csi'|" "$T/config/aigw"
start_agd
wait_sense 20 "z['kitchen']['moving'] in (1, True) and d['csi'] in (1, True)"

echo "== off again: everything forgotten"
agctl sense off > /dev/null || fail "agctl sense off"
sense | js "sys.exit(0 if d['enabled'] in (0, False) and 'zones' not in d else 1)" || fail "off: $(agctl -j sense)"
grep -q '"sensing"' "$T/state.json" && fail "off, yet the state file still has sensing"
grep -q "agreed with agctl" "$T/config/aigw" && fail "the consent was not cleared"
stop_agd
grep -q '"sensing"' "$T/state.json" && fail "after a stop, the state file has sensing"

echo "== the budget: agd --sense-bench"
"$BIN/agd/agd" --sense-bench | tee "$T/bench.txt"
grep -q '^sense-bench: [0-9.]* us a look, [0-9]* bytes of state' "$T/bench.txt" || fail "sense-bench"

echo "SENSE TEST OK"
