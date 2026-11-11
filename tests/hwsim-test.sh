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

config mgmt 'mgmt'
	option enabled '1'

config act 'act'
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
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & PIDS="$PIDS $!"

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
echo "HWSIM TEST OK"
