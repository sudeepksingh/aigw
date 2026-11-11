#!/bin/sh
# Data model test: ag.dm, the one layer USP (TR-369) and TR-069 adapters share.
# agd alone on a private ubus; no protocol client needed.
#
# Checks:
#   - schema: parameters, commands, events; docs/datamodel/aigw-dm.xml is what
#     scripts/dm-xml makes from it (the published file can't drift)
#   - get: whole subtrees, one instance, unknown paths and gone instances as errors
#   - set: all or nothing; read-only and invalid values refused
#   - a management system ("origin": "usp") doesn't see alert subjects with
#     mgmt.alert_details 0, nor may it quarantine with mgmt.actions 0; agctl may
#   - events: ag.dm.event Device.X_AIGW_.Alert!, without the name
#   - instance numbers: kept over a restart, never reused
# Needs root (ubusd objects), python3.
#
#   sudo tests/dm-test.sh [build-dir]
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

fail() { echo "DM TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

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

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '0'
	option alert_details '0'
	option firmware '0'
	option firmware_handler '$REPO/tests/fw/firmware'

config lan 'lan'
	list iface 'lo'
EOF
cat > "$T/leases" <<EOF
1999999999 02:00:00:00:98:01 10.96.0.11 printer *
1999999999 02:00:00:00:98:02 10.96.0.12 phone *
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_TEST_FW_DIR="$T/fw"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5

agctl() { "$BIN/agctl/agctl" "$@"; }
call() { "$UBUS" -s "$AIGW_UBUS" call ag.dm "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }

echo "== schema, and the published data model file"
agctl -j dm schema > "$T/schema.json"
js "sys.exit(0 if len(d['params']) >= 50 and len(d['commands']) == 7 and len(d['events']) == 2 and
         [c['path'] for c in d['commands'] if c['async']] == ['Device.DeviceInfo.FirmwareImage.{i}.Download()', 'Device.DeviceInfo.FirmwareImage.{i}.Activate()'] else 1)" < "$T/schema.json" ||
	fail "schema: $(head -c 300 "$T/schema.json")"
python3 "$REPO/scripts/dm-xml" "$T/schema.json" > "$T/dm.xml"
python3 - "$T/dm.xml" "$T/schema.json" <<'EOF' || fail "dm-xml output"
import json, sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
s = json.load(open(sys.argv[2]))
names = {o.get("name") + p.get("name") for o in root.iter("object") for p in o.findall("parameter")}
missing = [p["path"] for p in s["params"] if p["path"] not in names]
sys.exit("missing in XML: %s" % missing[:5] if missing else 0)
EOF
[ "${AIGW_UPDATE_DM_XML:-0}" = 1 ] && cp "$T/dm.xml" "$REPO/docs/datamodel/aigw-dm.xml"   # after changing the data model
cmp -s "$T/dm.xml" "$REPO/docs/datamodel/aigw-dm.xml" ||
	fail "docs/datamodel/aigw-dm.xml is out of date: agctl -j dm schema | scripts/dm-xml > docs/datamodel/aigw-dm.xml"
echo "   $(js "print(len(d['params']), 'parameters,', len(d['commands']), 'commands,', len(d['events']), 'events')" < "$T/schema.json")"

echo "== get"
call get '{"paths":["Device.Hosts."]}' > "$T/hosts.json"
js "v = d['values']; sys.exit(0 if v['Device.Hosts.HostNumberOfEntries'] == '2' and sorted(x for k, x in v.items() if k.endswith('.HostName')) == ['phone', 'printer'] else 1)" < "$T/hosts.json" ||
	fail "Device.Hosts.: $(cat "$T/hosts.json")"
PHONE=$(js "print([k[:-len('HostName')] for k, x in d['values'].items() if x == 'phone'][0])" < "$T/hosts.json")
PRINTER=$(js "print([k[:-len('HostName')] for k, x in d['values'].items() if x == 'printer'][0])" < "$T/hosts.json")
call get "{\"paths\":[\"$PHONE\"]}" | js "sys.exit(0 if all(k.startswith('$PHONE') for k in d['values']) and d['values']['${PHONE}PhysAddress'] == '02:00:00:00:98:02' else 1)" ||
	fail "one instance"
call get '{"paths":["Device.Nope.", "Device.Hosts.Host.99.HostName", "Device.X_AIGW_.Mode"]}' > "$T/err.json"
js "e = d.get('errors', {}); sys.exit(0 if set(e) == {'Device.Nope.', 'Device.Hosts.Host.99.HostName'} and d['values']['Device.X_AIGW_.Mode'] == 'advise' else 1)" < "$T/err.json" ||
	fail "unknown paths: $(cat "$T/err.json")"

echo "== set: all or nothing"
call set "{\"params\":{\"Device.X_AIGW_.Mode\":\"enforce\",\"${PHONE}HostName\":\"x\"}}" > "$T/set.json"
js "sys.exit(0 if not d['ok'] and d['errors']['${PHONE}HostName']['code'] == 'read_only' else 1)" < "$T/set.json" || fail "set: $(cat "$T/set.json")"
agctl dm get Device.X_AIGW_.Mode | grep -q '= advise' || fail "a refused set must change nothing"
call set '{"params":{"Device.X_AIGW_.Mode":"sometimes"}}' | js "sys.exit(0 if d['errors']['Device.X_AIGW_.Mode']['code'] == 'invalid_value' else 1)" ||
	fail "invalid value"
# "check": would it be taken? (TR-069 sets one value at a time and commits later)
call set '{"params":{"Device.X_AIGW_.Mode":"enforce"},"check":true}' | js "sys.exit(0 if d['ok'] else 1)" || fail "set check"
agctl dm get Device.X_AIGW_.Mode | grep -q '= advise' || fail "a set with check must change nothing"

echo "== firmware: two banks (tests/fw/firmware, the handler an OEM replaces)"
FW=Device.DeviceInfo.FirmwareImage
call get '{"paths":["Device.DeviceInfo."]}' > "$T/fw0.json"
js "v = d['values']
sys.exit(0 if v['Device.DeviceInfo.FirmwareImageNumberOfEntries'] == '2' and v['Device.DeviceInfo.ActiveFirmwareImage'] == '$FW.1' and
         v['$FW.1.Status'] == 'Active' and v['$FW.2.Status'] == 'NoImage' and v['Device.DeviceInfo.SoftwareVersion'] == '1.0' and
         v['Device.DeviceInfo.AdditionalSoftwareVersion'].startswith('AIGW OS ') else 1)" < "$T/fw0.json" ||
	fail "firmware images: $(cat "$T/fw0.json")"
printf 'AIGW-TEST-FIRMWARE 2.0\n' > "$T/good.img"
printf 'not firmware\n' > "$T/bad.img"
fwdo() { call operate "{\"command\":\"$FW.$1\",\"args\":{\"URL\":\"file://$2\"}${3:+,\"origin\":\"$3\"}}"; }
fwdo 1.Download\(\) "$T/good.img" | js "sys.exit(0 if not d['ok'] and 'running' in d['message'] else 1)" || fail "a download into the running image"
fwdo 2.Download\(\) "$T/good.img" usp | js "sys.exit(0 if not d['ok'] and 'mgmt.firmware' in d['message'] else 1)" ||
	fail "a download by a management system with mgmt.firmware 0"
fwdo 2.Download\(\) "$T/bad.img" | js "sys.exit(0 if not d['ok'] and 'not a firmware image' in d['message'] else 1)" || fail "a bad image"
agctl dm get "$FW.2.Status" | grep -q '= ValidationFailed' || fail "a bad image: $(agctl dm get "$FW.2.")"
fwdo 2.Download\(\) "$T/good.img" | js "sys.exit(0 if d['ok'] else 1)" || fail "a download"
agctl dm get "$FW.2." > "$T/fw2.txt"
grep -q 'Status = Available' "$T/fw2.txt" && grep -q 'Version = 2.0' "$T/fw2.txt" || fail "the downloaded image: $(cat "$T/fw2.txt")"
call operate "{\"command\":\"$FW.2.Activate()\"}" | js "sys.exit(0 if d['ok'] else 1)" || fail "activate"
agctl dm get Device.DeviceInfo.BootFirmwareImage | grep -q "= $FW.2" || fail "the image to boot"
i=0
until agctl dm get Device.DeviceInfo.SoftwareVersion | grep -q '= 2.0'; do
	i=$((i + 1)); [ $i -le 30 ] || fail "no reboot into the new image: $(cat "$T/fw/reboots" 2>/dev/null)"; sleep 0.3
done
agctl dm get Device.DeviceInfo.ActiveFirmwareImage | grep -q "= $FW.2" || fail "the active image after the reboot"
echo "   bank 2 downloaded, checked (a bad image refused), activated, rebooted into: version 2.0"

echo "== who asks: alert details and actions"
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & L=$!; PIDS="$PIDS $L"
sleep 0.3
"$UBUS" -s "$AIGW_UBUS" send ag.dns '{"client":"10.96.0.12","qname":"xkqjzvbwptmr.com","rcode":3}'
sleep 0.5
call get '{"paths":["Device.X_AIGW_.Alert."]}' > "$T/local.json"
ALERT=$(js "print([k[:-len('.Kind')] for k, x in d['values'].items() if k.endswith('.Kind') and x == 'dga'][0])" < "$T/local.json") ||
	fail "no alert in the data model: $(cat "$T/local.json")"
js "sys.exit(0 if d['values']['$ALERT.Subject'] == 'xkqjzvbwptmr.com' and d['values']['$ALERT.Host'] == '${PHONE%.}' else 1)" < "$T/local.json" ||
	fail "agctl sees the alert in full: $(cat "$T/local.json")"
call get "{\"paths\":[\"$ALERT.Subject\",\"$ALERT.Reason\",\"$ALERT.Kind\"],\"origin\":\"usp\"}" |
	js "v = d['values']; sys.exit(0 if v['$ALERT.Subject'] == '' and v['$ALERT.Reason'] == '' and v['$ALERT.Kind'] == 'dga' else 1)" ||
	fail "a management system must not see the name (alert_details 0)"
grep -q 'Device.X_AIGW_.Alert!' "$T/events.log" || fail "no Alert! event: $(cat "$T/events.log")"
grep -q 'xkqjzvbwptmr' "$T/events.log" && fail "the event must not carry the name"
call operate "{\"command\":\"Device.X_AIGW_.Quarantine()\",\"args\":{\"Host\":\"${PHONE%.}\"},\"origin\":\"usp\"}" |
	js "sys.exit(0 if not d['ok'] and d['code'] == 'refused' and 'mgmt.actions' in d['message'] else 1)" ||
	fail "quarantine by a management system with mgmt.actions 0"
call operate '{"command":"Device.X_AIGW_.Decision.77.Approve()"}' | js "sys.exit(0 if d['code'] == 'not_found' else 1)" ||
	fail "a decision that doesn't exist"

echo "== instance numbers survive a restart and are never reused"
kill "$AGD"; wait "$AGD" 2>/dev/null || true
sleep 1
cat > "$T/leases" <<EOF
1999999999 02:00:00:00:98:02 10.96.0.12 phone *
1999999999 02:00:00:00:98:03 10.96.0.13 laptop *
EOF
python3 - "$T/state.json" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
s["devices"] = [d for d in s["devices"] if d.get("mac") != "02:00:00:00:98:01"]   # the printer is gone
json.dump(s, open(sys.argv[1], "w"))
EOF
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
call get '{"paths":["Device.Hosts."]}' > "$T/after.json"
js "v = d['values']; sys.exit(0 if v.get('${PHONE}HostName') == 'phone' else 1)" < "$T/after.json" ||
	fail "the phone's number changed: $(cat "$T/after.json")"
LAPTOP=$(js "print([k[:-len('HostName')] for k, x in d['values'].items() if x == 'laptop'][0])" < "$T/after.json")
[ "$LAPTOP" != "$PRINTER" ] && [ "$LAPTOP" != "$PHONE" ] || fail "a number was reused: laptop $LAPTOP, printer was $PRINTER"
echo "   phone $PHONE kept; printer's $PRINTER not reused (laptop $LAPTOP)"

echo "== an OEM's names and vendor prefix (mgmt.vendor_prefix)"
restart_agd() {
	kill "$AGD"; wait "$AGD" 2>/dev/null || true
	sleep 0.5
	"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
	sleep 1.5
}
sed -i "/^config mgmt 'mgmt'/a\\
	option vendor_prefix 'X_EXAMPLE-COM_'\\
	option manufacturer 'Example Networks'\\
	option model_name 'EX-1000'\\
	option product_class 'EX Gateway'" "$T/config/aigw"
restart_agd
agctl -j dm schema > "$T/schema2.json"
js "
ps = [p['path'] for p in d['params']] + [c['path'] for c in d['commands']] + [e['path'] for e in d['events']] + [o['path'] for o in d['objects']]
bad = [p for p in ps if 'X_AIGW' in p]
ok = d['vendor_prefix'] == 'X_EXAMPLE-COM_' and not bad and 'Device.X_EXAMPLE-COM_.Mode' in ps and \
     'Device.Hosts.Host.{i}.X_EXAMPLE-COM_Type' in ps and 'Device.X_EXAMPLE-COM_.Decision.{i}.Approve()' in ps and \
     'Device.X_EXAMPLE-COM_.Alert!' in ps and len(d['params']) == $(js "print(len(d['params']))" < "$T/schema.json")
sys.exit(0 if ok else 'bad: %s' % bad[:3])" < "$T/schema2.json" || fail "schema with a vendor prefix: $(head -c 400 "$T/schema2.json")"
call get '{"paths":["Device.X_EXAMPLE-COM_.Mode","Device.X_AIGW_.Mode","Device.DeviceInfo."]}' > "$T/vx.json"
js "v = d['values']; e = d.get('errors', {})
sys.exit(0 if v['Device.X_EXAMPLE-COM_.Mode'] == 'advise' and set(e) == {'Device.X_AIGW_.Mode'} and
         v['Device.DeviceInfo.Manufacturer'] == 'Example Networks' and v['Device.DeviceInfo.ModelName'] == 'EX-1000' and
         v['Device.DeviceInfo.ProductClass'] == 'EX Gateway' and 'Device.DeviceInfo.X_EXAMPLE-COM_DataModelVersion' in v else 1)" < "$T/vx.json" ||
	fail "get with a vendor prefix: $(cat "$T/vx.json")"
: > "$T/events.log"
call operate '{"command":"Device.X_EXAMPLE-COM_.Block()","args":{"Domain":"bad-tracker.com"}}' > "$T/q.json"
js "sys.exit(0 if d['ok'] and d['out']['Decision'].startswith('Device.X_EXAMPLE-COM_.Decision.') else 1)" < "$T/q.json" ||
	fail "a command under the vendor prefix: $(cat "$T/q.json")"
sleep 0.5
grep -q 'Device.X_EXAMPLE-COM_.DecisionChanged!' "$T/events.log" || fail "events under the vendor prefix: $(cat "$T/events.log")"
echo "   $(js "print(d['vendor_prefix'])" < "$T/schema2.json"): $(js "print(d['values']['Device.DeviceInfo.Manufacturer'], d['values']['Device.DeviceInfo.ModelName'])" < "$T/vx.json")"
# a prefix that isn't one: the default, and a warning
sed -i "s/option vendor_prefix 'X_EXAMPLE-COM_'/option vendor_prefix 'acme'/" "$T/config/aigw"
restart_agd
agctl -j dm schema | js "sys.exit(0 if d['vendor_prefix'] == 'X_AIGW_' else 1)" || fail "an invalid prefix must fall back to X_AIGW_"
grep -q "vendor_prefix 'acme' is not" "$T/agd.log" || fail "no warning for an invalid prefix"

echo "DM TEST OK"
