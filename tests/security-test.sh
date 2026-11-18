#!/bin/sh
# Security checks from the v0.43 review (docs/security.md), on real kernel rules:
#
#   agdns guard   agdns listens on 0.0.0.0; aigw-nft lets the LAN, loopback and
#                 redirected DNS reach it, and drops anyone else (the WAN side of a
#                 PC as AP, which has no fw4): no open resolver. dns.guard 0: off
#   firmware      the handler never shows a URL's user:password (agd logs its
#                 message, an ACS gets it as the fault string)
#   TR-069        aigw-cwmp-run warns of unauthenticated connection requests and
#                 of an ACS over plain HTTP
#
# Three network namespaces: client -- [lan0] gateway [wan0] -- outside. A UDP
# and TCP echo stands in for agdns on the gateway's port 5300.
#
#   sudo tests/security-test.sh [build-dir]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$$
GW=agsec-gw$R CL=agsec-cl$R OUT=agsec-out$R
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	for n in $GW $CL $OUT; do ip netns del "$n" 2>/dev/null || true; done
	rm -rf "$T"
}
trap cleanup EXIT INT TERM
fail() { echo "SECURITY TEST FAIL: $*" >&2; exit 1; }
pass() { echo "    ok: $*"; }
[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft python3 uci; do command -v $t >/dev/null || fail "missing $t"; done

echo "== agdns answers the home only (aigw-nft's dns guard)"
for n in $GW $CL $OUT; do ip netns add "$n"; ip -n "$n" link set lo up; done
ip link add lan0 netns $GW type veth peer name eth0 netns $CL
ip link add wan0 netns $GW type veth peer name eth0 netns $OUT
ip -n $GW addr add 10.251.1.1/24 dev lan0; ip -n $GW link set lan0 up
ip -n $GW addr add 10.251.2.1/24 dev wan0; ip -n $GW link set wan0 up
ip -n $CL addr add 10.251.1.2/24 dev eth0; ip -n $CL link set eth0 up
ip -n $OUT addr add 10.251.2.2/24 dev eth0; ip -n $OUT link set eth0 up

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config dns 'dns'
	option listen '0.0.0.0'
	option port '5300'
	option intercept '1'
config lan 'lan'
	option iface 'lan0'
config flow 'flow'
	option sni '0'
EOF
# an echo on 0.0.0.0:5300, UDP and TCP, as agdns listens
cat > "$T/echo.py" <<'EOF'
import socket, threading
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("0.0.0.0", 5300))
t = socket.socket(); t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); t.bind(("0.0.0.0", 5300)); t.listen(8)
def tcp():
    while True:
        c, _ = t.accept(); c.sendall(c.recv(100)); c.close()
threading.Thread(target=tcp, daemon=True).start()
while True:
    d, a = u.recvfrom(100); u.sendto(d, a)
EOF
cat > "$T/ask.py" <<'EOF'
import socket, sys
host, port, proto = sys.argv[1], int(sys.argv[2]), sys.argv[3]
try:
    if proto == "udp":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1.5)
        s.sendto(b"hello", (host, port)); s.recvfrom(100)
    else:
        s = socket.create_connection((host, port), timeout=1.5); s.sendall(b"hello"); s.recv(100)
    print("answered")
except OSError:
    print("no answer")
