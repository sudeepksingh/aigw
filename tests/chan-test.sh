#!/bin/sh
# Channel self-healing against a fake hostapd and a stand-in for `iw`:
#
#   the 2.4 GHz access point sits on channel 6; the survey says 80 % of the air
#   is used by others; a scan finds 6 strong networks on 6, 3 on 11, 1 weak on 1
#
# Checks: agd measures the channel, raises "wifi-busy" naming channel 1, the
# wifi_busy policy proposes a "channel" decision; approving it sends hostapd a
# channel switch announcement (CHAN_SWITCH) for channel 1, undoing it switches
# back to 6; a manual "agctl channel 2.4 11" works, and a channel AIGW doesn't
# use is refused. Needs root (private ubusd) and python3.
#
#   sudo tests/chan-test.sh [build-dir]
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
fail() { echo "CHAN TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 50 "$T"/agd.log "$T"/hostapd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

echo "== a fake hostapd (2.4 GHz, channel 6) and a stand-in for iw"
mkdir -p "$T/hostapd"
echo 6 > "$T/channel"
python3 - "$T/hostapd/wlan24" "$T/hostapd.log" "$T/channel" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import socket, sys
path, log, chf = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    w = cmd.split()
    if not cmd.startswith(("STA", "STATUS")):
        open(log, "a").write(cmd + "\n")
    ch = int(open(chf).read())
    if cmd == "STATUS":
        r = "state=ENABLED\nfreq=%d\nchannel=%d\nsecondary_channel=0\nbssid[0]=02:aa:00:00:00:24\nssid[0]=AIGW-TEST\n" % (2407 + 5 * ch, ch)
    elif cmd.startswith("STA"):
        r = ""
    elif w[0] == "CHAN_SWITCH":
        f = int(w[2]); open(chf, "w").write(str((f - 2407) // 5)); r = "OK\n"
    else:
        r = "UNKNOWN COMMAND\n"
    if addr:
        s.sendto(r.encode(), addr)
EOF
PIDS="$PIDS $!"

cat > "$T/iw" <<EOF
#!/bin/sh
# stand-in for iw: survey of a crowded channel, a scan, the radio's channels
case "\$*" in
*"survey dump"*)
	t=\$(( \$(date +%s) - $(date +%s) + 1000 ))
	echo "Survey data from wlan24"
	echo "	frequency:			2437 MHz [in use]"
	echo "	noise:				-92 dBm"
	echo "	channel active time:		\$(( t * 1000 )) ms"
	echo "	channel busy time:		\$(( t * 850 )) ms"
	echo "	channel transmit time:		\$(( t * 50 )) ms" ;;
*"scan ap-force"*)
	n=0
	for f in 2437 2437 2437 2437 2437 2437 2462 2462 2462 2412; do
		n=\$((n + 1))
		case \$f in 2437) s=-50 ;; 2462) s=-70 ;; *) s=-85 ;; esac
		echo "BSS 02:bb:00:00:00:0\$n(on wlan24)"
		echo "	freq: \$f"
		echo "	signal: \$s.00 dBm"
	done ;;
*"dev wlan24 info"*)
	echo "Interface wlan24"; echo "	wiphy 0" ;;
*"phy phy0 info"*)
	for c in 1 2 3 4 5 6 7 8 9 10 11; do echo "			* \$((2407 + 5 * c)).0 MHz [\$c] (20.0 dBm)"; done ;;
*) exit 1 ;;
esac
EOF
chmod +x "$T/iw"

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

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option chan_poll '5'
	option chan_hold '10'
	option chan_scan_gap '3600'

config act 'act'
	option mode 'advise'
	option dns_policy '$T/dns-policy'

config policy 'wifi_busy'
	option alert 'wifi-busy'
	option action 'channel'
	option ttl '86400'

config plugins 'plugins'
	option enabled '0'
	option state_dir '$T/plugins'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config lan 'lan'
	list iface 'lo'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_IW="$T/iw"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
agctl() { "$BIN/agctl/agctl" "$@"; }

echo "== agd measures, then finds the channel busy for 10 s and looks for a quieter one"
i=0
until agctl -j decisions 2>/dev/null | grep -q '"action": "channel"'; do
	i=$((i + 1)); [ $i -gt 60 ] && fail "no channel proposal after 60 s"; sleep 1
done
agctl wifi
agctl alerts 3
agctl -j decisions > "$T/dec.json"
id=$(python3 -c "
import json,sys
d=[x for x in json.load(open(sys.argv[1]))['decisions'] if x['action']=='channel']
assert d and d[0]['state']=='proposed' and d[0]['target']=='wlan24:1', d
print(d[0]['id'])" "$T/dec.json") || fail "proposal is not 'channel wlan24:1'"
grep -q "CHAN_SWITCH" "$T/hostapd.log" 2>/dev/null && fail "switched before anyone approved (advise mode)"

echo "== approve: a channel switch announcement for channel 1"
agctl approve "$id" > /dev/null || fail "approve"
sleep 6
grep -q "^CHAN_SWITCH 5 2412 ht" "$T/hostapd.log" || fail "no CHAN_SWITCH to 2412 MHz"
[ "$(cat "$T/channel")" = 1 ] || fail "access point not on channel 1"
agctl -j wifi | grep -q '"channel": 1' || fail "agd does not see channel 1"
n=$(grep -c "^CHAN_SWITCH" "$T/hostapd.log")
sleep 62    # the minute's re-check must not switch again
[ "$(grep -c "^CHAN_SWITCH" "$T/hostapd.log")" = "$n" ] || fail "re-check switched again"

echo "== undo: back to channel 6"
agctl undo "$id" > /dev/null || fail "undo"
sleep 6
grep -q "^CHAN_SWITCH 5 2437 ht" "$T/hostapd.log" || fail "no switch back to 2437 MHz"
[ "$(cat "$T/channel")" = 6 ] || fail "not back on channel 6"

echo "== by hand: agctl channel 2.4 11 ; a channel AIGW doesn't use is refused"
agctl channel 2.4 11 10m || fail "agctl channel 2.4 11"
sleep 6
[ "$(cat "$T/channel")" = 11 ] || fail "not on channel 11"
agctl channel 2.4 14 2>&1 | grep -q "not <access point" || fail "channel 14 not refused"
agctl channel 2.4 11 2>&1 | grep -qi "already on channel 11\|already" || true
echo "CHAN TEST OK"
