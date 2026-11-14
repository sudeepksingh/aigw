#!/bin/sh
# Proactive care test: the gateway checks itself, tells the ISP, and keeps the
# experience score. agd alone on a private ubus; no radios needed (the Wi-Fi
# rules are in tests/test_care.c, the Wi-Fi case in tests/hwsim-test.sh).
#
#   - names don't resolve (no DNS server on the configured port): found by the
#     gateway's own checks, twice (a blip isn't a problem), reported at once
#     (DNS is the ISP's): Device.X_AIGW_.ProblemDetected!, Care.Status Problem
#   - agd restarted with the problem open: it is remembered, not reported twice
#   - names resolve again (a tiny DNS server): over, ProblemCleared!
#   - the score: checks, problems, reported, main cause DNS
#   - the checks are quiet: LastDiagnosis stays the one someone asked for
#   - someone asking Diagnose about a device during a check waits, isn't refused
# Needs root (ubusd objects, raw ICMP), python3.
#
#   sudo tests/care-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
PORT=15357

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "CARE TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)
awk 'NR > 1 && $2 == "00000000" {f = 1} END {exit !f}' /proc/net/route ||
	{ echo "CARE TEST SKIPPED: no default route (Diagnose would say NO_ROUTE)"; exit 0; }

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '$PORT'

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
	option state_dir '$T/plugins'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config diag 'diag'
	option targets '127.0.0.1'
	option name 'example.com'

config care 'care'
	option interval '3600'
	option recheck '3'

config mgmt 'mgmt'
	option enabled '1'

config lan 'lan'
	list iface 'lo'
EOF
echo "1999999999 02:00:00:00:97:02 10.97.0.12 phone *" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3

agctl() { "$BIN/agctl/agctl" "$@"; }
care_is() { # care_is <ok|problem> [reported]: wait for it
	i=0
	until agctl -j care | python3 -c "
import json, sys
d = json.load(sys.stdin)
p = d.get('problem', {})
sys.exit(0 if d.get('status') == '$1' and ('${2:-}' != 'reported' or p.get('reported') in (1, True)) else 1)"; do
		i=$((i + 1)); [ $i -gt 40 ] && fail "proactive care should be $1 ${2:-}: $(agctl care)"; sleep 1
	done
}
nev() { grep -c "$1" "$T/events.log" || true; }
dnsd() {
	python3 -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    q, a = s.recvfrom(512)
    r = bytearray(q); r[2] |= 0x80; r[3] = 0x80
    s.sendto(bytes(r), a)' "$PORT" > "$T/dnsd.log" 2>&1 & DNSD=$!; PIDS="$PIDS $DNSD"
	sleep 0.5
}

echo "== a help desk's diagnosis first (LastDiagnosis: theirs)"
agctl diagnose phone > "$T/asked.txt" || true
grep -q "code      " "$T/asked.txt" || fail "the asked diagnosis: $(cat "$T/asked.txt")"
ASKED=$(agctl dm get Device.X_AIGW_.LastDiagnosis.Code | sed -n 's/.*Code = //p')
[ -n "$ASKED" ] || fail "no LastDiagnosis"

echo "== names don't resolve: found by the gateway itself, reported at once"
agctl care --check > /dev/null || fail "agctl care --check"
care_is problem reported
agctl care | tee "$T/care.txt"
grep -q "now       DNS_FAILING (DNS, problem)" "$T/care.txt" && grep -q "reported to the ISP" "$T/care.txt" ||
	fail "agctl care should show the DNS problem, reported"
agctl dm get Device.X_AIGW_.Care. > "$T/care-dm.txt"
grep -q "Care.Status = Problem" "$T/care-dm.txt" && grep -q "Care.Problem.Code = DNS_FAILING" "$T/care-dm.txt" &&
	grep -q "Care.Problem.ReportedToISP = true" "$T/care-dm.txt" && grep -q "Care.Problem.Advice = ." "$T/care-dm.txt" ||
	fail "Device.X_AIGW_.Care.: $(cat "$T/care-dm.txt")"
i=0
until [ "$(nev 'ProblemDetected!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 10 ] && fail "no ProblemDetected! event: $(cat "$T/events.log")"; sleep 1
done
grep 'ProblemDetected!' "$T/events.log" | grep -q DNS_FAILING || fail "ProblemDetected! should name DNS_FAILING"
grep -q "care: problem found: DNS_FAILING" "$T/agd.log" || fail "agd should log the problem"
[ "$(grep -c 'care: reported to the ISP' "$T/agd.log")" = 1 ] || fail "reported once"

echo "== a device asked about during the checks waits, isn't refused"
agctl care --check > /dev/null
sleep 0.2
agctl diagnose phone > "$T/asked2.txt" || fail "Diagnose during a check: $(cat "$T/asked2.txt")"
grep -q "code      " "$T/asked2.txt" || fail "Diagnose during a check: $(cat "$T/asked2.txt")"
ASKED=$(agctl dm get Device.X_AIGW_.LastDiagnosis.Time | sed -n 's/.*Time = //p')

echo "== the checks are quiet"
agctl care --check > /dev/null
sleep 8                                 # a few of the gateway's own checks
[ "$(agctl dm get Device.X_AIGW_.LastDiagnosis.Time | sed -n 's/.*Time = //p')" = "$ASKED" ] ||
	fail "proactive care's checks must not replace LastDiagnosis"
[ "$(nev 'DiagnosisDone!')" = 2 ] || fail "DiagnosisDone! only for the 2 asked for, got $(nev 'DiagnosisDone!')"

echo "== agd restarted with the problem open: remembered, not reported twice"
D0=$(nev 'ProblemDetected!')
kill "$AGD"; wait "$AGD" 2>/dev/null || true
"$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5
grep -q "care: restored .*open problem DNS_FAILING" "$T/agd.log" || fail "the open problem should be restored"
agctl care --check > /dev/null
sleep 8
care_is problem reported
[ "$(nev 'ProblemDetected!')" = "$D0" ] || fail "the same problem must not be reported again after a restart"

echo "== names resolve again: over"
dnsd
agctl care --check > /dev/null
care_is ok
i=0
until [ "$(nev 'ProblemCleared!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 10 ] && fail "no ProblemCleared! event"; sleep 1
done
grep 'ProblemCleared!' "$T/events.log" | grep -q 'DNS_FAILING' || fail "ProblemCleared! should name DNS_FAILING"
agctl dm get Device.X_AIGW_.Care.Status | grep -q "= OK" || fail "Care.Status should be OK"

echo "== the score"
agctl -j care | python3 -c "
import json, sys
d = json.load(sys.stdin)['report']['day']
ok = d['checks'] >= 4 and d['problems'] == 1 and d['reported'] == 1 and d['score'] < 100 and d['main_cause'] == 'DNS' and d['problem_s'] > 0
sys.exit(0 if ok else 'day: %s' % d)" || fail "the day's report"
agctl care | grep "last day"
agctl dm get Device.X_AIGW_.Care.Stats.Day.MainCause | grep -q "= DNS" || fail "Care.Stats.Day.MainCause"
echo "CARE TEST OK"
