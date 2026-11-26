#!/bin/sh
# Proven self-repair, end to end (docs/proven-repair.md): agd in a gateway network namespace with a LAN, a fake
# hostapd (wlan0 5 GHz on channel 36, wlan1 2.4 GHz on 6) that keeps its deny lists and switches channels, a test
# plug-in that raises Wi-Fi alerts, policies that fix them (a device kept on 2.4 GHz, a channel), and the
# measurements from $T/probe (AIGW_TEST_REPAIR_FILE: "<metric> <subject> <value|gone>").
#
#   advise mode   a fix proposed: a shadow receipt (prediction, confidence), never applied; without the fix the
#                 loss stays: "needed"
#   enforce mode  (restarted: the shadow kept) a laptop's loss 30% -> 2%: helped, as predicted, it stays
#                 a channel 36 -> 44, the loss stays 20%: no change, undone: back on 36 (CHAN_SWITCH), the decision
#                 "tried", RepairUndone!
#                 a phone that leaves: unknown, not undone
#   the evidence  by fault and kind of fix; agctl repair, care and why; Device.X_AIGW_.Repair. and Repair!
#   a restart     receipts and the evidence kept
#   the gate      on (80% over 2 trials): the channel (0% of 1) and the band (1 of 1) wait for a person
#   the bound     3 receipts kept at most (AIGW_TEST_REPAIR_MAX): the newest
#
# Needs root (a network namespace, a private ubusd), python3, ip.
#
#   sudo tests/repair-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" proven wifi-heal plugins
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"         # the test plug-in runs as nobody
GW=aigwrep-gw
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	ip netns del "$GW" 2>/dev/null || true
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "REPAIR TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a 'repair\|decision' "$T"/agd.log | tail -n 40 >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v ip >/dev/null || fail "missing ip"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

ip netns del "$GW" 2>/dev/null || true
ip netns add "$GW"
ip -n "$GW" link set lo up
ip -n "$GW" link add lan0 type dummy
ip -n "$GW" addr add 10.95.0.1/24 dev lan0
ip -n "$GW" link set lan0 up

PHONE=02:00:00:00:9b:01
LAPTOP=02:00:00:00:9b:02
TV=02:00:00:00:9b:03
printf '%s -50 wlan0\n%s -55 wlan0\n%s -60 wlan0\n' "$PHONE" "$LAPTOP" "$TV" > "$T/stations"
: > "$T/probe"
probe() { printf '%s\n' "$@" > "$T/probe.new"; mv "$T/probe.new" "$T/probe"; }

echo "== a fake hostapd: wlan0 (5 GHz, channel 36) and wlan1 (2.4 GHz, 6); deny lists and channel switches kept"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/hostapd.log" 2>&1 <<'EOF' &
import os, select, socket, sys, time
d, stations = sys.argv[1:3]
aps = {"wlan0": [5180, "02:aa:00:00:00:01"], "wlan1": [2437, "02:aa:00:00:00:02"]}
socks = {}
for name in aps:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(os.path.join(d, name))
    socks[s] = name
deny = {n: set() for n in aps}
since = {}
def chan(f):
    return (f - 2407) // 5 if f < 5000 else (f - 5000) // 5
def present(ap):
    out = {}
    for l in open(stations):
        w = l.split()
        if len(w) == 3 and w[2] == ap and w[0].lower() not in deny[ap]:
            out[w[0]] = w[1]
    for m in out:
        since.setdefault((ap, m), time.time())
    return out
def sta(ap, m, sig):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%s\nconnected_time=%d\ntx_rate_info=1440\n" % (
        m, sig, int(time.time() - since[(ap, m)]))
