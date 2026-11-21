#!/bin/sh
# Neighbour-aware channels against a fake hostapd and a stand-in for `iw`, with
# five days of learned history seeded in the state file (the field case: the
# 2.4 GHz access point on channel 11, crowded by networks on 11 and 13; a few on
# 1; channel 6 nearly empty):
#
#   run 1 (advise)   while the home is busy (its own traffic high) nothing
#                    scans and nothing is proposed; once it is quiet, one
#                    learning scan, then channel 6 is proposed (not 1);
#                    approved, a channel switch announcement; the trial on 6
#                    (link rates 72 Mbit/s, before 40) is kept; agctl channels,
#                    the data model (ChannelPlan., ChannelPlan!), the timeline
#   run 2 (enforce)  the same history; applied by itself; on 6 the rates are
#                    worse (20 Mbit/s): moved back to 11, 6 avoided, and no new
#                    move until wifi.chan_plan_gap has passed
#
# Needs root (private ubusd) and python3.
#
#   sudo tests/chanplan-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
AGD=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "CHANPLAN TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/agd.log "$T"/hostapd.log "$T"/iw.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

echo "== a fake hostapd (2.4 GHz, channel 11, one phone) and a stand-in for iw"
mkdir -p "$T/hostapd"
echo 11 > "$T/channel"
echo 400 > "$T/rate_11"         # tx_rate_info, 100 kbit/s: 40 Mbit/s on 11
echo 720 > "$T/rate_6"          # 72 Mbit/s on 6 (run 1)
echo 300 > "$T/tx"              # the home's own share of the air, per mille: busy
python3 - "$T/hostapd/wlan24" "$T/hostapd.log" "$T" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, socket, sys
path, log, d = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
def rd(f, dflt):
    try:
        return open(os.path.join(d, f)).read().strip()
    except OSError:
        return dflt
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    w = cmd.split()
    if not cmd.startswith(("STA", "STATUS")):
        open(log, "a").write(cmd + "\n")
    ch = int(rd("channel", "11"))
    if cmd == "STATUS":
        r = "state=ENABLED\nfreq=%d\nchannel=%d\nsecondary_channel=0\nbssid[0]=02:aa:00:00:00:24\nssid[0]=AIGW-TEST\n" % (2407 + 5 * ch, ch)
    elif cmd == "STA-FIRST":
        r = "02:cc:00:00:00:01\nconnected_time=100\nsignal=-48\ntx_rate_info=%s\n" % rd("rate_%d" % ch, "400")
    elif cmd.startswith("STA"):
        r = ""
    elif w[0] == "CHAN_SWITCH":
        f = int(w[2]); open(os.path.join(d, "channel"), "w").write(str((f - 2407) // 5)); r = "OK\n"
    else:
        r = "UNKNOWN COMMAND\n"
    if addr:
        s.sendto(r.encode(), addr)
EOF
PIDS="$PIDS $!"

cat > "$T/iw" <<EOF
#!/bin/sh
# stand-in for iw: the survey of the channel in use, scans, the radio's channels
ch=\$(cat "$T/channel")
echo "\$*" >> "$T/iw.log"
case "\$*" in
*"survey dump"*)
	t=\$(( \$(date +%s) - $(date +%s) + 1000 ))
	case \$ch in 6) o=150 ;; *) o=550 ;; esac
	tx=\$(cat "$T/tx")
	echo "Survey data from wlan24"
	echo "	frequency:			\$((2407 + 5 * ch)) MHz [in use]"
	echo "	channel active time:		\$(( t * 1000 )) ms"
	echo "	channel busy time:		\$(( t * (o + tx) )) ms"
	echo "	channel transmit time:		\$(( t * tx )) ms" ;;
*"scan ap-force"*|*"scan dump"*)
	n=0
	for b in 2462:-55:200 2462:-62: 2472:-58:180 2472:-70: 2412:-72:60 2412:-80: 2422:-82: 2437:-88:; do
		n=\$((n + 1))
		f=\${b%%:*}; r=\${b#*:}; s=\${r%%:*}; u=\${r#*:}
		echo "BSS 02:bb:00:00:00:0\$n(on wlan24)"
		echo "	freq: \$f"
		echo "	signal: \$s.00 dBm"
		echo "	last seen: 900000 ms [boottime]"
		[ -n "\$u" ] && { echo "	BSS Load:"; echo "		 * station count: 4"; echo "		 * channel utilisation: \$u/255"; }
	done ;;
*"dev wlan24 info"*)
	echo "Interface wlan24"; echo "	wiphy 0" ;;
