#!/bin/sh
# Proactive care before the call, end to end: agd in a gateway network
# namespace whose WAN (a veth) leads to an "ISP" namespace, a fake hostapd with
# a 5 GHz and a 2.4 GHz access point, the timeline sampled every 2 s, and a
# look every 2 s.
#
#   all fine       no ticket
#   the internet   the ISP's side stops answering for the 3 devices: a ticket
#                  to the ISP (Raised) with the brief, the evidence, the next
#                  step and a plain note for the customer; no names or MACs;
#                  ProactiveTicket! once, however often it is seen
#   it comes back  Resolved (told once), a call likely avoided
#   a flap         down again soon: the same ticket, reopened, and not a word
#                  to the ISP; up again: over, still not a word
#   advise mode    (restarted: the tickets and counts kept) a laptop hopping
#                  between the bands: AIGW only proposes keeping it on 2.4 GHz,
#                  nothing applied; the ISP is told a fix waits for a yes
#   enforce mode   a TV hopping: AIGW keeps it on 2.4 GHz by itself, and the
#                  ISP hears "we noticed X and fixed it" once it is over
#   the day's limit  a phone dropping off Wi-Fi: past proactive.max_per_day,
#                  kept at home (Suppressed), no event
#
# Needs root (network namespaces, a private ubusd, raw ICMP), python3, ip.
#
#   sudo tests/proactive-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"         # the test plug-in runs as nobody
GW=aigwpro-gw
ISP=aigwpro-isp
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	ip netns del "$GW" 2>/dev/null || true
	ip netns del "$ISP" 2>/dev/null || true
	rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "PROACTIVE TEST FAIL: $*" >&2; echo "--- logs" >&2; grep -a 'proactive' "$T"/agd.log | tail -n 30 >&2 2>/dev/null || true; tail -n 15 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
command -v ip >/dev/null || fail "missing ip"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

echo "== the gateway's namespace, its WAN to the ISP's, the internet behind it (10.98.0.1)"
ip netns del "$GW" 2>/dev/null || true
ip netns del "$ISP" 2>/dev/null || true
ip netns add "$GW"
ip netns add "$ISP"
ip link add prow0 netns "$GW" type veth peer name prow1 netns "$ISP"
ip -n "$GW" link set lo up
ip -n "$ISP" link set lo up
ip -n "$GW" addr add 10.97.0.1/24 dev prow0
ip -n "$ISP" addr add 10.97.0.2/24 dev prow1
ip -n "$ISP" addr add 10.98.0.1/32 dev lo
ip -n "$GW" link set prow0 up
ip -n "$ISP" link set prow1 up
ip -n "$GW" route add default via 10.97.0.2
# the home's LAN (the devices' addresses are on it: AIGW acts only on devices of its own LAN)
ip -n "$GW" link add lan0 type dummy
ip -n "$GW" addr add 10.96.0.1/24 dev lan0
ip -n "$GW" link set lan0 up

PHONE=02:00:00:00:8a:01
LAPTOP=02:00:00:00:8a:02
TV=02:00:00:00:8a:03
stations() { printf '%s\n' "$@" > "$T/stations.new"; mv "$T/stations.new" "$T/stations"; }
# MAC signal access point (wlan0: 5 GHz, wlan1: 2.4 GHz)
stations "$PHONE -50 wlan0" "$LAPTOP -55 wlan0" "$TV -60 wlan1"

echo "== a fake hostapd: wlan0 (5 GHz) and wlan1 (2.4 GHz); the stations in $T/stations; DENY_ACL kept"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, select, socket, sys, time
d, stations = sys.argv[1:3]
aps = {"wlan0": ("5180", "36", "02:aa:00:00:00:01"), "wlan1": ("2437", "6", "02:aa:00:00:00:02")}
socks = {}
for name in aps:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(os.path.join(d, name))
    socks[s] = name
deny = {n: set() for n in aps}
since = {}
def present(ap):
    out = {}
    for l in open(stations):
        w = l.split()
        if len(w) == 3 and w[2] == ap and w[0].lower() not in deny[ap]:
            out[w[0]] = w[1]
    for m in out:
        since.setdefault((ap, m), time.time())
    return out
