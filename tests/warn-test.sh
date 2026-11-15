#!/bin/sh
# Early warning and the nightly speed test: agd alone on a private ubus, a
# local HTTP server whose speed the test turns down and up (the learning and
# the detectors themselves are unit-tested in tests/test_warn.c):
#
#   - speed tests (TR-143 downloads, care.speed_url) teach the home's normal speed
#   - slower than normal, though nobody said what the line should reach: an
#     early warning, Device.X_AIGW_.EarlyWarning! (DownloadSpeed); fast again:
#     EarlyWarningCleared!
#   - agd restarted: the normal is still known
#   - the line's speed known (care.speed_mbps): two slow tests are a problem,
#     LINE_SLOW_SPEED, reported at once (ProblemDetected!); one fine: over
#   - the day's stats: speed tests, slow ones, early warnings
# Needs root (ubusd objects, raw ICMP), python3.
#
#   sudo tests/warn-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
HTTP=15390
DNS=15391

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "WARN TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

config() { # config <speed_mbps>
	mkdir -p "$T/config"
	cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '$DNS'

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
	option learn_days '0'
	option speed_url 'http://127.0.0.1:$HTTP/speed'
	option speed_seconds '1'
	option speed_mbps '$1'
	option speed_min_pct '50'

config lan 'lan'
	list iface 'lo'
EOF
}
config 0
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

# /speed: about 200 Mbit/s (a steady line; loopback unpaced swings by thousands), or about 6 when $T/rate says slow
echo fast > "$T/rate"
cat > "$T/httpd.py" <<'EOF'
import sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def do_GET(self):
        slow = open(sys.argv[2]).read().strip() == "slow"
        self.send_response(200)
        self.end_headers()
        chunk, pause = (b"v" * 16384, 0.02) if slow else (b"v" * 65536, 0.0025)
        start, sent = time.monotonic(), 0
        try:
            for _ in range(200000):
                self.wfile.write(chunk)
                sent += 1
                # paced: chunk k leaves at k * pause, whatever the scheduler did before
                wait = start + sent * pause - time.monotonic()
                if wait > 0:
                    time.sleep(wait)
        except Exception:
            pass

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 "$T/httpd.py" "$HTTP" "$T/rate" > "$T/httpd.log" 2>&1 & PIDS="$PIDS $!"
python3 -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    q, a = s.recvfrom(512)
    r = bytearray(q); r[2] |= 0x80; r[3] = 0x80
    s.sendto(bytes(r), a)' "$DNS" > "$T/dnsd.log" 2>&1 & PIDS="$PIDS $!"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
"$UBUS" -s "$AIGW_UBUS" listen ag.dm.event > "$T/events.log" 2>&1 & PIDS="$PIDS $!"
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1.5; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
start_agd

agctl() { "$BIN/agctl/agctl" "$@"; }
nev() { grep -c "$1" "$T/events.log" || true; }
dmv() { agctl dm get "$1" | sed -n 's/^[^=]* = //p'; }
last() { agctl -j care | python3 -c "import json,sys; print(json.load(sys.stdin)['speed'].get('last', 0))"; }
speed_test() { # one speed test now; waits for it
	b=$(last)
	agctl care --speed > /dev/null || fail "agctl care --speed"
	i=0
	while [ "$(last)" = "$b" ]; do
		i=$((i + 1)); [ $i -gt 40 ] && fail "the speed test did not run: $(tail -n 5 "$T/agd.log")"; sleep 0.5
	done
	sleep 0.3
}

echo "== speed tests teach the home's normal speed"
agctl care | grep -q "speed     no test yet" || fail "no speed test yet: $(agctl care)"
for n in 1 2 3 4 5 6 7; do speed_test; done
agctl care | tee "$T/care1.txt"
grep -q "speed     [0-9.]* Mbit/s" "$T/care1.txt" || fail "agctl care should show the last speed"
N=$(dmv Device.X_AIGW_.Care.Normal.DownloadSpeed)
python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 50 else 1)" "$N" || fail "the normal speed: $N"
[ "$(nev 'EarlyWarning!')" = 0 ] || fail "no warning while the speed is as usual: $(grep EarlyWarning "$T/events.log")"
[ "$(dmv Device.X_AIGW_.Care.Speed.Status)" = OK ] && [ "$(dmv Device.X_AIGW_.Care.EarlyWarning.Status)" = None ] ||
	fail "Speed.Status OK, no warning: $(agctl dm get Device.X_AIGW_.Care.)"
echo "   normal: $N Mbit/s"

