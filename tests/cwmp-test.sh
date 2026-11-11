#!/bin/sh
# TR-069 test: an ACS manages AIGW OS through icwmp and aigw-cwmp-dm, the data
# model daemon in front of agd's ag.dm. Everything runs in network namespaces.
#
#   laptop 10.97.0.2 ─┐                gateway: ubusd, agd, aigw-cwmp-dm, icwmpd
#   tv     10.97.0.3 ─┴─ bridge ── 10.97.0.1    wan0 10.98.0.1 ── 10.98.0.2 ACS
#
# Part 1, the data model daemon alone, asked as icwmp asks it (ubus bbfdm):
#   - values and types; identity from agd; ConnectionRequestURL from the WAN
#   - names: next level, instances; this adapter's Request. and LastEvent.
#   - set: refusals with CWMP fault codes; a write checked at set and applied
#     only at commit, dropped at revert; ACS settings saved in both configs
#   - agd's events in LastEvent. (no name looked up: alert details off)
#   - commands as Request.: refused quarantine with mgmt.actions 0, an approved
#     decision that really quarantines in nft, then undone
# Part 2, with icwmp (AIGW_ICWMPD; skipped without it, AIGW_REQUIRE_CWMP=1 makes
# that a failure) and a real ACS (tests/cwmp/acs.py) over HTTP:
#   - the boot Inform: DeviceId and parameters from agd
#   - connection requests; GetParameterValues / Names; SetParameterValues
#     with faults; commands; an alert as an active notification (4 VALUE CHANGE)
# All of it under an ISP's own vendor prefix (mgmt.vendor_prefix X_EXAMPLE-COM_),
# so nothing in the TR-069 path may assume X_AIGW_.
# Needs root, ip, nft, python3; for part 2 also unshare and overlayfs (icwmp's
# paths, /etc/config/cwmp, /etc/icwmpd, are private to the test).
#
#   sudo [AIGW_ICWMPD=/path/to/icwmpd] tests/cwmp-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=agcwg$R NI=agcwi$R BR=agcb$R
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.5
	for p in $PIDS; do kill -9 "$p" 2>/dev/null || true; done
	for c in 2 3; do ip netns del "agcw$c$R" 2>/dev/null || true; done
	ip netns del "$NI" 2>/dev/null || true
	ip netns del "$NG" 2>/dev/null || true
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() {
	echo "CWMP TEST FAIL: $*" >&2
	echo "--- logs" >&2
	for f in "$T"/*.log; do
		[ -f "$f" ] || continue
		echo "==> $f <==" >&2
		# icwmp's config dump says nothing new
		case $f in */icwmp.log) grep -v 'CWMP CONFIG' "$f" | tail -n 120 >&2 ;; *) tail -n 40 "$f" >&2 ;; esac
	done
	exit 1
}

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft python3; do command -v $t >/dev/null || fail "missing $t"; done
[ -x "$BIN/cwmp/aigw-cwmp-dm" ] || fail "aigw-cwmp-dm not built"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

echo "== network"
ip netns add "$NG"
ip -n "$NG" link set lo up
ip -n "$NG" link add "$BR" type bridge
ip -n "$NG" addr add 10.97.0.1/24 dev "$BR"
ip -n "$NG" link set "$BR" up
for c in 2 3; do
	n=agcw$c$R
	ip netns add "$n"
	ip link add "cv$c$R" netns "$NG" type veth peer name eth0 netns "$n"
	ip -n "$NG" link set "cv$c$R" master "$BR" up
	ip -n "$n" link set eth0 address "02:00:00:00:96:0$c"
	ip -n "$n" addr add "10.97.0.$c/24" dev eth0
	ip -n "$n" link set eth0 up
	ip -n "$n" route add default via 10.97.0.1
done
ip netns add "$NI"
ip -n "$NI" link set lo up
ip link add wan0 netns "$NG" type veth peer name eth0 netns "$NI"
ip -n "$NG" addr add 10.98.0.1/24 dev wan0
ip -n "$NG" link set wan0 up
ip -n "$NI" addr add 10.98.0.2/24 dev eth0
ip -n "$NI" link set eth0 up
gw() { ip netns exec "$NG" "$@"; }