def sta(ap, m, sig):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%s\nconnected_time=%d\ntx_rate_info=1440\n" % (
        m, sig, int(time.time() - since[(ap, m)]))
while True:
    r, _, _ = select.select(list(socks), [], [])
    for s in r:
        ap = socks[s]
        data, addr = s.recvfrom(4096)
        cmd = data.decode().strip()
        w = cmd.split()
        if cmd == "STATUS":
            f, ch, bssid = aps[ap]
            rep = "state=ENABLED\nfreq=%s\nchannel=%s\nbssid[0]=%s\nssid[0]=AIGW-TEST\n" % (f, ch, bssid)
        elif cmd == "STA-FIRST":
            p = present(ap); k = sorted(p); rep = sta(ap, k[0], p[k[0]]) if k else ""
        elif w and w[0] == "STA-NEXT":
            p = present(ap); k = sorted(p); i = k.index(w[1]) + 1 if w[1] in k else len(k)
            rep = sta(ap, k[i], p[k[i]]) if i < len(k) else ""
        elif cmd == "DENY_ACL SHOW":
            rep = "".join("%s VLAN_ID=0\n" % m for m in sorted(deny[ap]))
        elif len(w) == 3 and w[0] == "DENY_ACL" and w[1] == "ADD_MAC":
            deny[ap].add(w[2].lower()); rep = "OK\n"
            print("deny", ap, w[2], flush=True)
        elif len(w) == 3 and w[0] == "DENY_ACL" and w[1] == "DEL_MAC":
            deny[ap].discard(w[2].lower()); rep = "OK\n"
        elif w and w[0] in ("DEAUTHENTICATE", "DISASSOCIATE", "BSS_TM_REQ"):
            rep = "OK\n"
        else:
            rep = "UNKNOWN COMMAND\n"
        if addr:
            s.sendto(rep.encode(), addr)
EOF
PIDS="$PIDS $!"

