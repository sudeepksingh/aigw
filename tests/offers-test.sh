#!/bin/sh
# Signals for the ISP, end to end: agd starts with a week of the home's days
# in its state (as it would have counted them from its timeline), with a fake
# hostapd for the devices' names.
#
#   the week      the line full 5 evenings of 7 (the line's speed sold: 50
#                 Mbit/s), a laptop and a TV on a weak signal for hours:
#                 PLAN_UPGRADE (high) and MESH_WIFI, with the evidence
#   names         agctl offers names the laptop and the TV; the data model and
#                 the event count devices, without names
#   the ISP       OfferFound! once for each, not again after a restart;
#                 Device.X_AIGW_.Offers.
#   share 0       the signals stay home: the data model has none, agctl still
#                 shows them
#
# Needs root (private ubusd) and python3.
#
#   sudo tests/offers-test.sh [build-dir]
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
fail() { echo "OFFERS TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/agd.log "$T"/events.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

LAPTOP=02:00:00:00:99:01
TV=02:00:00:00:99:02

config() {   # config <share>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15359'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config care 'care'
	option enabled '0'
	option speed_mbps '50'

config why 'why'
	option interval '2'
	option hours '1'
	option probe '0'
	option file '$T/why.bin'

config offers 'offers'
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
	list iface 'lo'
EOF
}
config 1
printf '1999999999 %s 10.96.0.31 laptop *\n1999999999 %s 10.96.0.32 tv *\n' "$LAPTOP" "$TV" > "$T/leases"

echo "== a week of the home's days, as agd counted them"
python3 - "$T/state.json" "$LAPTOP" "$TV" <<'EOF'
import json, sys, time
path, laptop, tv = sys.argv[1:4]
today = int(time.time()) // 86400
days = []
for k in range(7):
    d = {"day": today - k, "watched_s": 86400, "full_s": 0, "full_evening_s": 0, "full_slow_s": 0,
         "peak_kbps": 20000, "drops": 2, "weak_s": 0, "weak": []}
    if k < 5:                                   # full 5 evenings: 1 h 40 min, 90% of it in the evening
        d.update(full_s=6000, full_evening_s=5400, full_slow_s=600, peak_kbps=47000)
    if k < 6:                                   # the laptop 4 h, the TV 2 h on a weak signal
        d["weak"] = [{"mac": laptop, "s": 4 * 3600}, {"mac": tv, "s": 2 * 3600}]
        d["weak_s"] = 6 * 3600
    days.append(d)
json.dump({"offers": {"days": days}}, open(path, "w"))
EOF

export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
dmget() { agctl dm get "$1" | sed 's/^[^=]*= //'; }
found() {   # OfferFound! events so far, one code per line
	python3 - "$T/events.log" <<'EOF'
import json, sys
for line in open(sys.argv[1]):
    try:
        e = json.loads(line).get("ag.dm.event", {})
    except ValueError:
        continue
    if e.get("event") == "Device.X_AIGW_.OfferFound!":
        print(e["args"]["Code"], e["args"]["Confidence"], "|", e["args"]["Evidence"])
EOF
}

echo "== the signals"
i=0
until agctl -j offers | js "sys.exit(0 if 'laptop' in json.dumps(d) else 1)" 2>/dev/null; do
	i=$((i + 1)); [ $i -le 20 ] || fail "the devices' names (leases): $(agctl -j offers)"; sleep 0.5
done
agctl offers
agctl -j offers | js "
s = {x['code']: x for x in d['signals']}
p, m = s['PLAN_UPGRADE'], s['MESH_WIFI']
ok = d['days_watched'] == 7 and not d['learning'] and p['active'] and p['confidence'] == 'high' and p['days'] == 5
ok = ok and '47 of 50 Mbit/s' in p['evidence'] and '90% of it in the evening' in p['evidence']
ok = ok and m['active'] and '2 devices' in m['evidence'] and m['devices'] == 'laptop 4 h a day, tv 2 h a day'
ok = ok and not s['SMART_QUEUE']['active'] and not s['RETENTION_RISK']['active'] and not s['LINE_REPAIR']['active']
sys.exit(0 if ok else 1)" || fail "PLAN_UPGRADE and MESH_WIFI: $(agctl -j offers)"

echo "== the ISP: OfferFound! and Device.X_AIGW_.Offers., without names"
i=0
until [ "$(found | grep -c .)" -ge 2 ]; do
	i=$((i + 1)); [ $i -le 20 ] || fail "OfferFound! for each: $(cat "$T/events.log")"; sleep 0.5
done
found
found | grep -q '^PLAN_UPGRADE high' && found | grep -q '^MESH_WIFI ' || fail "the events' codes"
found | grep -q 'laptop' && fail "an event must not name a device"
[ "$(dmget Device.X_AIGW_.Offers.Signals)" = "PLAN_UPGRADE,MESH_WIFI" ] || fail "Offers.Signals: $(agctl dm get Device.X_AIGW_.Offers.)"
[ "$(dmget Device.X_AIGW_.Offers.PlanUpgrade.Confidence)" = high ] && [ "$(dmget Device.X_AIGW_.Offers.PlanUpgrade.Days)" = 5 ] ||
	fail "Offers.PlanUpgrade.: $(agctl dm get Device.X_AIGW_.Offers.PlanUpgrade.)"
dmget Device.X_AIGW_.Offers.MeshWiFi.Evidence | grep -q '2 devices' || fail "Offers.MeshWiFi.Evidence"
dmget Device.X_AIGW_.Offers.MeshWiFi.Evidence | grep -q laptop && fail "the data model must not name a device"
[ "$(dmget Device.X_AIGW_.Offers.SmartQueue.Active)" = false ] || fail "SmartQueue not active"

echo "== today is counted from the timeline's samples (one every 2 s)"
w0=$(agctl -j offers | js "print(d['today']['watched_s'])")
sleep 5
w1=$(agctl -j offers | js "print(d['today']['watched_s'])")
[ "$w1" -ge $((w0 + 4)) ] || fail "today's watched time should grow: $w0 then $w1"
echo "   watched today: $w0 s, then $w1 s"

echo "== told once: not again after a restart"
stop_agd
start_agd
sleep 3
[ "$(found | grep -c .)" = 2 ] || fail "OfferFound! again after a restart: $(found)"
agctl -j offers | js "sys.exit(0 if [x['code'] for x in d['signals'] if x['active']] == ['PLAN_UPGRADE', 'MESH_WIFI'] else 1)" ||
	fail "the days survive a restart: $(agctl -j offers)"

echo "== offers.share 0: the signals stay home"
stop_agd
config 0
start_agd
[ "$(dmget Device.X_AIGW_.Offers.Signals)" = "" ] && [ "$(dmget Device.X_AIGW_.Offers.PlanUpgrade.Active)" = false ] &&
	[ "$(dmget Device.X_AIGW_.Offers.Shared)" = false ] || fail "share 0: $(agctl dm get Device.X_AIGW_.Offers.)"
agctl offers | grep -q "doesn't see them" || fail "agctl offers with share 0"
agctl -j offers | js "sys.exit(0 if any(x['active'] for x in d['signals']) else 1)" || fail "still shown at home"
echo "OFFERS TEST OK"
