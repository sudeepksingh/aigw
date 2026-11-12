#!/bin/sh
# prplOS test: AIGW OS's data model as an Ambiorix component (aigw-amx in
# amxrt), on a private ubus with agd, read and driven the way prplOS's own
# USP agent, TR-069 client and CLI reach components (amxb_ubus).
#
#   - the objects come from agd's schema, under its vendor prefix
#   - reads are agd's values, typed (booleans, numbers, times)
#   - instances follow agd: a new alert appears as X_AIGW_.Alert.N
#   - an alert reaches subscribers as the signal "Alert!", without the name
#   - writes: an invalid value and, with mgmt.actions 0, a mode change refused;
#     with mgmt.actions 1 the mode set and saved
#   - commands: Block() returns its decision, which appears; Undo() on it
# Needs root, python3, Ambiorix (scripts/build-ambiorix.sh): AIGW_AMX_ROOT;
# skipped without it, AIGW_REQUIRE_AMX=1 makes that a failure.
#
#   sudo AIGW_AMX_ROOT=~/.cache/aigw-amx/root tests/amx-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
AMX=${AIGW_AMX_ROOT:-}
T=$(mktemp -d)
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "AMX TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }
skip() {
	[ "${AIGW_REQUIRE_AMX:-0}" = 1 ] && fail "$*"
	echo "AMX TEST SKIPPED: $*"; exit 0
}

[ "$(id -u)" = 0 ] || fail "run as root"
[ -n "$AMX" ] && [ -x "$AMX/bin/amxrt" ] || skip "no Ambiorix (AIGW_AMX_ROOT: scripts/build-ambiorix.sh)"
[ -f "$BIN/amx/aigw-amx.so" ] || skip "aigw-amx not built (cmake -DAIGW_AMX_ROOT=...)"
MOD=$(ls "$AMX"/usr/bin/mods/amxb/*mod-amxb-ubus.so 2>/dev/null | head -1)
[ -n "$MOD" ] || fail "no amxb_ubus module in $AMX"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

write_config() { # write_config <actions>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config flow 'flow'
	option enabled '0'

config meter 'meter'
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

config mgmt 'mgmt'
	option enabled '1'
	option actions '$1'
	option alert_details '0'

config lan 'lan'
	list iface 'lo'
EOF
}
write_config 0
echo "1999999999 02:00:00:00:98:02 10.96.0.12 phone *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
# as prplOS runs a component: amxrt with the ubus back-end and the ODL
LD_LIBRARY_PATH="$AMX/lib" "$AMX/bin/amxrt" -A -N -B "$MOD" -u "ubus:$AIGW_UBUS" "$BIN/amx/aigw-amx-build.odl" \
	> "$T/amxrt.log" 2>&1 & PIDS="$PIDS $!"

ub() { "$UBUS" -s "$AIGW_UBUS" "$@"; }
# the amxd status of a call (amxb_ubus sends it last, as "amxd-error-code"; ubus may still exit 0)
amxerr() { ub call "$@" 2>/dev/null | sed -n 's/.*"amxd-error-code": *\([0-9]*\).*/\1/p' | tail -1; }
# amxb_ubus answers in several messages: ubus prints one JSON object each; the first is the data
js() { python3 -c "import json,sys; d=json.JSONDecoder().raw_decode(sys.stdin.read().lstrip())[0]; $1"; }

echo "== the component on the bus, from agd's schema"
i=0
until ub list X_AIGW_ >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -le 40 ] || fail "X_AIGW_ not published"; sleep 0.25
done
ub -v list X_AIGW_ | grep -q '"Quarantine"' || fail "the commands should be methods"
ub -v list X_AIGW_.Decision | grep -q '"Approve"' || fail "the decisions' commands"
grep -q "X_AIGW_ with [0-9]* parameters, 5 commands, [0-9]* events" "$T/amxrt.log" || fail "what was published: $(cat "$T/amxrt.log")"
echo "   $(grep -o 'X_AIGW_ with.*' "$T/amxrt.log")"