write_config() { # write_config <actions>
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
	list protect '02:00:00:00:96:03'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '$1'
	option alert_details '0'
	option serial 'AIGWTEST0001'
	# an ISP's own vendor prefix: every name below is under it
	option vendor_prefix 'X_EXAMPLE-COM_'
	# firmware: two banks (the test handler, an OEM's A/B flash in miniature)
	option firmware_handler '$REPO/tests/fw/firmware'

config cwmp 'cwmp'
	# the ACS over HTTPS, and it wants this gateway's client certificate
	option url 'https://10.98.0.2:7557/'
	option ca_file '$T/tls/ca.crt'
	option client_cert '$T/tls/client.crt'
	option client_key '$T/tls/client.key'
	option periodic_interval '86400'
	option cr_username 'cr'
	option cr_password 'crpw'
	option wan 'wan'
	option wan_device 'wan0'
	option notify_interval '2'
	option log_level 'DEBUG'

config lan 'lan'
	list iface '$BR'
EOF
}
write_config 0
cat > "$T/leases" <<EOF
1999999999 02:00:00:00:96:02 10.97.0.2 laptop *
1999999999 02:00:00:00:96:03 10.97.0.3 tv *
1999999999 02:00:00:00:96:04 10.97.0.4 phone *
EOF
mkdir -p "$T/cwmpconf"
cat > "$T/cwmpconf/cwmp" <<EOF
config acs 'acs'
	option url 'http://10.98.0.2:7557/'
	option passwd 'acs-secret'
	option periodic_inform_interval '86400'

config cpe 'cpe'
	option port '7547'
	option userid 'cr'
	option passwd 'crpw'
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT_FILE="$T/aigw.nft" AIGW_TEST_FW_DIR="$T/fw"

ip netns exec "$NG" "$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
gw "$REPO/scripts/aigw-nft" apply 2> "$T/nft.log" || fail "aigw-nft apply"
ip netns exec "$NG" "$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
AIGW_CWMP_CONFDIR="$T/cwmpconf" ip netns exec "$NG" "$BIN/cwmp/aigw-cwmp-dm" > "$T/cwmp-dm.log" 2>&1 & DM=$!; PIDS="$PIDS $DM"

agctl() { "$BIN/agctl/agctl" "$@"; }
bb() { "$UBUS" -s "$AIGW_UBUS" call bbfdm "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
# the value of one parameter, as icwmp reads it
val() { bb get "{\"path\":\"$1\",\"optional\":{\"proto\":\"cwmp\"}}" | js "r = d['results'][0]; print(r.get('data', 'FAULT %s' % r.get('fault')))"; }
# set one value; prints "ok" or the fault code
setv() { bb set "{\"path\":\"$1\",\"value\":\"$2\",\"optional\":{\"proto\":\"cwmp\"}}" | js "r = d['results'][0]; print('ok' if 'data' in r else r['fault'])"; }
commit() { "$UBUS" -s "$AIGW_UBUS" call bbf.config commit '{"services":["/etc/config/aigw"],"proto":"cwmp"}' >/dev/null; }
revert() { "$UBUS" -s "$AIGW_UBUS" call bbf.config revert '{"services":["/etc/config/aigw"],"proto":"cwmp"}' >/dev/null; }

echo "== part 1: the data model daemon, as icwmp asks it"
i=0
until "$UBUS" -s "$AIGW_UBUS" list bbfdm >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -ge 30 ] && fail "aigw-cwmp-dm did not publish bbfdm"
	sleep 0.3
done
want=1.0        # the firmware's version: the test handler's first bank
bb get '{"path":"Device.DeviceInfo.SoftwareVersion"}' > "$T/ver.json"
js "r = d['results'][0]; sys.exit(0 if r['data'] == '$want' and r['type'] == 'xsd:string' else 1)" < "$T/ver.json" ||
	fail "SoftwareVersion: $(cat "$T/ver.json")"
[ "$(val Device.DeviceInfo.SerialNumber)" = AIGWTEST0001 ] || fail "SerialNumber from mgmt.serial"
val Device.DeviceInfo.ManufacturerOUI | grep -Eq '^[0-9A-F]{6}$' || fail "ManufacturerOUI: $(val Device.DeviceInfo.ManufacturerOUI)"
bb get '{"path":"Device.DeviceInfo.UpTime"}' | js "sys.exit(0 if d['results'][0]['type'] == 'xsd:unsignedInt' else 1)" || fail "UpTime's type"
[ "$(val Device.ManagementServer.ConnectionRequestURL)" = "http://10.98.0.1:7547/" ] ||
	fail "ConnectionRequestURL: $(val Device.ManagementServer.ConnectionRequestURL)"
[ "$(val Device.ManagementServer.URL)" = "http://10.98.0.2:7557/" ] || fail "ManagementServer.URL"
[ -z "$(val Device.ManagementServer.Password)" ] || fail "passwords must read as empty"
"$UBUS" -s "$AIGW_UBUS" call network.interface.wan status | js "sys.exit(0 if d['l3_device'] == 'wan0' else 1)" ||
	fail "network.interface.wan (no netifd on a PC)"
