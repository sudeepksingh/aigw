#!/bin/sh
# Firmware upgrades that check themselves, end to end: agd, the two-bank
# firmware handler (tests/fw/firmware, the kind an OEM with A/B flash has) and
# a fake hostapd whose devices come back after an upgrade, or don't.
#
#   bad upgrade   1.0 -> 2.0, then 3 of the 4 Wi-Fi devices don't come back:
#                 worse, rolled back to 1.0 by itself, the devices come back on
#                 it (confirmed), the ISP told (UpgradeChecked! RolledBack, then
#                 RollbackConfirmed), and 2.0 held off: downloading and
#                 activating it again is refused, until agctl upgrade --forget
#   good upgrade  1.0 -> 2.0, everyone back: kept at the end of the watch
#                 (UpgradeChecked! Kept)
#
# Needs root (private ubusd) and python3.
#
#   sudo tests/upgrade-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" upgrade
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "UPGRADE TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/agd.log "$T"/events.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

ALL="02:00:00:00:77:01 02:00:00:00:77:02 02:00:00:00:77:03 02:00:00:00:77:04"
echo "$ALL" > "$T/stations"

echo "== a fake hostapd: the devices in $T/stations are connected"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, select, socket, sys, time
d, stations = sys.argv[1:3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(os.path.join(d, "wlan0"))
start = time.time()
def present():
    return sorted(open(stations).read().split())
def sta(m):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=-50\nconnected_time=%d\ntx_rate_info=1440\n" % (
        m, int(time.time() - start))
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    w = cmd.split()
    if cmd == "STATUS":
        rep = "state=ENABLED\nfreq=2437\nchannel=6\nbssid[0]=02:aa:00:00:00:24\nssid[0]=AIGW-TEST\n"
    elif cmd == "STA-FIRST":
        p = present(); rep = sta(p[0]) if p else ""
    elif w[0] == "STA-NEXT":
        p = present(); i = p.index(w[1]) + 1 if w[1] in p else len(p)
        rep = sta(p[i]) if i < len(p) else ""
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
	option port '15357'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'

config care 'care'
	option enabled '0'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option firmware_handler '$REPO/tests/fw/firmware'

config upgrade 'upgrade'
	option watch '45'
	option reconnect '15'
	option report_delay '3'

config lan 'lan'
	list iface 'lo'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_TEST_FW_DIR="$T/fw"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
"$BIN/agctl/agctl" watch upgrade > "$T/watch.log" 2>&1 & PIDS="$PIDS $!"

agctl() { "$BIN/agctl/agctl" "$@"; }
call() { "$UBUS" -s "$AIGW_UBUS" call ag.dm "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
up() { agctl -j upgrade | js "$1"; }
# wait up to $1 s for a condition on agctl -j upgrade (d)
wait_up() {
	i=0
	until up "sys.exit(0 if $2 else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$3: $(agctl -j upgrade)"; sleep 0.5
	done
}
FW=Device.DeviceInfo.FirmwareImage
upgrade_to() {   # download $1 into bank 2 and activate it; prints the answer
	printf 'AIGW-TEST-FIRMWARE %s\n' "$1" > "$T/fw-$1.img"
	call operate "{\"command\":\"$FW.2.Download()\",\"args\":{\"URL\":\"file://$T/fw-$1.img\",\"AutoActivate\":\"true\"}}"
}
version() { agctl dm get Device.DeviceInfo.SoftwareVersion | sed 's/.*= //'; }

echo "== the home: 4 devices on Wi-Fi, firmware 1.0"
i=0
until [ "$(agctl -j wifi | js "print(sum(len(r['stations']) for r in d['radios']))")" = 4 ]; do
	i=$((i + 1)); [ $i -le 30 ] || fail "the 4 devices should be seen: $(agctl wifi)"; sleep 0.5
done
[ "$(version)" = 1.0 ] || fail "should run 1.0"
up "sys.exit(0 if d['state'] == 'None' and d['check'] and d['rollback'] else 1)" || fail "nothing checked yet: $(agctl -j upgrade)"

echo "== a bad upgrade: 2.0, after which 3 of the 4 devices stay away (agd stops for each reboot, as on a gateway)"
upgrade_to 2.0 | js "sys.exit(0 if d['ok'] else 1)" || fail "download and activate 2.0"
echo "02:00:00:00:77:01" > "$T/stations"
stop_agd                                            # the reboot into 2.0
"$REPO/tests/fw/firmware" reboot > /dev/null
start_agd
wait_up 20 "d['state'] == 'Watching' and d['to'] == '2.0' and len(d['devices']) == 4" "watching 2.0"
[ "$(version)" = 2.0 ] || fail "should run 2.0 now"
wait_up 40 "d['state'] in ('RollingBack', 'RolledBack')" "3 of 4 devices away for 15 s: worse, rolling back"
sleep 1
stop_agd                                            # the reboot back into 1.0 (if agd's own hasn't come first)
"$REPO/tests/fw/firmware" reboot > /dev/null
echo "$ALL" > "$T/stations"                         # on 1.0 they come back
start_agd
wait_up 20 "d['state'] == 'RolledBack'" "back on 1.0"
[ "$(version)" = 1.0 ] || fail "should run 1.0 again"
wait_up 20 "d.get('confirmed') == 'yes'" "the devices came back on 1.0: confirmed"
agctl upgrade
up "sys.exit(0 if d['reasons'] == 'DevicesMissing' and d['devices_missing'] == 3 and d['held'] == '2.0' and
    '3 of the 4 devices' in d['summary'] and 'Confirmed' in d['summary'] else 1)" || fail "the record: $(agctl -j upgrade)"
