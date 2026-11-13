#!/bin/sh
# Dual-band access point and band steering with real Wi-Fi software on
# simulated radios (mac80211_hwsim): two hostapd access points with the
# configs aigw-ap writes (5 GHz and 2.4 GHz, one name, one bridge), a real
# wpa_supplicant client, and agd. The client joins on 2.4 GHz; agd asks it
# (802.11v) to move to 5 GHz, and it does.
#
# Needs root, the mac80211_hwsim module (Ubuntu: linux-modules-extra), hostapd,
# wpa_supplicant, iw. Prints SKIP and succeeds where hwsim is missing (WSL),
# unless AIGW_REQUIRE_HWSIM=1 (CI), where a skip is a failure.
#
#   sudo tests/hwsim-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NC=aigwsta$R BR=aigwbr$R
PIDS=""
LOADED=0
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for f in "$T"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null || true; done
	ip netns del "$NC" 2>/dev/null || true
	ip link del "$BR" 2>/dev/null || true
	sleep 0.5
	[ "$LOADED" = 1 ] && rmmod mac80211_hwsim 2>/dev/null || true
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "HWSIM TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

skip() {
	[ "${AIGW_REQUIRE_HWSIM:-0}" = 1 ] && fail "cannot run here: $* (AIGW_REQUIRE_HWSIM=1)"
	echo "HWSIM TEST SKIP: $*"
	exit 0
}
[ "$(id -u)" = 0 ] || fail "run as root"
for t in hostapd wpa_supplicant iw; do command -v $t >/dev/null || skip "no $t"; done
lsmod | grep -q '^mac80211_hwsim' && skip "mac80211_hwsim already loaded (in use by something else)"
before=$(ls /sys/class/ieee80211 2>/dev/null || true)
modprobe mac80211_hwsim radios=3 2>/dev/null || skip "no mac80211_hwsim in this kernel"
LOADED=1
sleep 1
phys=""
for p in $(ls /sys/class/ieee80211); do
	echo "$before" | grep -qx "$p" || phys="$phys $p"