# every parameter icwmp puts in each Inform must answer: one fault there makes
# icwmp fail the rest of that Inform
for p in Device.RootDataModelVersion Device.DeviceInfo.HardwareVersion Device.DeviceInfo.SoftwareVersion \
	Device.DeviceInfo.ProvisioningCode Device.ManagementServer.ParameterKey \
	Device.ManagementServer.ConnectionRequestURL Device.ManagementServer.AliasBasedAddressing \
	Device.DeviceInfo.Manufacturer Device.DeviceInfo.SerialNumber Device.DeviceInfo.ProductClass Device.DeviceInfo.ManufacturerOUI; do
	val "$p" | grep -q '^FAULT' && fail "$p, which icwmp's Inform needs, is missing"
done
bb get '{"path":"Device.Nope.Nothing"}' | js "sys.exit(0 if d['results'][0]['fault'] == 9005 else 1)" || fail "unknown path: 9005"
bb get '{"path":"Device."}' | js "
p = {r['path'] for r in d['results']}
need = {'Device.DeviceInfo.SoftwareVersion', 'Device.ManagementServer.URL', 'Device.X_EXAMPLE-COM_.Mode', 'Device.X_EXAMPLE-COM_.Request.Status'}
sys.exit(0 if need <= p and len(p) > 40 else 'missing: %s' % (need - p))" || fail "get Device."
echo "   AIGW OS $want, serial AIGWTEST0001, CR URL http://10.98.0.1:7547/"

bb schema '{"path":"","first_level":true}' | js "sys.exit(0 if [r['path'] for r in d['results']] == ['Device.'] else 1)" ||
	fail "names: empty path, next level"
bb schema '{"path":"Device.","first_level":true}' > "$T/top.json"
js "
p = {r['path']: r for r in d['results']}
need = {'Device.DeviceInfo.', 'Device.Hosts.', 'Device.ManagementServer.', 'Device.X_EXAMPLE-COM_.', 'Device.RootDataModelVersion'}
deeper = [x for x in p if x.count('.') > 2 or (x.count('.') == 2 and not x.endswith('.'))]
sys.exit(0 if need <= set(p) and not deeper and p['Device.Hosts.']['type'] == 'xsd:object' else 'got %s' % sorted(p))" < "$T/top.json" ||
	fail "names: Device. next level"
bb schema '{"path":"Device.Hosts.Host.","first_level":true}' > "$T/inst.json"
js "sys.exit(0 if len(d['results']) == 3 and all(r['path'].endswith('.') for r in d['results']) else 1)" < "$T/inst.json" ||
	fail "host instances: $(cat "$T/inst.json")"
bb get '{"path":"Device.Hosts."}' > "$T/hosts.json"
host_of() { js "print([r['path'][:-len('.PhysAddress')] for r in d['results'] if r['data'] == '$1'][0])" < "$T/hosts.json"; }
LAPTOP=$(host_of 02:00:00:00:96:02) || fail "the laptop is not in Device.Hosts.: $(cat "$T/hosts.json")"
TV=$(host_of 02:00:00:00:96:03)
PHONE=$(host_of 02:00:00:00:96:04)
bb schema '{"path":"Device.X_EXAMPLE-COM_.Request.","first_level":true}' > "$T/req.json"
js "
p = {r['path']: r['data'] for r in d['results']}
r = 'Device.X_EXAMPLE-COM_.Request.'
sys.exit(0 if p.get(r + 'Command') == '1' and p.get(r + 'Host') == '1' and p.get(r + 'Domain') == '1' and p.get(r + 'Status') == '0' and p.get(r + 'Decision') == '0' else 'got %s' % p)" < "$T/req.json" ||
	fail "Request. names"
bb schema '{"path":"Device.Hosts.Host.{i}.","first_level":false,"optional":{"proto":"usp"}}' |
	js "sys.exit(0 if 'Device.Hosts.Host.{i}.PhysAddress' in [r['path'] for r in d['results']] else 1)" ||
	fail "templates (proto usp, icwmp checks multi-instance paths with them)"
echo "   devices: laptop $LAPTOP, tv $TV, phone $PHONE"

