#!/bin/sh
# Phase 4 "plug & IoT" integration test. Runs a private ubusd and agd (no
# kernel rules: nft is replaced by /bin/true), plug-ins as the "nobody" user,
# a fake hostapd for the Wi-Fi keys and, when mosquitto is installed, a
# private MQTT broker. Safe next to a live AIGW: own config, sockets and ports.
#
# Checks:
#   exit test   a third-party plug-in folder is added and starts by itself, with
#               no configuration, as an unprivileged user; its telemetry lands
#               on the device it names
#   wiring      a plug-in waiting for another plug-in's events starts when that
#               plug-in runs, and receives them; one waiting for a program starts
#               when the program appears
#   trust       an added plug-in's privileged requests are refused until
#               "agctl plugin allow"; packaged plug-ins are trusted
#   actions     an actuator plug-in's action is a decision (do, undo)
#   skills      a skill's policy turns another plug-in's alert into a proposal,
#               only for devices in its zone, advise-only while untrusted
#   safety      bad manifests and unsafe permissions are refused; a crashing
#               plug-in is restarted, then stopped; memory and CPU budgets hold;
#               removing a folder stops the plug-in
#   keys        a per-device Wi-Fi key with its QR code; the first device to join
#               owns it (MAC-bound, named, zoned); revoke disconnects it; unused
#               keys expire; keys survive a restart
#   mqtt        the connector waits for its broker, then: Home Assistant discovery,
#               presence, sensor telemetry in, a Quarantine switch, commands
# Needs root, python3, uci; mosquitto + mosquitto_sub/pub for the MQTT part,
# zbarimg to decode the QR code.
#
#   sudo tests/plug-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
MQTT_PORT=$((20000 + R % 20000))
PIDS=""
PLUG=02:00:00:00:98:05
PHONE=02:00:00:00:98:06
NEWDEV=02:00:00:00:98:07

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -n "${AGD:-}" ] && kill "$AGD" 2>/dev/null || true
	sleep 0.3
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "PLUG TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 uci; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

agctl() { "$BIN/agctl/agctl" "$@"; }

