#!/bin/sh
# Model back-end test: who computes the domain-risk scores. Runs agd with a
# private ubusd and a back-end directory holding the example libraries from
# plugins/examples/backend (safe next to a live AIGW).
#
# Checks: the built-in back-ends (interp, flat) pass their check and one is
# used; a loaded library that agrees with the reference (example-npu, float
# arithmetic) passes and can be chosen; one that disagrees (example-broken)
# is refused with the probe it got wrong, and asking for it falls back to
# the reference; a library writable by others, or in a directory writable by
# others, is not loaded; verdicts are the same whoever computes them.
# Needs root (the libraries must belong to root).
#
#   sudo tests/backend-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "BACKEND TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
[ -f "$BIN/tests/example_backend.so" ] || fail "build the example back-ends first (cmake --build)"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
agctl() { "$BIN/agctl/agctl" "$@"; }
# bcheck <python expression over b (back-ends by name) and j (the model list)> <message>
bcheck() {
	agctl -j models > "$T/m.json" 2>&1 || true
	python3 - "$T/m.json" "$1" <<'EOF' || { cat "$T/m.json" >&2; fail "$2"; }
import json, sys
j = json.load(open(sys.argv[1]))
b = {x["name"]: x for x in j.get("backends", [])}
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
verdicts() { for n in google.com xkqjzvbwptmr.com bydgoszcz.pl wikipedia.org a8f3k2l9q0zx.info; do
	agctl -j score $n | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['name'], j['verdict'], round(j['score'], 3))"; done; }

mkdir -p "$T/backends" "$T/config"
install -m 0644 -o root "$BIN/tests/example_backend.so" "$BIN/tests/example_backend_broken.so" "$T/backends/"
write_config() {
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config lan 'lan'
	option iface 'aigw-none0'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config plugins 'plugins'
	option enabled '0'

config act 'act'
	option dns_policy '$T/dns-policy'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'
	option backend '$1'
	option backend_dir '$T/backends'
EOF
}
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done; }

echo "== auto: every back-end is checked; one that agrees is used"
write_config auto
start_agd
bcheck "b['interp']['ok'] and b['flat']['ok'] and b['flat']['ns_per_score'] > 0" "the built-in back-ends should pass"
bcheck "b['example-npu']['ok'] and b['example-npu']['source'].endswith('example_backend.so')" \
	"the example library (float arithmetic, same verdicts) should pass"
bcheck "not b['example-broken']['ok'] and 'disagrees with the reference' in b['example-broken']['why']" \
	"the broken library should be refused, saying where it disagreed"
bcheck "sum(x['in_use'] for x in b.values()) == 1 and [x for x in b.values() if x['in_use']][0]['ok']" \
	"exactly one back-end that passed should be in use"
verdicts > "$T/v.auto"
agctl models | grep -q "IN USE" || fail "agctl models should say which back-end is in use"

for want in interp flat example-npu; do
	echo "== models.backend $want"
	stop_agd; write_config "$want"; start_agd
	bcheck "b['$want']['in_use']" "$want should be in use"
	verdicts > "$T/v.$want"
	diff -q "$T/v.auto" "$T/v.$want" > /dev/null || { diff "$T/v.auto" "$T/v.$want"; fail "$want gives other verdicts"; }
done

echo "== asking for the broken one falls back to the reference"
stop_agd; write_config example-broken; start_agd
bcheck "b['interp']['in_use'] and not b['example-broken']['in_use']" "the reference should be used instead"
grep -q "example-broken not usable" "$T/agd.log" || fail "agd should say why"

echo "== libraries others could change are not loaded"
stop_agd
chmod 0666 "$T/backends/example_backend.so"
write_config auto; start_agd
bcheck "'example-npu' not in b and 'example_backend.so' in j.get('backend_notes', '')" \
	"a library writable by others should be skipped, with a note"
stop_agd
chmod 0644 "$T/backends/example_backend.so"
chmod 0777 "$T/backends"
start_agd
bcheck "set(b) == {'interp', 'flat'} and 'writable by others' in j.get('backend_notes', '')" \
	"a directory writable by others should load nothing"

echo "BACKEND TEST OK"