done
set -- $phys
[ $# -ge 3 ] || fail "expected 3 simulated radios, got: $phys"
ifof() { ls "/sys/class/ieee80211/$1/device/net" | head -1; }
AP5=$(ifof "$1") AP24=$(ifof "$2") STA=$(ifof "$3") STAPHY=$3
echo "== radios: $AP5 (5 GHz), $AP24 (2.4 GHz), $STA (client)"
# the kernel reads the regulatory database once; a freshly installed one
# (wireless-regdb) needs a reload before a country can be set
iw reg reload 2>/dev/null || true
iw reg set US 2>/dev/null || true
n=0
until iw reg get | grep -q '^country US'; do
	n=$((n + 1))
	if [ $n -gt 10 ]; then
		iw reg get | head -3
		fail "the kernel did not take the country (regulatory database not loaded: wireless-regdb?)"
	fi
	sleep 0.5
done
echo "   regulatory: $(iw reg get | grep -m1 '^country')"

echo "== access points from aigw-ap's own configs, in one bridge"
printf 'SSID=AIGW-HWSIM\nPASSPHRASE=hwsim-test-pass\nCOUNTRY=US\n' > "$T/pc-ap.conf"
export AIGW_PCAP_CONF="$T/pc-ap.conf" AIGW_RUN="$T/run" AIGW_WIFI_KEYS="$T/wifi-keys"
AP="sh $REPO/platform/pc-ap/aigw-ap"
ip link add "$BR" type bridge
ip link set "$BR" up
ip addr add 10.95.0.1/24 dev "$BR"
# hostapd in the foreground: its whole log stays (with -B, errors after the
# fork are lost); up = AP-ENABLED in that log within 20 s
start_hapd() {   # conf log pidfile
	hostapd -t "$1" > "$2" 2>&1 &
	echo $! > "$3"
	n=0
	while [ $n -lt 40 ]; do
		grep -q AP-ENABLED "$2" && return 0
		kill -0 "$(cat "$3")" 2>/dev/null || return 1
		sleep 0.5; n=$((n + 1))
	done
	return 1
}
regdump() {
	echo "--- regulatory"; iw reg get 2>&1 | head -30
	echo "--- $AP5 channel 36"; iw phy "$(basename "$(readlink -f /sys/class/net/$AP5/phy80211)")" info 2>&1 | grep -E "5180|5200" | head -4
}
# configs and pidfiles where aigw-ap keeps them: the Wi-Fi handler restarts
# an access point through it (aigw-ap reconf)
mkdir -p "$T/run"
H24="$T/run/hostapd-$AP24"
H5="$T/run/hostapd-$AP5"
$AP conf "$AP24" 2.4 6 20 "$BR" > "$H24.conf"
start_hapd "$H24.conf" "$T/hostapd24.log" "$H24.pid" || fail "2.4 GHz hostapd did not start"

echo "== the client joins (only 2.4 GHz is up yet)"
ip netns add "$NC"
iw phy "$STAPHY" set netns name "$NC"
cat > "$T/wpas.conf" <<EOF
ctrl_interface=$T/wpas
network={
	ssid="AIGW-HWSIM"
	psk="hwsim-test-pass"
}
EOF
ip netns exec "$NC" wpa_supplicant -B -P "$T/wpas.pid" -i "$STA" -c "$T/wpas.conf" -f "$T/wpas.log" \
	|| fail "wpa_supplicant did not start"
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 24'; do
	i=$((i + 1)); [ $i -gt 30 ] && fail "client did not join the 2.4 GHz access point"; sleep 1
done
echo "   joined: $(ip netns exec "$NC" iw dev "$STA" link | grep -E 'freq|signal' | tr -s ' \t' ' ')"

echo "== the 5 GHz access point comes up"
w5=""
for w in 80 40 20; do
	$AP conf "$AP5" 5 36 $w "$BR" > "$H5.conf"
	if start_hapd "$H5.conf" "$T/hostapd5-$w.log" "$H5.pid"; then w5=$w; break; fi
	kill "$(cat "$H5.pid")" 2>/dev/null || true
	echo "   5 GHz at $w MHz did not come up: $(grep -iE 'fail|not allowed|could not|error|DFS|NO-IR|disabled' "$T/hostapd5-$w.log" | head -3 | tr '\n' ' ')"
done
if [ -z "$w5" ]; then
	regdump
	fail "5 GHz hostapd did not start"
fi
echo "   5 GHz channel 36, $w5 MHz"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15356'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/run/hostapd'
	option keys '$T/wifi-keys'
	option steer_hold '3'
	option handler '$REPO/platform/pc-ap/wifi-config'
	option heal_hold '20'
	option heal_trial '40'
	option heal_retry '60'

config diag 'diag'
	option targets '10.95.0.1'
	option name 'example.com'

config mgmt 'mgmt'
	option enabled '1'

config act 'act'
	option mode 'enforce'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'
	option state_dir '$T/plugins'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config lan 'lan'
	list iface '$BR'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_AP="$REPO/platform/pc-ap/aigw-ap"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"

echo "== agd steers the client to 5 GHz"
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 5'; do
	i=$((i + 1))
	if [ $i -gt 90 ]; then
		echo "--- agd's view"; "$BIN/agctl/agctl" wifi || true
		echo "--- 5 GHz hostapd"; tail -n 15 "$T/hostapd5-$w5.log"; regdump
		echo "--- hostapd's station entry"; hostapd_cli -p "$T/run/hostapd" -i "$AP24" all_sta 2>&1 || true
		fail "client still on 2.4 GHz after 90 s"
	fi
	sleep 1
done
echo "   moved after ${i}s: $(ip netns exec "$NC" iw dev "$STA" link | grep -E 'freq' | tr -s ' \t' ' ')"
sleep 20      # agd's check, 15 s after asking
"$BIN/agctl/agctl" wifi
"$BIN/agctl/agctl" -j wifi > "$T/radios.json"
grep -q "steer .* from 2.4 GHz to 5 GHz by btm" "$T/agd.log" || fail "agd did not ask with 802.11v"
python3 - "$T/radios.json" <<'EOF' || fail "agd's view after the move"
import json, sys
r = json.load(open(sys.argv[1]))
on5 = [s for x in r["radios"] if x["band"] == "5" for s in x["stations"]]
ok = r["steering"]["dual_band"] and r["steering"]["moves_done"] >= 1 and len(on5) == 1 and on5[0]["bss_transition"]
print("  steering:", r["steering"])
sys.exit(0 if ok else 1)
EOF

echo "== channel switch: agctl channel 5 44; the client follows the announcement"
assoc_before=$(grep -c "CTRL-EVENT-CONNECTED" "$T/wpas.log" || true)
"$BIN/agctl/agctl" channel 5 44 10m || fail "agctl channel 5 44"
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 5220'; do
	i=$((i + 1))
	if [ $i -gt 30 ]; then
		echo "--- 5 GHz hostapd"; tail -n 20 "$T/hostapd5-$w5.log"
		echo "--- agd"; tail -n 10 "$T/agd.log"
		fail "client not on channel 44 (5220 MHz) after 30 s"
	fi
	sleep 1
done
echo "   on channel 44 after ${i}s: $(ip netns exec "$NC" iw dev "$STA" link | grep -E 'freq' | tr -s ' \t' ' ')"
grep -q "switching from channel 36 to 44" "$T/agd.log" || fail "agd did not announce the switch (fell back to a restart?)"
if [ "$(grep -c "CTRL-EVENT-CONNECTED" "$T/wpas.log" || true)" != "$assoc_before" ]; then
	echo "   (the client reconnected instead of following)"
fi
sleep 6
"$BIN/agctl/agctl" wifi
# decisions as "id state action origin target", one per line
decs() {
	"$BIN/agctl/agctl" -j decisions | python3 -c "
import json, sys
for x in json.load(sys.stdin).get('decisions', []):
    print(x['id'], x['state'], x['action'], x.get('origin', '').replace(' ', '_'), x.get('target', ''))
"
}
# done with it: undone, so Self-Healing Wi-Fi may try channels on this access point later
CID=$(decs | awk '$2 == "active" && $3 == "channel" {print $1; exit}')
[ -n "$CID" ] && "$BIN/agctl/agctl" undo "$CID" > /dev/null || fail "undo the manual channel decision"
sleep 6

echo "== Wi-Fi settings from a management system (TR-181, through the PC's Wi-Fi handler)"
dm() { "${UBUS:-$(command -v ubus || echo /usr/local/bin/ubus)}" -s "$AIGW_UBUS" call ag.dm "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
dm get '{"paths":["Device.WiFi."]}' > "$T/wifi.json"
N=$(js "print([k.split('.')[3] for k, v in d['values'].items() if k.startswith('Device.WiFi.SSID.') and k.endswith('.Name') and v == '$AP24'][0])" < "$T/wifi.json") ||
	fail "no SSID instance for $AP24: $(cat "$T/wifi.json")"
js "v = d['values']; sys.exit(0 if v['Device.WiFi.SSID.$N.SSID'] == 'AIGW-HWSIM' and v['Device.WiFi.Radio.$N.Channel'] == '6' and
   v['Device.WiFi.Radio.$N.OperatingFrequencyBand'] == '2.4GHz' and v['Device.WiFi.AccessPoint.$N.Security.ModeEnabled'] == 'WPA2-Personal' and
   v['Device.WiFi.AccessPoint.$N.Security.KeyPassphrase'] == '' else 1)" < "$T/wifi.json" || fail "Wi-Fi as read: $(cat "$T/wifi.json")"
SSIDP=Device.WiFi.SSID.$N.SSID KEYP=Device.WiFi.AccessPoint.$N.Security.KeyPassphrase
dm set "{\"params\":{\"$KEYP\":\"short\"},\"origin\":\"usp\"}" | js "sys.exit(0 if d['errors']['$KEYP']['code'] == 'invalid_value' else 1)" ||
	fail "a too short passphrase"
dm set "{\"params\":{\"Device.WiFi.Radio.$N.Channel\":\"14\"},\"origin\":\"usp\"}" | js "sys.exit(0 if not d['ok'] else 1)" ||
	fail "channel 14"
dm set "{\"params\":{\"$SSIDP\":\"AIGW-NEW\",\"$KEYP\":\"new-pass-1234\",\"Device.WiFi.Radio.$N.Channel\":\"11\"},\"origin\":\"usp\"}" |
	js "sys.exit(0 if d['ok'] else 1)" || fail "the new settings"
dm get "{\"paths\":[\"$SSIDP\"]}" | js "sys.exit(0 if d['values']['$SSIDP'] == 'AIGW-NEW' else 1)" || fail "the new name, as read at once"
i=0
until hostapd_cli -p "$T/run/hostapd" -i "$AP24" get_config 2>/dev/null | grep -q '^ssid=AIGW-NEW'; do
	i=$((i + 1))
	if [ $i -gt 40 ]; then
		echo "--- agd"; grep -i wifi "$T/agd.log" | tail -n 10
		fail "the 2.4 GHz access point did not come back with the new name"
	fi
	sleep 0.5
done
hostapd_cli -p "$T/run/hostapd" -i "$AP24" status | grep -q '^channel=11' || fail "the new channel: $(hostapd_cli -p "$T/run/hostapd" -i "$AP24" status | grep channel)"
grep -q "^SSID='AIGW-NEW'" "$T/pc-ap.conf" && grep -q "^PASSPHRASE='new-pass-1234'" "$T/pc-ap.conf" && grep -q "^CHANNEL='11'" "$T/pc-ap.conf" ||
	fail "the settings should be saved in pc-ap.conf: $(grep -vi pass "$T/pc-ap.conf")"
# the handler is done once it has restarted the other band too (a new name is on both)
i=0
until grep -q "wifi: $AP24 settings applied (asked by usp)" "$T/agd.log"; do
	i=$((i + 1))
	if [ $i -gt 60 ]; then
		echo "--- agd"; grep -i wifi "$T/agd.log" | tail -n 10
		fail "agd should log who changed it, once the handler is done"
	fi
	sleep 1
done
# the client joins again with the new passphrase
ip netns exec "$NC" wpa_cli -p "$T/wpas" -i "$STA" set_network 0 ssid '"AIGW-NEW"' > /dev/null
ip netns exec "$NC" wpa_cli -p "$T/wpas" -i "$STA" set_network 0 psk '"new-pass-1234"' > /dev/null
ip netns exec "$NC" wpa_cli -p "$T/wpas" -i "$STA" reassociate > /dev/null
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'SSID: AIGW-NEW'; do
	i=$((i + 1)); [ $i -gt 30 ] && fail "the client did not join AIGW-NEW with the new passphrase"; sleep 1
done
echo "   name, passphrase and channel 11 set over the data model, saved, applied; the client joined again"

echo "== Self-Healing Wi-Fi: what the client feels, and why (loss injected on its access point)"
STAMAC=$(ip netns exec "$NC" cat "/sys/class/net/$STA/address")
ip -n "$NC" addr add 10.95.0.50/24 dev "$STA" 2>/dev/null || true
echo "1999999999 $STAMAC 10.95.0.50 hwsim-phone *" >> "$T/leases"
# in use (a phone being used passes traffic; one asleep isn't measured)
ip netns exec "$NC" ping -q -i 0.2 10.95.0.1 > /dev/null 2>&1 & PIDS="$PIDS $!"
radio_of_client() {   # the access point the client is on, and its health: "<iface> <status> <cause>"
	"$BIN/agctl/agctl" -j wifi | python3 -c "
import json, sys
for r in json.load(sys.stdin)['radios']:
    if any(s.get('mac', '').lower() == '$STAMAC'.lower() for s in r.get('stations', [])):
        h = r.get('health', {})
        print(r['iface'], h.get('status', ''), h.get('cause', ''))
"
}
wait_health() {   # wait_health <status regex> <cause regex> <seconds>
	i=0
	while [ $i -lt "$3" ]; do
		set -- "$1" "$2" "$3" $(radio_of_client)
		if [ -n "${5:-}" ] && echo "$5" | grep -Eq "^($1)$" && echo "${6:-}" | grep -Eq "^($2)$"; then
			AP_H=$4
			return 0
		fi
		sleep 2; i=$((i + 2))
	done
	echo "--- agd"; grep -i 'wifi health' "$T/agd.log" | tail -n 5
	"$BIN/agctl/agctl" wifi
	return 1
}
wait_health Good None 120 || fail "the client's access point should be measured as Good first"
echo "   $AP_H: Good"

echo "== Self-Healing Wi-Fi: a device kept on 2.4 GHz (stay24), refused on 5 GHz only"
"$BIN/agctl/agctl" do stay24 "$STAMAC" 30m || fail "agctl do stay24"
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 24'; do
	i=$((i + 1)); [ $i -gt 60 ] && fail "the client should be on 2.4 GHz with stay24"; sleep 1
done
sleep 10
ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 24' || fail "the client should stay on 2.4 GHz (not steered back)"
hostapd_cli -p "$T/run/hostapd" -i "$AP5" deny_acl SHOW | grep -qi "$STAMAC" || fail "the 5 GHz access point should refuse it"
hostapd_cli -p "$T/run/hostapd" -i "$AP24" deny_acl SHOW | grep -qi "$STAMAC" && fail "the 2.4 GHz access point should not refuse it"
SID=$(decs | awk '$2 == "active" && $3 == "stay24" {print $1; exit}')
[ -n "$SID" ] || fail "no active stay24 decision"
echo "   on 2.4 GHz, refused on $AP5 only (decision #$SID)"
wait_health Good None 120 || fail "the client's 2.4 GHz access point should be measured as Good"
[ "$AP_H" = "$AP24" ] || fail "the client should be measured on $AP24, is on $AP_H"
"${UBUS:-$(command -v ubus || echo /usr/local/bin/ubus)}" -s "$AIGW_UBUS" listen ag.dm.event > "$T/heal-events.log" 2>&1 & PIDS="$PIDS $!"
command -v tc >/dev/null || fail "missing tc"
modprobe sch_netem 2>/dev/null || true
tc qdisc add dev "$AP_H" root netem loss 30% || fail "netem (sch_netem) on $AP_H"
wait_health 'Degraded|Poor' Congestion 200 || { tc qdisc del dev "$AP_H" root 2>/dev/null; fail "30% loss with a strong signal should read as Congestion"; }
N=$("$BIN/agctl/agctl" dm get Device.WiFi.SSID. | sed -n "s/^Device\.WiFi\.SSID\.\([0-9]*\)\.Name = $AP_H$/\1/p")
"$BIN/agctl/agctl" dm get "Device.X_AIGW_.WiFiHealth.$N." > "$T/heal-dm.txt"
grep -q 'Cause = Congestion' "$T/heal-dm.txt" && grep -q "AffectedHosts = Device.Hosts.Host" "$T/heal-dm.txt" ||
	fail "the data model: $(cat "$T/heal-dm.txt")"
grep -q 'WiFiHealthChanged!' "$T/heal-events.log" || fail "no WiFiHealthChanged! event"
echo "   30% loss: $(grep -o 'Status = .*' "$T/heal-dm.txt"), Congestion; $(sed -n 's/.*Explanation = //p' "$T/heal-dm.txt" | cut -c1-110)..."

echo "== Self-Healing Wi-Fi: congestion held: a channel trial (enforce mode), kept when it measures better"
CH0=$(hostapd_cli -p "$T/run/hostapd" -i "$AP_H" status | sed -n 's/^channel=//p')
i=0
until decs | grep -q ' active channel policy_wifi_congestion '; do
	i=$((i + 1))
	if [ $i -gt 120 ]; then
		tc qdisc del dev "$AP_H" root 2>/dev/null
		echo "--- agd"; grep -iE 'wifi health|alert|channel' "$T/agd.log" | tail -n 12
		fail "no channel trial after congestion held (heal_hold 20 s)"
	fi
	sleep 1
done
TID=$(decs | awk '$2 == "active" && $4 == "policy_wifi_congestion" {print $1; exit}')
CH1=$(decs | awk '$1 == '"$TID"' {print $5}' | sed 's/.*://')
echo "   trial #$TID: channel $CH0 -> $CH1; the interference goes away"
tc qdisc del dev "$AP_H" root 2>/dev/null || true
i=0
until decs | grep -q "^$TID kept "; do
	i=$((i + 1))
	if [ $i -gt 150 ]; then
		echo "--- agd"; grep -iE 'wifi health|trial|decision' "$T/agd.log" | tail -n 12
		decs
		fail "the trial should be kept (devices did clearly better)"
	fi
	sleep 1
done
hostapd_cli -p "$T/run/hostapd" -i "$AP_H" status | grep -q "^channel=$CH1$" || fail "the access point should stay on channel $CH1"
i=0
until grep -q "^CHANNEL='$CH1'" "$T/pc-ap.conf"; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "the kept channel should be saved in pc-ap.conf: $(grep CHANNEL "$T/pc-ap.conf")"; sleep 1
done
grep -q "wifi health: $AP_H Channel $CH0 -> $CH1 worked" "$T/agd.log" || fail "agd should say the trial worked"
echo "   kept: channel $CH1, saved (no restart); $("$BIN/agctl/agctl" decisions | grep -m1 'note: kept' | sed 's/^ *//')"
wait_health Good None 240 || fail "Good again once the loss is gone"
echo "   Good again"

echo "== Self-Healing Wi-Fi: a trial that doesn't help is switched back"
tc qdisc add dev "$AP_H" root netem loss 30% || fail "netem again on $AP_H"
i=0
until decs | awk '$1 != '"$TID"' && $2 == "active" && $4 == "policy_wifi_congestion"' | grep -q .; do
	i=$((i + 1))
	if [ $i -gt 240 ]; then
		tc qdisc del dev "$AP_H" root 2>/dev/null
		echo "--- agd"; grep -iE 'wifi health|alert' "$T/agd.log" | tail -n 12
		fail "no second channel trial"
	fi
	sleep 1
done
TID2=$(decs | awk '$1 != '"$TID"' && $2 == "active" && $4 == "policy_wifi_congestion" {print $1; exit}')
CH2=$(decs | awk '$1 == '"$TID2"' {print $5}' | sed 's/.*://')
echo "   trial #$TID2: channel $CH1 -> $CH2; the interference stays"
i=0
until decs | grep -q "^$TID2 tried "; do
	i=$((i + 1))
	if [ $i -gt 150 ]; then
		tc qdisc del dev "$AP_H" root 2>/dev/null
		echo "--- agd"; grep -iE 'wifi health|trial|decision' "$T/agd.log" | tail -n 12
		decs
		fail "the trial should be switched back (no better)"
	fi
	sleep 1
done
# the interference goes away now (left on, another trial would follow after heal_retry)
tc qdisc del dev "$AP_H" root
i=0
until hostapd_cli -p "$T/run/hostapd" -i "$AP_H" status | grep -q "^channel=$CH1$"; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "back on channel $CH1"; sleep 1
done
grep -q "^CHANNEL='$CH1'" "$T/pc-ap.conf" || fail "the saved channel should still be $CH1"
grep -q "wifi health: $AP_H Channel $CH1 -> $CH2 did not help" "$T/agd.log" || fail "agd should say the trial did not help"
echo "   tried: back on channel $CH1, $CH2 avoided for a day"
wait_health Good None 240 || fail "Good again once the loss is gone"

echo "== the healing report: what was found and fixed, for the ISP"
"$BIN/agctl/agctl" wifi --report | tee "$T/report.txt"
# the trials on the client's 2.4 GHz access point; stay24 on the one it was on (5 GHz) when it was set
# (at least: the loss lingers in the 2-minute window, so one more trial may have run meanwhile)
grep -A3 "^$AP_H " "$T/report.txt" | grep -Eq "channel trials kept [1-9], switched back [1-9]" ||
	fail "agctl wifi --report should show trials kept and switched back on $AP_H"
grep -q "devices kept on 2.4 GHz 1" "$T/report.txt" || fail "agctl wifi --report should count the device kept on 2.4 GHz"
"$BIN/agctl/agctl" dm get "Device.X_AIGW_.WiFiHealth.$N.Stats.Day." > "$T/stats.txt"
KEPT=$(sed -n 's/.*Stats\.Day\.ChannelTrialsKept = //p' "$T/stats.txt")
grep -Eq "Stats.Day.ChannelTrialsKept = [1-9]" "$T/stats.txt" && grep -Eq "Stats.Day.ChannelTrialsTried = [1-9]" "$T/stats.txt" &&
	grep -Eq "Stats.Day.CongestionFound = [2-9]" "$T/stats.txt" && grep -q "Stats.Day.Fixes = $KEPT$" "$T/stats.txt" &&
	grep -Eq "Stats.Day.LossBeforePercent = [1-9]" "$T/stats.txt" && grep -Eq "Stats.Day.MeanTimeToFix = [1-9]" "$T/stats.txt" ||
	fail "the data model's Stats.Day: $(cat "$T/stats.txt")"
echo "   Stats.Day: $(grep -E 'GoodPercent|ProblemsFound|Fixes|MeanTimeToFix|LossBefore|LossAfter' "$T/stats.txt" | sed 's/.*Stats\.Day\.//' | tr '\n' ' ')"
# kept across a restart (agd's state file); from here on, a person approves (advise mode)
sed -i "s/option mode 'enforce'/option mode 'advise'/" "$T/config/aigw"
kill "$AGD"
i=0; while kill -0 "$AGD" 2>/dev/null; do i=$((i + 1)); [ $i -gt 20 ] && fail "agd did not stop"; sleep 0.5; done
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
i=0
until "$BIN/agctl/agctl" wifi --report > "$T/report2.txt" 2>/dev/null &&
	[ "$(grep -A3 "^$AP_H " "$T/report.txt" | grep -m1 -o 'trials kept [0-9]*, switched back [0-9]*')" = \
	  "$(grep -A3 "^$AP_H " "$T/report2.txt" | grep -m1 -o 'trials kept [0-9]*, switched back [0-9]*')" ]; do
	i=$((i + 1)); [ $i -gt 30 ] && { cat "$T/report2.txt"; fail "the healing report should survive a restart"; }; sleep 1
done
grep -q "restored [0-9]* days of the healing report" "$T/agd.log" || fail "agd should say it restored the report"
echo "   kept across an agd restart"

echo "== Self-Healing Wi-Fi: a trial proposed, agd restarted, then approved: it still knows where it came from"
tc qdisc add dev "$AP_H" root netem loss 30% || fail "netem again on $AP_H"
i=0
until decs | grep -q ' proposed channel policy_wifi_congestion '; do
	i=$((i + 1))
	if [ $i -gt 240 ]; then
		tc qdisc del dev "$AP_H" root 2>/dev/null
		echo "--- agd"; grep -iE 'wifi health|alert|acting' "$T/agd.log" | tail -n 12
		fail "no channel trial proposed (advise mode)"
	fi
	sleep 1
done
TID3=$(decs | awk '$2 == "proposed" && $4 == "policy_wifi_congestion" {print $1; exit}')
CH3=$(decs | awk '$1 == '"$TID3"' {print $5}' | sed 's/.*://')
kill "$AGD"
i=0; while kill -0 "$AGD" 2>/dev/null; do i=$((i + 1)); [ $i -gt 20 ] && fail "agd did not stop"; sleep 0.5; done
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
# measured again after the restart, then a person says yes
wait_health 'Degraded|Poor' Congestion 200 || { tc qdisc del dev "$AP_H" root 2>/dev/null; fail "Congestion again after the restart"; }
decs | grep -q "^$TID3 proposed " || fail "the proposal #$TID3 should survive the restart"
"$BIN/agctl/agctl" approve "$TID3" > /dev/null || fail "approve #$TID3"
echo "   trial #$TID3 proposed (channel $CH1 -> $CH3), agd restarted, approved"
i=0
until decs | grep -Eq "^$TID3 (tried|kept) "; do
	i=$((i + 1))
	if [ $i -gt 150 ]; then
		tc qdisc del dev "$AP_H" root 2>/dev/null
		echo "--- agd"; grep -iE 'wifi health|trial|decision' "$T/agd.log" | tail -n 12
		decs
		fail "trial #$TID3 should be judged"
	fi
	sleep 1
done
tc qdisc del dev "$AP_H" root
LINE=$(grep "wifi health: $AP_H trial on channel $CH3 " "$T/agd.log" | tail -n 1)
echo "$LINE" | grep -q "(from $CH1)" || fail "the trial should know it came from channel $CH1: $LINE"
echo "$LINE" | grep -Eq "before: loss [1-9]" || fail "the trial should know what devices felt before: $LINE"
decs | grep -q "^$TID3 tried " || fail "trial #$TID3 should be switched back (the loss stayed): $(decs | grep "^$TID3 ")"
grep "decision $TID3 channel .*: tried -- " "$T/agd.log" | grep -q "back to $CH1$" ||
	fail "trial #$TID3's note should say back to $CH1: $(grep "decision $TID3 channel" "$T/agd.log" | tail -n 1)"
i=0
until hostapd_cli -p "$T/run/hostapd" -i "$AP_H" status | grep -q "^channel=$CH1$"; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "back on channel $CH1"; sleep 1
done
echo "   ${LINE#*wifi health: $AP_H }; switched back to $CH1"
wait_health Good None 240 || fail "Good again once the loss is gone"

echo "== stay24 undone: the 5 GHz access point takes the client again"
"$BIN/agctl/agctl" undo "$SID" > /dev/null || fail "undo stay24"
hostapd_cli -p "$T/run/hostapd" -i "$AP5" deny_acl SHOW | grep -qi "$STAMAC" && fail "the 5 GHz access point should take it again"
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'freq: 5'; do
	i=$((i + 1)); [ $i -gt 60 ] && break; sleep 1
done
[ $i -le 60 ] && echo "   steered back to 5 GHz after ${i}s" || echo "   (not steered back within 60 s; the ban is lifted)"

echo "== a hostapd that answers late: each access point keeps its own band and channel"
# paused for longer than agd waits (300 ms): its replies come after agd gave
# up, and must not be read as the answer to agd's next command (found on the
# PC access point: the two access points' STATUS swapped until agd restarted)
kill -STOP "$(cat "$H24.pid")"
sleep 3
kill -CONT "$(cat "$H24.pid")"
sleep 6
"$BIN/agctl/agctl" -j wifi | python3 -c "
import json, sys
for a in json.load(sys.stdin).get('radios', []):
    print(a.get('iface'), a.get('band'), a.get('channel'), a.get('bssid'))
" > "$T/aps.txt"
for ifc in "$AP24" "$AP5"; do
	want=$(cat "/sys/class/net/$ifc/address")
	got=$(awk -v i="$ifc" '$1 == i {print $4}' "$T/aps.txt")
	[ "$got" = "$want" ] || fail "after a late reply, $ifc shows another access point's status: $(cat "$T/aps.txt")"
done
awk -v i="$AP24" '$1 == i {print $2}' "$T/aps.txt" | grep -qx 2.4 || fail "$AP24 should be 2.4 GHz: $(cat "$T/aps.txt")"
awk -v i="$AP5" '$1 == i {print $2}' "$T/aps.txt" | grep -qx 5 || fail "$AP5 should be 5 GHz: $(cat "$T/aps.txt")"
echo "   $(tr '\n' ';' < "$T/aps.txt")"

echo "== Diagnose: \"my internet is slow\", answered by the gateway"
# here the "internet" (diag.targets) is the gateway's own LAN address, which
# answers (not the client: it is disconnected below); no DNS server runs on the
# configured port, so names don't resolve: that is the verdict
"$BIN/agctl/agctl" diagnose | tee "$T/diag.txt"
grep -q "code      DNS_FAILING (DNS, problem)" "$T/diag.txt" || fail "Diagnose should find that names don't resolve"
grep -q "INTERNET_OK" "$T/diag.txt" || fail "Diagnose should find the internet (here: the client) answering"
grep -q "advice    " "$T/diag.txt" || fail "Diagnose should say what to do"
# AIGW itself keeping the device off is named first, whatever else is wrong
i=0
until ip netns exec "$NC" iw dev "$STA" link | grep -q 'Connected'; do
	i=$((i + 1)); [ $i -gt 60 ] && fail "the client should be connected before the next check"; sleep 1
done
"$BIN/agctl/agctl" disconnect hwsim-phone 5m > /dev/null || fail "agctl disconnect hwsim-phone"
DID=$(decs | awk '$2 == "active" && $3 == "disconnect" {print $1; exit}')
[ -n "$DID" ] || fail "no active disconnect decision"
"$BIN/agctl/agctl" diagnose hwsim-phone | tee "$T/diag2.txt"
grep -q "code      DEVICE_BLOCKED (AIGW, problem)" "$T/diag2.txt" || fail "Diagnose should name AIGW's own disconnect first"
grep -q "disconnect (#$DID" "$T/diag2.txt" || fail "Diagnose should say which decision keeps it off"
grep -q "WIFI_DEVICE" "$T/diag2.txt" && fail "a device AIGW keeps off shouldn't get Wi-Fi findings"
# the same for the ISP, over the data model (asynchronous, as USP runs it)
"$BIN/agctl/agctl" dm do "Device.X_AIGW_.Diagnose()" Host=hwsim-phone > "$T/diag-dm.txt" ||
	fail "Device.X_AIGW_.Diagnose(): $(cat "$T/diag-dm.txt")"
grep -q "Code = DEVICE_BLOCKED" "$T/diag-dm.txt" && grep -q "Findings = DEVICE_BLOCKED,.*DNS_FAILING" "$T/diag-dm.txt" ||
	fail "Device.X_AIGW_.Diagnose() outputs: $(cat "$T/diag-dm.txt")"
"$BIN/agctl/agctl" dm get Device.X_AIGW_.LastDiagnosis. > "$T/diag-last.txt"
grep -q "LastDiagnosis.Code = DEVICE_BLOCKED" "$T/diag-last.txt" || fail "LastDiagnosis: $(cat "$T/diag-last.txt")"
grep -q 'DiagnosisDone!' "$T/heal-events.log" || fail "no DiagnosisDone! event"
"$BIN/agctl/agctl" undo "$DID" > /dev/null || fail "undo the disconnect"
echo "   $(grep -m1 'Code = ' "$T/diag-dm.txt" | sed 's/^ *//'); $(sed -n 's/.*Summary = //p' "$T/diag-dm.txt" | cut -c1-120)"
echo "HWSIM TEST OK"