# jcheck <agctl args...> -- <python expression over j> <message>
jcheck() {
	args=""
	while [ "$1" != "--" ]; do args="$args $1"; shift; done
	shift
	# shellcheck disable=SC2086
	agctl -j $args > "$T/j.json" 2>&1 || true
	python3 - "$T/j.json" "$1" <<'EOF' || { cat "$T/j.json" >&2; fail "$2"; }
import json, sys
j = json.load(open(sys.argv[1]))
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
# wait_for <seconds> <agctl args...> -- <python expression> <message>
wait_for() {
	n=$1; shift
	i=0
	while :; do
		if ( jcheck "$@" ) > /dev/null 2>&1; then return 0; fi
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && jcheck "$@"
		sleep 0.5
	done
}
plugin_state() { # plugin_state <name> <state> [seconds]
	wait_for "${3:-12}" plugins -- "any(p['name'] == '$1' and p['state'] == '$2' for p in j['plugins'])" \
		"plug-in $1 is not $2"
}

mkdir -p "$T/sys" "$T/user" "$T/src" "$T/bin" "$T/out" "$T/hostapd" "$T/config"
chmod 777 "$T/out"

echo "== a fake hostapd that knows per-device keys"
python3 - "$T/hostapd/wlan-test" "$T/hostapd.log" "$T/sta" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, socket, sys
path, log, sta = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    with open(log, "a") as f:
        f.write(cmd + "\n")
    if cmd == "STA-FIRST":
        r = ""
        if os.path.exists(sta):
            mac, kid = open(sta).read().split()
            r = mac + "\nsignal=-50\nconnected_time=5\n" + ("keyid=" + kid + "\n" if kid != "-" else "")
    elif cmd.startswith("STA-NEXT"):
        r = ""
    elif cmd == "GET_CONFIG":
        r = "bssid=02:00:00:00:00:01\nssid=AIGW-TEST\nwpa=2\nkey_mgmt=WPA-PSK\n"
    elif cmd in ("RELOAD_WPA_PSK",) or cmd.startswith("DEAUTHENTICATE"):
        r = "OK\n"
    else:
        r = "UNKNOWN COMMAND\n"
    if addr:
        s.sendto(r.encode(), addr)
EOF
PIDS="$PIDS $!"

cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option mode 'enforce'
	option max_auto_per_hour '10'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/user'
	option user 'nobody'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config plugin 'mqtt'
	option port '$MQTT_PORT'
	option prefix 'aigwtest'
EOF
cat > "$T/leases" <<EOF
1999999999 $PLUG 10.98.0.5 smart-plug *
1999999999 $PHONE 10.98.0.6 phone *
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { PATH="$T/bin:$PATH" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; sleep 1.5; }
stop_agd() { kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done; AGD=""; }

# plugin <dir> <name> <manifest json> <script|-> : writes a plug-in folder
plugin() {
	mkdir -p "$1/$2"
	printf '%s\n' "$3" > "$1/$2/manifest.json"
	if [ "$4" != - ]; then
		printf '#!/bin/sh\n%s\n' "$4" > "$1/$2/run.sh"
		chmod 755 "$1/$2/run.sh"
	fi
}

echo "== a plug-in that needs another plug-in's events waits for it"
plugin "$T/user" consumer '{"name":"consumer","kind":"sensor","exec":"run.sh","needs":["event:iot.temp"],"subscribes":["iot.temp"]}' \
	"while IFS= read -r l; do case \$l in *iot.temp*) echo \"\$l\" >> $T/out/consumer.log;; esac; done"
plugin "$T/user" waiter '{"name":"waiter","kind":"sensor","exec":"run.sh","needs":["bin:aigw-tool-'$R'"]}' \
	"while read -r l; do :; done"
start_agd
plugin_state consumer waiting
plugin_state waiter waiting
jcheck plugin consumer -- "'iot.temp' in j['plugins'][0]['why']" "consumer should say what it waits for"

echo "== exit test: a third-party plug-in is added and starts by itself"
plugin "$T/src" thermo '{"name":"thermo","kind":"sensor","version":"1.0","exec":"run.sh","emits":["iot.temp"]}' \
	"while :; do printf '{\"emit\":\"iot.temp\",\"body\":{\"mac\":\"$PLUG\",\"celsius\":21.5}}\\n'; sleep 1; done"
agctl plugin add "$T/src/thermo" > "$T/add.out" 2>&1 || { cat "$T/add.out"; fail "agctl plugin add"; }
plugin_state thermo running 12
pid=$(agctl -j plugin thermo | python3 -c "import json,sys; print(json.load(sys.stdin)['plugins'][0]['process']['pid'])")
[ "$(ps -o user= -p "$pid" | tr -d ' ')" = nobody ] || fail "plug-in does not run as nobody"
wait_for 10 devices -- "any(d['mac'] == '$PLUG' and d.get('plugins', {}).get('events', 0) > 0 and \
	d['plugins']['last'] == 'iot.temp' for d in j['devices'])" "telemetry did not reach the device"

echo "== wiring: the waiting plug-ins start and get the events"
plugin_state consumer running 12
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/out/consumer.log" ] && break; sleep 1; done
grep -q '"celsius"' "$T/out/consumer.log" 2>/dev/null || fail "consumer did not receive iot.temp events"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/aigw-tool-$R"; chmod 755 "$T/bin/aigw-tool-$R"
plugin_state waiter running 12

echo "== safety: bad manifests and unsafe files are refused"
plugin "$T/user" badkind '{"name":"badkind","kind":"virus","exec":"run.sh"}' "exit 0"
plugin "$T/user" wrongname '{"name":"other","kind":"sensor","exec":"run.sh"}' "exit 0"
plugin "$T/user" spoofer '{"name":"spoofer","kind":"sensor","exec":"run.sh","emits":["ag.alert"]}' "exit 0"
plugin "$T/user" unsafe '{"name":"unsafe","kind":"sensor","exec":"run.sh"}' "exit 0"
chmod 777 "$T/user/unsafe/run.sh"
plugin_state badkind invalid
plugin_state wrongname invalid
plugin_state spoofer invalid
plugin_state unsafe invalid
jcheck plugin unsafe -- "'writable by others' in j['plugins'][0]['why']" "unsafe permissions not explained"

echo "== trust: an added plug-in's privileged requests wait for your OK"
plugin "$T/user" asker '{"name":"asker","kind":"connector","exec":"run.sh","caps":["decision.request","devices.read"]}' \
	"printf '{\"request\":{\"action\":\"priority\",\"target\":\"smart-plug\",\"ttl\":600,\"id\":5}}\\n'
printf '{\"query\":\"devices\",\"id\":6}\\n'
while IFS= read -r l; do case \$l in *reply*) echo \"\$l\" >> $T/out/asker.log;; esac; done"
plugin_state asker running
jcheck plugin asker -- "set(j['plugins'][0]['waiting_for_ok']) == {'decision.request', 'devices.read'}" "asker grants"
sleep 1
grep -q 'agctl plugin allow' "$T/out/asker.log" || fail "privileged request not refused with a hint"
jcheck decisions -- "not any(x['origin'] == 'plug-in asker' for x in j['decisions'])" "refused request created a decision"
agctl plugin allow asker > /dev/null || fail "agctl plugin allow"
wait_for 12 decisions -- "any(x['origin'] == 'plug-in asker' and x['action'] == 'priority' and \
	x['state'] == 'active' for x in j['decisions'])" "allowed request did not act"