echo "== part 1: writes, with CWMP faults"
[ "$(setv Device.DeviceInfo.SoftwareVersion 9)" = 9008 ] || fail "a read-only parameter: 9008"
[ "$(setv Device.Nope.Thing 1)" = 9005 ] || fail "an unknown parameter: 9005"
[ "$(setv Device.X_EXAMPLE-COM_.Mode enforce)" = 9001 ] || fail "Mode with mgmt.actions 0: 9001"
[ "$(setv Device.X_EXAMPLE-COM_.Mode sometimes)" = 9007 ] || fail "an invalid Mode: 9007"
[ "$(setv Device.ManagementServer.PeriodicInformInterval soon)" = 9007 ] || fail "an invalid interval: 9007"
[ "$(setv Device.X_EXAMPLE-COM_.Request.Command Bogus)" = 9007 ] || fail "an unknown command: 9007"
[ "$(setv Device.ManagementServer.PeriodicInformInterval 300)" = ok ] || fail "interval 300"
revert
[ "$(val Device.ManagementServer.PeriodicInformInterval)" = 86400 ] || fail "a reverted write must change nothing"
[ "$(setv Device.ManagementServer.PeriodicInformInterval 300)" = ok ] || fail "interval 300"
[ "$(val Device.ManagementServer.PeriodicInformInterval)" = 86400 ] || fail "a write must wait for the commit"
commit
[ "$(val Device.ManagementServer.PeriodicInformInterval)" = 300 ] || fail "the committed interval"
grep -q "periodic_interval '300'" "$T/config/aigw" || fail "the interval should be saved in /etc/config/aigw too"
echo "   faults 9001 9005 9007 9008; PeriodicInformInterval 300 after commit, saved"

echo "== part 1: an alert becomes LastEvent., without the name"
"$UBUS" -s "$AIGW_UBUS" send ag.dns '{"client":"10.97.0.2","qname":"xkqjzvbwptmr.com","rcode":3}'
sleep 0.7
bb get '{"path":"Device.X_EXAMPLE-COM_.LastEvent."}' > "$T/ev.json"
# the alert, then (the dga policy) a proposed quarantine: the last event is either
js "
v = {r['path'].split('.')[-1]: r['data'] for r in d['results']}
alert = v['Name'] == 'Device.X_EXAMPLE-COM_.Alert!' and v['Kind'] == 'dga'
proposal = v['Name'] == 'Device.X_EXAMPLE-COM_.DecisionChanged!' and v['State'] == 'proposed'
sys.exit(0 if int(v['Count']) >= 1 and (alert or proposal) and v['Host'] == '$LAPTOP' and v['Subject'] == '' else 'got %s' % v)" < "$T/ev.json" ||
	fail "LastEvent."
echo "   LastEvent: $(js "print(', '.join(r['path'].split('.')[-1] + '=' + r['data'] for r in d['results'] if r['data']))" < "$T/ev.json")"

echo "== part 1: commands as Request."
[ "$(setv Device.X_EXAMPLE-COM_.Request.Command Quarantine)" = ok ] || fail "Request.Command"
[ "$(setv Device.X_EXAMPLE-COM_.Request.Host "$LAPTOP")" = ok ] || fail "Request.Host"
commit
[ "$(val Device.X_EXAMPLE-COM_.Request.Status)" = Refused ] || fail "quarantine with mgmt.actions 0: $(val Device.X_EXAMPLE-COM_.Request.Status)"
val Device.X_EXAMPLE-COM_.Request.Message | grep -q mgmt.actions || fail "the refusal should say why"
[ -z "$(val Device.X_EXAMPLE-COM_.Request.Host)" ] || fail "inputs are used once"
bb get '{"path":"Device.X_EXAMPLE-COM_.Decision."}' > "$T/dec.json"
DEC=$(js "print([r['path'][:-len('.State')] for r in d['results'] if r['path'].endswith('.State') and r['data'] == 'proposed'][0])" < "$T/dec.json") ||
	fail "no proposed decision: $(cat "$T/dec.json")"
[ "$(setv Device.X_EXAMPLE-COM_.Request.Command "$DEC.Approve()")" = ok ] || fail "Request.Command = $DEC.Approve()"
commit
[ "$(val Device.X_EXAMPLE-COM_.Request.Status)" = Done ] || fail "approve: $(val Device.X_EXAMPLE-COM_.Request.Status) $(val Device.X_EXAMPLE-COM_.Request.Message)"
[ "$(val Device.X_EXAMPLE-COM_.Request.State)" = active ] || fail "approve: State $(val Device.X_EXAMPLE-COM_.Request.State)"
gw nft list set inet ag quarantine 2>/dev/null | grep -qi '02:00:00:00:96:02' || fail "the laptop should be quarantined in nft"
agctl -j decisions | js "sys.exit(0 if any('approved by acs' in x['origin'] for x in d['decisions']) else 1)" ||
	fail "the decision should say the ACS approved it"
