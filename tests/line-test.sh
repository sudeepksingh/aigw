#!/bin/sh
# Line health and area fault prediction on the gateway, end to end (docs/area-prediction.md):
# a private cortex and one home (its own ubusd, agd and bridge plug-in, reporting every 2 s),
# with a test clock where a "day" lasts 2 s (AIGW_TEST_LINE_DAY_S) and a fake WAN handler.
#
#   learning   the first days: Learning, no event
#   drifting   the fake DSL line's noise margin slides 0.5 dB a "day" and its CRC errors grow:
#              agctl line says Drifting, with the days to the fault; LineHealth. and its Metric.{i}
#              say so; LineDegrading! once; the bridge sends the line health vector to cortex
#   plant      five more homes on the same cabinet (simulated, over cortex's HTTP API) slide the
#              same days: cortex predicts the plant and its answer reaches agd: agctl line and
#              LineHealth.SharedPlant* show it
#   offers     no faster plan on a degrading line: LINE_REPAIR, with the line's words
#   restart    agd restarts: the days come back, no second LineDegrading!
#   migration  a care state saved by 0.53 with the "-1 Mbit/s" early warning: read back as 0 or more
#   share 0    with line.share 0 the bridge sends no vector
#
# Needs root (private ubusd, plug-ins run as nobody), python3, curl, ubus.
#
#   sudo tests/line-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" areapredict plugins
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
PORT=$((20000 + R % 20000))
PIDS=""
H=h1

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -f "$T/agd.pid" ] && kill "$(cat "$T/agd.pid")" 2>/dev/null || true
	sleep 0.5; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() {
	echo "LINE TEST FAIL: $*" >&2; echo "--- logs" >&2
	tail -n 30 "$T"/cortex.log "$T"/agd.log >&2 2>/dev/null || true
	exit 1
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }
h() { AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" "$BIN/agctl/agctl" "$@"; }
dmv() { h dm get "Device.X_AIGW_.LineHealth.$1" 2>/dev/null | sed 's/^[^=]*= //'; }
nev() { grep -c "$1" "$T/events.log" 2>/dev/null || true; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
wait_for() {
	i=0
	until eval "$3"; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$2"; sleep 0.5
	done
}
start_agd() {
	( export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true AIGW_TEST_LINE_DAY_S=2
	  exec "$BIN/agd/agd" >> "$T/agd.log" 2>&1 ) &
	echo $! > "$T/agd.pid"
}

mkdir -p "$T/sys/bridge" "$T/cortex" "$T/config" "$T/user"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"
# the fake WAN handler: a DSL line, steady for 6 "days" from its first call, then the noise margin
# 0.5 dB worse a day, and CRC errors growing
cat > "$T/line-stats" <<'EOF'
#!/bin/sh
now=$(date +%s); f=/var/tmp/line-test-start.$PPID
[ -f "$T0" ] || echo "$now" > "$T0"
start=$(cat "$T0"); d=$(( (now - start) / 2 ))
python3 - "$d" "$now" "$start" <<'PY'
import sys
d, now, start = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
m = 10.0 - (0.5 * (d - 6) if d > 6 else 0)
crc = 5 * (now - start) + (40 * (now - start) * max(0, d - 6))
print('{"tech": "dsl", "down_kbps": 80000, "up_kbps": 20000, "margin_db": %.2f, "fec": %d, "crc": %d, "uptime_s": %d}'
      % (m, 100 * (now - start), crc, 1000 + now - start))
PY
EOF
sed -i "s|\$T0|$T/handler-start|g" "$T/line-stats"
chmod 755 "$T/line-stats"

# a care state saved by 0.53: the speed's warning said "-1 Mbit/s" (77 numbers a metric, no level)
NOW=$(date +%s)
python3 - "$T/state.json" "$NOW" <<'EOF'
import json, sys
now = int(sys.argv[2])
rows = []
for m in range(5):
    if m == 4:      # the speed: normal 95, how much worse lately 96 (the old bug's cause)
        rows.append([95, 2, 14, 9, 96] + [95, 2, 14] * 24)
    else:
        rows.append([0] * 77)
state = {"care": {"learn": {"started": now - 10 * 86400, "metrics": rows,
                            "warning": {"metric": 4, "kind": 0, "since": now - 3600, "normal": 95, "current": -1,
                                        "per_day": 0, "days": 0, "predict": -1, "became": 0}}}}
json.dump(state, open(sys.argv[1], "w"))
EOF

cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config lan 'lan'
	option iface 'aigw-none0'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config care 'care'
	option enabled '1'
	option interval '86400'

config act 'act'
	option dns_policy '$T/dns-policy'
	option mode 'advise'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/user'
	option user 'nobody'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config plugin 'bridge'
	option interval '2'
	option devices 'none'

config line 'line'
	option interval '1'
	option handler '$T/line-stats'
EOF
: > "$T/leases"

echo "== cortex, and a home on cabinet olt-T/cab-1"
cortex serve --port "$PORT" --quiet > "$T/cortex.log" 2>&1 &
PIDS="$PIDS $!"
"$UBUSD" -s "$T/ubus.sock" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
start_agd
TOKEN=$(cortex enroll "$H" --group olt-T/cab-1 2>> "$T/enroll.log")
sleep 1
"$UBUS" -s "$T/ubus.sock" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
h bridge "http://127.0.0.1:$PORT" > /dev/null || fail "agctl bridge"
echo "$TOKEN" | h bridge token > /dev/null || fail "bridge token"

echo "== the -1 Mbit/s warning of 0.53, read back"
h -j care > "$T/care.json" || fail "agctl care"
js "w=d['early_warning']; sys.exit(0 if w['status'] == 'warning' and '-' not in w['summary'] and 'Mbit/s' in w['summary'] else 1)" < "$T/care.json" ||
	fail "the old warning should read 0 or more: $(cat "$T/care.json")"

echo "== learning first"
wait_for 15 "agd should look at the line: $(h line 2>&1)" \
	"h -j line > '$T/line.json' 2>/dev/null && js \"sys.exit(0 if d['tech'] == 'dsl' else 1)\" < '$T/line.json'"
js "sys.exit(0 if d['enabled'] and d['tech'] == 'dsl' and d['status'] in ('', 'Learning') else 1)" < "$T/line.json" ||
	fail "learning: $(cat "$T/line.json")"
[ "$(nev 'LineDegrading!')" = 0 ] || fail "no LineDegrading! while learning"

echo "== the noise margin slides: Drifting, with when it is a fault"
wait_for 70 "the line should drift: $(h line)" \
	"h -j line | js \"sys.exit(0 if d['status'] in ('Drifting', 'Fault') and d['worst'] == 'margin' else 1)\""
h line
h -j line > "$T/line.json"
js "m=[x for x in d['metrics'] if x['metric'] == 'margin'][0]; sys.exit(0 if m['slope'] < -0.3 and m['words'].startswith('The noise margin fell') else 1)" < "$T/line.json" ||
	fail "the margin's trend: $(cat "$T/line.json")"
js "sys.exit(0 if any(x['metric'] == 'crc' and x['days'] >= 5 for x in d['metrics']) and any(x['metric'] == 'drops' for x in d['metrics']) else 1)" < "$T/line.json" ||
	fail "the CRC rate and the drops should be counted: $(cat "$T/line.json")"
[ "$(dmv WorstMetric)" = NoiseMargin ] && [ "$(dmv Technology)" = dsl ] || fail "LineHealth.: $(h dm get Device.X_AIGW_.LineHealth.)"
case "$(dmv Status)" in Drifting|Fault) ;; *) fail "LineHealth.Status: $(dmv Status)" ;; esac
h dm get Device.X_AIGW_.LineHealth.Metric.2.Name | grep -q NoiseMargin || fail "Metric.2 is the noise margin: $(h dm get Device.X_AIGW_.LineHealth.Metric.)"
wait_for 5 "LineDegrading!" "[ \"\$(nev 'LineDegrading!')\" -ge 1 ]"
grep 'LineDegrading!' "$T/events.log" | grep -q NoiseMargin || fail "LineDegrading! should name the metric: $(cat "$T/events.log")"
wait_for 15 "the bridge should send the line health vector" \
	"PYTHONPATH='$REPO/cortex' python3 -c \"