grep -q '"smart-plug"' "$T/out/asker.log" || fail "allowed devices query got no devices"
grep -q 'allow' "$T/config/aigw" || fail "allow not saved in the config"

echo "== actions: an actuator plug-in's action is a decision"
cp -R "$REPO/plugins/examples/door-sensor" "$T/sys/door-sensor"
plugin_state door-sensor running
agctl do door-sensor.flash smart-plug 5m > "$T/do.out" 2>&1 || { cat "$T/do.out"; fail "agctl do"; }
sleep 1
grep -q 'flashing the hall light for 10.98.0.5' "$T/agd.log" || fail "actuator plug-in did not act"
id=$(agctl -j decisions | python3 -c "import json,sys; print([x['id'] for x in json.load(sys.stdin)['decisions'] if x['action'] == 'door-sensor.flash' and x['state'] == 'active'][0])")
agctl undo "$id" > /dev/null || fail "undo plug-in action"
sleep 1
grep -q 'light back to normal' "$T/agd.log" || fail "actuator plug-in did not undo"
agctl do door-sensor.nosuch smart-plug 2>&1 | grep -q "no plug-in offers" || fail "unknown plug-in action not refused"

echo "== skills: an alert from one plug-in, a policy from a skill, an action from a third"
plugin "$T/user" heat-guard '{"name":"heat-guard","kind":"skill","policies":[{"alert":"too-hot","zone":"iot","action":"door-sensor.flash","mode":"enforce"}],"card":{"title":"Heat guard","text":"Flash the light when a plug overheats"}}' -
plugin_state heat-guard active
agctl zone smart-plug iot > /dev/null || fail "agctl zone"
plugin "$T/sys" alerter '{"name":"alerter","kind":"sensor","exec":"run.sh","alerts":["too-hot"]}' \
	"printf '{\"alert\":{\"kind\":\"too-hot\",\"device\":\"phone\",\"reason\":\"The phone is hot\"}}\\n'
printf '{\"alert\":{\"kind\":\"too-hot\",\"device\":\"smart-plug\",\"reason\":\"The plug is at 80 C\",\"severity\":\"warning\"}}\\n'
while read -r l; do :; done"
plugin_state alerter running
wait_for 10 alerts -- "sum(a['kind'] == 'too-hot' for a in j['alerts']) == 2" "plug-in alerts not raised"
# enforce mode, but the skill was added by hand: it only proposes
wait_for 10 decisions -- "any(x['action'] == 'door-sensor.flash' and x['origin'] == 'skill heat-guard' and \
	x['state'] == 'proposed' and x['target'] == '10.98.0.5' for x in j['decisions'])" "skill did not propose"
jcheck decisions -- "not any(x['origin'] == 'skill heat-guard' and x['target'] == '10.98.0.6' for x in j['decisions'])" \
	"skill acted outside its zone"

echo "== safety: crash loop, memory and CPU budgets, removal"
plugin "$T/user" crasher '{"name":"crasher","kind":"sensor","exec":"run.sh"}' "exit 3"
plugin "$T/user" hog '{"name":"hog","kind":"sensor","exec":"run.sh","budget":{"ram_kb":4096}}' \
	"x=\$(head -c 30000000 /dev/zero | tr '\\0' a); sleep 300"