[ "$(setv Device.X_EXAMPLE-COM_.Request.Command "$DEC.Undo")" = ok ] || fail "Request.Command = $DEC.Undo"
commit
[ "$(val "$DEC.State")" = reverted ] || fail "undo: $(val "$DEC.State")"
gw nft list set inet ag quarantine 2>/dev/null | grep -qi '02:00:00:00:96:02' && fail "undo should lift the quarantine"
echo "   $DEC: proposed -> active -> reverted, by acs; quarantine refused with mgmt.actions 0"

echo "== part 1: firmware, as icwmp asks for it (fwbank, bbfdm operate)"
FW=Device.DeviceInfo.FirmwareImage
fwb() { "$UBUS" -s "$AIGW_UBUS" call fwbank "$@"; }
fwb dump > "$T/banks.json"
js "b = {x['id']: x for x in d['bank']}
sys.exit(0 if b[1]['active'] and b[1]['fwver'] == '1.0' and not b[2]['active'] and b[2]['status'] == 'NoImage' else 'got %s' % d)" < "$T/banks.json" ||
	fail "fwbank dump: $(cat "$T/banks.json")"
printf 'AIGW-TEST-FIRMWARE 2.0\n' > "$T/good.img"
printf 'not firmware\n' > "$T/bad.img"
op() { bb operate "{\"path\":\"$FW.$1\",\"input\":{\"URL\":\"file://$2\",\"X_EXAMPLE-COM_KeepConfig\":true}}"; }
# icwmp counts a fault inside a result's output as a failed upgrade (9010)
op 2.Download\(\) "$T/bad.img" | js "o = d['results'][0]['output']; sys.exit(0 if o and o[0]['fault'] == 9010 else 1)" ||
	fail "a bad image should fail with 9010"
op 2.Download\(\) "$T/good.img" | js "sys.exit(0 if d['results'][0]['output'] == [] else 1)" || fail "Download()"
fwb set_bootbank '{"bank":2}' | js "sys.exit(0 if d['success'] else 1)" || fail "set_bootbank"
[ "$(val Device.DeviceInfo.BootFirmwareImage)" = "$FW.2" ] || fail "BootFirmwareImage: $(val Device.DeviceInfo.BootFirmwareImage)"
sleep 4
# icwmp reboots by itself: agd must not (origin acs)
[ ! -f "$T/fw/reboots" ] && [ "$(val Device.DeviceInfo.SoftwareVersion)" = 1.0 ] || fail "agd rebooted for the ACS"
rm -rf "$T/fw"          # part 2 starts from bank 1 again
echo "   banks listed; a bad image fault 9010; downloaded, set to boot next; no reboot by agd"

ICWMPD=${AIGW_ICWMPD:-$(command -v icwmpd || true)}
if [ -z "$ICWMPD" ] || [ ! -x "$ICWMPD" ]; then
	[ "${AIGW_REQUIRE_CWMP:-0}" = 1 ] && fail "no icwmpd (set AIGW_ICWMPD)"
	echo "   (part 2 skipped: no icwmpd; set AIGW_ICWMPD)"
	echo "CWMP TEST OK"
	exit 0
fi
command -v unshare >/dev/null || fail "missing unshare"

echo "== part 2: icwmp and an ACS"
kill "$DM"; wait "$DM" 2>/dev/null || true
sleep 0.5
mkdir -p "$T/acs/files" "$T/tls"
# the ACS speaks HTTPS and lets in only a CPE with a certificate from its CA
(
	cd "$T/tls"
	openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=AIGW test ISP CA" -keyout ca.key -out ca.crt
	openssl req -newkey rsa:2048 -nodes -subj "/CN=acs.test" -keyout acs.key -out acs.csr
	printf 'subjectAltName=IP:10.98.0.2\n' > acs.ext
	openssl x509 -req -in acs.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 -extfile acs.ext -out acs.crt
	openssl req -newkey rsa:2048 -nodes -subj "/CN=AIGWTEST0001" -keyout client.key -out client.csr
	openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 -out client.crt
) > "$T/openssl.log" 2>&1 || fail "test certificates: $(tail -n 5 "$T/openssl.log")"
ip netns exec "$NI" python3 "$REPO/tests/cwmp/acs.py" serve "$T/acs" --port 7557 \
	--tls-cert "$T/tls/acs.crt" --tls-key "$T/tls/acs.key" --tls-ca "$T/tls/ca.crt" > "$T/acs.log" 2>&1 & PIDS="$PIDS $!"
