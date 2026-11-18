#!/bin/sh
# Area outages on the gateway, end to end: a private cortex and three homes
# (each its own ubusd, agd and bridge plug-in, reporting every 2 s), plus a
# tiny test plug-in ("nudge") that raises a Wi-Fi alert or sends its own area
# notice when the test asks.
#
#   uplink     home A has its uplink id in its config, home B gets it from the
#              data model (Device.X_AIGW_.AreaOutage.UplinkID, as an ACS would
#              write it; its config had no area section), home C is on another
#              uplink; the bridges send it, cortex groups by it
#   found      four more homes on A's uplink (simulated, over cortex's HTTP API)
#              lose packets within minutes: cortex opens an incident and tells
#              them in its answer; A and B are fine, so not part of it, and their
#              brief at cortex notes the outage nearby
#   declared   the NOC declares an outage on the uplink: within a few seconds A
#              and B show it (AreaOutage.Status Active, AreaOutage! with the
#              incident), C doesn't; A's own brief says "known outage in your
#              area", no visit, nothing to fix; a fix AIGW would make (keeping a
#              device on 2.4 GHz) is held, and HeldFixes says so
#   over       the NOC ends it: AreaOutageCleared! with what was held; the same
#              fix goes ahead again
#   lapse      home C's bridge doesn't pass notices on (bridge area 0); a notice
#              that isn't renewed lapses by itself
#
# Needs root (private ubusd, plug-ins run as nobody), python3, curl, ubus.
#
#   sudo tests/area-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
PORT=$((20000 + R % 20000))
PIDS=""
HOMES="hA hB hC"

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for H in $HOMES; do [ -f "$T/$H/agd.pid" ] && kill "$(cat "$T/$H/agd.pid")" 2>/dev/null || true; done
	sleep 0.5; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() {
	echo "AREA TEST FAIL: $*" >&2; echo "--- logs" >&2
	tail -n 25 "$T"/cortex.log "$T"/h*/agd.log >&2 2>/dev/null || true
	exit 1
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }
h() { H=$1; shift; AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" "$BIN/agctl/agctl" "$@"; }
dmv() { h "$1" dm get "Device.X_AIGW_.AreaOutage.$2" 2>/dev/null | sed 's/^[^=]*= //'; }
nev() { grep -c "$2" "$T/$1/events.log" 2>/dev/null || true; }
# ev <home> <event, e.g. AreaOutage!> <python expression over a (its args)>: one such event matches
ev() {
	python3 - "$T/$1/events.log" "Device.X_AIGW_.$2" "$3" <<'EOF'
import json, sys
for line in open(sys.argv[1]):
    try:
        e = json.loads(line).get("ag.dm.event", {})
    except ValueError:
        continue
    a = e.get("args", {})
    if e.get("event") == sys.argv[2] and eval(sys.argv[3]):
        sys.exit(0)
sys.exit(1)
EOF
}
# wait_for <seconds> <message> <shell condition>
wait_for() {
	i=0
	until eval "$3"; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$2"; sleep 0.5
	done
}
# cx <python expression over s (the store) and outage>
cx() {
	PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$1" <<'EOF'
import sys
from aigw_cortex import outage
from aigw_cortex.store import Store
s = Store(sys.argv[1])
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
start_agd() {
	( export AIGW_UCI_CONFDIR="$T/$1/config" AIGW_UBUS="$T/$1/ubus.sock" AIGW_NFT=/bin/true \
		AIGW_TEST_AREA_HOLD_MIN=1
	  exec "$BIN/agd/agd" >> "$T/$1/agd.log" 2>&1 ) &
	echo $! > "$T/$1/agd.pid"
}

mkdir -p "$T/sys/bridge" "$T/sys/nudge" "$T/cortex"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"
# the test's own plug-in: on a file in its state directory, an alert AIGW would fix, or an area notice
cat > "$T/sys/nudge/manifest.json" <<'EOF'
{ "name": "nudge", "kind": "sensor", "version": "1.0.0", "description": "area-test: raises a Wi-Fi alert on request",
  "exec": "run.sh", "alerts": ["wifi-hopping"], "caps": ["area.notice"], "hosts": ["linux", "openwrt"],
  "budget": { "ram_kb": 8192, "cpu_pct": 5 } }
EOF
cat > "$T/sys/nudge/run.sh" <<'EOF'
#!/bin/sh
n=0
while :; do
	if [ -f "$AIGW_STATE/alert" ]; then
		rm -f "$AIGW_STATE/alert"; n=$((n + 1))
		echo "{\"alert\": {\"kind\": \"wifi-hopping\", \"device\": \"10.99.0.$n\", \"subject\": \"5 GHz edge $n\", \"reason\": \"area-test\"}}"
	fi
	if [ -f "$AIGW_STATE/notice" ]; then
		rm -f "$AIGW_STATE/notice"
		echo '{"area": {"incident": 77, "kind": "SLOW", "since": 1, "homes": 3, "of": 9, "confidence": "medium", "uplink": "OLT-T/2", "hold_s": 2}}'
	fi
	sleep 0.3
done
EOF
chmod 755 "$T/sys/nudge/run.sh"

echo "== cortex, and three homes: A and B on uplink OLT-T/1, C on OLT-T/2"
cortex serve --port "$PORT" --quiet > "$T/cortex.log" 2>&1 &
PIDS="$PIDS $!"
for H in $HOMES; do
	mkdir -p "$T/$H/config" "$T/$H/user"
	case $H in
	hA) AREA="config area 'area'
	option uplink_id 'OLT-T/1'" ;;
	hB) AREA="" ;;                  # no area section: the data model makes it
	hC) AREA="config area 'area'
	option uplink_id 'OLT-T/2'" ;;
	esac
	cat > "$T/$H/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/$H/leases'
	option state_file '$T/$H/state.json'
	option model_dir '$REPO/models'

