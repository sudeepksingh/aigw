#!/bin/sh
# Smarter mesh Wi-Fi, end to end, over a simulated EasyMesh network: agd reads
# a fake controller (tests/mesh/fakectl.py, the mesh handler) that answers
# with the Data Elements model the way prplMesh's controller does over ubus,
# simulates where each device hears each access point, and records the
# steering and channel requests AIGW sends it.
#
# The home: the gateway (2.4 GHz ch 1, 5 GHz ch 36/80) and the extender
# "bedroom" on a wireless 5 GHz backhaul (2.4 GHz ch 1, 5 GHz ch 36/80).
#
#   read          two access points, the extender by its name, the backhaul, the
#                 devices with their 802.11v/k support; the channel plan seen
#   advise mode   the laptop struggles on the gateway's 2.4 GHz (-80 dBm), the
#                 extender's 5 GHz hears it at -58: AIGW only proposes the move
#                 (nothing sent to the controller), MeshDecision! Proposed; the
#                 TV, as weak, is protected (act.protect): never moved
#   approved      the move goes to the controller (ClientSteering: btm), lands,
#                 is measured and kept (much better); the timeline has it
#   enforce mode  the phone gets weak on the extender; the gateway hears it at
#                 -60 but its link there is slower and resends more: moved,
#                 judged worse, moved back; that move avoided afterwards
#   a miss        a tablet the controller doesn't move: missed, not moved back
#   the controller's own steering   a device it just steered is left alone
#   channels      the extender's 2.4 GHz on the gateway's channel: moved to 6
#                 through the controller, kept
#   backhaul      the extender's backhaul turns weak (-78 dBm, 54 Mbit/s) with
#                 the phone behind it: found, "hurts more than it helps", in the
#                 data model, a proactive ticket for the ISP, the help-desk brief
#   a snapshot    mesh.source: "path=value" lines read without a handler
#
# Needs root (a private ubusd publishes agd's objects) and python3.
#
#   sudo tests/mesh-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "MESH TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a 'mesh' "$T"/agd.log | tail -n 30 >&2 2>/dev/null || true;
	 echo "--- requests" >&2; cat "$T/requests.log" >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

GW=02:00:00:00:00:01
EXT=02:00:00:00:00:02
GW24=02:00:00:00:01:01
GW5=02:00:00:00:02:01
GWBH=02:00:00:00:02:02
EXT24R=02:00:00:00:03:00
EXT24=02:00:00:00:03:01
EXT5=02:00:00:00:04:01
LAPTOP=aa:00:00:00:00:01
PHONE=aa:00:00:00:00:02
TV=aa:00:00:00:00:03
TABLET=aa:00:00:00:00:04

echo "== the simulated home and its fake controller"
cp "$REPO/tests/mesh/fakectl.py" "$T/fakectl"
chmod 755 "$T/fakectl"
cat > "$T/world.json" <<EOF
{"network": "home-1", "controller": "$GW",
 "agents": [
  {"id": "$GW", "model": "gateway", "backhaul": null, "radios": [
    {"id": "02:00:00:00:01:00", "class": 81, "ch": 1, "util": 40, "bss": [{"bssid": "$GW24", "ssid": "home"}]},
    {"id": "02:00:00:00:02:00", "class": 128, "ch": 42, "util": 30, "bss": [{"bssid": "$GW5", "ssid": "home"},
      {"bssid": "$GWBH", "ssid": "home-bh", "backhaul": true, "fronthaul": false}]}]},
  {"id": "$EXT", "model": "extender",
   "backhaul": {"type": "Wi-Fi", "mac": "$GWBH", "parent": "$GW", "own": "02:00:00:00:00:2f", "dbm": -60,
                "kbps": 600000, "class": 128, "ch": 42},
   "radios": [
    {"id": "$EXT24R", "class": 81, "ch": 1, "util": 20, "bss": [{"bssid": "$EXT24", "ssid": "home"}]},
    {"id": "02:00:00:00:04:00", "class": 128, "ch": 42, "util": 20, "bss": [{"bssid": "$EXT5", "ssid": "home"}]}]}],
 "stations": {
  "$LAPTOP": {"at": "$GW24", "links": {"$GW24": [-80, 6500, 30], "$EXT5": [-58, 300000, 3]}},
  "$PHONE": {"at": "$EXT5", "links": {"$EXT5": [-55, 400000, 2]}},
  "$TV": {"at": "$GW24", "links": {"$GW24": [-82, 5000, 35], "$EXT5": [-60, 200000, 3]}}}}