# firmware images from a plain file server, as ISPs often have
ip netns exec "$NI" python3 -m http.server 7558 --bind 10.98.0.2 --directory "$T/acs/files" > "$T/files.log" 2>&1 & PIDS="$PIDS $!"
# icwmp reboots after a firmware download (ubus system, rpc-sys, then the
# "reboot" command): in its namespace "reboot" is this script, which switches
# the test's firmware banks and stops icwmp, as a reboot would. It is first in
# PATH and mounted over the real one, so the test can never reboot the machine.
mkdir -p "$T/shim"
cat > "$T/shim/reboot" <<EOF
#!/bin/sh
echo "reboot \$*" >> "$T/reboots.log"
"$REPO/tests/fw/firmware" reboot >> "$T/reboots.log" 2>&1
p=\$\$
while [ "\$p" -gt 1 ]; do
	p=\$(ps -o ppid= -p "\$p" | tr -d ' ')
	[ -n "\$p" ] || break
	if [ "\$(ps -o comm= -p "\$p")" = icwmpd ]; then kill -9 "\$p"; break; fi
done
exit 0
EOF
# icwmp checks an image with "sysupgrade -T" (OpenWRT's): the test images pass
cat > "$T/shim/sysupgrade" <<'EOF'
#!/bin/sh
[ "$1" = -T ] && head -n 1 "$2" | grep -q '^AIGW-TEST-FIRMWARE ' && exit 0
exit 1
EOF
chmod 755 "$T/shim/reboot" "$T/shim/sysupgrade"

# icwmp's own paths (/etc/config/cwmp, /etc/icwmpd, /var/run, /var/state) are
# private to this test: overlays in a mount namespace of their own. /run is
# fresh each time (a reboot clears it); /etc and /var keep what was written.
cat > "$T/cwmp-ns.sh" <<EOF
set -e
mkdir -p "$T/ov/etc" "$T/ov/etcw" "$T/ov/var" "$T/ov/varw"
mount -t overlay overlay -o lowerdir=/etc,upperdir=$T/ov/etc,workdir=$T/ov/etcw /etc
mount -t overlay overlay -o lowerdir=/var,upperdir=$T/ov/var,workdir=$T/ov/varw /var
mount -t tmpfs tmpfs /run
for r in /sbin/reboot /usr/sbin/reboot /bin/reboot; do
	[ -e "\$r" ] && mount --bind "$T/shim/reboot" "\$r" || true
done
mkdir -p /run/ubus /var/state /etc/config /etc/icwmpd
ln -s "$AIGW_UBUS" /run/ubus/ubus.sock
ln -s "$AIGW_UBUS" /run/ubus.sock
[ -f "$T/ov/started" ] || rm -f /etc/config/cwmp
: > "$T/ov/started"
export PATH="$T/shim:\$PATH"
export AIGW_ICWMPD="$ICWMPD" AIGW_CWMP_DM="$BIN/cwmp/aigw-cwmp-dm" AIGW_CWMP_DIR=/var/run/aigw-cwmp
# AIGW's defaultreset (icwmp's FactoryReset), as installed in libexec
export AIGW_LIBEXEC="$REPO/cwmp"
exec sh "$REPO/cwmp/aigw-cwmp-run"
EOF
ip netns exec "$NG" unshare -m --propagation private sh "$T/cwmp-ns.sh" > "$T/icwmp.log" 2>&1 & CWMP=$!; PIDS="$PIDS $CWMP"
acs() { python3 "$REPO/tests/cwmp/acs.py" "$@"; }
job() { # job <file>: run it in the next session, waking the CPE with a connection request
	ip netns exec "$NI" python3 "$REPO/tests/cwmp/acs.py" job "$T/acs" "$1" --cr http://10.98.0.1:7547/ --user cr --password crpw --timeout 60
}

acs wait "$T/acs" "1 BOOT" --timeout 150 > "$T/boot.json" || acs wait "$T/acs" "0 BOOTSTRAP" --timeout 5 > "$T/boot.json" ||
	fail "no boot Inform from icwmp"
js "
dev, p = d['device'], d['params']
ok = dev['SerialNumber'] == 'AIGWTEST0001' and dev['ProductClass'] == 'AIGW OS gateway' and len(dev['OUI']) == 6
ok = ok and p.get('Device.DeviceInfo.SoftwareVersion') == '$want' and p.get('Device.ManagementServer.ConnectionRequestURL') == 'http://10.98.0.1:7547/'
ok = ok and d.get('client_cn') == 'AIGWTEST0001'      # over HTTPS, with its client certificate
sys.exit(0 if ok else 'got %s' % d)" < "$T/boot.json" || fail "the boot Inform"
echo "   Inform over HTTPS, client certificate $(js "print(d['client_cn'])" < "$T/boot.json"): $(js "print(d['events'], d['device'])" < "$T/boot.json")"