while True:
    r, _, _ = select.select(list(socks), [], [])
    for s in r:
        ap = socks[s]
        data, addr = s.recvfrom(4096)
        cmd = data.decode().strip()
        w = cmd.split()
        if cmd == "STATUS":
            f, bssid = aps[ap]
            rep = "state=ENABLED\nfreq=%d\nchannel=%d\nbssid[0]=%s\nssid[0]=AIGW-TEST\n" % (f, chan(f), bssid)
        elif cmd == "STA-FIRST":
            p = present(ap); k = sorted(p); rep = sta(ap, k[0], p[k[0]]) if k else ""
        elif w and w[0] == "STA-NEXT":
            p = present(ap); k = sorted(p); i = k.index(w[1]) + 1 if w[1] in k else len(k)
            rep = sta(ap, k[i], p[k[i]]) if i < len(k) else ""
        elif cmd == "DENY_ACL SHOW":
            rep = "".join("%s VLAN_ID=0\n" % m for m in sorted(deny[ap]))
        elif len(w) == 3 and w[0] == "DENY_ACL" and w[1] == "ADD_MAC":
            deny[ap].add(w[2].lower()); rep = "OK\n"
            print("deny", ap, w[2].lower(), flush=True)
        elif len(w) == 3 and w[0] == "DENY_ACL" and w[1] == "DEL_MAC":
            if w[2].lower() in deny[ap]:
                print("allow", ap, w[2].lower(), flush=True)
            deny[ap].discard(w[2].lower()); rep = "OK\n"
        elif w and w[0] == "CHAN_SWITCH":
            aps[ap][0] = int(w[2]); rep = "OK\n"
            print("switch", ap, chan(int(w[2])), flush=True)
        elif w and w[0] in ("DEAUTHENTICATE", "DISASSOCIATE", "BSS_TM_REQ"):
            rep = "OK\n"
        else:
            rep = "UNKNOWN COMMAND\n"
        if addr:
            s.sendto(rep.encode(), addr)
EOF
PIDS="$PIDS $!"

config() {   # config <act mode> [gate]
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15375'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'
	option heal '0'
	option chan_heal '0'
	option chan_plan '0'

config care 'care'
	option enabled '1'
	option interval '3600'

config diag 'diag'
	option targets '10.95.0.250'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '0'

config proactive 'proactive'
	option enabled '0'

config act 'act'
	option mode '$1'
	option dns_policy '$T/dns-policy'

config policy 'trouble'
	option alert 'wifi-trouble'
	option action 'stay24'
	option ttl '3600'
	option mode 'enforce'

config policy 'noise'
	option alert 'wifi-noise'
	option action 'channel'
	option ttl '3600'
	option mode 'enforce'

config repair 'repair'
	option enabled '1'
	option window '8'
	option settle '3'
	option tick '1'
	option gate '${2:-0}'
	option gate_fix_rate '80'
	option gate_trials '2'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/user'
	option user 'nobody'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '0'

config brief 'brief'
	option names '0'

config lan 'lan'
	list iface 'lan0'
EOF
}

# the test's plug-in: an alert from a file in its state directory, when one is put there
mkdir -p "$T/sys/nudge" "$T/user"
cat > "$T/sys/nudge/manifest.json" <<'EOF'
{ "name": "nudge", "kind": "sensor", "version": "1.0.0", "description": "repair-test: a Wi-Fi alert on request",
  "exec": "run.sh", "alerts": ["wifi-trouble", "wifi-noise"], "hosts": ["linux", "openwrt"],
  "budget": { "ram_kb": 8192, "cpu_pct": 5 } }
EOF
cat > "$T/sys/nudge/run.sh" <<'EOF'
#!/bin/sh
while :; do
	if [ -f "$AIGW_STATE/alert" ]; then
		cat "$AIGW_STATE/alert"; rm -f "$AIGW_STATE/alert"
	fi
	sleep 0.3
done
EOF
chmod 755 "$T/sys/nudge/run.sh"
printf '1999999999 %s 10.95.0.41 dads-phone *\n1999999999 %s 10.95.0.42 sara-laptop *\n1999999999 %s 10.95.0.43 living-room-tv *\n' \
	"$PHONE" "$LAPTOP" "$TV" > "$T/leases"