EOF
export FAKECTL_WORLD="$T/world.json" FAKECTL_DIR="$T"
world() {   # world <python statements on w>
	python3 - "$T/world.json" <<EOF
import json, os, sys, time
p = sys.argv[1]
w = json.load(open(p))
st = w["stations"]
$1
json.dump(w, open(p + ".t", "w"), indent=1)
os.replace(p + ".t", p)
EOF
}

config() {   # config <act mode> <channel plan 0|1> [mesh.source: then no handler to act through]
	mkdir -p "$T/config"
	if [ -n "${3:-}" ]; then HANDLER="$T/no-handler"; else HANDLER="$T/fakectl"; fi
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15384'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'
	option heal '0'

config care 'care'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '0'

config act 'act'
	option mode '$1'
	option max_auto '20'
	option dns_policy '$T/dns-policy'
	list protect '$TV'

config plugins 'plugins'
	option enabled '0'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '0'

config brief 'brief'
	option enabled '1'
	option names '0'

config proactive 'proactive'
	option enabled '1'
	option fix '1'
	option interval '2'
	option threshold '1'
	option min_duration '2'
	option clear '6'
	option max_per_day '5'

config mesh 'mesh'
	option enabled '1'
	option handler '$HANDLER'
	option source '${3:-}'
	option poll '1'
	option steer '1'
	option channel '$2'
	option trial '4'
	option hold '2'
	option cooldown '10'
	option quiet '3'
	list name '$EXT=bedroom'

config lan 'lan'
	list iface 'lo'
EOF
}
mkdir -p "$T/hostapd"
printf '1999999999 %s 10.96.0.41 sara-laptop *\n1999999999 %s 10.96.0.42 dads-phone *\n1999999999 %s 10.96.0.43 living-room-tv *\n1999999999 %s 10.96.0.44 kids-tablet *\n' \
	"$LAPTOP" "$PHONE" "$TV" "$TABLET" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
mesh() { agctl -j mesh | js "m = d['mesh']; $1"; }
wait_mesh() {   # wait up to $1 s until a condition on the mesh (m) holds
	n=$1; c=$2
	i=0
	until agctl -j mesh | js "m = d['mesh']; sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "waited for $c: $(agctl mesh)"; sleep 0.5
	done
}
sta() { echo "[s for s in m['stations'] if s['mac'] == '$1'][0]"; }
decision() {   # the newest decision of an action on a target prefix: its JSON
	agctl -j decisions 50 | js "
l = [x for x in d['decisions'] if x['action'] == '$1' and x['target'].startswith('$2')]
print(json.dumps(l[0]) if l else '{}')"
}
wait_decision() {   # wait_decision <s> <action> <target prefix> <state>
	i=0
	until [ "$(decision "$2" "$3" | js "print(d.get('state', ''))")" = "$4" ]; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "waited for $2 $3 to be $4: $(agctl decisions)"; sleep 0.5
	done
}
requests() { if [ -f "$T/requests.log" ]; then grep -c "$1" "$T/requests.log" || true; else echo 0; fi; }
dm_events() { grep -c "MeshDecision!" "$T/events.log" || true; }
last_event() {   # the last MeshDecision!'s argument $1
	python3 -c "
import json
ev = [json.loads(l) for l in open('$T/events.log').read().splitlines() if 'MeshDecision!' in l]
print(ev[-1].get('ag.dm.event', ev[-1])['args']['$1'] if ev else '')"
}

config advise 0
start_agd

