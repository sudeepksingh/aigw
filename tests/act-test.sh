#!/bin/sh
# Phase 3 "act" integration test: decisions, guardrails and all three
# actuators against real kernel rules. The gateway runs in its own network
# namespace, so its nftables table and conntrack never touch the host's
# (safe to run next to a live AIGW):
#
#   client 10.97.0.2 ── gateway 10.97.0.1 | 198.18.0.1 ── server 198.18.0.2
#    (namespace)         (namespace: agd, agdns, nft)       (namespace: DNS + web)
#
# Checks: an alert becomes a proposal and nothing changes until it is
# approved; quarantine cuts the device off but DNS to the gateway still
# works; undo; time limits expire on their own; block answers NXDOMAIN for a
# domain and its subdomains over UDP and TCP; priority marks DSCP CS5;
# guardrails refuse the gateway, protected devices and bad names; actions
# survive an agd restart and an nft reload; enforce mode acts by itself
# within its hourly limit. Needs root, ip, nft, uci, dnsmasq, curl, python3, dig.
#
#   sudo tests/act-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NG=aigwg$R NC=aigwc$R NS=aigws$R
GIF=agg$R
PIDS=""
CMAC=02:00:00:00:97:02

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -f "$T/dns.pid" ] && kill "$(cat "$T/dns.pid")" 2>/dev/null || true
	for n in "$NC" "$NS" "$NG"; do ip netns del "$n" 2>/dev/null || true; done
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() { echo "ACT TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft uci dnsmasq curl python3 dig; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

gw() { ip netns exec "$NG" "$@"; }
cl() { ip netns exec "$NC" "$@"; }
agctl() { "$BIN/agctl/agctl" "$@"; }
reach() { cl curl -s -o /dev/null --connect-timeout 2 --max-time 5 http://198.18.0.2:8080/file; }
lookup() { cl dig +time=2 +tries=1 @10.97.0.1 -p 15353 "$@"; }
status_of() { lookup "$@" | sed -n 's/.*status: \([A-Z]*\).*/\1/p'; }
qset() { gw nft list set inet ag quarantine; }

# decision field check: dcheck <python expression over list d (live first)>
dcheck() {
	agctl -j decisions 50 > "$T/dec.json"
	python3 - "$T/dec.json" "$1" <<'EOF' || { cat "$T/dec.json" >&2; fail "decision check: $2"; }
import json, sys
d = json.load(open(sys.argv[1]))["decisions"]
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
# id of the newest decision matching action/state
did() {
	agctl -j decisions 50 | python3 -c "
import json, sys
d = [x for x in json.load(sys.stdin)['decisions'] if x['action'] == '$1' and x['state'] == '$2']
print(max(x['id'] for x in d) if d else '')"
}

echo "== network: client, gateway and server namespaces"
for n in "$NG" "$NC" "$NS"; do ip netns add "$n"; ip -n "$n" link set lo up; done
ip link add "$GIF" netns "$NG" type veth peer name c0 netns "$NC"
ip link add ags$R netns "$NG" type veth peer name s0 netns "$NS"
ip -n "$NC" link set c0 address $CMAC
ip -n "$NG" addr add 10.97.0.1/24 dev "$GIF"; ip -n "$NG" link set "$GIF" up
ip -n "$NG" addr add 198.18.0.1/24 dev ags$R; ip -n "$NG" link set ags$R up
ip -n "$NC" addr add 10.97.0.2/24 dev c0; ip -n "$NC" link set c0 up
ip -n "$NC" route add default via 10.97.0.1
ip -n "$NS" addr add 198.18.0.2/24 dev s0; ip -n "$NS" link set s0 up
ip -n "$NS" route add default via 198.18.0.1
gw sysctl -qw net.ipv4.ip_forward=1

echo "== server: DNS and a web server; it counts packets arriving with DSCP CS5"
ip netns exec "$NS" dnsmasq --conf-file=/dev/null --no-resolv --no-hosts --bind-interfaces \
	--listen-address=198.18.0.2 --address=/shop.aigw.test/198.18.0.2 \
	--address=/other.aigw.test/198.18.0.2 --address=/xkqjzvbwptmr.com/198.18.0.4 \
	--address=/qwxzkvjrtbpl.com/198.18.0.4 --address=/zvbxqkrtwplm.net/198.18.0.4 \
	--pid-file="$T/dns.pid"
mkdir -p "$T/www"
head -c 100000 /dev/urandom > "$T/www/file"
ip netns exec "$NS" python3 -m http.server 8080 --bind 0.0.0.0 --directory "$T/www" \
	> "$T/web.log" 2>&1 & PIDS="$PIDS $!"
ip netns exec "$NS" nft -f - <<'EOF'
table ip dscp_probe {
	chain input {
		type filter hook input priority 0; policy accept;
		tcp dport 8080 ip dscp cs5 counter
	}
}
EOF
cs5() { ip netns exec "$NS" nft list chain ip dscp_probe input | sed -n 's/.*packets \([0-9]*\).*/\1/p'; }

write_config() {   # $1 = act mode, $2 = max automatic actions per hour
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '10.97.0.1'
	option port '15353'
	option upstream '198.18.0.2'

config flow 'flow'
	option warmup '0'
	option poll '1'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'

config act 'act'
	option mode '$1'
	option max_auto_per_hour '$2'
	option dns_policy '$T/dns-policy'
	list protect 'protected-box'

config policy 'dga'
	option alert 'dga'
	option action 'quarantine'
	option ttl '600'

config lan 'lan'
	list iface '$GIF'
EOF
}
write_config advise 3
cat > "$T/leases" <<EOF
1999999999 $CMAC 10.97.0.2 test-client *
1999999999 02:00:00:00:97:09 10.97.0.9 protected-box *
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT_FILE="$T/aigw.nft"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
gw "$REPO/scripts/aigw-nft" apply || fail "aigw-nft apply"
# "ip netns exec" execs the program, so $! is the daemon itself (not a subshell)
start_agd() { ip netns exec "$NG" "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 2; }
stop_agd() { kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done; }
start_agd
ip netns exec "$NG" "$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 1
agctl status | grep -q "advise mode" || fail "agd not in advise mode"

echo "== before anything: the client reaches the server"
reach || fail "client cannot reach the server"

echo "== guardrails"
agctl quarantine 10.97.0.1 2>&1 | tee "$T/g1" ; grep -q "gateway" "$T/g1" || fail "gateway not refused"
agctl quarantine protected-box 2>&1 | tee "$T/g2"; grep -q "protected" "$T/g2" || fail "protected device not refused"
agctl block com 2>&1 | tee "$T/g3"; grep -q "not a domain" "$T/g3" || fail "bare TLD not refused"
agctl quarantine nosuch-device 2>&1 | tee "$T/g4"; grep -q "no device" "$T/g4" || fail "unknown device not refused"
if agctl quarantine nosuch-device >/dev/null 2>&1; then fail "a refused request exited 0"; fi
dcheck "len(d) == 0" "refused requests must not create decisions"

echo "== advise: an alert becomes a proposal, and nothing changes yet"
lookup +short xkqjzvbwptmr.com > /dev/null
sleep 1
agctl decisions
dcheck "any(x['state'] == 'proposed' and x['action'] == 'quarantine' and x['target'] == '$CMAC' and x['alert'] > 0 and x['origin'] == 'policy dga' for x in d)" "no quarantine proposal for the DGA alert"
qset | grep -q "$CMAC" && fail "advise mode changed the kernel rules"
reach || fail "advise mode cut the device off"

echo "== approve: the device is quarantined, but DNS to the gateway still works"
ID=$(did quarantine proposed)
agctl approve "$ID" || fail "approve"
qset | grep -q "$CMAC" || fail "MAC not in the quarantine set"
qset | grep -q "expires" || fail "quarantine element has no timeout"
if reach; then fail "quarantined device still reaches the server"; fi
[ "$(status_of shop.aigw.test)" = NOERROR ] || fail "DNS to the gateway broken by quarantine"
dcheck "any(x['id'] == $ID and x['state'] == 'active' and 'approved by you' in x['origin'] for x in d)" "approved decision not active"

echo "== undo"
agctl undo "$ID" || fail "undo"
qset | grep -q "$CMAC" && fail "MAC still quarantined after undo"
reach || fail "device not reachable after undo"
dcheck "any(x['id'] == $ID and x['state'] == 'reverted' for x in d)" "decision not reverted"
if agctl undo "$ID" >/dev/null 2>&1; then fail "undo of a finished decision exited 0"; fi

echo "== block a domain: NXDOMAIN for it and its subdomains, UDP and TCP"
agctl block Shop.AIGW.test. 2m || fail "block"
sleep 2   # agdns notices the new block list
[ "$(status_of shop.aigw.test)" = NXDOMAIN ] || fail "blocked name resolved (UDP)"
[ "$(status_of www.shop.aigw.test)" = NXDOMAIN ] || fail "subdomain of a blocked name resolved"
[ "$(status_of +tcp shop.aigw.test)" = NXDOMAIN ] || fail "blocked name resolved over TCP"
[ "$(status_of other.aigw.test)" = NOERROR ] || fail "unblocked name affected"
agctl -j dns 5 | grep -q '"rcode": 3' || fail "blocked lookup not in the DNS log"
BID=$(did block active)
agctl undo "$BID" || fail "undo block"
sleep 2
[ "$(status_of shop.aigw.test)" = NOERROR ] || fail "name still blocked after undo"

echo "== prioritize: the device's traffic is marked DSCP CS5"
before=$(cs5)
agctl prioritize test-client 1m || fail "prioritize"
gw nft list set inet ag priority4 | grep -q 10.97.0.2 || fail "IP not in the priority set"
reach || fail "prioritized device lost connectivity"
[ "$(cs5)" -gt "$before" ] || fail "no DSCP CS5 packets arrived"
PID_=$(did priority active)
agctl undo "$PID_" || fail "undo priority"
before=$(cs5); reach; [ "$(cs5)" = "$before" ] || fail "DSCP still set after undo"

echo "== time limit: a 10 s quarantine ends by itself"
agctl quarantine test-client 10s || fail "short quarantine"
if reach; then fail "short quarantine not applied"; fi
sleep 13
reach || fail "quarantine did not expire"
dcheck "any(x['action'] == 'quarantine' and x['state'] == 'expired' for x in d)" "decision not marked expired"

echo "== an action survives an agd restart and an nft reload"
agctl quarantine "$CMAC" 5m || fail "quarantine by MAC"
RID=$(did quarantine active)
stop_agd
gw "$REPO/scripts/aigw-nft" apply           # reloading the table empties its sets
qset | grep -q "$CMAC" && fail "nft reload did not empty the set (test assumption)"
start_agd
qset | grep -q "$CMAC" || fail "active quarantine not restored after restart"
dcheck "any(x['id'] == $RID and x['state'] == 'active' for x in d)" "decision not active after restart"
agctl undo "$RID" || fail "undo after restart"

echo "== enforce: a policy acts by itself, at most max_auto_per_hour times"
write_config enforce 1
stop_agd; start_agd
agctl status | grep -q "enforce mode" || fail "agd not in enforce mode"
lookup +short qwxzkvjrtbpl.com > /dev/null; sleep 1
dcheck "any(x['state'] == 'active' and x['automatic'] and x['target'] == '$CMAC' for x in d)" "enforce mode did not act"
if reach; then fail "automatic quarantine not applied"; fi
agctl undo "$(did quarantine active)" || fail "undo automatic"
lookup +short zvbxqkrtwplm.net > /dev/null; sleep 1
dcheck "any(x['state'] == 'proposed' and 'waiting for you' in x['note'] for x in d)" "hourly limit did not hold back the second automatic action"
reach || fail "second automatic action was applied despite the hourly limit"

agctl decisions 5
echo "ACT TEST OK"