*"phy phy0 info"*)
	for c in 1 2 3 4 5 6 7 8 9 10 11 12 13; do echo "			* \$((2407 + 5 * c)).0 MHz [\$c] (20.0 dBm)"; done ;;
*) exit 1 ;;
esac
EOF
chmod +x "$T/iw"

# five days learned: 11 crowded (the air 55 % used by others, devices at 40 Mbit/s, 6 % loss), neighbours by scans
seed() {
	python3 - "$T/state.json" <<'EOF'
import json, sys, time
cells = []
for h in range(24):
    cells.append([11, h, 550, 20, 128, 3, 60, 1500, 20, 400, 20])
    cells.append([6, h, 0, 0, 14, 3, 0, 0, 0, 0, 0])
    cells.append([1, h, 0, 0, 35, 3, 0, 0, 0, 0, 0])
use = []
for h in range(24):
    use += [h, 200, 20]         # the home uses its Wi-Fi alike all day: every hour counts
st = {"chanplan": {"wlan24": {"band": 2, "first": int(time.time()) - 5 * 86400, "scans": 6, "cells": cells, "use": use}}}
json.dump(st, open(sys.argv[1], "w"))
EOF
}

config() {      # config <advise|enforce>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15358'

config flow 'flow'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option handler '/bin/true'
	option chan_poll '1'
	option chan_hold '3600'
	option chan_plan_every '2'
	option chan_plan_hold '2'
	option chan_plan_trial '10'
	option chan_plan_gap '3600'
	option chan_learn_gap '10'

config act 'act'
	option mode '$1'
	option dns_policy '$T/dns-policy'

# a configuration from before neighbour-aware channels: no wifi_plan policy (built in)
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
}

: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_IW="$T/iw"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
radio() { agctl -j channels | js "r = d['radios'][0]; $1"; }
wait_radio() {  # wait up to $1 s until a condition on the radio (r) holds
	i=0
	until agctl -j channels 2>/dev/null | js "r = d['radios'][0]; sys.exit(0 if $2 else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "waited for $2: $(agctl channels)"; sleep 0.5
	done
}
decision() {    # the newest channel decision: a field
	agctl -j decisions 50 | js "
l = [x for x in d['decisions'] if x['action'] == 'channel']
print(l[0].get('$1', '') if l else '')"
}
wait_decision() {       # wait_decision <s> <state>
	i=0
	until [ "$(decision state)" = "$2" ]; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "waited for a channel decision $2: $(agctl decisions)"; sleep 0.5
	done
}
scans() { grep -c "scan ap-force" "$T/iw.log" 2>/dev/null || true; }

echo "== run 1 (advise): five days learned; the home busy now"
seed
config advise
start_agd
wait_radio 20 "r['status'] == 'Planned' and r['best'] == 6"
agctl channels
radio "assert r['metric'] == 'neighbours' and r['gain_pct'] >= 30 and r['days'] == 5, r
assert 'busy' in r['waits'], r['waits']
assert len(r['channels']) == 3 and r['channels'][2]['air'][0] == 55, r['channels']" || fail "the verdict"
[ "$(scans)" = 0 ] || fail "scanned while the home was busy"
[ -z "$(decision state)" ] || fail "proposed while the home was busy"
agctl wifi | grep -q "channels        Channel 6 has clearly fewer" || fail "agctl wifi doesn't say it: $(agctl wifi)"

echo "== the home quiet: one learning scan, then channel 6 proposed (not 1)"
echo 10 > "$T/tx"
wait_decision 20 proposed
[ "$(decision target)" = "wlan24:6" ] || fail "proposed $(decision target), not wlan24:6"
decision origin | grep -q "policy wifi_plan" || fail "not by policy wifi_plan: $(decision origin)"
wait_radio 20 "r['scans'] >= 7 and r['proposed'] == 1"     # chan_learn_gap after the start, at a quiet moment
n=$(scans)      # one for this hour of the day (two if the test crosses an hour)
[ "$n" -ge 1 ] && [ "$n" -le 2 ] || fail "not one learning scan at the quiet moment: $n"
grep -q "CHAN_SWITCH" "$T/hostapd.log" 2>/dev/null && fail "switched before anyone approved (advise mode)"