export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true AIGW_TEST_REPAIR_FILE="$T/probe"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { ip netns exec "$GW" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
rep() { agctl -j repair; }
wait_rep() {   # wait up to $1 s until a condition on `agctl -j repair` (d) holds
	n=$1; c=$2; i=0
	until rep | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "waited for $c: $(agctl repair)"; sleep 0.5
	done
}
rcpt() { echo "[r for r in d['receipts'] if $1]"; }
alert() {   # alert <kind> <device or target> <subject> (into the plug-in's own state directory, which agd makes)
	i=0
	until [ -d "$T/pstate/nudge" ]; do i=$((i + 1)); [ $i -le 20 ] || fail "the test plug-in did not start"; sleep 0.5; done
	echo "{\"alert\": {\"kind\": \"$1\", \"device\": \"$2\", \"subject\": \"$3\", \"reason\": \"repair-test\"}}" > "$T/pstate/nudge/alert.new"
	mv "$T/pstate/nudge/alert.new" "$T/pstate/nudge/alert"
}
decision() { agctl -j decisions | js "x = [k for k in d['decisions'] if k['id'] == $1]; print(x[0]['state'] if x else '')"; }
events() { grep -c "$1" "$T/events.log" || true; }

config advise
start_agd
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
agctl repair | grep -q "Proven repair" || fail "agctl repair: $(agctl repair)"

echo "== advise mode: the TV loses 26%: a fix proposed, a shadow receipt, never applied"
probe "DeviceLoss $TV 26"
alert wifi-trouble 10.95.0.43 "tv 1"
wait_rep 15 "$(rcpt "r['mode'] == 'shadow' and r['action'] == 'stay24'")"
SH=$(rep | js "print($(rcpt "r['mode'] == 'shadow'")[0]['id'])")
SD=$(rep | js "print([r for r in d['receipts'] if r['id'] == $SH][0]['decision'])")
rep | js "r = [r for r in d['receipts'] if r['id'] == $SH][0]
sys.exit(0 if r['fault'] == 'wifi-trouble' and r['metric'] == 'DeviceLoss' and r['before'] == 26 and r['expected'] == 5 and
         'loss 26% -> under 5% within' in r['prediction'] and r['result'] == 'Pending' and 'confidence' not in r else 1)" ||
	fail "the shadow's prediction: $(agctl -j repair)"
[ "$(decision "$SD")" = proposed ] || fail "advise mode: only proposed: $(agctl decisions)"
wait_rep 15 "[r for r in d['receipts'] if r['id'] == $SH][0]['result'] == 'Needed'"
grep -q "deny" "$T/hostapd.log" && fail "advise mode: nothing may be applied: $(cat "$T/hostapd.log")"
rep | js "r = [r for r in d['receipts'] if r['id'] == $SH][0]
sys.exit(0 if 'was needed' in r['summary'] and 'not applied' in r['summary'] else 1)" || fail "shadow words: $(agctl repair)"
sleep 1
grep 'Repair!' "$T/events.log" | grep -q '"Mode":"Shadow"' || fail "Repair! for the shadow: $(cat "$T/events.log")"
echo "   receipt $SH: $(rep | js "print([r for r in d['receipts'] if r['id'] == $SH][0]['summary'])")"
agctl reject "$SD" > /dev/null || fail "reject $SD"

echo "== enforce mode (restarted): the laptop loses 30%: kept on 2.4 GHz, then 2%: helped, it stays"
stop_agd
config enforce
start_agd
rep | js "sys.exit(0 if [r for r in d['receipts'] if r['id'] == $SH and r['result'] == 'Needed'] else 1)" ||
	fail "the shadow kept over a restart: $(agctl -j repair)"
probe "DeviceLoss $TV 26" "DeviceLoss $LAPTOP 30"
alert wifi-trouble 10.95.0.42 "laptop 1"
wait_rep 15 "$(rcpt "r['mode'] == 'applied' and r['action'] == 'stay24' and r['before'] == 30")"
LR=$(rep | js "print($(rcpt "r['mode'] == 'applied' and r['before'] == 30")[0]['id'])")
LD=$(rep | js "print([r for r in d['receipts'] if r['id'] == $LR][0]['decision'])")
grep -q "deny wlan0 $LAPTOP" "$T/hostapd.log" || fail "the laptop refused on 5 GHz: $(cat "$T/hostapd.log")"
probe "DeviceLoss $TV 26" "DeviceLoss $LAPTOP 2"
wait_rep 20 "[r for r in d['receipts'] if r['id'] == $LR][0]['result'] != 'Pending'"
rep | js "r = [r for r in d['receipts'] if r['id'] == $LR][0]
sys.exit(0 if r['result'] == 'Helped' and r['met'] and not r['undone'] and r['after'] <= 3 and r['improvement_pct'] > 80 else 1)" ||
	fail "the laptop's fix: helped: $(agctl -j repair)"
[ "$(decision "$LD")" = active ] || fail "a fix that helped stays: $(agctl decisions)"
grep -q "allow wlan0 $LAPTOP" "$T/hostapd.log" && fail "a fix that helped must not be undone"
echo "   receipt $LR: $(rep | js "print([r for r in d['receipts'] if r['id'] == $LR][0]['summary'])")"

echo "== enforce mode: wlan0 36 -> 44, the devices still lose 20%: no change, undone, back on 36"
probe "RadioLoss wlan0 20"
alert wifi-noise wlan0:44 "noise 1"
wait_rep 15 "$(rcpt "r['action'] == 'channel' and r['mode'] == 'applied'")"
CR=$(rep | js "print($(rcpt "r['action'] == 'channel'")[0]['id'])")
CD=$(rep | js "print([r for r in d['receipts'] if r['id'] == $CR][0]['decision'])")
grep -q "switch wlan0 44" "$T/hostapd.log" || fail "the channel switched to 44: $(cat "$T/hostapd.log")"
wait_rep 20 "[r for r in d['receipts'] if r['id'] == $CR][0]['result'] != 'Pending'"
rep | js "r = [r for r in d['receipts'] if r['id'] == $CR][0]
sys.exit(0 if r['result'] == 'NoChange' and r['undone'] and r['undone_by'] == 'proven repair' and 'back' in r['summary'] else 1)" ||
	fail "the channel: no change, undone: $(agctl -j repair)"
grep -q "switch wlan0 36" "$T/hostapd.log" || fail "back on channel 36: $(cat "$T/hostapd.log")"
agctl -j wifi | js "a = [r for r in d['radios'] if r['iface'] == 'wlan0'][0]; sys.exit(0 if a['channel'] == 36 else 1)" ||
	{ sleep 6; agctl -j wifi | js "a = [r for r in d['radios'] if r['iface'] == 'wlan0'][0]; sys.exit(0 if a['channel'] == 36 else 1)"; } ||
	fail "agd sees channel 36 again (exactly the state before): $(agctl wifi)"
[ "$(decision "$CD")" = tried ] || fail "the decision: tried (undone, not by a person): $(agctl decisions)"
sleep 1
[ "$(events 'RepairUndone!')" = 1 ] || fail "RepairUndone! once: $(cat "$T/events.log")"
echo "   receipt $CR: $(rep | js "print([r for r in d['receipts'] if r['id'] == $CR][0]['summary'])")"

echo "== enforce mode: the phone leaves while its fix is measured: unknown, not undone"
probe "DeviceLoss $PHONE 25"
alert wifi-trouble 10.95.0.41 "phone 1"
wait_rep 15 "$(rcpt "r['action'] == 'stay24' and r['before'] == 25")"
PR=$(rep | js "print($(rcpt "r['before'] == 25")[0]['id'])")
PD=$(rep | js "print([r for r in d['receipts'] if r['id'] == $PR][0]['decision'])")
probe "DeviceLoss $PHONE gone"
wait_rep 20 "[r for r in d['receipts'] if r['id'] == $PR][0]['result'] != 'Pending'"
rep | js "r = [r for r in d['receipts'] if r['id'] == $PR][0]
sys.exit(0 if r['result'] == 'Unknown' and not r['undone'] and 'left' in r['summary'] else 1)" || fail "the phone: unknown: $(agctl -j repair)"
[ "$(decision "$PD")" = active ] || fail "unknown: not undone: $(agctl decisions)"

echo "== the evidence: by fault and kind of fix, agctl, the data model"
agctl repair
rep | js "
s = {(x['fault'], x['action']): x['month'] for x in d['stats']}
a, b = s[('wifi-trouble', 'stay24')], s[('noise', 'channel')] if ('noise', 'channel') in s else s[('wifi-noise', 'channel')]
w = d['summary']['week']
ok = a['helped'] == 1 and a['unknown'] == 1 and a['fix_rate'] == 100 and a['shadows'] == 1 and a['shadows_needed'] == 1
ok = ok and b['no_change'] == 1 and b['undone'] == 1 and b['fix_rate'] == 0 and b['undo_rate'] == 100
ok = ok and w['fixes'] == 3 and w['judged'] == 2 and w['fix_rate'] == 50
sys.exit(0 if ok else 1)" || fail "the stats: $(agctl -j repair --stats)"
agctl repair | grep -q "fix rate 50%" || fail "agctl repair says the fix rate: $(agctl repair)"
agctl -j repair --shadow | js "sys.exit(0 if d['receipts'] and all(r['mode'] == 'shadow' for r in d['receipts']) else 1)" ||
	fail "agctl repair --shadow"
agctl care | grep -q "repairs   last 7 days: 3 fixes: 1 helped" || fail "agctl care's line: $(agctl care)"
agctl why --last 10m | grep -q "AIGW's fixes then" || fail "agctl why's lines: $(agctl why --last 10m)"
agctl why --last 10m | grep -q "AIGW's fix judged" || fail "the timeline has the verdicts: $(agctl why --last 10m)"
"$UBUS" -s "$AIGW_UBUS" call ag.dm get '{"paths":["Device.X_AIGW_.Repair."],"origin":"usp"}' > "$T/dm.json"
js "v = d['values']; p = 'Device.X_AIGW_.Repair.'
ok = v[p + 'Enable'] == 'true' and v[p + 'Undo'] == 'true' and v[p + 'Gate'] == 'false' and v[p + 'WeekFixes'] == '3'
ok = ok and v[p + 'WeekFixRate'] == '50' and v[p + 'WeekUndone'] == '1' and int(v[p + 'ReceiptNumberOfEntries']) == 4
ok = ok and v[p + 'Receipt.$CR.Result'] == 'NoChange' and v[p + 'Receipt.$CR.Undone'] == 'true' and v[p + 'Receipt.$CR.Mode'] == 'Applied'
ok = ok and v[p + 'Receipt.$LR.Result'] == 'Helped' and v[p + 'Receipt.$LR.PredictionMet'] == 'true' and v[p + 'Receipt.$LR.Before'] == '30.0'
ok = ok and v[p + 'Receipt.$SH.Mode'] == 'Shadow' and v[p + 'Receipt.$SH.Result'] == 'Needed' and v[p + 'Receipt.$LR.Judged'].endswith('Z')
ok = ok and not [k for k in v if 'sara-laptop' in v[k] or 'living-room-tv' in v[k]]      # the ISP's view: no names
ok = ok and int(v[p + 'StatsNumberOfEntries']) == 2
sys.exit(0 if ok else 1)" < "$T/dm.json" || fail "Device.X_AIGW_.Repair.: $(cat "$T/dm.json")"
"$UBUS" -s "$AIGW_UBUS" call ag.dm schema | js "e = [x['path'] for x in d['events']]
sys.exit(0 if tuple(map(int, d['version'].split('.'))) >= (1, 24) and 'Device.X_AIGW_.Repair!' in e and 'Device.X_AIGW_.RepairUndone!' in e else 1)" ||
	fail "data model 1.24 or later with Repair! and RepairUndone!"
python3 - "$T/events.log" <<EOF || fail "Repair! events: $(cat "$T/events.log")"
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1]).read().splitlines() if 'Repair!' in l]
a = [e.get('ag.dm.event', e)['args'] for e in ev]
x = [e for e in a if e['Receipt'] == '$LR']
ok = x and x[0]['Result'] == 'Helped' and x[0]['Mode'] == 'Applied' and x[0]['PredictionMet'] == 'true' and x[0]['Metric'] == 'DeviceLoss'
ok = ok and not any('sara-laptop' in json.dumps(e) for e in a)
sys.exit(0 if ok else 1)
EOF