echo "== read: the gateway, the extender 'bedroom' on a wireless backhaul, the devices"
wait_mesh 20 "m['status'] == 'Ok' and len(m['agents']) == 2 and len(m['stations']) == 3"
agctl mesh
mesh "
gw = [a for a in m['agents'] if a['role'] == 'gateway'][0]
ex = [a for a in m['agents'] if a['role'] == 'extender'][0]
ok = gw['id'] == '$GW' and ex['words'] == \"the extender 'bedroom'\" and ex['backhaul']['type'] == 'Wi-Fi'
ok = ok and ex['backhaul']['signal'] == -60 and ex['backhaul']['band'] == 5 and ex['backhaul']['parent'] == '$GW'
r5 = [r for r in gw['radios'] if r['band'] == 5][0]
ok = ok and r5['channel'] == 36 and r5['width'] == 80 and any(b['backhaul'] in (1, True) for b in r5['bss'])
l = $(sta $LAPTOP)
ok = ok and l['signal'] == -80 and l['band'] == 2 and l['on'] == 'the gateway' and l['btm'] == 'yes' and l['rrm'] == 'yes'
ok = ok and len(m['plan']) == 1 and m['plan'][0]['radio'] == '$EXT24R' and m['plan'][0]['to'] == 6
sys.exit(0 if ok else 1)" || fail "the read: $(agctl -j mesh)"
agctl mesh | grep -q "the extender 'bedroom'" || fail "agctl mesh: the extender's name"

echo "== advise mode: the laptop struggles; AIGW proposes moving it to the extender, sends nothing"
wait_decision 20 meshsteer "$LAPTOP" proposed
sleep 2
[ "$(requests steer)" = 0 ] || fail "advise mode sent a steer"
decision meshsteer "$LAPTOP" | js "
ok = d['target'] == '$LAPTOP@$EXT5' and d['scope'] == '$GW24' and 'sara-laptop' in d['device'] and d['origin'] == 'mesh'
ok = ok and '-58 dBm' in d['reason'] and 'measured by that access point' in d['reason']
sys.exit(0 if ok else 1)" || fail "the proposal: $(decision meshsteer $LAPTOP)"
[ "$(last_event State)" = Proposed ] && [ "$(last_event Kind)" = Steer ] || fail "MeshDecision! Proposed: $(cat "$T/events.log")"
last_event Reason | grep -q sara-laptop && fail "a device's name in MeshDecision! (brief.names 0)"
mesh "t = $(sta $TV); sys.exit(0 if 'protected' in t.get('now', '') else 1)" || fail "the TV: protected: $(agctl mesh)"
decision meshsteer "$TV" | grep -q '"id"' && fail "a decision about the protected TV"

echo "== approved: the controller moves it (BTM), the trial keeps it"
ID=$(decision meshsteer "$LAPTOP" | js "print(d['id'])")
agctl approve "$ID" > /dev/null || fail "approve $ID"
[ "$(requests "steer $LAPTOP $EXT5 btm")" = 1 ] || fail "ClientSteering for the laptop"
wait_decision 20 meshsteer "$LAPTOP" kept
decision meshsteer "$LAPTOP" | js "sys.exit(0 if d['note'].startswith('kept: clearly better') and '-80 -> -58 dBm' in d['note'] else 1)" ||
	fail "kept, and why: $(decision meshsteer $LAPTOP)"
mesh "c = m['counts']; l = $(sta $LAPTOP); sys.exit(0 if c['steers'] == 1 and c['kept'] == 1 and l['on'] == \"the extender 'bedroom'\" and l['kept'] == 1 else 1)" ||
	fail "counts after the kept move: $(agctl mesh)"
[ "$(last_event State)" = Kept ] && last_event Before | grep -q -- '-80 dBm' && last_event After | grep -q -- '-58 dBm' ||
	fail "MeshDecision! Kept with before/after: $(tail -n 2 "$T/events.log")"
agctl dm get Device.X_AIGW_.Mesh. > "$T/dm.txt"
grep -q 'SteersKept.*1' "$T/dm.txt" && grep -q 'Extenders.*1' "$T/dm.txt" && grep -q 'Status.*Ok' "$T/dm.txt" ||
	fail "Device.X_AIGW_.Mesh.: $(cat "$T/dm.txt")"