EOF
ip netns exec $GW python3 "$T/echo.py" & PIDS="$PIDS $!"
sleep 0.5
nftapply() {
	ip netns exec $GW env AIGW_UCI_CONFDIR="$T/config" AIGW_NFT_FILE="$T/aigw.nft" sh "$REPO/scripts/aigw-nft" apply \
		2>> "$T/nft.log" || { cat "$T/nft.log"; fail "aigw-nft apply"; }
}
ask() { ip netns exec "$1" python3 "$T/ask.py" "$2" "$3" "$4"; }
nftapply
grep -q "chain dns_guard" "$T/aigw.nft" || fail "no dns guard in the rules"
for p in udp tcp; do
	[ "$(ask $CL 10.251.1.1 53 $p)" = answered ] || fail "a LAN device's DNS ($p 53, redirected) got no answer"
	[ "$(ask $CL 10.251.1.1 5300 $p)" = answered ] || fail "a LAN device asking agdns's port ($p) got no answer"
	[ "$(ask $GW 127.0.0.1 5300 $p)" = answered ] || fail "the gateway itself (loopback, $p) got no answer"
	[ "$(ask $OUT 10.251.2.1 5300 $p)" = "no answer" ] || fail "the WAN side reached agdns ($p 5300): an open resolver"
	# the WAN side's own port 53 isn't redirected (intercept is for the LAN): nothing there either
	[ "$(ask $OUT 10.251.2.1 53 $p)" = "no answer" ] || fail "the WAN side got a DNS answer on $p 53"
done
ip netns exec $GW nft list chain inet ag dns_from | grep -q "counter packets [1-9]" ||
	fail "the guard counted no dropped packets"
pass "LAN (port 53 redirected, 5300) and loopback answered; the WAN side dropped (UDP and TCP)"

uci -c "$T/config" set aigw.dns.guard=0; uci -c "$T/config" commit aigw
nftapply
grep -q "chain dns_guard" "$T/aigw.nft" && fail "dns.guard 0 still guards"
[ "$(ask $OUT 10.251.2.1 5300 udp)" = answered ] || fail "dns.guard 0: the WAN side still dropped"
pass "dns.guard 0 turns it off"
ip netns exec $GW sh "$REPO/scripts/aigw-nft" remove 2>/dev/null || true

echo "== the firmware handler keeps a URL's password out of its messages"
mkdir -p "$T/fwroot/etc" "$T/fwdir"
out=$(AIGW_FW_ROOT="$T/fwroot" AIGW_FW_DIR="$T/fwdir" AIGW_FW_KEEP=off PATH="$T/nobin:$PATH" \
	sh "$REPO/platform/openwrt/aigw/files/firmware" download 2 "http://isp-user:s3cr3t-pw@127.0.0.1:9/fw.img" 2>&1) &&
	fail "a download from a closed port succeeded"
echo "$out" | grep -q "download from http://127.0.0.1:9/fw.img failed" || fail "message: $out"
echo "$out" | grep -q "s3cr3t" && fail "the password is in the handler's message: $out"
pass "\"$(echo "$out" | tail -n 1)\""

echo "== TR-069: unauthenticated connection requests and plain HTTP are warned of"
cat > "$T/config/aigw" <<EOF
config cwmp 'cwmp'
	option url 'http://acs.example.net:7547/'
EOF
AIGW_UCI_CONFDIR="$T/config" sh "$REPO/cwmp/aigw-cwmp-run" --print > "$T/cwmp.out" 2> "$T/cwmp.err" ||
	fail "aigw-cwmp-run --print"
grep -q "connection requests are not authenticated" "$T/cwmp.err" || fail "no warning: $(cat "$T/cwmp.err")"
grep -q "plain HTTP" "$T/cwmp.err" || fail "no plain HTTP warning: $(cat "$T/cwmp.err")"
cat >> "$T/config/aigw" <<EOF
	option cr_username 'cr'
	option cr_password 'cr-secret'
	option password 'acs-secret'
EOF
uci -c "$T/config" set aigw.cwmp.url=https://acs.example.net/; uci -c "$T/config" commit aigw
AIGW_UCI_CONFDIR="$T/config" sh "$REPO/cwmp/aigw-cwmp-run" --print > "$T/cwmp.out" 2> "$T/cwmp.err" ||
	fail "aigw-cwmp-run --print"
[ ! -s "$T/cwmp.err" ] || fail "warned with credentials and HTTPS: $(cat "$T/cwmp.err")"
grep -q "secret" "$T/cwmp.out" && fail "--print shows a password: $(cat "$T/cwmp.out")"
pass "warned without credentials or over HTTP; quiet otherwise, passwords hidden"

echo "SECURITY TEST OK"