echo "== slower than normal: an early warning"
echo slow > "$T/rate"
speed_test
speed_test
i=0
until [ "$(nev 'EarlyWarning!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "no EarlyWarning! event: $(agctl care)"; sleep 0.5
done
grep 'EarlyWarning!' "$T/events.log" | grep -q '"Metric":"DownloadSpeed"' || fail "EarlyWarning! should be about DownloadSpeed"
[ "$(dmv Device.X_AIGW_.Care.EarlyWarning.Status)" = Warning ] && [ "$(dmv Device.X_AIGW_.Care.EarlyWarning.Metric)" = DownloadSpeed ] &&
	[ "$(dmv Device.X_AIGW_.Care.Status)" = OK ] || fail "Care.EarlyWarning.: $(agctl dm get Device.X_AIGW_.Care.)"
agctl care | tee "$T/care2.txt"
grep -q "warning   DownloadSpeed (Internet, shift)" "$T/care2.txt" || fail "agctl care should show the warning"
echo "   $(dmv Device.X_AIGW_.Care.EarlyWarning.Summary)"

echo "== fast again: over"
echo fast > "$T/rate"
i=0
until [ "$(nev 'EarlyWarningCleared!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 12 ] && fail "no EarlyWarningCleared! after fast tests: $(agctl care)"; speed_test
done
grep 'EarlyWarningCleared!' "$T/events.log" | grep -q '"BecameProblem":"false"' || fail "EarlyWarningCleared! BecameProblem"
[ "$(dmv Device.X_AIGW_.Care.EarlyWarning.Status)" = None ] || fail "the warning should be over"

echo "== agd restarted: the normal is still known"
stop_agd
start_agd
grep -q "days of the home's normal" "$T/agd.log" || fail "agd should say it restored the normal"
python3 -c "import sys; sys.exit(0 if float(sys.argv[1]) > 50 else 1)" "$(dmv Device.X_AIGW_.Care.Normal.DownloadSpeed)" ||
	fail "the normal speed after the restart"

echo "== the line's speed known: two slow tests are a problem (LINE_SLOW_SPEED), reported"
stop_agd
config 100000
start_agd
echo slow > "$T/rate"
speed_test
[ "$(dmv Device.X_AIGW_.Care.Status)" = OK ] || fail "one slow test isn't a problem yet"
speed_test
i=0
until [ "$(nev 'ProblemDetected!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "no ProblemDetected!: $(agctl care)"; sleep 0.5
done
grep 'ProblemDetected!' "$T/events.log" | grep -q LINE_SLOW_SPEED || fail "ProblemDetected! should be LINE_SLOW_SPEED"
[ "$(dmv Device.X_AIGW_.Care.Problem.Code)" = LINE_SLOW_SPEED ] && [ "$(dmv Device.X_AIGW_.Care.Speed.Status)" = Low ] &&
	[ "$(dmv Device.X_AIGW_.Care.Problem.ReportedToISP)" = true ] || fail "Care.Problem.: $(agctl dm get Device.X_AIGW_.Care.)"
agctl care | grep -q "now       LINE_SLOW_SPEED (Internet, warning)" || fail "agctl care should show LINE_SLOW_SPEED: $(agctl care)"
[ "$(nev 'EarlyWarning!')" = 1 ] || fail "a slow line known to be slow is a problem, not another early warning"

echo "== fast enough again: over"
stop_agd
config 1
start_agd
echo fast > "$T/rate"
speed_test
i=0
until [ "$(nev 'ProblemCleared!')" -ge 1 ]; do
	i=$((i + 1)); [ $i -gt 20 ] && fail "no ProblemCleared!: $(agctl care)"; sleep 0.5
done
grep 'ProblemCleared!' "$T/events.log" | grep -q LINE_SLOW_SPEED || fail "ProblemCleared! should be LINE_SLOW_SPEED"
[ "$(dmv Device.X_AIGW_.Care.Status)" = OK ] || fail "Care.Status after the line is fast again"

echo "== the day's stats"
agctl dm get Device.X_AIGW_.Care.Stats.Day. > "$T/day.txt"
python3 - "$T/day.txt" <<'EOF' || fail "Care.Stats.Day: $(cat "$T/day.txt")"
import re, sys
v = dict(re.findall(r"Stats\.Day\.(\w+) = (\S*)", open(sys.argv[1]).read()))
ok = int(v["SpeedTests"]) >= 12 and int(v["SpeedTestsLow"]) >= 2 and int(v["EarlyWarnings"]) >= 1 and \
     float(v["MeanSpeedMbps"]) > float(v["MinSpeedMbps"]) > 0 and int(v["Problems"]) >= 1 and int(v["ReportedToISP"]) >= 1
sys.exit(0 if ok else 1)
EOF
agctl care | grep "last day"
echo "WARN TEST OK"