echo "== reads are agd's values, typed"
ub call X_AIGW_ _get '{"rel_path":""}' > "$T/get.json"
js "v = d['X_AIGW_.']
sys.exit(0 if v['Mode'] == 'advise' and v['ActionsAllowed'] is False and v['AlertNumberOfEntries'] == 0 else 'got %s' % v)" < "$T/get.json" ||
	fail "X_AIGW_.: $(cat "$T/get.json")"

echo "== an alert: a new instance, and the signal Alert! (without the name)"
ub subscribe X_AIGW_ > "$T/signals.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
ub send ag.dns '{"client":"10.96.0.12","qname":"xkqjzvbwptmr.com","rcode":3}'
i=0
until ub list X_AIGW_.Alert.1 >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -le 40 ] || fail "no X_AIGW_.Alert.1"; sleep 0.25
done
ub call X_AIGW_.Alert _get '{"rel_path":"1."}' > "$T/alert.json"
js "v = d['X_AIGW_.Alert.1.']
sys.exit(0 if v['Kind'] == 'dga' and v['Subject'] == '' and v['Count'] == 1 else 'got %s' % v)" < "$T/alert.json" ||
	fail "the alert: $(cat "$T/alert.json")"
i=0
until grep -q 'Alert!' "$T/signals.log"; do
	i=$((i + 1)); [ $i -le 20 ] || fail "no Alert! signal: $(cat "$T/signals.log")"; sleep 0.25
done
grep -q xkqjzvbwptmr "$T/signals.log" && fail "the signal must not carry the name"
echo "   X_AIGW_.Alert.1: dga; Alert! signalled"

echo "== writes and commands, refused with mgmt.actions 0"
[ "$(amxerr X_AIGW_ _set '{"rel_path":"","parameters":{"Mode":"sometimes"}}')" = 10 ] || fail "an invalid mode: invalid value (10)"
[ "$(amxerr X_AIGW_ _set '{"rel_path":"","parameters":{"Mode":"enforce"}}')" = 23 ] ||
	fail "a mode change with mgmt.actions 0: permission denied (23)"
[ "$(amxerr X_AIGW_ Block '{"Domain":"bad-tracker.com"}')" = 23 ] || fail "Block() with mgmt.actions 0: permission denied (23)"
ub call X_AIGW_ _get '{"rel_path":"Mode"}' | grep -q '"advise"' || fail "the mode must not have changed"

echo "== mgmt.actions 1 (agd restarted): the mode set, a block and its undo"
kill "$AGD"; wait "$AGD" 2>/dev/null || true
write_config 1
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
[ "$(amxerr X_AIGW_ _set '{"rel_path":"","parameters":{"Mode":"enforce"}}')" = 0 ] || fail "setting the mode"
grep -q "option mode 'enforce'" "$T/config/aigw" || fail "the mode should be saved"
ub call X_AIGW_ Block '{"Domain":"bad-tracker.com","Duration":"600"}' > "$T/block.json" || fail "Block()"
DEC=$(js "print(d.get('Decision') or d.get('retval', {}).get('Decision', ''))" < "$T/block.json")
case $DEC in Device.X_AIGW_.Decision.*) ;; *) fail "Block() should return its decision: $(cat "$T/block.json")" ;; esac
N=${DEC##*.}
i=0
until ub list "X_AIGW_.Decision.$N" >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -le 40 ] || fail "no X_AIGW_.Decision.$N"; sleep 0.25
done
ub call "X_AIGW_.Decision.$N" Undo '{}' > "$T/undo.json" || fail "Undo()"
ub call X_AIGW_.Decision _get "{\"rel_path\":\"$N.State\"}" | grep -q '"reverted"' || fail "the decision should be reverted"
grep -q "by prplos" "$T/agd.log" || fail "agd should record who asked (prplos)"
echo "   mode enforce (saved); $DEC: blocked, undone"

echo "AMX TEST OK"
