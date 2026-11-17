#!/bin/sh
# Staged firmware rollout, end to end: four homes (each its own ubusd, agd,
# bridge plug-in and two-bank firmware: tests/fw/firmware) and a private
# cortex rolling 1.0 -> 2.0 out through the bridges in three waves:
#
#   paused     planned paused: no home gets anything
#   wave 1     home A: the bridge takes the offer, agd downloads it into the
#              other bank (its own Download path), activates it, reboots,
#              watches the home on 2.0 and keeps it; the verdict reaches cortex
#              in the report: wave 1 passes
#   wave 2     homes B and C: B keeps it; on C agd keeps restarting, so C rolls
#              back to 1.0 by itself. 1 of 2 homes worse: the rollout stops by
#              itself and says why, with counts and no homes' names
#   wave 3     home D never gets the firmware
#
# Needs root (private ubusd, plug-ins run as nobody), python3 and curl.
#
#   sudo tests/rollout-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
PORT=$((20000 + R % 20000))
PIDS=""
HOMES="hA hB hC hD"

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for H in $HOMES; do [ -f "$T/$H/agd.pid" ] && kill "$(cat "$T/$H/agd.pid")" 2>/dev/null || true; done
	sleep 0.5; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() {
	echo "ROLLOUT TEST FAIL: $*" >&2; echo "--- logs" >&2
	tail -n 30 "$T"/cortex.log "$T"/h*/agd.log >&2 2>/dev/null || true
	cortex rollout status spring --homes >&2 2>/dev/null || true
	exit 1
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl sha256sum; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }
h() { H=$1; shift; AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" "$BIN/agctl/agctl" "$@"; }
# hcheck <home> <python expression over u (agctl -j upgrade) and v (the firmware version)>
hcheck() {
	h "$1" -j upgrade > "$T/u.json" 2>/dev/null || return 1
	v=$(h "$1" dm get Device.DeviceInfo.SoftwareVersion 2>/dev/null | sed 's/.*= //')
	python3 - "$T/u.json" "$v" "$2" <<'EOF'
import json, sys
u, v = json.load(open(sys.argv[1])), sys.argv[2]
sys.exit(0 if eval(sys.argv[3]) else 1)
EOF
}
hwait() { # hwait <seconds> <home> <expr> <message>
	i=0
	until hcheck "$2" "$3"; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$2: $4: $(h "$2" -j upgrade 2>&1)"; sleep 0.5
	done
}
# cx <python expression over st (the rollout's status, with homes)>
cx() {
	PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$1" <<'EOF'
import sys
from aigw_cortex import rollout
from aigw_cortex.store import Store
s = Store(sys.argv[1])
st = rollout.status(s, "spring", homes=True)
home = {x["home"]: x for x in st["list"]}
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
cx_wait() {
	i=0
	until cx "$2" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$3"; sleep 0.5
	done
}
bank2() { cat "$T/$1/fw/bank2.status" 2>/dev/null || echo NoImage; }
start_agd() {
	( export AIGW_UCI_CONFDIR="$T/$1/config" AIGW_UBUS="$T/$1/ubus.sock" AIGW_NFT=/bin/true \
		AIGW_TEST_FW_DIR="$T/$1/fw"
	  exec "$BIN/agd/agd" >> "$T/$1/agd.log" 2>&1 ) &
	echo $! > "$T/$1/agd.pid"
}
stop_agd() { kill "$(cat "$T/$1/agd.pid")" 2>/dev/null || true; sleep 0.5; }

mkdir -p "$T/sys/bridge" "$T/cortex"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"
printf 'AIGW-TEST-FIRMWARE 2.0\n' > "$T/fw-2.0.img"
chmod 644 "$T/fw-2.0.img"
SUM=$(sha256sum "$T/fw-2.0.img" | cut -d' ' -f1)

echo "== cortex and four homes on firmware 1.0, their bridges taking firmware (firmware 1)"
cortex serve --port "$PORT" --quiet > "$T/cortex.log" 2>&1 &
PIDS="$PIDS $!"
for H in $HOMES; do
	mkdir -p "$T/$H/config" "$T/$H/user"
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

config act 'act'
	option dns_policy '$T/$H/dns-policy'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/$H/user'
	option user 'nobody'
	option state_dir '$T/$H/pstate'

config models 'models'
	option dir '$T/$H/models'
	option keys '$T/$H/keys.d'

config mgmt 'mgmt'
	option firmware_handler '$REPO/tests/fw/firmware'

config upgrade 'upgrade'
	option watch '20'
	option reconnect '10'
	option report_delay '1'

config plugin 'bridge'
	option interval '2'
	option devices 'anonymous'
	option firmware '1'
EOF
	: > "$T/$H/leases"
	"$UBUSD" -s "$T/$H/ubus.sock" > "$T/$H/ubusd.log" 2>&1 & PIDS="$PIDS $!"
	sleep 0.3
	start_agd $H
	TOKEN=$(cortex enroll "$H" 2>> "$T/enroll.log")
	sleep 1
	h $H bridge "http://127.0.0.1:$PORT" > /dev/null || fail "$H: agctl bridge"
	echo "$TOKEN" | h $H bridge token > /dev/null || fail "$H: bridge token"
done
for H in $HOMES; do
	hwait 15 $H "v == '1.0' and u['state'] == 'None'" "should run 1.0, no upgrade yet"
	i=0
	until h $H -j plugin bridge 2>/dev/null | python3 -c "import json,sys; p=json.load(sys.stdin)['plugins'][0]; \
sys.exit(0 if p['state'] == 'running' and 'firmware.install' in p.get('granted', ['firmware.install']) else 1)"; do
		i=$((i + 1)); [ $i -le 30 ] || fail "$H: the bridge should run with firmware.install: $(h $H plugin bridge)"; sleep 0.5
	done
done

echo "== planned paused: 2.0 to home A, then B and C, then D"
cortex rollout new spring --version 2.0 --url "file://$T/fw-2.0.img" --sha256 "$SUM" --via bridge \
	--wave hA --wave hB,hC --wave hD --paused > "$T/new.txt" || fail "rollout new: $(cat "$T/new.txt")"
grep -q "paused, wave 1 of 3" "$T/new.txt" || fail "should be paused: $(cat "$T/new.txt")"
sleep 6
[ "$(bank2 hA)" = NoImage ] || fail "paused: home A should get nothing (bank 2: $(bank2 hA))"
cortex rollout resume spring > /dev/null || fail "rollout resume"

echo "== wave 1: home A takes 2.0 through its bridge and agd's Download path, keeps it"
hwait 30 hA "v == '2.0' and u['state'] == 'Watching'" "home A should run and watch 2.0"
hwait 40 hA "u['state'] == 'Kept'" "home A should keep 2.0"
grep -q "firmware: downloading into image 2 (asked by plug-in bridge)" "$T/hA/agd.log" ||
	fail "agd's own Download path, asked by the bridge"
cx_wait 20 "st['wave'] == 2 and home['hA']['state'] == 'kept'" "wave 1 should pass"
cx "'Wave 1 of 3 passed: 1 of the 1 homes kept it' in st['log'][-1]['text'] and st['state'] == 'running'" ||
	fail "wave 1's reason: $(cortex rollout status spring)"
PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" <<'EOF' || fail "the report's upgrade check should carry no devices"
import sys
from aigw_cortex.store import Store
s = Store(sys.argv[1])
u = s.snapshot("hA")["upgrade"]
sys.exit(0 if u["state"] == "Kept" and u["to"] == "2.0" and not u.get("devices") and "devices_before" in u else 1)
EOF
[ "$(bank2 hD)" = NoImage ] || fail "home D is in wave 3: nothing yet"

echo "== wave 2: home B keeps it; home C keeps restarting on 2.0, rolls back by itself"
hwait 30 hC "v == '2.0' and u['state'] == 'Watching'" "home C should run and watch 2.0"
n=1
while [ $n -le 3 ]; do                  # AIGW starts 4 times on the new firmware: worse
	stop_agd hC; start_agd hC
	n=$((n + 1))
	hwait 20 hC "u.get('starts', 0) >= $n or u['state'] != 'Watching'" "start $n on 2.0 should be counted"
done
hwait 40 hC "v == '1.0' and u['state'] == 'RolledBack' and 'Restarts' in u['reasons']" "home C should roll back to 1.0"
hwait 40 hB "v == '2.0' and u['state'] == 'Kept'" "home B should keep 2.0"

echo "== the rollout stops by itself, says why; home D never gets 2.0"
cx_wait 30 "st['state'] == 'stopped' and home['hC']['state'] == 'rolled_back'" "the rollout should stop"
cortex rollout status spring > "$T/status.txt"
cat "$T/status.txt"
grep -q "Stopped in wave 2 of 3: 1 of the 2 homes got worse on it (1 rolled back)" "$T/status.txt" ||
	fail "the reason in words"
for H in $HOMES; do
	grep -q "$H" "$T/status.txt" && fail "the status names a home ($H): counts and words only"
done
cx "home['hD']['state'] == 'waiting'" || fail "home D should still wait"
sleep 5
[ "$(bank2 hD)" = NoImage ] || fail "home D must never get the firmware (bank 2: $(bank2 hD))"
hcheck hD "v == '1.0' and u['state'] == 'None'" || fail "home D should run 1.0, untouched"
cortex rollout resume spring > "$T/resume.txt" 2>&1 && fail "a stopped rollout should not resume"
grep -q "stays stopped" "$T/resume.txt" || fail "why it can't resume: $(cat "$T/resume.txt")"
cortex fleet > "$T/fleet.txt"
grep -q "rollout      spring (2.0, 4 homes): stopped, wave 2 of 3" "$T/fleet.txt" ||
	fail "the fleet report's rollout line: $(cat "$T/fleet.txt")"
echo "ROLLOUT TEST OK"
