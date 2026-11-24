#!/bin/sh
# The demo image's simulated home (demo/aigw-demo.c, docs/try-it.md), with the
# real agd: what the seeder writes is what agd keeps, and agd explains it.
#
#   the gate      outside QEMU, aigw-demo refuses to seed (and says why); it
#                 never replaces a state file without --replace
#   the home      nine devices, two weeks of proactive care, the upgrade kept
#   the evening   agctl why: yesterday 20:00-20:40 the line was full (LINE_FULL),
#                 AIGW turned on smart queueing; agctl brief: not a fault
#   what AIGW did smart queueing kept (the trial's verdict from AIGW's own rules),
#                 put back on the WAN through the handler; the last call smooth on
#                 the full line, first in the queue (the data model too)
#   the label     with the demo mark: ag status says demo, agctl prints DEMO;
#                 without it, nothing does
#
# Needs root (a private ubusd) and python3.
#
#   sudo tests/demo-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
# the demo home is built with every feature only
[ -x "$BIN/demo/aigw-demo" ] || { echo "DEMO TEST SKIP: the demo is built with every feature only"; exit 0; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "DEMO TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/agd.log >&2 2>/dev/null || true; exit 1; }
pass() { echo "    ok: $*"; }
[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
DEMO="$BIN/demo/aigw-demo"
V=$(sed -n 's/^project(aigw VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")

echo "== the gate"
if "$DEMO" check > "$T/check.out"; then
	echo "    (this is a QEMU virtual machine: the refusal isn't tested here)"
else
	grep -q "not a QEMU virtual machine" "$T/check.out" || fail "check: $(cat "$T/check.out")"
	if "$DEMO" seed --state "$T/x.json" --why "$T/x.bin" --upgrade "$T/x-up.json" 2> "$T/seed.err"; then
		fail "seeded outside QEMU without --test"
	fi
	grep -q "only for the demo image in QEMU" "$T/seed.err" || fail "refusal: $(cat "$T/seed.err")"
	[ ! -e "$T/x.json" ] || fail "a state file was written anyway"
	pass "outside QEMU: $(cat "$T/check.out"); seeding refused"
fi
echo '{"version":1,"devices":[]}' > "$T/state.json"
if "$DEMO" seed --test --state "$T/state.json" --why "$T/why.bin" --upgrade "$T/upgrade.json" 2> "$T/seed.err"; then
	fail "replaced an existing state file without --replace"
fi
grep -q "not replaced without --replace" "$T/seed.err" || fail "$(cat "$T/seed.err")"
pass "an existing state file is kept"
# v0.43: --test never writes a system's files (a real router's /etc/aigw/agd-state.json), also
# not through a link to /etc
ln -s /etc "$T/etc-link"
for f in "/etc/aigw-demo-test.$$.json" "$T/etc-link/aigw-demo-test.$$.json"; do
	if "$DEMO" seed --test --replace --state "$f" --why "$T/why.bin" --upgrade "$T/upgrade.json" 2> "$T/seed.err"; then
		rm -f "/etc/aigw-demo-test.$$.json"
		fail "--test wrote $f"
	fi
	grep -q "test suite's own files" "$T/seed.err" || fail "--test refusal: $(cat "$T/seed.err")"
	[ ! -e "/etc/aigw-demo-test.$$.json" ] || { rm -f "/etc/aigw-demo-test.$$.json"; fail "--test wrote into /etc"; }
done
pass "--test refuses a system's files (/etc, also through a link)"

echo "== the simulated home"
"$DEMO" seed --test --replace --state "$T/state.json" --why "$T/why.bin" --upgrade "$T/upgrade.json" ||
	fail "seed failed"
printf '#!/bin/sh\necho "$*" >> %s/sqm.log\ncase $1 in check) echo cake ;; status) echo off ;;\n' "$T" > "$T/sqm-handler"
printf 'apply) echo "smart queueing on $2: $3/$4 kbit/s (cake)" ;; *) echo ok ;; esac\n' >> "$T/sqm-handler"
chmod +x "$T/sqm-handler"
mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'info'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15361'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config diag 'diag'
	option targets '127.0.0.1'

config care 'care'
	option interval '3600'

config upgrade 'upgrade'
	option file '$T/upgrade.json'

config why 'why'
	option file '$T/why.bin'
	option probe '0'

config sqm 'sqm'
	option handler '$T/sqm-handler'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option enabled '1'

config lan 'lan'
	list iface 'lo'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_DEMO_MARK="$T/demo-mark"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
agctl() { "$BIN/agctl/agctl" "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
check() { # check <what> <python condition on d> <agctl args...>
	what=$1 cond=$2; shift 2
	agctl -j "$@" > "$T/out.json" 2>&1 || fail "$what: agctl $* failed: $(head -c 400 "$T/out.json")"
	js "sys.exit(0 if ($cond) else 1)" < "$T/out.json" || fail "$what: $(head -c 1500 "$T/out.json")"
	pass "$what"
}
start_agd

check "nine devices" "len(d['devices']) == 9 and any(x.get('hostname') == 'Living-room-TV' for x in d['devices'])" devices
agctl why --from "yesterday 20:00" --to "yesterday 20:40" > "$T/why.txt" 2>&1 || fail "agctl why: $(cat "$T/why.txt")"
grep -q "LINE_FULL" "$T/why.txt" || fail "why: no LINE_FULL: $(cat "$T/why.txt")"
grep -q "Mayas-MacBook" "$T/why.txt" || fail "why: the download isn't named: $(cat "$T/why.txt")"
grep -q "AIGW acted: sqm" "$T/why.txt" || fail "why: smart queueing isn't in the story: $(cat "$T/why.txt")"
grep -q "Microsoft Teams" "$T/why.txt" || fail "why: the rough call isn't in the story: $(cat "$T/why.txt")"
grep -q "ISP_LOSS\|ISP_DOWN\|GATEWAY_OFF" "$T/why.txt" && fail "why: a finding that isn't in the story: $(cat "$T/why.txt")"
pass "agctl why: the slow evening explained (the line full, the download named, the call, smart queueing)"
check "smart queueing kept by its trial" \
	"d['status'] == 'On' and d['trial']['verdict'] == 'kept' and d['interface'] == 'eth1' and d['trial']['before_ms'] > 150 and d['trial']['after_ms'] < 40" sqm
grep -q "^apply eth1 " "$T/sqm.log" || fail "smart queueing wasn't put back on the WAN: $(cat "$T/sqm.log" 2>/dev/null)"
pass "the kept smart queueing put back through the handler: $(grep '^apply' "$T/sqm.log" | head -n 1)"
check "the upgrade to $V kept" "d.get('state') == 'Kept' and d.get('to') == '$V'" upgrade
agctl dm get Device.X_AIGW_.CallsFirst.Last.Verdict Device.X_AIGW_.CallsFirst.Last.Prioritized \
	Device.X_AIGW_.SQM.Status Device.X_AIGW_.SQM.LatencyAfterMs > "$T/dm.txt" 2>&1 || fail "dm: $(cat "$T/dm.txt")"
grep -q "Last.Verdict = Smooth" "$T/dm.txt" && grep -q "Last.Prioritized = true" "$T/dm.txt" &&
	grep -q "SQM.Status = On" "$T/dm.txt" || fail "data model: $(cat "$T/dm.txt")"
pass "data model: $(tr '\n' ' ' < "$T/dm.txt" | head -c 300)"
agctl brief "yesterday 20:00-21:00" --no-check > "$T/brief.txt" 2>&1 || fail "brief: $(cat "$T/brief.txt")"
grep -q "NONE (LINE_FULL" "$T/brief.txt" || fail "brief: $(cat "$T/brief.txt")"
grep -q "smart queueing" "$T/brief.txt" || fail "brief: AIGW's fix isn't in it: $(cat "$T/brief.txt")"
pass "agctl brief: not a fault, the line was full; AIGW turned on smart queueing"
check "a week of proactive care: one problem, fixed by AIGW; the nightly speed test" \
	"d['report']['week']['problems'] == 1 and d['report']['week']['fixed_by_aigw'] == 1 and d['report']['week']['score'] >= 95 and d['speed']['status'] == 'OK'" care

agctl care proactive > "$T/pro.txt" 2>&1 || fail "care proactive: $(cat "$T/pro.txt")"
grep -q "LINE_FULL" "$T/pro.txt" && grep -qi "fixed" "$T/pro.txt" || fail "proactive: no fixed ticket: $(cat "$T/pro.txt")"
pass "proactive care: yesterday's evening a ticket AIGW fixed itself"

echo "== the label"
check "no demo mark: not labelled" "'demo' not in d" status
agctl status | grep -q "^DEMO" && fail "agctl labelled it without the mark"
touch "$AIGW_DEMO_MARK"
check "the demo mark: ag status says demo" "d.get('demo') in (1, True)" status
agctl status | head -n 1 | grep -q "^DEMO  a simulated home" || fail "agctl: no DEMO line: $(agctl status | head -n 2)"
agctl -j status | head -c 1 | grep -q '{' || fail "agctl -j must stay JSON"
pass "agctl prints DEMO first; -j stays JSON"

echo "== kept over a restart"
stop_agd
start_agd
check "the timeline and the trial after a restart" "d['trial']['verdict'] == 'kept'" sqm
agctl why --from "yesterday 20:00" --to "yesterday 20:40" | grep -q LINE_FULL || fail "the timeline was lost"
pass "agd keeps the simulated home as its own"
echo "DEMO TEST OK"
