#!/bin/sh
# Lookup burst test: behaviour that gives malware away even when its generated
# names look like words.
#
#   infected 10.96.0.2 ─┐
#   browser  10.96.0.3 ─┤ bridge ── host (agd, agdns) ── 198.18.1.2 fake DNS
#   retrier  10.96.0.4 ─┤          10.96.0.1
#   filtered 10.96.0.5 ─┘
#
# The fake DNS answers names on its list, 0.0.0.0 for "ad" names (as filtering
# resolvers do), and NXDOMAIN for everything else.
#
# Checks:
#   - the home learns the sites that work (and keeps them over an agd restart)
#   - a device trying 25 never-used names that don't exist (pronounceable and
#     dictionary-word names, which the name model mostly passes) raises
#     dga-burst, a quarantine is proposed, and the one name that answered is
#     named as the likely control server
#   - no alert for: a browser visiting 40 new sites and making a few typos,
#     a device retrying one dead name, names answered 0.0.0.0, names AIGW blocked
#   - names that don't exist are not learned as sites the device uses
# Needs root, ip, python3, dig.
#
#   sudo tests/burst-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NS=aigus$R BR=agb$R
PIDS=""
CLIENTS="2 3 4 5"

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for c in $CLIENTS; do ip netns del "aigu$c$R" 2>/dev/null || true; done
	ip netns del "$NS" 2>/dev/null || true
	ip link del "$BR" 2>/dev/null || true
	ip link del "aus$R" 2>/dev/null || true
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "BURST TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip python3 dig; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

echo "== network"
ip link add "$BR" type bridge
ip addr add 10.96.0.1/24 dev "$BR"; ip link set "$BR" up
for c in $CLIENTS; do
	n=aigu$c$R
	ip netns add "$n"
	ip link add "au$c$R" type veth peer name eth0 netns "$n"
	ip link set "au$c$R" master "$BR" up
	ip -n "$n" link set eth0 address "02:00:00:00:96:0$c"
	ip -n "$n" addr add "10.96.0.$c/24" dev eth0
	ip -n "$n" link set eth0 up; ip -n "$n" link set lo up
done
ip netns add "$NS"
ip link add "aus$R" type veth peer name eth0 netns "$NS"
ip addr add 198.18.1.1/24 dev "aus$R"; ip link set "aus$R" up
ip -n "$NS" addr add 198.18.1.2/24 dev eth0
ip -n "$NS" link set eth0 up; ip -n "$NS" link set lo up
ip -n "$NS" route add default via 198.18.1.1

echo "== names"
python3 - "$REPO" "$T" <<'EOF'
import random, sys
sys.path.insert(0, sys.argv[1] + "/cortex/training/domain_risk")
from dga import syllables, TLDS
r = random.Random(17)
WORDS = ("river stone portal garden silver market cloud summer winter bright north harbor maple "
         "golden forest ocean metro spring valley crystal falcon tiger lunar solar rapid smart "
         "green bridge castle meadow copper amber velvet orbit pixel prime urban island").split()
def dictionary():
    return r.choice(WORDS) + r.choice(WORDS) + ("" if r.random() < 0.6 else r.choice(WORDS))
def dga(i):
    lab = syllables(r) if i % 2 else dictionary()
    return lab + "." + r.choice(TLDS)
infected = []
while len(infected) < 25:
    n = dga(len(infected))
    if n not in infected:
        infected.append(n)
hit = infected[13]                         # the one its author registered
popular = ["google.com", "wikipedia.org", "youtube.com", "bbc.co.uk", "amazon.com", "netflix.com",
           "github.com", "apple.com", "microsoft.com", "whatsapp.net", "instagram.com", "zoom.us",
           "spotify.com", "reddit.com", "irctc.co.in", "flipkart.com", "hotstar.com", "paytm.com",
           "linkedin.com", "yahoo.com"]
browse = []
while len(browse) < 40:                   # new sites that work, some word-like too
    n = (dictionary() if len(browse) % 2 else r.choice(WORDS) + "-" + r.choice(WORDS)) + "." + r.choice(["com", "in", "org", "co.uk", "net"])
    if n not in browse and n not in infected:
        browse.append(n)
typos = ["gooogle.com", "wikipeda.org", "youtub.com", "amazn.com"]
ads = ["%s%s-ads%d.com" % (r.choice(WORDS), r.choice(WORDS), i) for i in range(20)]
blocked = ["%s%s.biz" % (r.choice(WORDS), r.choice(WORDS)) for _ in range(12)]
d = sys.argv[2]
open(d + "/resolve", "w").write("\n".join(popular + browse + [hit]) + "\n")
open(d + "/zero", "w").write("\n".join(ads) + "\n")
for name, lst in (("popular", popular), ("infected", infected), ("browse", browse), ("typos", typos),
                  ("ads", ads), ("blocked", blocked)):
    open(d + "/" + name, "w").write("\n".join(lst) + "\n")
open(d + "/hit", "w").write(hit + "\n")
EOF

cat > "$T/dns.py" <<'EOF'
import socket, struct, sys
d = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("198.18.1.2", 53))
def names(f):
    return {l.strip().lower() for l in open(d + "/" + f) if l.strip()}
while True:
    q, a = s.recvfrom(1500)
    if len(q) < 12:
        continue
    i, labels = 12, []
    while q[i]:
        labels.append(q[i + 1:i + 1 + q[i]].decode("ascii", "replace"))
        i += 1 + q[i]
    qend = i + 5
    name, qtype = ".".join(labels).lower(), struct.unpack(">H", q[i + 1:i + 3])[0]
    ip = None
    if name in names("resolve"):
        ip = "198.18.1.2"
    elif name in names("zero"):
        ip = "0.0.0.0"
    rcode = 0 if ip else 3
    an = 1 if ip and qtype == 1 else 0
    hdr = q[:2] + struct.pack(">HHHHH", 0x8180 | rcode, 1, an, 0, 0)
    body = q[12:qend]
    if an:
        body += b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(ip)
    s.sendto(hdr + body, a)