plugin "$T/user" spinner '{"name":"spinner","kind":"sensor","exec":"run.sh","budget":{"cpu_pct":5}}' \
	"while :; do :; done"
plugin_state hog failed 25
jcheck plugin hog -- "'memory' in j['plugins'][0]['why']" "memory budget not explained"
plugin_state crasher failed 40
jcheck plugin crasher -- "'stopped 5 times' in j['plugins'][0]['why']" "crash loop not explained"
plugin_state spinner failed 40
jcheck plugin spinner -- "'CPU' in j['plugins'][0]['why']" "CPU budget not explained"
rm -rf "$T/user/hog" "$T/user/spinner"
agctl plugin enable crasher > /dev/null
wait_for 10 plugin crasher -- "j['plugins'][0]['state'] in ('running', 'waiting')" "enable did not retry"
agctl plugin disable crasher > /dev/null
plugin_state crasher disabled
pid=$(agctl -j plugin thermo | python3 -c "import json,sys; print(json.load(sys.stdin)['plugins'][0]['process']['pid'])")
agctl plugin remove thermo > /dev/null || fail "agctl plugin remove"
wait_for 10 plugins -- "not any(p['name'] == 'thermo' for p in j['plugins'])" "removed plug-in still listed"
sleep 1
kill -0 "$pid" 2>/dev/null && fail "removed plug-in still running"
agctl plugin remove door-sensor 2>&1 | grep -q "package" || fail "removing a packaged plug-in not refused"

echo "== keys: a device gets its own Wi-Fi key"
agctl -j key add "Living room plug" iot 2h > "$T/key.json" || { cat "$T/key.json"; fail "key add"; }
python3 - "$T/key.json" <<'EOF' || fail "key add reply"
import json, sys
k = json.load(open(sys.argv[1]))
assert k["ok"] and k["ssid"] == "AIGW-TEST" and len(k["passphrase"]) == 16 and k["zone"] == "iot"
assert k["qr"] == "WIFI:T:WPA;S:AIGW-TEST;P:%s;;" % k["passphrase"]
EOF
kid=$(python3 -c "import json; print(json.load(open('$T/key.json'))['id'])")
pass=$(python3 -c "import json; print(json.load(open('$T/key.json'))['passphrase'])")
grep -q "^keyid=$kid 00:00:00:00:00:00 $pass\$" "$T/wifi-keys" || fail "key not in wpa_psk_file"
grep -q RELOAD_WPA_PSK "$T/hostapd.log" || fail "hostapd not told to reload"
[ "$(stat -c %a "$T/wifi-keys")" = 600 ] || fail "wpa_psk_file readable by others"
agctl key add "Living room plug" 2>&1 | grep -q "already" || fail "duplicate key name accepted"
agctl key add "x" "Bad Zone" 2>&1 | grep -q "zone" || fail "bad zone accepted"
agctl key qr "Living room plug" | grep -q "$pass" || fail "key qr"
if command -v zbarimg > /dev/null; then
	agctl qr "WIFI:T:WPA;S:AIGW-TEST;P:$pass;;" --pbm > "$T/qr.pbm"
	[ "$(zbarimg -q --raw "$T/qr.pbm")" = "WIFI:T:WPA;S:AIGW-TEST;P:$pass;;" ] || fail "QR code does not decode"
else
	echo "   NOTE: zbarimg not installed: QR decode skipped"
fi
echo "$NEWDEV $kid" > "$T/sta"
wait_for 12 keys -- "j['keys'][0]['state'] == 'in use' and j['keys'][0]['mac'] == '$NEWDEV'" "key not bound on join"
grep -q "^keyid=$kid $NEWDEV $pass\$" "$T/wifi-keys" || fail "bound key not limited to the device's MAC"
jcheck device "$NEWDEV" -- "j['zone'] == 'iot' and j['wifi_key'] == 'Living room plug' and \
	j['hostname'] == 'Living room plug'" "joined device not named and zoned"