echo "== a restart: the receipts and the evidence kept"
N0=$(rep | js "print(len(d['receipts']))")
stop_agd
python3 -c "import json; r = json.load(open('$T/state.json'))['repair']; assert len(r['receipts']) == $N0 and len(r['rows']) == 2" ||
	fail "the state file has the receipts"
start_agd
rep | js "w = d['summary']['week']; sys.exit(0 if len(d['receipts']) == $N0 and w['fixes'] == 3 and w['fix_rate'] == 50 and
         [r for r in d['receipts'] if r['id'] == $CR and r['undone'] and r['result'] == 'NoChange'] else 1)" ||
	fail "after a restart: $(agctl -j repair)"

echo "== the confidence gate (80% over 2 trials): neither the channel (0 of 1) nor the band (1 of 1) acts by itself"
stop_agd
config enforce 1
start_agd
probe "RadioLoss wlan1 20" "DeviceLoss $TV 26"
alert wifi-noise wlan1:11 "noise 2"
sleep 3
agctl -j decisions | js "x = [k for k in d['decisions'] if k['action'] == 'channel' and k['target'] == 'wlan1:11']
sys.exit(0 if x and x[0]['state'] == 'proposed' and 'not proven here yet' in x[0]['note'] else 1)" ||
	fail "the gate holds the channel: $(agctl decisions)"