EOF
ip netns exec "$NS" python3 "$T/dns.py" "$T" > "$T/dns.log" 2>&1 & PIDS="$PIDS $!"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '10.96.0.1'
	option port '15355'
	option upstream '198.18.1.2'
	option cache '0'

config flow 'flow'
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

# only this policy, so the quarantine proposed is the burst's (not the name model's)
config policy 'dga_burst'
	option alert 'dga-burst'
	option action 'quarantine'

config lan 'lan'
	list iface '$BR'
EOF
cat > "$T/leases" <<EOF
1999999999 02:00:00:00:96:02 10.96.0.2 infected-laptop *
1999999999 02:00:00:00:96:03 10.96.0.3 browser-phone *
1999999999 02:00:00:00:96:04 10.96.0.4 retry-tv *
1999999999 02:00:00:00:96:05 10.96.0.5 filtered-tablet *
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 2

agctl() { "$BIN/agctl/agctl" "$@"; }
lookup() { # lookup <client number> <file>: each name once, in order
	while read -r q; do
		ip netns exec "aigu$1$R" dig +short +time=2 +tries=1 @10.96.0.1 -p 15355 "$q" A > /dev/null || true
	done < "$T/$2"
}
bursts() { # bursts <ip>: dga-burst alerts for that device, as "subject|reason" lines
	agctl -j alerts 200 | python3 -c "
import json, sys
for a in json.load(sys.stdin)['alerts']:
    if a['kind'] == 'dga-burst' and a['ip'] == '$1':
        print(a['subject'] + '|' + a['reason'])"
}

echo "== the home uses its usual sites"
lookup 3 popular
sleep 0.5
n=$(agctl -j status | python3 -c "import json,sys; print(json.load(sys.stdin)['bursts']['home_sites'])")
[ "$n" -ge 20 ] || fail "the home should have learned 20 sites, has $n"
echo "   the home has used $n sites"

echo "== a browser visits 40 new sites, makes 4 typos"
lookup 3 browse
lookup 3 typos
sleep 0.5
[ -z "$(bursts 10.96.0.3)" ] || fail "false burst for a browser: $(bursts 10.96.0.3)"

echo "== a TV retries one dead name 30 times"
for i in $(seq 30); do echo "update.deadvendor-cloud.com"; done > "$T/retry"
echo "time.deadvendor-ntp.com" >> "$T/retry"
lookup 4 retry
sleep 0.5
[ -z "$(bursts 10.96.0.4)" ] || fail "false burst for retries of one name"

echo "== a tablet behind a filtering resolver (0.0.0.0), and names AIGW blocks"
lookup 5 ads
while read -r q; do agctl block "$q" 1h > /dev/null || fail "agctl block $q"; done < "$T/blocked"
sleep 2
lookup 5 blocked
sleep 0.5
[ -z "$(bursts 10.96.0.5)" ] || fail "false burst for filtered or blocked names: $(bursts 10.96.0.5)"

echo "== an infected laptop tries 25 generated names; one answers"
lookup 2 infected
sleep 1
HIT=$(cat "$T/hit")
bursts 10.96.0.2 | sed 's/^/   /'
bursts 10.96.0.2 | grep -q '^failed lookups|' || fail "no dga-burst alert for the infected laptop"
bursts 10.96.0.2 | grep -q "^$HIT|" || fail "the name that answered ($HIT) should be named"
agctl -j decisions > "$T/decisions.json"
python3 - "$T/decisions.json" <<'EOF' || { cat "$T/decisions.json" >&2; fail "no quarantine proposed for the infected laptop"; }
import json, sys
d = json.load(open(sys.argv[1]))["decisions"]
sys.exit(0 if any(x["action"] == "quarantine" and "names in" in x["reason"] and x["state"] == "proposed"
                  for x in d) else 1)
EOF
model=$(agctl -j alerts 200 | python3 -c "
import json, sys
names = {l.strip() for l in open('$T/infected')}
print(len({a['subject'] for a in json.load(sys.stdin)['alerts'] if a['kind'] == 'dga' and a['subject'] in names}))")
echo "   name model alone: $model of 25 names flagged; behaviour: the device flagged, its control server named"
known=$(agctl -j devices | python3 -c "
import json, sys
print([d['baseline']['known_sites'] for d in json.load(sys.stdin)['devices'] if d.get('ip') == '10.96.0.2'][0])")
[ "$known" -le 1 ] || fail "names that don't exist were learned as the laptop's sites ($known known)"

echo "== the home's memory survives an agd restart"
before=$(agctl -j status | python3 -c "import json,sys; print(json.load(sys.stdin)['bursts']['home_sites'])")
kill "$AGD"; wait "$AGD" 2>/dev/null || true
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 2
after=$(agctl -j status | python3 -c "import json,sys; print(json.load(sys.stdin)['bursts']['home_sites'])")
[ "$after" = "$before" ] || fail "home memory not restored ($before before, $after after)"
lookup 3 browse                          # all known now: still no alert, nothing new learned
sleep 0.5
now=$(agctl -j status | python3 -c "import json,sys; print(json.load(sys.stdin)['bursts']['home_sites'])")
[ "$now" = "$after" ] || fail "known sites were learned again ($after -> $now)"
[ -z "$(bursts 10.96.0.3)" ] || fail "false burst after the restart"
echo "   $after sites before and after"
agctl status | grep -E 'lookup bursts' || fail "agctl status should show lookup bursts"

echo "BURST TEST OK"