config lan 'lan'
	option iface 'aigw-none0'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/$H/hostapd'
	option keys '$T/$H/wifi-keys'

config care 'care'
	option enabled '0'

config why 'why'
	option enabled '0'

config act 'act'
	option dns_policy '$T/$H/dns-policy'
	option mode 'advise'

config policy 'wifi_hopping'
	option alert 'wifi-hopping'
	option action 'stay24'
	option ttl '86400'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/$H/user'
	option user 'nobody'
	option state_dir '$T/$H/pstate'

config models 'models'
	option dir '$T/$H/models'
	option keys '$T/$H/keys.d'

config plugin 'bridge'
	option interval '2'
	option devices 'anonymous'
$( [ $H = hC ] && printf "\toption area '0'" || true )

$AREA
EOF
	: > "$T/$H/leases"
	"$UBUSD" -s "$T/$H/ubus.sock" > "$T/$H/ubusd.log" 2>&1 & PIDS="$PIDS $!"
	sleep 0.3
	start_agd $H
	TOKEN=$(cortex enroll "$H" 2>> "$T/enroll.log")
	sleep 1
	"$UBUS" -s "$T/$H/ubus.sock" listen ag.dm.event > "$T/$H/events.log" 2>&1 & PIDS="$PIDS $!"
	h $H bridge "http://127.0.0.1:$PORT" > /dev/null || fail "$H: agctl bridge"
	echo "$TOKEN" | h $H bridge token > /dev/null || fail "$H: bridge token"
done

echo "== the uplink: from the config (A), from the data model as an ACS writes it (B)"
h hB dm set Device.X_AIGW_.AreaOutage.UplinkID OLT-T/1 > "$T/set.txt" 2>&1 || fail "dm set UplinkID: $(cat "$T/set.txt")"
[ "$(dmv hB UplinkID)" = OLT-T/1 ] || fail "UplinkID should read back: $(dmv hB UplinkID)"
grep -q "uplink_id 'OLT-T/1'" "$T/hB/config/aigw" || fail "UplinkID should be saved (an area section made): $(cat "$T/hB/config/aigw")"
h hB dm set Device.X_AIGW_.AreaOutage.UplinkID "a,b" > /dev/null 2>&1 && fail "a comma in UplinkID should be refused"
for H in $HOMES; do
	[ "$(dmv $H Status)" = None ] || fail "$H: no area outage yet: $(dmv $H Status)"
done
wait_for 20 "cortex should group A and B by OLT-T/1, C by OLT-T/2: $(cortex gateways)" \
	"cx \"{h['name']: h['uplink'] for h in outage._homes(s).values()} == {'hA': 'OLT-T/1', 'hB': 'OLT-T/1', 'hC': 'OLT-T/2'}\""
cx "'wan_gateway' in s.snapshot('hA')['status'] and 'bng' in s.snapshot('hA')['status'] and s.snapshot('hA')['interval'] == 2" ||
	fail "the bridge should send its next hop, its BNG and its interval"