cat > "$T/j1.json" <<EOF
{"rpcs": [
 {"method": "GetParameterValues", "names": ["Device.DeviceInfo.SoftwareVersion", "Device.Hosts."]},
 {"method": "GetParameterNames", "path": "Device.X_EXAMPLE-COM_.", "next": true},
 {"method": "SetParameterValues", "params": [["Device.X_EXAMPLE-COM_.Mode", "enforce", "xsd:string"]], "key": "k1"},
 {"method": "SetParameterValues", "params": [["Device.X_EXAMPLE-COM_.Request.Command", "Quarantine", "xsd:string"],
                                            ["Device.X_EXAMPLE-COM_.Request.Host", "$LAPTOP", "xsd:string"]], "key": "k2"},
 {"method": "GetParameterValues", "names": ["Device.X_EXAMPLE-COM_.Request.Status", "Device.X_EXAMPLE-COM_.Mode"]},
 {"method": "SetParameterValues", "params": [["Device.ManagementServer.PeriodicInformInterval", "600", "xsd:unsignedInt"]], "key": "k3"},
 {"method": "GetParameterValues", "names": ["Device.ManagementServer.ParameterKey", "Device.ManagementServer.PeriodicInformInterval"]}
]}
EOF
job "$T/j1.json" > "$T/r1.json" || fail "the ACS's first job did not run"
python3 - "$T/r1.json" "$want" "$LAPTOP" <<'EOF' || fail "first job: $(cat "$T/r1.json")"
import json, sys
r = json.load(open(sys.argv[1]))["results"]
want, laptop = sys.argv[2], sys.argv[3]
assert r[0]["values"]["Device.DeviceInfo.SoftwareVersion"] == want, r[0]
assert r[0]["values"].get(laptop + ".PhysAddress") == "02:00:00:00:96:02", "hosts"
n = r[1]["names"]
assert n.get("Device.X_EXAMPLE-COM_.Mode") is True and "Device.X_EXAMPLE-COM_.Decision." in n and "Device.X_EXAMPLE-COM_.Request." in n, n
assert r[2].get("fault") == 9003 and r[2]["params"][0]["fault"] == 9001, r[2]          # mgmt.actions 0
assert r[3].get("status") is not None, r[3]
assert r[4]["values"]["Device.X_EXAMPLE-COM_.Request.Status"] == "Refused" and r[4]["values"]["Device.X_EXAMPLE-COM_.Mode"] == "advise", r[4]
assert r[6]["values"]["Device.ManagementServer.ParameterKey"] == "k3", r[6]
assert r[6]["values"]["Device.ManagementServer.PeriodicInformInterval"] == "600", r[6]
print("   GPV, GPN, SPV fault 9001 on Mode, refused Quarantine, ParameterKey k3, interval 600")
EOF
grep -q "periodic_interval '600'" "$T/config/aigw" || fail "the ACS's interval should be saved in /etc/config/aigw"

echo "== part 2: an alert reaches the ACS as an active notification"
n=$(wc -l < "$T/acs/informs.jsonl")
"$UBUS" -s "$AIGW_UBUS" send ag.dns '{"client":"10.97.0.4","qname":"qpwmzkrtvbxnly.com","rcode":3}'
acs wait "$T/acs" "4 VALUE CHANGE" --after "$n" --timeout 60 > "$T/vc.json" || fail "no 4 VALUE CHANGE Inform"
js "sys.exit(0 if 'Device.X_EXAMPLE-COM_.LastEvent.Count' in d['params'] else 'got %s' % d['params'])" < "$T/vc.json" ||
	fail "the value change should carry LastEvent.Count"
cat > "$T/j2.json" <<EOF
{"rpcs": [{"method": "GetParameterValues", "names": ["Device.X_EXAMPLE-COM_.LastEvent.", "Device.X_EXAMPLE-COM_.Decision."]}]}
EOF
job "$T/j2.json" > "$T/r2.json" || fail "the ACS's second job did not run"
DEC2=$(python3 - "$T/r2.json" "$PHONE" <<'EOF'
import json, sys
v = json.load(open(sys.argv[1]))["results"][0]["values"]
assert v["Device.X_EXAMPLE-COM_.LastEvent.Subject"] == "", "the name must not reach the ACS"
d = [k[:-len(".State")] for k, x in v.items() if k.startswith("Device.X_EXAMPLE-COM_.Decision.") and k.endswith(".State")
     and x == "proposed" and v[k[:-len("State")] + "Host"] == sys.argv[2]]
print(d[-1])
EOF
) || fail "second job: $(cat "$T/r2.json")"
echo "   4 VALUE CHANGE; proposed $DEC2"