[ "$(grep -c . "$T/fw/reboots")" -ge 2 ] || fail "two reboots (into 2.0, back to 1.0): $(cat "$T/fw/reboots")"
agctl dm get Device.X_AIGW_.Upgrade. > "$T/dm.txt"
grep -q 'Status = RolledBack' "$T/dm.txt" && grep -q 'RollbackConfirmed = Yes' "$T/dm.txt" &&
	grep -q 'Reasons = DevicesMissing' "$T/dm.txt" && grep -q 'HeldVersion = 2.0' "$T/dm.txt" &&
	grep -q 'DevicesBefore = 4' "$T/dm.txt" && grep -q 'DevicesBack = 1' "$T/dm.txt" || fail "Device.X_AIGW_.Upgrade.: $(cat "$T/dm.txt")"
echo "   rolled back by itself, the 3 devices back on 1.0, 2.0 held off"

echo "== the ISP was told (what was found at start: once agd is on ubus, upgrade.report_delay later)"
checked() {   # the UpgradeChecked! events so far, one JSON per line
	python3 - "$T/events.log" <<'EOF'
import json, sys
for line in open(sys.argv[1]):
    try:
        e = json.loads(line).get("ag.dm.event", {})
    except ValueError:
        continue
    if e.get("event") == "Device.X_AIGW_.UpgradeChecked!":
        print(json.dumps(e["args"]))
EOF
}
i=0
until [ "$(checked | grep -c .)" -ge 2 ]; do       # the delay, and agd's 5 s tick
	i=$((i + 1)); [ $i -le 30 ] || break; sleep 0.5
done
checked > "$T/checked.json"
python3 - "$T/checked.json" <<'EOF' || fail "UpgradeChecked! events: $(cat "$T/events.log")"
import json, sys
ev = [{"args": json.loads(l)} for l in open(sys.argv[1])]
res = [e["args"]["Result"] for e in ev]
print("   UpgradeChecked!:", ", ".join(res))
ok = res[:2] == ["RolledBack", "RollbackConfirmed"] and ev[0]["args"]["Reasons"] == "DevicesMissing" and \
     ev[0]["args"]["FromVersion"] == "1.0" and ev[0]["args"]["ToVersion"] == "2.0"
sys.exit(0 if ok else 1)
EOF

echo "== 2.0 again (an ACS that retries): refused while it is held off"
upgrade_to 2.0 > "$T/again.json"
js "sys.exit(0 if not d['ok'] and 'rolled 2.0 back' in d['message'] else 1)" < "$T/again.json" ||
	fail "download and activate 2.0 again should be refused: $(cat "$T/again.json")"
call operate "{\"command\":\"$FW.2.Activate()\"}" | js "sys.exit(0 if not d['ok'] and 'rolled 2.0 back' in d['message'] else 1)" ||
	fail "activating 2.0 again should be refused"
[ "$(version)" = 1.0 ] || fail "should still run 1.0"
agctl upgrade --forget | grep -q "hold is lifted" || fail "agctl upgrade --forget"

echo "== a good upgrade: 2.0, everyone back; agd stops for the reboot, as on a gateway"
upgrade_to 2.0 | js "sys.exit(0 if d['ok'] else 1)" || fail "download and activate 2.0 after forget"
stop_agd                                            # the reboot: agd stops, the new image boots
"$REPO/tests/fw/firmware" reboot > /dev/null
start_agd
wait_up 20 "d['state'] == 'Watching' and d['to'] == '2.0' and d['starts'] == 1" "watching 2.0 after agd started on it"
wait_up 15 "d['devices_back'] == 4" "all 4 back"
stop_agd                                            # agd restarts during the watch: it goes on
start_agd
wait_up 10 "d['state'] == 'Watching' and d['starts'] == 2 and d['devices_back'] == 4" "the watch goes on over a restart"
wait_up 70 "d['state'] == 'Kept'" "kept at the end of the watch"
up "sys.exit(0 if not d['reasons'] and 'No worse' in d['summary'] and '4 of 4 devices back' in d['summary'] else 1)" ||
	fail "kept: $(agctl -j upgrade)"
[ "$(version)" = 2.0 ] || fail "should run 2.0"
checked | grep -q '"Result": "Kept"' || fail "UpgradeChecked! Kept: $(cat "$T/events.log")"
echo "--- live events"; cat "$T/watch.log"
grep -q "rolling-back" "$T/watch.log" && grep -q "kept" "$T/watch.log" || fail "agctl watch upgrade"
echo "UPGRADE TEST OK"
