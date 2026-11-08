#!/bin/sh
# Connection names from TLS and QUIC handshakes, end to end:
#
#   client 10.96.0.2 ── host 10.96.0.1 | 198.18.0.1 ── server 198.18.0.2 (DNS), 198.19.0.3 (TLS on 443)
#                        (agd, agdns, nft log → NFLOG)
#
# The client connects with HTTPS without asking AIGW's DNS (curl --resolve,
# like an app using DNS over HTTPS or a hard-coded address) and sends QUIC
# Initial packets. Checks: the connections are named from their handshakes,
# a machine-generated name and a DNS-over-HTTPS server are caught that way,
# a TLS connection without a name still counts as "no DNS", and a name from
# DNS stays. Needs root, ip, nft, dnsmasq, curl, openssl, python3, dig.
#
#   sudo tests/sni-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
R=$(od -An -N2 -tx1 /dev/urandom | tr -d ' \n')
NC=aigwc$R NS=aigws$R
GROUP=$((20000 + 0x$R % 20000))
PIDS=""
TW=/proc/sys/net/netfilter/nf_conntrack_tcp_timeout_time_wait
UDPT=/proc/sys/net/netfilter/nf_conntrack_udp_timeout
TW_OLD="" UDPT_OLD=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -f "$T/dns.pid" ] && kill "$(cat "$T/dns.pid")" 2>/dev/null || true
	nft delete table inet "agsni$R" 2>/dev/null || true
	ip netns del "$NC" 2>/dev/null || true
	ip netns del "$NS" 2>/dev/null || true
	if command -v iptables >/dev/null; then
		iptables -D FORWARD -i "agc$R" -j ACCEPT 2>/dev/null || true
		iptables -D FORWARD -i "ags$R" -j ACCEPT 2>/dev/null || true
	fi
	[ -n "$TW_OLD" ] && echo "$TW_OLD" > "$TW" 2>/dev/null || true
	[ -n "$UDPT_OLD" ] && echo "$UDPT_OLD" > "$UDPT" 2>/dev/null || true
	rm -rf "$T"
}
trap cleanup EXIT INT TERM
fail() { echo "SNI TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in ip nft dnsmasq curl openssl python3 dig; do command -v $t >/dev/null || fail "missing $t"; done
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
modprobe nf_conntrack_netlink 2>/dev/null || true
modprobe nfnetlink_log 2>/dev/null || true
modprobe nft_log 2>/dev/null || true

echo "== network: client and server namespaces"
ip netns add "$NC"
ip netns add "$NS"
ip link add "agc$R" type veth peer name "agc${R}p"
ip link add "ags$R" type veth peer name "ags${R}p"
ip link set "agc${R}p" netns "$NC"
ip link set "ags${R}p" netns "$NS"
ip addr add 10.96.0.1/24 dev "agc$R"; ip link set "agc$R" up
ip addr add 198.18.0.1/24 dev "ags$R"; ip link set "ags$R" up
ip -n "$NC" addr add 10.96.0.2/24 dev "agc${R}p"
ip -n "$NC" link set "agc${R}p" up; ip -n "$NC" link set lo up
ip -n "$NC" route add default via 10.96.0.1
ip -n "$NS" addr add 198.18.0.2/24 dev "ags${R}p"
ip -n "$NS" addr add 198.19.0.3/24 dev "ags${R}p"
ip -n "$NS" link set "ags${R}p" up; ip -n "$NS" link set lo up
ip -n "$NS" route add default via 198.18.0.1
ip route add 198.19.0.0/24 via 198.18.0.2
sysctl -qw net.ipv4.ip_forward=1
if command -v iptables >/dev/null; then
	iptables -I FORWARD -i "agc$R" -j ACCEPT
	iptables -I FORWARD -i "ags$R" -j ACCEPT
fi
if [ -w "$TW" ]; then TW_OLD=$(cat "$TW"); echo 2 > "$TW"; fi
if [ -w "$UDPT" ]; then UDPT_OLD=$(cat "$UDPT"); echo 3 > "$UDPT"; fi

echo "== server: DNS for shop.aigw.test, TLS on 198.18.0.2:443 and 198.19.0.3:443"
ip netns exec "$NS" dnsmasq --conf-file=/dev/null --no-resolv --no-hosts --bind-interfaces \
	--listen-address=198.18.0.2 --address=/shop.aigw.test/198.18.0.2 --pid-file="$T/dns.pid"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 1 \
	-subj "/CN=aigw-test" > /dev/null 2>&1 || fail "openssl req"
for ip in 198.18.0.2 198.19.0.3; do
	ip netns exec "$NS" openssl s_server -quiet -accept "$ip:443" -cert "$T/cert.pem" -key "$T/key.pem" -www \
		> "$T/tls-$ip.log" 2>&1 & PIDS="$PIDS $!"
done

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '10.96.0.1'
	option port '15354'
	option upstream '198.18.0.2'

config flow 'flow'
	option warmup '0'
	option poll '1'
	option sni_group '$GROUP'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'
	option state_dir '$T/plugins'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config lan 'lan'
	list iface 'agc$R'
EOF
echo "1999999999 02:00:00:00:96:02 10.96.0.2 test-client *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

echo "== kernel rules: aigw-nft's connection-name table (as agsni$R, group $GROUP)"
sh "$REPO/scripts/aigw-nft" render-sni | sed "s/inet ag_sni/inet agsni$R/" > "$T/sni.nft"
grep -q "log group $GROUP" "$T/sni.nft" || fail "aigw-nft rendered no log rule"
nft -f "$T/sni.nft" || fail "kernel refused the log rules (nft_log / nfnetlink_log missing?)"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 2
grep -q "connection names on" "$T/agd.log" || fail "SNI sensor did not start"

c() { ip netns exec "$NC" "$@"; }
echo "== HTTPS without a lookup through AIGW (curl --resolve)"
c curl -sk -o /dev/null --max-time 5 --resolve xkqjzvbwptmr.com:443:198.19.0.3 https://xkqjzvbwptmr.com/ || fail "https 1"
c curl -sk -o /dev/null --max-time 5 --resolve dns.google:443:198.19.0.3 https://dns.google/ || fail "https 2"
c curl -sk -o /dev/null --max-time 5 --resolve www.wikipedia.org:443:198.19.0.3 https://www.wikipedia.org/ || fail "https 3"
echo "== HTTPS to a bare address: no name anywhere"
c curl -sk -o /dev/null --max-time 5 https://198.19.0.3/ || fail "https 4"
echo "== HTTPS after a lookup through AIGW"
ans=$(c dig +short +time=3 +tries=2 @10.96.0.1 -p 15354 shop.aigw.test A)
[ "$ans" = 198.18.0.2 ] || fail "lookup through agdns returned '$ans'"
c curl -sk -o /dev/null --max-time 5 --resolve shop.aigw.test:443:198.18.0.2 https://shop.aigw.test/ || fail "https 5"
echo "== QUIC: a client Initial in two datagrams (ClientHello split and reordered)"
c python3 - "$REPO/tests/data" <<'EOF' || fail "quic send"
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for f in ("sni-quic1-a.bin", "sni-quic1-b.bin"):
    s.sendto(open(sys.argv[1] + "/" + f, "rb").read(), ("198.19.0.3", 443))
    time.sleep(0.05)
EOF

sleep 7   # TLS and QUIC flows close, leave TIME_WAIT / UDP timeout; the bare-address one is judged

agctl() { "$BIN/agctl/agctl" "$@"; }
agctl flows 10
agctl -j flows 30 > "$T/flows.json"
agctl -j alerts > "$T/alerts.json"
agctl -j status > "$T/status.json"
python3 - "$T/flows.json" "$T/alerts.json" "$T/status.json" <<'EOF' || fail "connection name checks"
import json, sys
flows = json.load(open(sys.argv[1]))["flows"]
alerts = json.load(open(sys.argv[2]))["alerts"]
st = json.load(open(sys.argv[3]))
problems = []
def flow(dom, proto="tcp"):
    return next((f for f in flows if f["domain"] == dom and f["proto"] == proto), None)
for dom, proto in [("xkqjzvbwptmr.com", "tcp"), ("dns.google", "tcp"), ("www.wikipedia.org", "tcp"),
                   ("video.example.net", "udp")]:
    f = flow(dom, proto)
    if not f: problems.append("%s connection to %s not named from its handshake" % (proto, dom))
    elif f["named_by"] != "sni" or f["no_dns"]: problems.append("%s: named_by %r no_dns %r" % (dom, f["named_by"], f["no_dns"]))
bare = [f for f in flows if f["dst"] == "198.19.0.3" and f["proto"] == "tcp" and not f["domain"]]
if not bare or not bare[0]["no_dns"]: problems.append("TLS without a name was not counted as no-DNS: %s" % bare)
shop = flow("shop.aigw.test")
if not shop or shop["named_by"] != "dns": problems.append("name from DNS not kept: %s" % shop)
kinds = {(a["kind"], a["subject"]) for a in alerts}
if ("dga", "xkqjzvbwptmr.com") not in kinds: problems.append("no DGA alert from a handshake name")
if ("dns-bypass", "dns.google") not in kinds: problems.append("no DNS-bypass alert from a handshake name")
if ("no-dns", "198.19.0.3") not in kinds: problems.append("no no-DNS alert for the bare address")
if any(a["subject"] in ("www.wikipedia.org", "shop.aigw.test") for a in alerts): problems.append("false alert on a normal name")
sni = st.get("sni", {})
print("  sensor: %d packets copied, %d TLS and %d QUIC names; %d connections named from handshakes"
      % (sni.get("packets", 0), sni.get("tls", 0), sni.get("quic", 0), st.get("flows_sni", 0)))
if sni.get("tls", 0) < 4 or sni.get("quic", 0) < 1: problems.append("sensor counts: %s" % sni)
for p in problems: print("  -", p)
sys.exit(1 if problems else 0)
EOF
echo "SNI TEST OK"