echo "== part 2: the ACS approves the decision"
cat > "$T/j3.json" <<EOF
{"rpcs": [
 {"method": "SetParameterValues", "params": [["Device.X_EXAMPLE-COM_.Request.Command", "$DEC2.Approve()", "xsd:string"]], "key": "k4"},
 {"method": "GetParameterValues", "names": ["Device.X_EXAMPLE-COM_.Request.Status", "$DEC2.State"]}
]}
EOF
job "$T/j3.json" > "$T/r3.json" || fail "the ACS's third job did not run"
js "v = d['results'][1]['values']; sys.exit(0 if v['Device.X_EXAMPLE-COM_.Request.Status'] == 'Done' and v['$DEC2.State'] == 'active' else 'got %s' % v)" < "$T/r3.json" ||
	fail "approve: $(cat "$T/r3.json")"
gw nft list set inet ag quarantine 2>/dev/null | grep -qi '02:00:00:00:96:04' || fail "the phone should be quarantined in nft"
echo "   $DEC2 active, approved by acs"

echo "== part 2: a firmware upgrade from the ACS (Download, reboot, TransferComplete)"
printf 'AIGW-TEST-FIRMWARE 2.5\n' > "$T/acs/files/fw.img"
size=$(wc -c < "$T/acs/files/fw.img")
cat > "$T/j4.json" <<EOF
{"rpcs": [{"method": "Download", "url": "http://10.98.0.2:7558/fw.img", "key": "dl1", "size": $size}]}
EOF
n=$(wc -l < "$T/acs/informs.jsonl")
job "$T/j4.json" > "$T/r4.json" || fail "the ACS's Download did not run"
js "sys.exit(0 if d['results'][0].get('status') in (0, 1) else 1)" < "$T/r4.json" || fail "Download: $(cat "$T/r4.json")"
# icwmp downloads, checks, has agd put the image in bank 2 and boot it next, reboots
i=0
while kill -0 "$CWMP" 2>/dev/null; do
	i=$((i + 1)); [ $i -le 120 ] || fail "icwmp did not reboot after the download: $(cat "$T/reboots.log" 2>/dev/null)"; sleep 0.5
done
grep -q 'rebooted into bank 2' "$T/reboots.log" || fail "the reboot: $(cat "$T/reboots.log" 2>/dev/null)"
[ -f "$T/ov/etc/icwmpd/icwmpd_backup_session.xml" ] || fail "icwmp's session should have been kept in /etc/icwmpd"
# the boot: /run is fresh, aigw-cwmp-run puts icwmp's session back
ip netns exec "$NG" unshare -m --propagation private sh "$T/cwmp-ns.sh" >> "$T/icwmp.log" 2>&1 & CWMP=$!; PIDS="$PIDS $CWMP"
acs wait "$T/acs" "7 TRANSFER COMPLETE" --after "$n" --timeout 150 > "$T/tc.json" || fail "no TRANSFER COMPLETE Inform after the reboot"
js "sys.exit(0 if '1 BOOT' in d['events'] and d['params'].get('Device.DeviceInfo.SoftwareVersion') == '2.5' else 'got %s' % d)" < "$T/tc.json" ||
	fail "the Inform after the upgrade: $(cat "$T/tc.json")"
i=0
until [ -s "$T/acs/transfers.jsonl" ]; do
	i=$((i + 1)); [ $i -le 60 ] || fail "no TransferComplete"; sleep 0.5
done
head -n 1 "$T/acs/transfers.jsonl" | js "sys.exit(0 if d['key'] == 'dl1' and d['fault'] == 0 else 'got %s' % d)" ||
	fail "TransferComplete: $(cat "$T/acs/transfers.jsonl")"
rm -f /tmp/firmware.bin /tmp/icwmp_download
echo "   $(js "print(d['events'])" < "$T/tc.json"), SoftwareVersion 2.5, TransferComplete dl1 without a fault"

echo "== part 2: FactoryReset from the ACS (icwmp's defaultreset, through agd)"
cat > "$T/j5.json" <<EOF
{"rpcs": [{"method": "FactoryReset"}]}
EOF
job "$T/j5.json" > "$T/r5.json" || fail "the ACS's FactoryReset did not run"
i=0
until grep -q factory-reset "$T/fw/reboots" 2>/dev/null; do
	i=$((i + 1)); [ $i -le 60 ] || fail "FactoryReset did not reach the firmware handler: $(grep -i 'system\|reset' "$T/agd.log" | tail -n 5)"; sleep 0.5
done
grep -q "system: factory-reset asked by acs" "$T/agd.log" || fail "agd should log who asked for the reset"
echo "   FactoryReset -> defaultreset -> agd (mgmt.reset) -> the firmware handler"

echo "CWMP TEST OK"