echo "== approved: a channel switch announcement; the trial on 6 (72 Mbit/s, before 40) is kept"
agctl approve "$(decision id)" > /dev/null || fail "approve"
wait_radio 10 "r['status'] == 'Trial' and r['trial_from'] == 11"
grep -q "^CHAN_SWITCH 5 2437 ht" "$T/hostapd.log" || fail "no CHAN_SWITCH to 2437 MHz"
wait_decision 40 kept
agctl channels
radio "assert r['channel'] == 6 and r['kept'] == 1 and r['tried'] == 0 and 'kept' in r['last'], r
assert 'link rates are clearly higher' in r['last'], r['last']" || fail "the verdict wasn't kept"
decision note | grep -q "kept: channel 6, link rates" || fail "decision note: $(decision note)"
"$UBUS" -s "$AIGW_UBUS" call ag.dm get '{"paths":["Device.X_AIGW_.ChannelPlan."]}' > "$T/dm.json"
python3 - "$T/dm.json" <<'EOF' || fail "the data model: $(cat "$T/dm.json")"
import json, sys
p = json.load(open(sys.argv[1]))["values"]
assert p["Device.X_AIGW_.ChannelPlan.Enable"] == "true", p
r = "Device.X_AIGW_.ChannelPlan.Radio.1."
assert p[r + "Channel"] == "6" and p[r + "Kept"] == "1" and p[r + "LearnedDays"] == "5", p
assert p[r + "Radio"] == "Device.WiFi.Radio.1" and p[r + "BusyHours"] == "all day", p
EOF
sleep 1
events() {      # the ChannelPlan! events' states, in order
	python3 -c "
import json
ev = [json.loads(l) for l in open('$T/events.log').read().splitlines() if 'ChannelPlan!' in l]
print(' '.join(e.get('ag.dm.event', e)['args']['State'] for e in ev))"
}
[ "$(events)" = "Proposed Applied Kept" ] || fail "ChannelPlan! events: $(events)"
agctl why --last 10m | grep -q "planned channel kept" || fail "not on the timeline: $(agctl why --last 10m)"
stop_agd
python3 -c "
import json; c = json.load(open('$T/state.json'))['chanplan']['wlan24']
assert c['kept'] == 1 and c['proposed'] == 1, c
assert c['first'] and len(c['cells']) >= 72 and c['scans'] >= 7, c.keys()" || fail "the learned table is not saved"

echo "== run 2 (enforce): the same history; on 6 the link rates are worse (20 Mbit/s)"
echo 11 > "$T/channel"
echo 200 > "$T/rate_6"
: > "$T/hostapd.log"
seed
config enforce
start_agd
wait_radio 30 "r['status'] == 'Trial'"
grep -q "^CHAN_SWITCH 5 2437 ht" "$T/hostapd.log" || fail "enforce: not applied by itself"
wait_decision 40 tried
grep -q "^CHAN_SWITCH 5 2462 ht" "$T/hostapd.log" || fail "not switched back to 2462 MHz"
wait_radio 15 "r['channel'] == 11"
agctl channels
radio "assert r['channel'] == 11 and r['tried'] == 1 and r['kept'] == 0, r
assert [a['channel'] for a in r['avoided']] == [6], r['avoided']
assert 'link rates are lower' in r['last'], r['last']" || fail "the trial wasn't moved back"
decision note | grep -q "tried: channel 6, link rates are lower" || fail "decision note: $(decision note)"
events | grep -q "Kept Proposed Applied MovedBack$" || fail "ChannelPlan! events: $(events)"
echo "== 6 avoided; 1 is better than 11 too, but no new move until chan_plan_gap has passed"
sleep 6
radio "assert r['best'] != 6, r" || fail "6 chosen again"
n=$(agctl -j decisions 50 | js "print(len([x for x in d['decisions'] if x['action'] == 'channel']))")
[ "$n" = 1 ] || fail "another move within chan_plan_gap ($n channel decisions)"
radio "assert r['best'] != 1 or 'days ago' in r['waits'], r" || fail "the wait isn't said"
echo "CHANPLAN TEST OK"
