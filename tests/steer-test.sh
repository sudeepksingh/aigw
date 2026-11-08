#!/bin/sh
# Band steering and presence against a fake dual-band hostapd: two control
# sockets, a 2.4 GHz and a 5 GHz access point with one name, and devices
# that answer the way real ones do:
#
#   phone    2.4 GHz, -45 dBm, 802.11v, dual-band   -> asked to move (BSS_TM_REQ), moves to 5 GHz
#   laptop   2.4 GHz, -50 dBm, no 802.11v, dual     -> refused on 2.4 GHz for 30 s and kicked, joins 5 GHz
#   far      5 GHz,   -82 dBm, 802.11v              -> moved down to 2.4 GHz, stays there (no ping-pong)
#   plug     2.4 GHz, -40 dBm, 2.4 GHz only         -> never moved
#   tv       2.4 GHz, -40 dBm, dual, 802.11v, in act.protect -> never moved
#   stubborn 2.4 GHz, -40 dBm, dual, 802.11v, ignores requests -> left alone after 3 misses
#   watch    2.4 GHz, -42 dBm, 802.11v, lists no bands -> asked anyway (it may decline), moves to 5 GHz
#   visitor  joins, then leaves                     -> "arrived", then "left" (presence)
#
# Needs root (private ubusd) and python3.
#
#   sudo tests/steer-test.sh [build-dir]
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
fail() { echo "STEER TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/agd.log "$T"/hostapd.log >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

echo "== a fake dual-band hostapd"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/hostapd.log" "$T/gone" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, select, socket, sys, time
d, log, gone = sys.argv[1:4]
APS = {"wlan24": dict(freq=2437, channel=6, bssid="02:aa:00:00:00:24"),
       "wlan5": dict(freq=5180, channel=36, bssid="02:aa:00:00:00:05")}
BTM = "ext_capab=0400080000000040"            # byte 2 bit 3: BSS transition
NOBTM = "ext_capab=0400000000000040"
DUAL = "supp_op_classes=51515354737475767778797a7b7c7d7e7f80"   # lists 115+ (0x73)
ONLY24 = "supp_op_classes=515153"
now = time.time()
# mac: [ap, signal on 2.4, signal on 5, btm, dual, obeys, joined]
st = {
    "02:00:00:00:51:01": ["wlan24", -45, -52, True, True, True, now - 100],   # phone
    "02:00:00:00:51:02": ["wlan24", -50, -55, False, True, True, now - 100],  # laptop
    "02:00:00:00:51:03": ["wlan5", -70, -82, True, True, True, now - 100],    # far
    "02:00:00:00:51:04": ["wlan24", -40, -40, False, False, True, now - 100], # plug
    "02:00:00:00:51:05": ["wlan24", -40, -45, True, True, True, now - 100],   # tv (protected)
    "02:00:00:00:51:06": ["wlan24", -40, -45, True, True, False, now - 100],  # stubborn
    "02:00:00:00:51:07": ["wlan24", -60, -66, False, False, True, now - 100], # visitor
    "02:00:00:00:51:08": ["wlan24", -42, -48, True, None, True, now - 100],   # watch: no bands listed
}
deny = {"wlan24": set(), "wlan5": set()}
socks = {}
for ap in APS:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(os.path.join(d, ap))
    socks[s] = ap
def present(ap):
    g = open(gone).read().split() if os.path.exists(gone) else []
    return [m for m, v in sorted(st.items()) if v[0] == ap and m not in g]
def sta(ap, m):
    v = st[m]
    sig = v[1] if ap == "wlan24" else v[2]
    ops = DUAL if v[4] else "" if v[4] is None else ONLY24
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=%d\nconnected_time=%d\ntx_rate_info=1440\n%s\n%s\n" % (
        m, sig, int(time.time() - v[6]), BTM if v[3] else NOBTM, ops)
while True:
    r, _, _ = select.select(list(socks), [], [])
    for s in r:
        ap = socks[s]
        data, addr = s.recvfrom(4096)
        cmd = data.decode().strip()
        w = cmd.split()
        if not cmd.startswith(("STA", "STATUS")):
            open(log, "a").write("%s %s\n" % (ap, cmd))
        if cmd == "STATUS":
            a = APS[ap]
            rep = "state=ENABLED\nfreq=%d\nchannel=%d\nbssid[0]=%s\nssid[0]=AIGW-TEST\n" % (a["freq"], a["channel"], a["bssid"])
        elif cmd == "STA-FIRST":
            p = present(ap); rep = sta(ap, p[0]) if p else ""
        elif w[0] == "STA-NEXT":
            p = present(ap); i = p.index(w[1]) + 1 if w[1] in p else len(p)
            rep = sta(ap, p[i]) if i < len(p) else ""
        elif w[0] == "BSS_TM_REQ":
            m = w[1]
            rep = "OK\n"
            if m in st and st[m][5]:                     # it moves to the access point named
                to = "wlan5" if "02:aa:00:00:00:05" in cmd else "wlan24"
                st[m][0] = to; st[m][6] = time.time()
        elif w[:2] == ["DENY_ACL", "ADD_MAC"]:
            deny[ap].add(w[2]); rep = "OK\n"
        elif w[:2] == ["DENY_ACL", "DEL_MAC"]:
            deny[ap].discard(w[2]); rep = "OK\n"
        elif cmd == "DENY_ACL SHOW":
            rep = "".join(m + " VLAN_ID=0\n" for m in deny[ap])
        elif w[0] == "DEAUTHENTICATE":
            m = w[1]; rep = "OK\n"
            if m in st and st[m][0] == ap and m in deny[ap]:   # refused here: joins the other band
                st[m][0] = "wlan5" if ap == "wlan24" else "wlan24"; st[m][6] = time.time()
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
	option port '15355'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer_hold '5'
	option steer_gap '10'
	option steer_rate '20'
	option presence_away '10'