grep -q "switch wlan1" "$T/hostapd.log" && fail "held by the gate: nothing applied"
alert wifi-trouble 10.95.0.43 "tv 2"
sleep 3
agctl -j decisions | js "x = [k for k in d['decisions'] if k['action'] == 'stay24' and k['state'] == 'proposed']
sys.exit(0 if x and 'measured on 1 of the 2 trials' in x[0]['note'] else 1)" || fail "the gate holds the band: $(agctl decisions)"
rep | js "x = [s for s in d['stats'] if s['action'] == 'channel']; sys.exit(0 if x and x[0]['gate'] == 'held' else 1)" ||
	fail "the stats say the gate holds it"
echo "   $(agctl -j decisions | js "print([k for k in d['decisions'] if k['target'] == 'wlan1:11'][0]['note'])")"

echo "== bounded: 3 receipts at most (AIGW_TEST_REPAIR_MAX), the newest kept"
NEWEST=$(rep | js "print(max(r['id'] for r in d['receipts']))")
stop_agd
AIGW_TEST_REPAIR_MAX=3
export AIGW_TEST_REPAIR_MAX
start_agd
rep | js "ids = [r['id'] for r in d['receipts']]; sys.exit(0 if len(ids) == 3 and $NEWEST in ids else 1)" ||
	fail "3 receipts, the newest: $(agctl -j repair)"
stop_agd
echo "REPAIR TEST OK"