import sys; from aigw_cortex.store import Store; from aigw_cortex import areapredict
s = Store('$T/cortex'); areapredict._table(s)
sys.exit(0 if s.q(\\\"SELECT 1 FROM line_vectors v JOIN gateways g ON g.id = v.gateway WHERE g.name = '$H' AND v.status IN ('Drifting', 'Fault')\\\") else 1)\""

echo "== five more homes on the cabinet slide the same days: the shared plant"
for S in s1 s2 s3 s4 s5; do cortex enroll $S --group olt-T/cab-1 2>> "$T/enroll.log" > "$T/$S.token"; done
for S in q1 q2 q3 q4 q5 q6 q7 q8 q9 q10; do cortex enroll $S --group olt-T/cab-2 2>> "$T/enroll.log" > "$T/$S.token"; done
PYTHONPATH="$REPO/cortex" python3 - "$PORT" "$T" > "$T/sim.txt" 2>&1 <<'EOF' || fail "simulated homes: $(cat "$T/sim.txt")"
import json, sys, time, urllib.request
from aigw_cortex import areapredict
from aigw_cortex.store import Store
port, t = sys.argv[1], sys.argv[2]
# the real home's day numbers (its test clock: a day is 2 s) and when its margin began to slide
s = Store(f"{t}/cortex")
r = s.q("SELECT v.vec FROM line_vectors v JOIN gateways g ON g.id = v.gateway WHERE g.name = 'h1' ORDER BY day DESC LIMIT 1")
real = json.loads(r[0]["vec"])
day = real["day"]
onset = [m for m in real["metrics"] if m["metric"] == "margin"][0]["onset"]
def vec(bad):
    m = {"metric": "margin", "state": "Drifting" if bad else "OK", "days": 14, "level": 7.0 if bad else 10.0,
         "base": 10.0, "slope": -0.5 if bad else 0.0, "fault": 3.0, "onset": onset if bad else 0,
         "days_left": 8 if bad else -1, "conf": 80 if bad else 0, "tau7": 0.9 if bad else 0.0,
         "slope7": -0.5 if bad else 0.0}
    return {"v": 1, "tech": "dsl", "day": day, "status": m["state"], "metrics": [m]}
for s in ("s1", "s2", "s3", "s4", "s5") + tuple(f"q{i}" for i in range(1, 11)):
    tok = open(f"{t}/{s}.token").read().strip()
    body = {"bridge": "0.54.0", "interval": 300, "status": {"version": "0.54.0"}, "line": vec(s.startswith("s"))}
    r = urllib.request.Request(f"http://127.0.0.1:{port}/v1/report", data=json.dumps(body).encode(),
                               headers={"Authorization": f"Bearer {tok}"})
    print(s, json.loads(urllib.request.urlopen(r).read())["line"])
EOF
grep -q "^s5 {'plant': [1-9]" "$T/sim.txt" || fail "the plant should be predicted: $(cat "$T/sim.txt"; cortex predict)"
cortex predict
cortex predict | grep -q "olt-T/cab-1 is degrading" || fail "cortex predict: $(cortex predict)"
wait_for 15 "agd should show the shared plant: $(h dm get Device.X_AIGW_.LineHealth.)" "[ \"\$(dmv SharedPlant)\" = olt-T/cab-1 ]"
[ "$(dmv SharedPlantHomes)" -ge 6 ] || fail "SharedPlantHomes: $(dmv SharedPlantHomes)"
h line | grep -q "cortex .*The plant serving" || fail "agctl line should say what cortex sees: $(h line)"
grep -q "shared plant [0-9]*: told agd" "$T/agd.log" || fail "the bridge should say it told agd"

echo "== no faster plan on a degrading line"
if has offers; then
	h -j offers | js "r=[s for s in d['signals'] if s['code'] == 'LINE_REPAIR'][0]; sys.exit(0 if r['active'] and 'degrades day by day' in r['evidence'] else 1)" ||
		fail "LINE_REPAIR on a degrading line: $(h offers)"
fi

echo "== agd restarts: the days come back, no second event"
N=$(nev 'LineDegrading!')
kill "$(cat "$T/agd.pid")"; sleep 1.5
start_agd
wait_for 15 "agd should restore its line days: $(tail -5 "$T/agd.log")" "grep -q 'line: restored [0-9]* days' '$T/agd.log'"
sleep 4
[ "$(nev 'LineDegrading!')" = "$N" ] || fail "no second LineDegrading! after a restart ($N, then $(nev 'LineDegrading!'))"

echo "== line.share 0: no vector for cortex"
h -j line | js "sys.exit(0 if d['shared'] else 1)" || fail "shared by default"
cat >> "$T/config/aigw" <<'EOF'
	option share '0'
EOF
kill "$(cat "$T/agd.pid")"; sleep 1.5
start_agd
wait_for 10 "agd back" "h -j line > /dev/null 2>&1"
h -j line | js "sys.exit(0 if not d['shared'] else 1)" || fail "line.share 0: $(h -j line)"
h line | grep -q "not shared with the ISP" || fail "agctl line should say it isn't shared"
echo "LINE TEST OK"