grep -q -i 'sara-laptop' "$T/dm.txt" && fail "a device's name in the data model: $(cat "$T/dm.txt")"
sleep 2
agctl -j why --last 10m --raw | js "sys.exit(0 if any(e['kind'] == 'mesh' and 'stays on' in e['text'] for e in d['events']) and
	any(e['kind'] == 'decision' and 'meshsteer' in e['text'] for e in d['events']) else 1)" ||
	fail "the timeline: $(agctl -j why --last 10m --raw | head -c 1500)"

echo "== enforce mode (agd restarted): the phone, moved by itself, judged worse, moved back"
stop_agd
config enforce 0
world "st['$PHONE']['links'] = {'$EXT5': [-79, 20000, 5], '$GW5': [-60, 8000, 40]}"
start_agd
wait_mesh 20 "m['status'] == 'Ok' and m['counts']['kept'] == 1"
wait_decision 30 meshsteer "$PHONE" tried
[ "$(requests "steer $PHONE $GW5")" = 1 ] && [ "$(requests "steer $PHONE $EXT5")" = 1 ] ||
	fail "moved, then moved back: $(cat "$T/requests.log")"
decision meshsteer "$PHONE" | js "
ok = d['automatic'] in (1, True) and d['note'].startswith('moved back: worse') and 'rate 20 -> 8 Mbit/s' in d['note']
sys.exit(0 if ok else 1)" || fail "moved back, and why: $(decision meshsteer $PHONE)"
[ "$(last_event State)" = Undone ] || fail "MeshDecision! Undone"
sleep 12    # past the cooldown: the same move is not tried again (avoided for a day)
[ "$(requests "steer $PHONE $GW5")" = 1 ] || fail "the phone moved again to where it did worse"
mesh "p = $(sta $PHONE); sys.exit(0 if p['undone'] == 1 and p['on'] == \"the extender 'bedroom'\" else 1)" ||
	fail "the phone's history: $(agctl mesh)"

echo "== a miss: the controller doesn't move the tablet"
world "st['$TABLET'] = {'at': '$GW24', 'obey': False, 'joined': time.time() - 600, 'links': {'$GW24': [-81, 5000, 30], '$EXT5': [-57, 300000, 2]}}"
wait_decision 40 meshsteer "$TABLET" tried
decision meshsteer "$TABLET" | js "sys.exit(0 if d['note'].startswith('missed') else 1)" || fail "missed: $(decision meshsteer $TABLET)"
[ "$(requests "steer $TABLET $GW24")" = 0 ] || fail "a missed steer moved back"
[ "$(last_event State)" = Missed ] || fail "MeshDecision! Missed"
mesh "t = $(sta $TABLET); sys.exit(0 if t['misses'] == 1 else 1)" || fail "the tablet's miss: $(agctl mesh)"

echo "== the controller's own steering: a device it keeps moving is left alone"
world "st['$TABLET']['ctl_steered'] = time.time() + 3600"     # the controller steered it a second ago, at every read
wait_mesh 20 "'controller steered it' in $(sta $TABLET).get('now', '')"
sleep 12    # past AIGW's own cooldown: still not moved again
[ "$(requests "steer $TABLET")" = 1 ] || fail "AIGW fought the controller: $(cat "$T/requests.log")"
world "del st['$TABLET']"

echo "== channels: the extender's 2.4 GHz shares channel 1 with the gateway: moved to 6, kept"
stop_agd
config enforce 1
start_agd
wait_decision 40 meshchan "$EXT24R" kept
[ "$(requests "channel $EXT24R 6 81 6 20")" = 1 ] || fail "the channel request: $(cat "$T/requests.log")"
mesh "sys.exit(0 if m['counts']['channel_changes'] == 1 and m['counts']['channels_kept'] == 1 and not m['plan'] else 1)" ||
	fail "after the channel change: $(agctl mesh)"
