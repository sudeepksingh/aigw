#!/bin/sh
# Runtime smoke test: ubusd + agd + agdns on loopback. Needs no Wi-Fi
# hardware, so it runs on any Linux box and in CI. Run as root: ubusd only
# lets root (or users listed in its ACL files) publish objects and events.
#
#   sudo tests/smoke.sh [build-dir]        (default: build)
#   sudo UPSTREAM=1.1.1.1 tests/smoke.sh   (pick the upstream resolver)
set -eu

BIN=$(cd "${1:-build}" && pwd)
T=$(mktemp -d)
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "SMOKE FAIL: $*" >&2; echo "--- logs" >&2; cat "$T"/*.log >&2 || true; exit 1; }

UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
[ -x "$UBUSD" ] || fail "ubusd not found (run scripts/build-deps.sh)"
command -v dig >/dev/null || fail "dig not found (apt install dnsutils)"

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$(cd "$(dirname "$0")/.." && pwd)/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15353'
	option upstream '${UPSTREAM:-auto}'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	# a live AIGW's plug-ins (e.g. MQTT to Home Assistant) stay out of the test
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'
EOF
echo "1999999999 aa:bb:cc:dd:ee:ff 192.0.2.10 smoke-host *" > "$T/leases"

export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & PIDS="$PIDS $!"
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 1.5

echo "== UDP query through agdns"
dig +short +time=3 +tries=2 @127.0.0.1 -p 15353 example.com A | grep -E '^[0-9.]+$' || fail "no UDP answer"
echo "== TCP query through agdns"
dig +short +tcp +time=3 +tries=2 @127.0.0.1 -p 15353 example.org A | grep -E '^[0-9.]+$' || fail "no TCP answer"
echo "== NXDOMAIN passes through"
dig +time=3 +tries=2 @127.0.0.1 -p 15353 does-not-exist.invalid A | grep -q NXDOMAIN || fail "no NXDOMAIN"
echo "== HTTPS record lookup"
dig +time=3 +tries=2 @127.0.0.1 -p 15353 cloudflare.com HTTPS > /dev/null || fail "HTTPS lookup failed"
echo "== TCP query is reported to the brain"
dig +short +tcp +time=3 +tries=2 @127.0.0.1 -p 15353 example.net A | grep -E '^[0-9.]+$' || fail "no TCP answer"

sleep 0.5
echo "== agctl status"
"$BIN/agctl/agctl" status || fail "agctl status"
echo "== agctl dns"
"$BIN/agctl/agctl" dns 10 | tee "$T/dns.out"
grep -q example.com "$T/dns.out" || fail "query missing from the brain's DNS log"
echo "== agctl devices"
"$BIN/agctl/agctl" devices | tee "$T/dev.out"
grep -q smoke-host "$T/dev.out" || fail "lease hostname missing from device list"
grep -q '^127\.0\.0\.1 ' "$T/dev.out" && fail "the gateway's own address is listed as a device"
grep -q 'HTTPS record' "$T/dns.out" || fail "HTTPS lookup not shown as a record"
grep -q 'example.net' "$T/dns.out" || fail "DNS-over-TCP query missing from the brain's DNS log"

echo "SMOKE OK"