agctl key revoke "Living room plug" > /dev/null || fail "key revoke"
grep -q "keyid=$kid" "$T/wifi-keys" && fail "revoked key still in wpa_psk_file"
grep -q "DEAUTHENTICATE $NEWDEV" "$T/hostapd.log" || fail "revoked device not disconnected"
rm -f "$T/sta"
agctl key add "Short lived" 2s > /dev/null
agctl key add "Keeper" > /dev/null
wait_for 12 keys -- "[k['name'] for k in j['keys']] == ['Keeper']" "unused key did not expire"
stop_agd
start_agd
jcheck keys -- "[k['name'] for k in j['keys']] == ['Keeper']" "keys lost on restart"
jcheck plugins -- "any(p['name'] == 'asker' and p['trusted'] for p in j['plugins'])" "allow lost on restart"

echo "== mqtt: the connector waits for its broker, then talks to Home Assistant"
if command -v mosquitto > /dev/null && command -v mosquitto_sub > /dev/null; then
	mkdir -p "$T/sys/mqtt"
	cp "$BIN/plugins/ag-mqtt" "$REPO/plugins/mqtt/manifest.json" "$T/sys/mqtt/"
	plugin_state mqtt waiting
	jcheck plugin mqtt -- "'$MQTT_PORT' in j['plugins'][0]['why']" "mqtt should wait for its broker"
	printf 'listener %s 127.0.0.1\nallow_anonymous true\npersistence false\n' "$MQTT_PORT" > "$T/mosquitto.conf"
	mosquitto -c "$T/mosquitto.conf" > "$T/mosquitto.log" 2>&1 & PIDS="$PIDS $!"
	plugin_state mqtt running 15
	sub() { mosquitto_sub -p "$MQTT_PORT" -t "$1" -v -W "${2:-4}" 2>/dev/null || true; }
	for i in 1 2 3 4 5 6 7 8 9 10; do
		sub 'homeassistant/#' 2 > "$T/ha.txt"
		grep -q 'device_tracker/aigw_020000009806/config' "$T/ha.txt" && break
		sleep 1
	done
	grep -q 'device_tracker/aigw_020000009806/config' "$T/ha.txt" || fail "no Home Assistant device tracker"
	grep -q 'switch/aigw_020000009806_quarantine/config' "$T/ha.txt" || fail "no Home Assistant quarantine switch"
	grep -q 'sensor/aigw_alerts/config' "$T/ha.txt" || fail "no Home Assistant alerts sensor"
	sub 'aigwtest/status' 2 | grep -q online || fail "status not online"
	sub 'aigwtest/device/020000009806/presence' 2 | grep -q 'home' || fail "no presence"

	mosquitto_pub -p "$MQTT_PORT" -t aigwtest/sensor/kitchen/temp -m "{\"mac\":\"$PHONE\",\"celsius\":22}"
	wait_for 10 devices -- "any(d['mac'] == '$PHONE' and d.get('plugins', {}).get('last') == 'iot.mqtt' \
		for d in j['devices'])" "MQTT telemetry did not reach the device"

	mosquitto_pub -p "$MQTT_PORT" -t aigwtest/device/020000009806/quarantine/set -m ON
	wait_for 10 decisions -- "any(x['action'] == 'quarantine' and x['target'] == '$PHONE' and \
		x['state'] == 'active' and x['origin'] == 'plug-in mqtt' for x in j['decisions'])" "HA switch did not quarantine"
	for i in 1 2 3 4 5; do sub 'aigwtest/device/020000009806/quarantine' 1 | grep -q ON && break; sleep 1; done
	sub 'aigwtest/device/020000009806/quarantine' 1 | grep -q ON || fail "switch state not ON"
	mosquitto_pub -p "$MQTT_PORT" -t aigwtest/device/020000009806/quarantine/set -m OFF
	wait_for 10 decisions -- "any(x['action'] == 'quarantine' and x['target'] == '$PHONE' and \
		x['state'] == 'reverted' for x in j['decisions'])" "HA switch did not undo"

	( sub 'aigwtest/result' 5 > "$T/result.txt" ) &
	subpid=$!
	sleep 1
	mosquitto_pub -p "$MQTT_PORT" -t aigwtest/cmd/priority -m '{"target":"phone","ttl":60}'
	wait "$subpid"
	grep -q '"ok":true' "$T/result.txt" || { cat "$T/result.txt"; fail "MQTT command not carried out"; }
	agctl plugin disable mqtt > /dev/null
	sleep 2
	sub 'aigwtest/status' 2 | grep -q offline || fail "status not offline after stop"
else
	echo "   NOTE: mosquitto not installed: MQTT checks skipped"
fi

echo "PLUG TEST PASSED"