[ "$(last_event Kind)" = Channel ] && [ "$(last_event State)" = Kept ] || fail "MeshDecision! Channel Kept"

echo "== backhaul: weak (-78 dBm, 54 Mbit/s) with the phone and laptop behind it: hurts more than it helps"
world "w['agents'][1]['backhaul'].update(dbm=-78, kbps=54000)"
wait_mesh 20 "m['backhaul'] and m['backhaul'][0]['kind'] == 'Weak' and m['backhaul'][0]['hurts'] in (1, True)"
agctl mesh | grep -q "the extender 'bedroom' has a weak wireless backhaul to the gateway (-78 dBm, 54 Mbit/s, 5 GHz)" ||
	fail "agctl mesh: $(agctl mesh)"
[ "$(agctl dm get Device.X_AIGW_.Mesh.HurtingExtenders | sed 's/.*= *//')" = 1 ] ||
	fail "HurtingExtenders: $(agctl dm get Device.X_AIGW_.Mesh.)"
i=0
until agctl -j care proactive | js "sys.exit(0 if [t for t in d['tickets'] if t['code'] == 'MESH_BACKHAUL'] else 1)" 2>/dev/null; do
	i=$((i + 1)); [ $i -le 40 ] || fail "no proactive ticket: $(agctl care proactive)"; sleep 0.5
done
agctl -j care proactive | python3 -c "
import json, re, sys
d = json.load(sys.stdin)
t = [t for t in d['tickets'] if t['code'] == 'MESH_BACKHAUL'][0]
n = t['customer_note']
ok = t['fault'] == 'HOME_WIFI' and 'extender' in n and not re.search(r'[A-Z]{3,}|_|GHz|dBm|Mbit', n) and n.startswith('We noticed')
sys.exit(0 if ok else 1)" || fail "the proactive ticket: $(agctl care proactive)"
agctl -j brief now --no-check | js "sys.exit(0 if any(e['source'] == 'mesh' and 'weak wireless backhaul' in e['text'] for e in d['evidence']) else 1)" ||
	fail "the brief's evidence: $(agctl brief now --no-check)"
agctl -j why --last 10m --raw | js "sys.exit(0 if any(e['kind'] == 'mesh' and 'weak wireless backhaul' in e['text'] for e in d['events']) else 1)" ||
	fail "the timeline: the backhaul"

echo "== state kept: counts and the phone's avoided move survive a restart"
stop_agd
start_agd
wait_mesh 20 "m['status'] == 'Ok' and m['counts']['steers'] == 3 and m['counts']['undone'] == 1 and m['counts']['misses'] == 1"
mesh "p = $(sta $PHONE); sys.exit(0 if p['undone'] == 1 else 1)" || fail "the phone's history after a restart"

echo "== a snapshot instead of a handler: path=value lines (as ubus-cli prints them)"
touch "$T/lines"
"$T/fakectl" get "$T/snapshot.txt"
rm -f "$T/lines"
grep -q '^Device.WiFi.DataElements.Network.Device.2.MultiAPDevice.Backhaul.LinkType="Wi-Fi"$' "$T/snapshot.txt" ||
	fail "the lines: $(head -n 5 "$T/snapshot.txt")"
stop_agd
config advise 0 "$T/snapshot.txt"
start_agd
wait_mesh 20 "m['status'] == 'Ok' and m['source'] == 'snapshot' and len(m['agents']) == 2 and len(m['stations']) == 3"
agctl mesh steer sara-laptop "$GW5" > "$T/steer.out" 2>&1 && fail "a steer with no handler to act through"
grep -q "no mesh handler to act through" "$T/steer.out" || fail "why it can't: $(cat "$T/steer.out")"

echo "== no controller: said plainly"
stop_agd
config advise 0 "$T/missing.json"
start_agd
wait_mesh 10 "m['status'] == 'NoController'"
agctl mesh | grep -q "EasyMesh controller" || fail "agctl mesh without a controller: $(agctl mesh)"

grep -a -i 'segfault\|AddressSanitizer' "$T/agd.log" && fail "agd crashed"
echo "MESH TEST PASS"