config act 'act'
	option dns_policy '$T/dns-policy'
	option protect '02:00:00:00:51:05'

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
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
sleep 1
"$BIN/agctl/agctl" watch wifi > "$T/watch.log" 2>&1 & PIDS="$PIDS $!"

echo "== devices settle (20 s on an access point before any move), then steering"
sleep 25
echo "== the visitor leaves"
echo "02:00:00:00:51:07" > "$T/gone"
sleep 60    # moves, 15 s checks, the stubborn device's three misses, the visitor's 10 s away

agctl() { "$BIN/agctl/agctl" "$@"; }
agctl wifi
agctl -j wifi > "$T/radios.json"
echo "--- live events"; cat "$T/watch.log"
python3 - "$T/radios.json" "$T/hostapd.log" "$T/watch.log" "$T/agd.log" <<'EOF' || fail "steering checks"
import json, sys
r = json.load(open(sys.argv[1]))
hlog, watch, agd = open(sys.argv[2]).read(), open(sys.argv[3]).read(), open(sys.argv[4]).read()
where = {s["mac"]: rad["band"] for rad in r["radios"] for s in rad["stations"]}
problems = []
def want(mac, band, what):
    if where.get(mac) != band: problems.append("%s: on %s GHz, expected %s (%s)" % (what, where.get(mac), band, what))
want("02:00:00:00:51:01", "5", "phone moved up by 802.11v")
want("02:00:00:00:51:02", "5", "laptop moved up by refusal")
want("02:00:00:00:51:03", "2.4", "far device moved down, and not back")
want("02:00:00:00:51:04", "2.4", "2.4 GHz-only plug untouched")
want("02:00:00:00:51:05", "2.4", "protected tv untouched")
want("02:00:00:00:51:06", "2.4", "stubborn device")
want("02:00:00:00:51:08", "5", "802.11v device that lists no bands, asked anyway")
if "BSS_TM_REQ 02:00:00:00:51:01" not in hlog: problems.append("no BSS_TM_REQ for the phone")
if "neighbor=02:aa:00:00:00:05,0x0000000f,115,36,9" not in hlog: problems.append("BSS_TM_REQ without the 5 GHz neighbour")
if "wlan24 DENY_ACL ADD_MAC 02:00:00:00:51:02" not in hlog: problems.append("laptop not refused on 2.4 GHz")
if "wlan24 DENY_ACL DEL_MAC 02:00:00:00:51:02" not in hlog: problems.append("laptop's refusal not lifted after 30 s")
if "BSS_TM_REQ 02:00:00:00:51:03" not in hlog or "neighbor=02:aa:00:00:00:24,0x0000000f,81,6,7" not in hlog:
    problems.append("no move down for the far device")
for m in ("02:00:00:00:51:04", "02:00:00:00:51:05"):
    if m in hlog: problems.append("%s was asked to move" % m)
if hlog.count("BSS_TM_REQ 02:00:00:00:51:06") != 3: problems.append("stubborn device asked %d times, expected 3" % hlog.count("BSS_TM_REQ 02:00:00:00:51:06"))
if "leaving it on its band for a day" not in agd: problems.append("stubborn device not left alone")
if hlog.count("BSS_TM_REQ 02:00:00:00:51:03") != 1: problems.append("far device ping-ponged")
if "moved as asked" not in watch or "stayed where it was" not in watch: problems.append("steering results missing from events")
if "left" not in watch: problems.append("no 'left' event for the visitor")
st = r["steering"]
print("  steering: %d moves asked, %d done" % (st["moves_asked"], st["moves_done"]))
if not st["dual_band"] or st["moves_done"] != 4: problems.append("steering summary: %s" % st)
for p in problems: print("  -", p)
sys.exit(1 if problems else 0)
EOF
agctl -j device 02:00:00:00:51:07 > "$T/visitor.json"
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if not d['presence']['home'] else 1)" "$T/visitor.json" \
	|| fail "visitor still counted as home"
echo "STEER TEST OK"
