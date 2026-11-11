#!/bin/sh
# USP (TR-369) test: a real USP controller over MQTT 5 manages AIGW OS through
# OB-USP-Agent and the aigw-usp plug-in. Everything runs in network namespaces,
# so actions change only the test's own nft rules.
#
#   laptop 10.96.0.2 ─┐                gateway namespace: ubusd, mosquitto, agd,
#   tv     10.96.0.3 ─┴─ bridge ── 10.96.0.1  obuspa + aigw-usp.so, the controller
#
# Checks:
#   - the controller reads AIGW OS's data model: software version, devices,
#     and lists its commands and events
#   - an alert reaches the controller as a USP event (Device.X_AIGW_.Alert!),
#     without the name looked up while alert details are off
#   - it approves and undoes the decision AIGW proposed; the decision records who
#   - with mgmt.actions 0 it may not quarantine or change the mode; with 1 it may
#     block and change the mode (saved), but never quarantine a protected device
#   - device instance numbers stay the same over an agd restart
# Needs root, ip, nft, mosquitto, python3 (paho-mqtt, protobuf), protoc, and
# obuspa (AIGW_OBUSPA; skipped without it, AIGW_REQUIRE_USP=1 makes that a failure).
#
#   sudo AIGW_OBUSPA=/path/to/obuspa tests/usp-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=aiguspg$R BR=agub$R
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for c in 2 3; do ip netns del "aigusp$c$R" 2>/dev/null || true; done
	ip netns del "$NG" 2>/dev/null || true
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "USP TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }
skip() {
	[ "${AIGW_REQUIRE_USP:-0}" = 1 ] && fail "$*"
	echo "USP TEST SKIP: $*"; exit 0
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft python3; do command -v $t >/dev/null || fail "missing $t"; done
OBUSPA=${AIGW_OBUSPA:-$(command -v obuspa || true)}
[ -n "$OBUSPA" ] && [ -x "$OBUSPA" ] || skip "no obuspa (set AIGW_OBUSPA)"
[ -f "$BIN/usp/aigw-usp.so" ] || skip "aigw-usp.so not built (cmake -DAIGW_OBUSPA_SRC=...)"
command -v mosquitto >/dev/null || skip "no mosquitto"
command -v protoc >/dev/null || skip "no protoc"
python3 -c "import paho.mqtt, google.protobuf" 2>/dev/null || skip "python3 needs paho-mqtt and protobuf"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

echo "== network"
ip netns add "$NG"
ip -n "$NG" link set lo up
ip -n "$NG" link add "$BR" type bridge
ip -n "$NG" addr add 10.96.0.1/24 dev "$BR"
ip -n "$NG" link set "$BR" up
for c in 2 3; do
	n=aigusp$c$R
	ip netns add "$n"
	ip link add "uv$c$R" netns "$NG" type veth peer name eth0 netns "$n"
	ip -n "$NG" link set "uv$c$R" master "$BR" up
	ip -n "$n" link set eth0 address "02:00:00:00:97:0$c"
	ip -n "$n" addr add "10.96.0.$c/24" dev eth0
	ip -n "$n" link set eth0 up
	ip -n "$n" route add default via 10.96.0.1
done
gw() { ip netns exec "$NG" "$@"; }

echo "== USP messages (protoc)"
mkdir -p "$T/pb"
protoc -I"$REPO/tests/usp" --python_out="$T/pb" "$REPO/tests/usp/usp-msg-1-5.proto" "$REPO/tests/usp/usp-record-1-5.proto" ||
	fail "protoc"
[ -f "$T/pb/usp_msg_1_5_pb2.py" ] && [ -f "$T/pb/usp_record_1_5_pb2.py" ] || fail "protoc wrote: $(ls "$T/pb")"

write_config() { # write_config <actions> <alert details>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option mode 'advise'
	option dns_policy '$T/dns-policy'
	list protect '02:00:00:00:97:03'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '$1'
	option alert_details '$2'
	option firmware_handler '$REPO/tests/fw/firmware'

config usp 'usp'
	option broker '127.0.0.1'
	option port '18831'
	option tls '0'
	option agent_id 'proto::aigw-test'
	option agent_topic '/usp/agent/test'
	option log_level '2'

config lan 'lan'
	list iface '$BR'
EOF
}
write_config 0 0
cat > "$T/leases" <<EOF
1999999999 02:00:00:00:97:02 10.96.0.2 laptop *
1999999999 02:00:00:00:97:03 10.96.0.3 tv *
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT_FILE="$T/aigw.nft" AIGW_TEST_FW_DIR="$T/fw"
export AIGW_USP_DIR="$T/usp" AIGW_OBUSPA="$OBUSPA" AIGW_USP_PLUGIN="$BIN/usp/aigw-usp.so" AIGW_AGCTL="$BIN/agctl/agctl"
export USP_PB_DIR="$T/pb" USP_PORT=18831 USP_AGENT_ID=proto::aigw-test USP_AGENT_TOPIC=/usp/agent/test

# daemons are started with ip netns exec directly (not through gw), so $! is the daemon itself
ip netns exec "$NG" "$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
ip netns exec "$NG" mosquitto -p 18831 > "$T/mosquitto.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
gw "$REPO/scripts/aigw-nft" apply 2> "$T/nft.log" || fail "aigw-nft apply"
ip netns exec "$NG" "$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
ip netns exec "$NG" sh "$REPO/usp/aigw-usp-run" > "$T/obuspa.log" 2>&1 & PIDS="$PIDS $!"

agctl() { "$BIN/agctl/agctl" "$@"; }
uspc() { gw python3 "$REPO/tests/usp/uspc.py" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }

echo "== the controller reaches the agent"
i=0
until uspc get Device.DeviceInfo.SoftwareVersion > "$T/ver.json" 2>/dev/null; do
	i=$((i + 1)); [ $i -ge 15 ] && fail "the USP agent does not answer"
	sleep 1
done
# the firmware's version (its handler's; the test handler starts at 1.0), AIGW's next to it
got=$(js "print(d.get('Device.DeviceInfo.SoftwareVersion', ''))" < "$T/ver.json")
[ "$got" = 1.0 ] || fail "SoftwareVersion: '$got', expected the firmware's, 1.0"
want="AIGW OS $("$BIN/agd/agd" -v | cut -d' ' -f2)"
uspc get Device.DeviceInfo.AdditionalSoftwareVersion | js "sys.exit(0 if d['Device.DeviceInfo.AdditionalSoftwareVersion'] == '$want' else 1)" ||
	fail "AdditionalSoftwareVersion should be $want"
# the identity TR-069 reports too: agd's
serial=$(agctl dm get Device.DeviceInfo.SerialNumber | sed -n 's/^[^=]* = //p')
uspc get Device.DeviceInfo.SerialNumber | js "sys.exit(0 if d.get('Device.DeviceInfo.SerialNumber') == '$serial' and '$serial' else 1)" ||
	fail "SerialNumber over USP should be agd's ($serial)"
echo "   AIGW OS $got, serial $serial"

uspc get "Device.Hosts.Host.*.HostName" "Device.Hosts.Host.*.PhysAddress" > "$T/hosts.json"
LAPTOP=$(js "print([k[:-len('.PhysAddress')] for k, v in d.items() if v == '02:00:00:00:97:02'][0])" < "$T/hosts.json") ||
	fail "the laptop is not in Device.Hosts: $(cat "$T/hosts.json")"
TV=$(js "print([k[:-len('.PhysAddress')] for k, v in d.items() if v == '02:00:00:00:97:03'][0])" < "$T/hosts.json")
js "sys.exit(0 if d['$LAPTOP.HostName'] == 'laptop' else 1)" < "$T/hosts.json" || fail "laptop's HostName"
echo "   devices: laptop $LAPTOP, tv $TV"

uspc supported Device.X_AIGW_. > "$T/sup.json"
js "sys.exit(0 if 'Device.X_AIGW_.Decision.{i}.Approve()' in d['commands'] and 'Device.X_AIGW_.Alert!' in d['events'] and 'Device.X_AIGW_.Quarantine()' in d['commands'] else 1)" < "$T/sup.json" ||
	fail "commands and events missing: $(cat "$T/sup.json")"

echo "== an alert arrives as a USP event, without the name"
uspc watch 'Device.X_AIGW_.Alert!' 10 -- "ubus -s $AIGW_UBUS send ag.dns '{\"client\":\"10.96.0.2\",\"qname\":\"xkqjzvbwptmr.com\",\"rcode\":3}'" > "$T/ev.json"
cat "$T/ev.json" | sed 's/^/   /' | head -14
js "p = d['params']; sys.exit(0 if d['event'] == 'Device.X_AIGW_.Alert!' and p['Kind'] == 'dga' and p['Host'] == '$LAPTOP' and p['Subject'] == '' else 1)" < "$T/ev.json" ||
	fail "wrong or missing Alert! event"
ALERT=$(js "print(d['params']['Alert'])" < "$T/ev.json")
uspc get "$ALERT.Subject" "$ALERT.Kind" > "$T/alert.json"
js "sys.exit(0 if d['$ALERT.Subject'] == '' and d['$ALERT.Kind'] == 'dga' else 1)" < "$T/alert.json" ||
	fail "the alert's subject should be hidden (alert_details 0): $(cat "$T/alert.json")"

echo "== the controller approves the proposed quarantine, then undoes it"
sleep 0.5
uspc get "Device.X_AIGW_.Decision.*.State" "Device.X_AIGW_.Decision.*.Alert" > "$T/dec.json"
DEC=$(js "print([k[:-len('.State')] for k, v in d.items() if k.endswith('.State') and v == 'proposed'][0])" < "$T/dec.json") ||
	fail "no proposed decision: $(cat "$T/dec.json")"
js "sys.exit(0 if d['$DEC.Alert'] == '$ALERT' else 1)" < "$T/dec.json" || fail "the decision should point at the alert"
uspc operate "$DEC.Approve()" > "$T/ap.json"
js "sys.exit(0 if d['ok'] and d['out']['State'] == 'active' else 1)" < "$T/ap.json" || fail "approve: $(cat "$T/ap.json")"
gw nft list set inet ag quarantine 2>/dev/null | grep -qi '02:00:00:00:97:02' || fail "the laptop should be quarantined in nft"
agctl -j decisions | js "sys.exit(0 if any('approved by usp' in x['origin'] for x in d['decisions']) else 1)" ||
	fail "the decision should say the controller approved it"
uspc operate "$DEC.Undo()" > "$T/undo.json"
js "sys.exit(0 if d['ok'] and d['out']['State'] == 'reverted' else 1)" < "$T/undo.json" || fail "undo: $(cat "$T/undo.json")"
gw nft list set inet ag quarantine 2>/dev/null | grep -qi '02:00:00:00:97:02' && fail "undo should lift the quarantine"
echo "   $DEC: proposed -> active -> reverted"

echo "== mgmt.actions 0: no quarantine, no mode change"
uspc operate "Device.X_AIGW_.Quarantine()" "Host=$LAPTOP" > "$T/q0.json"
js "sys.exit(0 if not d['ok'] and 'mgmt.actions' in d['err_msg'] else 1)" < "$T/q0.json" || fail "quarantine should be refused: $(cat "$T/q0.json")"
uspc set "Device.X_AIGW_.Mode=enforce" > "$T/m0.json"
js "sys.exit(0 if not d['ok'] else 1)" < "$T/m0.json" || fail "the mode change should be refused: $(cat "$T/m0.json")"

echo "== mgmt.actions 1 and alert details on (agd restarted)"
kill "$AGD"; wait "$AGD" 2>/dev/null || true
sleep 1                                   # ubusd drops the old agd's objects
write_config 1 1
ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 2
uspc get "Device.Hosts.Host.*.PhysAddress" "$ALERT.Subject" > "$T/after.json"
js "sys.exit(0 if d.get('$LAPTOP.PhysAddress') == '02:00:00:00:97:02' else 1)" < "$T/after.json" ||
	fail "the laptop's instance number changed over the restart: $(cat "$T/after.json")"
js "sys.exit(0 if d['$ALERT.Subject'] == 'xkqjzvbwptmr.com' else 1)" < "$T/after.json" || fail "alert details should now be shared"
uspc operate "Device.X_AIGW_.Quarantine()" "Host=$TV" > "$T/qtv.json"
js "sys.exit(0 if not d['ok'] and 'protect' in d['err_msg'] else 1)" < "$T/qtv.json" ||
	fail "a protected device must never be quarantined: $(cat "$T/qtv.json")"
uspc operate "Device.X_AIGW_.Block()" "Domain=bad-tracker.com" "Duration=600" > "$T/blk.json"
js "sys.exit(0 if d['ok'] and d['out']['Decision'].startswith('Device.X_AIGW_.Decision.') else 1)" < "$T/blk.json" || fail "block: $(cat "$T/blk.json")"
B=$(js "print(d['out']['Decision'])" < "$T/blk.json")
uspc get "$B.State" "$B.Target" "$B.Origin" > "$T/b.json"
js "sys.exit(0 if d['$B.State'] == 'active' and d['$B.Target'] == 'bad-tracker.com' and 'usp' in d['$B.Origin'] else 1)" < "$T/b.json" ||
	fail "the block decision: $(cat "$T/b.json")"
uspc set "Device.X_AIGW_.Mode=enforce" > "$T/m1.json"
js "sys.exit(0 if d['ok'] else 1)" < "$T/m1.json" || fail "mode: $(cat "$T/m1.json")"
grep -q "option mode 'enforce'" "$T/config/aigw" || fail "the new mode should be saved in the config"
agctl -j status | js "sys.exit(0 if d['mode'] == 'enforce' else 1)" || fail "agd should be in enforce mode"
echo "   blocked bad-tracker.com ($B), mode enforce, saved"

echo "== the same data model, locally"
agctl dm get Device.X_AIGW_.Mode | grep -q '= enforce' || fail "agctl dm get"

echo "== firmware over USP: Download() with AutoActivate, an asynchronous command"
FW=Device.DeviceInfo.FirmwareImage
mkdir -p "$T/www"
printf 'AIGW-TEST-FIRMWARE 3.1\n' > "$T/www/fw.img"
SUM=$(sha256sum "$T/www/fw.img" | cut -d' ' -f1)
ip netns exec "$NG" python3 -m http.server 18089 --bind 127.0.0.1 --directory "$T/www" > "$T/http.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
uspc supported "$FW." | js "sys.exit(0 if '$FW.{i}.Download()' in d['commands'] else 1)" || fail "Download() not listed"
uspc operate-async "$FW.2.Download()" 30 "URL=http://127.0.0.1:18089/fw.img" "CheckSumAlgorithm=SHA-256" "CheckSum=0000" > "$T/fwbad.json"
js "sys.exit(0 if not d['ok'] and 'checksum' in d.get('err_msg', '') else 1)" < "$T/fwbad.json" ||
	fail "a wrong checksum: $(cat "$T/fwbad.json")"
uspc operate-async "$FW.2.Download()" 30 "URL=http://127.0.0.1:18089/fw.img" "AutoActivate=true" \
	"CheckSumAlgorithm=SHA-256" "CheckSum=$SUM" > "$T/fw.json"
js "sys.exit(0 if d['ok'] else 1)" < "$T/fw.json" || fail "Download(): $(cat "$T/fw.json")"
# agd reboots into it (the test handler pretends to)
i=0
until agctl dm get Device.DeviceInfo.ActiveFirmwareImage | grep -q "= $FW.2"; do
	i=$((i + 1)); [ $i -le 30 ] || fail "no reboot into the new image: $(agctl dm get Device.DeviceInfo.)"; sleep 0.3
done
uspc get "$FW.2.Version" "$FW.2.Status" | js "sys.exit(0 if d['$FW.2.Version'] == '3.1' and d['$FW.2.Status'] == 'Active' else 1)" ||
	fail "the new image over USP"
echo "   a wrong checksum refused; downloaded over HTTP, checked, activated, rebooted into 3.1"

echo "USP TEST OK"