echo "== four more homes on OLT-T/1 (simulated) lose packets within minutes: an incident, without A and B"
for S in s1 s2 s3 s4; do cortex enroll $S 2>> "$T/enroll.log" > "$T/$S.token"; done
PYTHONPATH="$REPO/cortex" python3 - "$PORT" "$T" > "$T/sim.txt" <<'EOF' || fail "simulated homes: $(cat "$T/sim.txt")"
import json, sys, time, urllib.request
port, t = sys.argv[1], sys.argv[2]
now = int(time.time())
for k, s in enumerate(("s1", "s2", "s3", "s4")):
    tok = open(f"{t}/{s}.token").read().strip()
    body = {"bridge": "0.39.0", "interval": 300, "status": {"version": "0.39.0", "uplink_id": "OLT-T/1"},
            "care": {"status": "problem", "problem": {"code": "ISP_LOSS", "area": "Internet", "since": now - 300 + k * 40,
                                                       "reported": True}}}
    r = urllib.request.Request(f"http://127.0.0.1:{port}/v1/report", data=json.dumps(body).encode(),
                               headers={"Authorization": f"Bearer {tok}"})
    print(s, json.loads(urllib.request.urlopen(r).read())["area"])
EOF
grep -q "^s1 {'incident': 0}" "$T/sim.txt" && grep -q "^s3 {'incident': [1-9]" "$T/sim.txt" ||
	fail "the third home should open the incident: $(cat "$T/sim.txt")"
FOUND=$(sed -n "s/^s4 {'incident': \([0-9]*\).*/\1/p" "$T/sim.txt")
cx "[(o['id'], o['homes'], o['homes_in_area'], o['kind']) for o in outage.incidents(s, open_only=True)] == [($FOUND, 4, 6, 'LOSS')]" ||
	fail "one incident: 4 of 6 homes on OLT-T/1: $(cortex outages)"
sleep 3
[ "$(dmv hA Status)" = None ] || fail "home A is fine: not part of it"
cortex brief hA --json | python3 -c "import json,sys; b=json.load(sys.stdin); sys.exit(0 if 'area_note' in b and 'area_outage' not in b else 1)" ||
	fail "A's brief at cortex should note the outage nearby: $(cortex brief hA)"

echo "== the NOC declares an outage on OLT-T/1: A and B are told within seconds, C isn't"
cortex outages declare OLT-T/1 --kind LOSS --note "OLT card swap" > "$T/declare.txt" || fail "declare: $(cat "$T/declare.txt")"
ID=$(cortex outages --json | python3 -c "import json,sys; print([o['id'] for o in json.load(sys.stdin) if o['declared']][0])")
for H in hA hB; do
	wait_for 15 "$H should show the area outage: $(h $H dm get Device.X_AIGW_.AreaOutage.)" "[ \"\$(dmv $H Status)\" = Active ]"
	[ "$(dmv $H Incident)" = "$ID" ] && [ "$(dmv $H Kind)" = LOSS ] && [ "$(dmv $H Uplink)" = OLT-T/1 ] &&
		[ "$(dmv $H Homes)" = 6 ] && [ "$(dmv $H HomesInArea)" = 6 ] || fail "$H: AreaOutage.: $(h $H dm get Device.X_AIGW_.AreaOutage.)"
	wait_for 5 "$H: AreaOutage! event" "[ \"\$(nev $H 'AreaOutage!')\" -ge 1 ]"
	ev $H 'AreaOutage!' "a['Incident'] == '$ID' and a['Kind'] == 'LOSS' and a['Uplink'] == 'OLT-T/1' and a['Homes'] == '6'" ||
		fail "$H: AreaOutage! should carry the incident: $(cat "$T/$H/events.log")"
done
[ "$(nev hA 'AreaOutage!')" = 1 ] || fail "AreaOutage! once, not with every report: $(nev hA 'AreaOutage!')"
grep -q "area outage $ID on OLT-T/1: told agd" "$T/hA/agd.log" || fail "the bridge should say it told agd"
h hA -j status | python3 -c "import json,sys; a=json.load(sys.stdin)['area']; sys.exit(0 if a['status'] == 'active' and a['incident'] == $ID else 1)" ||
	fail "agctl status: $(h hA -j status)"