echo "== the gateway's own DNS (where Diagnose asks): a stand-in that answers every name"
ip netns exec "$GW" python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 15374))
while True:
    q, a = s.recvfrom(1500)
    i = 12
    while i < len(q) and q[i]:
        i += q[i] + 1
    qe = i + 5
    s.sendto(q[:2] + b'\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00' + q[12:qe] +
             b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04' + bytes([10, 98, 0, 1]), a)
" > "$T/dns.log" 2>&1 &
PIDS="$PIDS $!"

config() {   # config <act mode> <threshold>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15374'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'
	option heal '0'
	option heal_hops '2'

config diag 'diag'
	option targets '10.98.0.1'
	option name 'example.com'

config care 'care'
	option enabled '0'

config why 'why'
	option interval '2'
	option hours '1'
	option file '$T/why.bin'

config sqm 'sqm'
	option enabled '0'

config act 'act'
	option mode '$1'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/user'
	option user 'nobody'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'
	option actions '0'

config brief 'brief'
	option enabled '1'
	option names '0'

config proactive 'proactive'
	option enabled '1'
	option fix '1'
	option interval '2'
	option threshold '$2'
	option min_duration '4'
	option clear '6'
	option cooldown '600'
	option max_per_day '3'

config lan 'lan'
	list iface 'lan0'
EOF
}
config advise 3
# a test plug-in standing in for cortex's area outage notice (the bridge passes it on the same way):
# renewed every second while $T/pstate/nudge/outage exists, lapsing 3 s after
mkdir -p "$T/sys/nudge" "$T/user"
cat > "$T/sys/nudge/manifest.json" <<'EOF'
{ "name": "nudge", "kind": "sensor", "version": "1.0.0", "description": "proactive-test: an area outage notice on request",
  "exec": "run.sh", "caps": ["area.notice"], "hosts": ["linux", "openwrt"], "budget": { "ram_kb": 8192, "cpu_pct": 5 } }
EOF
cat > "$T/sys/nudge/run.sh" <<'EOF'
#!/bin/sh
while :; do
	[ -f "$AIGW_STATE/outage" ] &&
		echo '{"area": {"incident": 41, "kind": "DOWN", "since": 1, "homes": 5, "of": 8, "confidence": "high", "uplink": "OLT-P/1", "hold_s": 3}}'
	sleep 1
done
EOF
chmod 755 "$T/sys/nudge/run.sh"
printf '1999999999 %s 10.96.0.41 dads-phone *\n1999999999 %s 10.96.0.42 sara-laptop *\n1999999999 %s 10.96.0.43 living-room-tv *\n' \
	"$PHONE" "$LAPTOP" "$TV" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { AIGW_TEST_AREA_HOLD_MIN=1 ip netns exec "$GW" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & L=$!; PIDS="$PIDS $L"

agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
pro() { agctl -j care proactive "$@"; }
wait_pro() {   # wait up to $1 s until a condition on `agctl -j care proactive` (d) holds
	n=$1; c=$2
	i=0
	until pro | js "sys.exit(0 if $c else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $((n * 2)) ] || fail "waited for $c: $(agctl care proactive)"; sleep 0.5
	done
}
ticket() { echo "[t for t in d['tickets'] if t['code'] $1]"; }
events() { grep -c 'ProactiveTicket!' "$T/events.log" || true; }
last_status() {   # the last ProactiveTicket!'s Status
	python3 -c "
import json, sys
ev = [json.loads(l) for l in open('$T/events.log').read().splitlines() if 'ProactiveTicket!' in l]
print(ev[-1].get('ag.dm.event', ev[-1])['args']['Status'] if ev else '')"
}
# the words a customer wouldn't know, and the home's names, never in what the ISP gets
PLAIN="import re
def plain(s):
    return not re.search(r'[A-Z]{3,}|_|\\b(DNS|SQM|MAC|ISP|IP|GHz|dBm|ms|Mbit)\\b|packet|latency|jitter|channel|router|band', s)
NAMES = ('dads-phone', 'sara-laptop', 'living-room-tv', '$PHONE', '$LAPTOP', '$TV', '10.96.0.4')"

echo "== all fine: no ticket"
sleep 6
pro --run | js "sys.exit(0 if d['enabled'] in (1, True) and d['open'] == 0 and not d['tickets'] else 1)" ||
	fail "all fine: $(agctl care proactive)"

echo "== the internet stops answering (the ISP's side), for the 3 devices"
ip -n "$ISP" addr del 10.98.0.1/32 dev lo
wait_pro 40 "$(ticket "in ('ISP_DOWN', 'LINE_DOWN')") and $(ticket "in ('ISP_DOWN', 'LINE_DOWN')")[0]['status'] == 'Raised'"
agctl care proactive
pro | python3 -c "
import json, sys
$PLAIN
d = json.load(sys.stdin)
t = [t for t in d['tickets'] if t['code'] in ('ISP_DOWN', 'LINE_DOWN')][0]
text = json.dumps(d)
ok = t['fault'] == 'LINE' and t['open'] in (1, True) and t['told_isp'] in (1, True) and t['devices'] == 3
ok = ok and t['points'] >= 3 and 'a problem +2' in t['reason'] and t['summary'] and t['next_step'] and 'Worth acting on' in t['evidence']
ok = ok and plain(t['customer_note']) and t['customer_note'].startswith('We noticed') and 'on it' in t['customer_note']
ok = ok and not any(n in t['summary'] + t['evidence'] + t['next_step'] + t['customer_note'] for n in NAMES)
sys.exit(0 if ok else 1)" || fail "the internet down: $(agctl care proactive)"
TID=$(pro | js "print([t for t in d['tickets'] if t['code'] in ('ISP_DOWN', 'LINE_DOWN')][0]['id'])")
sleep 1
[ "$(events)" = 1 ] || fail "ProactiveTicket! once: $(cat "$T/events.log")"
python3 - "$T/events.log" <<EOF || fail "the event: $(cat "$T/events.log")"
import json, sys
$PLAIN
raw = open(sys.argv[1]).read()
ev = [json.loads(l) for l in raw.splitlines() if 'ProactiveTicket!' in l][0]
a = ev['ag.dm.event']['args'] if 'ag.dm.event' in ev else ev['args']
ok = a['Ticket'] == '$TID' and a['Status'] == 'Raised' and a['Fault'] == 'LINE' and a['Since'].endswith('Z')
ok = ok and plain(a['CustomerNote']) and a['NextStep'] and a['Evidence'] and not any(n in raw for n in NAMES)
sys.exit(0 if ok else 1)
EOF
echo "   ticket $TID raised to the ISP: $(pro | js "print([t for t in d['tickets'] if t['id'] == $TID][0]['customer_note'])")"
"$UBUS" -s "$AIGW_UBUS" call ag.dm get '{"paths":["Device.X_AIGW_.Proactive."]}' > "$T/dm.json"
js "v = d['values']; p = 'Device.X_AIGW_.Proactive.'
ok = v[p + 'Enable'] == 'true' and v[p + 'OpenTickets'] == '1' and v[p + 'ToldISP'] == '1' and v[p + 'TicketNumberOfEntries'] == '1'
ok = ok and v[p + 'Ticket.$TID.Status'] == 'Raised' and v[p + 'Ticket.$TID.Fault'] == 'LINE' and v[p + 'Ticket.$TID.Open'] == 'true'
ok = ok and v[p + 'Ticket.$TID.CustomerNote'].startswith('We noticed') and v[p + 'Ticket.$TID.Opened'].endswith('Z')
sys.exit(0 if ok else 1)" < "$T/dm.json" || fail "TR-181 Device.X_AIGW_.Proactive.: $(cat "$T/dm.json")"
"$UBUS" -s "$AIGW_UBUS" call ag.dm schema > "$T/schema.json"
js "sys.exit(0 if d['version'] == '1.20' and 'Device.X_AIGW_.ProactiveTicket!' in [e['path'] for e in d['events']] else 1)" \
	< "$T/schema.json" || fail "data model 1.20 with ProactiveTicket!: $(head -c 400 "$T/schema.json")"

echo "== seen again and again: still one ticket, one event"
sleep 6
pro | js "sys.exit(0 if d['open'] == 1 and d['counts']['tickets'] == 1 and d['counts']['told_isp'] == 1 else 1)" ||
	fail "one ticket: $(agctl care proactive)"
[ "$(events)" = 1 ] || fail "no new event while it lasts: $(cat "$T/events.log")"

echo "== the internet answers again: Resolved, a call likely avoided"
ip -n "$ISP" addr add 10.98.0.1/32 dev lo
wait_pro 60 "$(ticket "in ('ISP_DOWN', 'LINE_DOWN')")[0]['status'] == 'Resolved' and d['open'] == 0"
pro | js "c = d['counts']; sys.exit(0 if c['calls_likely_avoided'] == 1 and c['called_anyway'] == 0 else 1)" ||
	fail "a call likely avoided: $(agctl care proactive)"
sleep 1
[ "$(events)" = 2 ] || fail "Resolved told once: $(cat "$T/events.log")"
[ "$(last_status)" = Resolved ] || fail "the last event: Resolved: $(cat "$T/events.log")"

echo "== a flap: down again soon, then up: the same ticket, not a word to the ISP"
ip -n "$ISP" addr del 10.98.0.1/32 dev lo
wait_pro 40 "$(ticket "in ('ISP_DOWN', 'LINE_DOWN')")[0]['open'] in (1, True) and $(ticket "in ('ISP_DOWN', 'LINE_DOWN')")[0]['flaps'] == 1"
ip -n "$ISP" addr add 10.98.0.1/32 dev lo
pro | js "t = [t for t in d['tickets'] if t['id'] == $TID]; sys.exit(0 if t and d['counts']['tickets'] == 1 else 1)" ||
	fail "the same ticket: $(agctl care proactive)"
wait_pro 60 "d['open'] == 0"
sleep 1
[ "$(events)" = 2 ] || fail "a flap: no event: $(cat "$T/events.log")"
pro | js "c = d['counts']; sys.exit(0 if c['deduplicated'] >= 1 and c['told_isp'] == 1 and c['calls_likely_avoided'] == 1 else 1)" ||
	fail "a flap counts: $(agctl care proactive)"
echo "   ticket $TID reopened and over again, the ISP not told twice"

echo "== advise mode (restarted, threshold 1): a laptop hopping between the bands"
stop_agd
config advise 1
start_agd
pro | js "sys.exit(0 if d['counts']['tickets'] == 1 and d['counts']['told_isp'] == 1 and $(ticket "in ('ISP_DOWN', 'LINE_DOWN')") else 1)" ||
	fail "the tickets and counts must survive a restart: $(agctl care proactive)"
at() {   # at <mac> <where>: the three devices where they usually are, this one at <where>
	l1="$PHONE -50 wlan0"; l2="$LAPTOP -55 wlan0"; l3="$TV -60 wlan1"
	[ "$1" = "$PHONE" ] && l1="$1 $2"
	[ "$1" = "$LAPTOP" ] && l2="$1 $2"
	[ "$1" = "$TV" ] && l3="$1 $2"
	stations "$l1" "$l2" "$l3"
}
hop() {   # hop <mac> <times>: 5 GHz at a marginal -80 dBm, 2.4 GHz at -60, and back (agd looks every 5 s)
	k=0
	while [ $k -lt "$2" ]; do
		at "$1" "-80 wlan0"; sleep 6
		at "$1" "-60 wlan1"; sleep 6
		k=$((k + 1))
	done
}
hop "$LAPTOP" 2
wait_pro 40 "$(ticket "== 'WIFI_HOPPING'") and $(ticket "== 'WIFI_HOPPING'")[0]['status'] == 'Proposed'"
agctl care proactive
LT=$(pro | js "print([t for t in d['tickets'] if t['code'] == 'WIFI_HOPPING'][0]['id'])")
DEC=$(pro | js "print([t for t in d['tickets'] if t['id'] == $LT][0]['decision'])")
pro | js "t = [t for t in d['tickets'] if t['id'] == $LT][0]; sys.exit(0 if t['target'] == '$LAPTOP' and t['told_isp'] in (1, True) else 1)" ||
	fail "at home, the laptop's ticket names it: $(agctl -j care proactive)"
agctl -j decisions | js "
x = [k for k in d['decisions'] if k['id'] == $DEC]
sys.exit(0 if x and x[0]['action'] == 'stay24' and x[0]['state'] == 'proposed' and 'proactive care' in x[0]['origin'] else 1)" ||
	fail "advise mode: stay24 only proposed (decision $DEC): $(agctl decisions)"
grep -q "deny wlan0 $LAPTOP" "$T/fakehapd.log" && fail "advise mode: nothing may be applied: $(cat "$T/fakehapd.log")"
sleep 1
[ "$(last_status)" = Proposed ] || fail "the ISP told a fix waits: $(cat "$T/events.log")"
grep -q "$LAPTOP\|sara-laptop" "$T/events.log" && fail "the event must not name the laptop: $(cat "$T/events.log")"
echo "   ticket $LT: keeping it on 2.4 GHz proposed as decision $DEC, nothing applied"

echo "== enforce mode (restarted): a TV hopping: AIGW keeps it on 2.4 GHz itself"
agctl reject "$DEC" > /dev/null || fail "reject $DEC"
stop_agd
config enforce 1
start_agd
EV=$(events)
hop "$TV" 2
at "$TV" "-60 wlan1"
wait_pro 40 "[t for t in d['tickets'] if t['code'] == 'WIFI_HOPPING' and t['id'] != $LT and t['status'] in ('Fixing', 'Fixed')]"
TT=$(pro | js "print([t for t in d['tickets'] if t['code'] == 'WIFI_HOPPING' and t['id'] != $LT][0]['id'])")
TDEC=$(pro | js "print([t for t in d['tickets'] if t['id'] == $TT][0]['decision'])")
agctl -j decisions | js "
x = [k for k in d['decisions'] if k['id'] == $TDEC]
sys.exit(0 if x and x[0]['action'] == 'stay24' and x[0]['state'] == 'active' else 1)" ||
	fail "enforce mode: stay24 applied (decision $TDEC): $(agctl decisions)"
grep -qi "deny wlan0 $TV" "$T/fakehapd.log" || fail "the TV refused on 5 GHz: $(cat "$T/fakehapd.log")"
wait_pro 60 "[t for t in d['tickets'] if t['id'] == $TT][0]['status'] == 'Fixed'"
agctl care proactive
sleep 1
python3 - "$T/events.log" <<EOF || fail "the ISP hears it was fixed: $(cat "$T/events.log")"
import json, sys
$PLAIN
raw = open(sys.argv[1]).read()
ev = [json.loads(l) for l in raw.splitlines() if 'ProactiveTicket!' in l]
a = [e.get('ag.dm.event', e)['args'] for e in ev]
f = [x for x in a if x['Ticket'] == '$TT']
ok = len(f) == 1 and f[0]['Status'] == 'Fixed' and 'nothing you need to do' in f[0]['CustomerNote'] and plain(f[0]['CustomerNote'])
ok = ok and not any(n in raw for n in NAMES)
sys.exit(0 if ok else 1)
EOF
pro | js "c = d['counts']; sys.exit(0 if c['fixed_by_aigw'] == 1 and c['calls_likely_avoided'] >= 2 else 1)" ||
	fail "fixed by AIGW, counted: $(agctl care proactive)"
echo "   ticket $TT: fixed by AIGW (decision $TDEC); the ISP told only once it was over"

echo "== the day's limit: a phone dropping off Wi-Fi, kept at home"
pro | js "sys.exit(0 if d['left_today'] == 0 else 1)" || fail "3 new tickets told today: $(agctl care proactive)"
EV=$(events)
k=0
while [ $k -lt 3 ]; do
	at "$PHONE" "-50 gone"; sleep 6
	at "$PHONE" "-50 wlan0"; sleep 6
	k=$((k + 1))
done
wait_pro 30 "$(ticket "== 'WIFI_DROPS'") and $(ticket "== 'WIFI_DROPS'")[0]['status'] == 'Suppressed'"
sleep 1
[ "$(events)" = "$EV" ] || fail "kept at home: no event: $(cat "$T/events.log")"
pro | js "c = d['counts']; sys.exit(0 if c['suppressed'] >= 1 else 1)" || fail "counted as kept at home: $(agctl care proactive)"
agctl care proactive

echo "== a known area outage: no tickets, no notes, not a word to the ISP"
EV=$(events)
N0=$(pro | js "print(d['counts']['tickets'])")
touch "$T/pstate/nudge/outage"
wait_pro 20 "d['area_outage'] in (1, True)"
ip -n "$ISP" addr del 10.98.0.1/32 dev lo
wait_pro 45 "d['counts']['held_area_outage'] >= 1"     # the line down, worth a ticket: held
sleep 6                                         # and more looks
pro | js "
c = d['counts']
ok = c['tickets'] == $N0 and c['held_area_outage'] >= 1
ok = ok and not [t for t in d['tickets'] if t['code'] in ('ISP_DOWN', 'LINE_DOWN') and t['open'] in (1, True)]
sys.exit(0 if ok else 1)" || fail "an area outage: no ticket: $(agctl care proactive)"
[ "$(events)" = "$EV" ] || fail "an area outage: no event: $(cat "$T/events.log")"
"$UBUS" -s "$AIGW_UBUS" call ag.dm get '{"paths":["Device.X_AIGW_.Proactive.HeldForAreaOutage"]}' |
	js "sys.exit(0 if int(d['values']['Device.X_AIGW_.Proactive.HeldForAreaOutage']) >= 1 else 1)" ||
	fail "HeldForAreaOutage"
echo "   the internet down in a known area outage: no ticket, no event"
rm -f "$T/pstate/nudge/outage"
wait_pro 20 "d['area_outage'] in (0, False)"
wait_pro 30 "[t for t in d['tickets'] if t['code'] in ('ISP_DOWN', 'LINE_DOWN') and t['open'] in (1, True)]"
echo "   the notice lapsed, the line still down: its ticket again"
ip -n "$ISP" addr add 10.98.0.1/32 dev lo

echo "== the help desk is asked: open tickets were not in time"
agctl brief --no-check > /dev/null
pro | js "c = d['counts']; sys.exit(0 if c['called_anyway'] >= 1 else 1)" || fail "called anyway: $(agctl care proactive)"
stop_agd
python3 -c "import json,sys; p=json.load(open('$T/state.json'))['proactive']; sys.exit(0 if p['tickets'] >= 4 and p['list'] else 1)" ||
	fail "the tickets are kept in the state"
echo "PROACTIVE TEST OK"