sleep 3
[ "$(dmv hC Status)" = None ] || fail "home C is on another uplink"

echo "== A's own brief: a known outage in the area, no visit, nothing to fix in the home"
h hA -j brief --no-check > "$T/brief.json" || fail "agctl brief: $(cat "$T/brief.json")"
python3 - "$T/brief.json" <<'EOF' || fail "A's brief: $(cat "$T/brief.json")"
import json, sys
b = json.load(open(sys.argv[1]))
b = b.get("brief", b)
ok = (b["fault"] == "LINE" and b["code"] == "AREA_OUTAGE" and b["verdict"].startswith("Known outage in the area")
      and b["visit"] == "No" and not b["fix"]["possible"] and b["evidence"][0]["source"] == "area")
sys.exit(0 if ok else 1)
EOF

echo "== a fix AIGW would make (keep a device on 2.4 GHz) is held during the outage"
touch "$T/hA/pstate/nudge/alert"
wait_for 10 "the stay24 fix should be held: $(grep -i stay24 "$T/hA/agd.log")" \
	"grep -q 'no stay24 during the area outage' '$T/hA/agd.log'"
[ "$(dmv hA HeldFixes)" = Stay24 ] || fail "HeldFixes: $(dmv hA HeldFixes)"
h hA -j decisions | grep -q stay24 && fail "no stay24 decision during the outage: $(h hA decisions)"

echo "== the NOC ends it: AreaOutageCleared!, and the fix goes ahead again"
cortex outages end "$ID" > /dev/null || fail "outages end"
for H in hA hB; do
	wait_for 15 "$H: the outage should be over: $(h $H dm get Device.X_AIGW_.AreaOutage.)" "[ \"\$(dmv $H Status)\" = None ]"
	wait_for 5 "$H: AreaOutageCleared!" "[ \"\$(nev $H 'AreaOutageCleared!')\" -ge 1 ]"
	[ "$(dmv $H LastIncident)" = "$ID" ] || fail "$H: LastIncident: $(dmv $H LastIncident)"
done
ev hA 'AreaOutageCleared!' "a['Incident'] == '$ID' and a['HeldFixes'] == 'Stay24' and 'cortex says it is over' in a['Reason']" ||
	fail "AreaOutageCleared! should say what was held, and why it is over: $(grep Cleared "$T/hA/events.log")"
HELD=$(grep -c 'no stay24 during the area outage' "$T/hA/agd.log")
touch "$T/hA/pstate/nudge/alert"
wait_for 10 "after the outage the alert should be decided on again: $(tail -5 "$T/hA/agd.log")" \
	"grep 'policy wifi_hopping' '$T/hA/agd.log' | grep -v -q 'during the area outage' || h hA -j decisions | grep -q stay24"
sleep 1
[ "$(grep -c 'no stay24 during the area outage' "$T/hA/agd.log")" = "$HELD" ] || fail "nothing held once it is over"

echo "== C's bridge passes no notices on (bridge area 0); a notice not renewed lapses by itself"
cortex outages declare OLT-T/2 --kind DOWN > /dev/null || fail "declare OLT-T/2"
sleep 5
[ "$(dmv hC Status)" = None ] || fail "bridge area 0: C should not be told"
touch "$T/hC/pstate/nudge/notice"
wait_for 10 "C: a notice from a plug-in: $(h hC dm get Device.X_AIGW_.AreaOutage.)" "[ \"\$(dmv hC Incident)\" = 77 ]"
wait_for 10 "C: the notice should lapse (hold_s 2): $(h hC dm get Device.X_AIGW_.AreaOutage.)" \
	"[ \"\$(dmv hC Status)\" = None ] && [ \"\$(nev hC 'AreaOutageCleared!')\" -ge 1 ]"
ev hC 'AreaOutageCleared!' "a['Incident'] == '77' and 'lapsed' in a['Reason']" ||
	fail "C: lapsed, it should say: $(cat "$T/hC/events.log")"

echo "== the fleet report and the outages list"
cortex outages --all > "$T/outages.txt"
cat "$T/outages.txt"
grep -q "Area outage $ID, declared (OLT card swap): the 6 homes on OLT-T/1 losing packets" "$T/outages.txt" ||
	fail "the declared outage in words"
grep -q "Area outage $FOUND: 4 of 6 homes on OLT-T/1 losing packets" "$T/outages.txt" || fail "the found outage in words"
echo "AREA TEST OK"
