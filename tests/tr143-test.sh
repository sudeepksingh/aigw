#!/bin/sh
# TR-143 diagnostics test: the standard tests an ISP runs from the gateway,
# against agd on a private ubus, with a local HTTP server and DNS responder
# (no internet needed):
#
#   - IPPing() to 127.0.0.1: every echo answered, IPAddressUsed; a name that
#     doesn't resolve: Error_CannotResolveHostName
#   - TraceRoute() to 127.0.0.1: one hop, RouteHops in the outputs and the object
#   - DownloadDiagnostics(): the whole file (TR-143's times, bytes), time based
#     on an endless stream, a 404 (Error_TransferFailed), nothing listening
#     (Error_InitConnectionFailed); one at a time
#   - UploadDiagnostics(): TestFileLength bytes arrive; time based (chunked)
#   - NSLookupDiagnostics(): the gateway's own DNS answers (Success, the address),
#     a name it doesn't know (Error_HostNameNotResolved)
#   - TR-069's way: inputs and DiagnosticsState = Requested written, the command
#     run as icwmp runs it, the results read from the object; an input changed
#     afterwards makes DiagnosticsState None
#   - agctl test ping / download / dns
# Needs root (raw ICMP, ubusd objects), python3.
#
#   sudo tests/tr143-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PIDS=""
HTTP=15380
DNS=15381

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "TR143 TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

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

config care 'care'
	option enabled '0'

config lan 'lan'
	list iface 'lo'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"

# GET /bytes/N: N bytes; GET /stream: no end; GET anything else: 404; PUT: the body's length, to put.log
cat > "$T/httpd.py" <<'EOF'
import sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith("/bytes/"):
            n = int(self.path[7:])
            self.send_response(200)
            self.send_header("Content-Length", str(n))
            self.end_headers()
            chunk = b"z" * 65536
            while n > 0:
                k = min(n, len(chunk)); self.wfile.write(chunk[:k]); n -= k
        elif self.path == "/stream":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(b"s" * 65536)
            except Exception:
                pass
            self.close_connection = True
        else:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
    def do_PUT(self):
        got = 0
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if size == 0:
                    self.rfile.readline(); break
                got += len(self.rfile.read(size)); self.rfile.readline()
        else:
            n = int(self.headers.get("Content-Length", "0"))
            while got < n:
                b = self.rfile.read(min(65536, n - got))
                if not b: break
                got += len(b)
        with open(sys.argv[2], "a") as f:
            f.write("%d %s\n" % (got, self.headers.get("Transfer-Encoding", "length")))
        self.send_response(201); self.send_header("Content-Length", "0"); self.end_headers()

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 "$T/httpd.py" "$HTTP" "$T/put.log" > "$T/httpd.log" 2>&1 & PIDS="$PIDS $!"
# the gateway's own DNS: example.com is 192.0.2.7, everything else NXDOMAIN
python3 -c '
import socket, struct, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    q, a = s.recvfrom(512)
    i, name = 12, []
    while q[i]:
        name.append(q[i + 1:i + 1 + q[i]].decode()); i += 1 + q[i]
    qend = i + 5
    if ".".join(name).lower() == "example.com":
        r = q[:2] + b"\x81\x80" + q[4:6] + b"\x00\x01\x00\x00\x00\x00" + q[12:qend]
        r += b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + bytes([192, 0, 2, 7])
    else:
        r = q[:2] + b"\x81\x83" + q[4:6] + b"\x00\x00\x00\x00\x00\x00" + q[12:qend]
    s.sendto(r, a)' "$DNS" > "$T/dnsd.log" 2>&1 & PIDS="$PIDS $!"

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.3
"$BIN/agd/agd" > "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"
sleep 1.5

agctl() { "$BIN/agctl/agctl" "$@"; }
# run <command()> [Arg=value...]: the outputs as JSON in $T/out.json
run() { agctl -j dm do "$@" > "$T/out.json" || fail "$1 failed: $(cat "$T/out.json")"; }
out() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['out'].get(sys.argv[2], ''))" "$T/out.json" "$1"; }
dmv() { agctl dm get "$1" | sed -n 's/^[^=]* = //p'; }

echo "== the schema: five TR-143 commands, the capabilities"
agctl -j dm schema > "$T/schema.json"
python3 - "$T/schema.json" <<'EOF' || fail "the schema"
import json, sys
s = json.load(open(sys.argv[1]))
cmds = {c["path"]: c for c in s["commands"]}
for c in ("Device.IP.Diagnostics.IPPing()", "Device.IP.Diagnostics.TraceRoute()",
          "Device.IP.Diagnostics.DownloadDiagnostics()", "Device.IP.Diagnostics.UploadDiagnostics()",
          "Device.DNS.Diagnostics.NSLookupDiagnostics()"):
    assert c in cmds and cmds[c]["async"], c
assert "RouteHops.{i}.HostAddress" in cmds["Device.IP.Diagnostics.TraceRoute()"]["out"]
assert "TCPOpenResponseTime" in cmds["Device.IP.Diagnostics.DownloadDiagnostics()"]["out"]
params = {p["path"]: p for p in s["params"]}
assert params["Device.IP.Diagnostics.IPPing.DiagnosticsState"]["writable"]
assert not params["Device.IP.Diagnostics.IPPing.SuccessCount"]["writable"]
objs = [o["path"] for o in s["objects"]]
assert "Device.IP.Diagnostics.TraceRoute.RouteHops.{i}." in objs
EOF
[ "$(dmv Device.IP.Diagnostics.IPv4PingSupported)" = true ] && [ "$(dmv Device.IP.Diagnostics.IPv6PingSupported)" = false ] &&
	[ "$(dmv Device.IP.Diagnostics.DownloadTransports)" = HTTP ] || fail "the capabilities"
[ "$(dmv Device.IP.Diagnostics.IPPing.DiagnosticsState)" = None ] || fail "DiagnosticsState before any test"

echo "== IPPing"
run "Device.IP.Diagnostics.IPPing()" Host=127.0.0.1 NumberOfRepetitions=4 Timeout=1000 DataBlockSize=100
[ "$(out Status)" = Complete ] && [ "$(out SuccessCount)" = 4 ] && [ "$(out FailureCount)" = 0 ] &&
	[ "$(out IPAddressUsed)" = 127.0.0.1 ] && [ "$(out MaximumResponseTimeDetailed)" -ge "$(out MinimumResponseTimeDetailed)" ] ||
	fail "IPPing to 127.0.0.1: $(cat "$T/out.json")"
echo "   4 of 4 answered, $(out AverageResponseTimeDetailed) us on average"
# TEST-NET-1: nothing answers (or there is no route): every echo request fails, none counts as answered
run "Device.IP.Diagnostics.IPPing()" Host=192.0.2.1 NumberOfRepetitions=2 Timeout=300
[ "$(out Status)" = Complete ] && [ "$(out SuccessCount)" = 0 ] && [ "$(out FailureCount)" = 2 ] ||
	fail "IPPing to an address that doesn't answer: $(cat "$T/out.json")"
run "Device.IP.Diagnostics.IPPing()" Host=no-such-host.invalid
[ "$(out Status)" = Error_CannotResolveHostName ] || fail "an unknown name: $(cat "$T/out.json")"
agctl -j dm do "Device.IP.Diagnostics.IPPing()" Host=127.0.0.1 NumberOfRepetitions=0 > "$T/bad.json" || true
grep -q '"ok": false' "$T/bad.json" && grep -q NumberOfRepetitions "$T/bad.json" || fail "0 repetitions must be refused"

echo "== TraceRoute"
run "Device.IP.Diagnostics.TraceRoute()" Host=127.0.0.1 NumberOfTries=2 Timeout=1000
[ "$(out Status)" = Complete ] && [ "$(out RouteHops.1.HostAddress)" = 127.0.0.1 ] && [ -z "$(out RouteHops.2.HostAddress)" ] ||
	fail "TraceRoute to 127.0.0.1: $(cat "$T/out.json")"
[ "$(dmv Device.IP.Diagnostics.TraceRoute.RouteHopsNumberOfEntries)" = 1 ] &&
	[ "$(dmv Device.IP.Diagnostics.TraceRoute.RouteHops.1.HostAddress)" = 127.0.0.1 ] || fail "RouteHops in the object"
echo "   1 hop, RTTimes $(out RouteHops.1.RTTimes)"

echo "== DownloadDiagnostics"
run "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:$HTTP/bytes/3000000"
[ "$(out Status)" = Complete ] && [ "$(out TestBytesReceived)" -gt 3000000 ] && [ "$(out TotalBytesReceived)" -ge "$(out TestBytesReceived)" ] ||
	fail "a 3 MB download: $(cat "$T/out.json")"
python3 - "$T/out.json" <<'EOF' || fail "TR-143's times: $(cat "$T/out.json")"
import json, sys, datetime
o = json.load(open(sys.argv[1]))["out"]
t = {k: datetime.datetime.strptime(o[k], "%Y-%m-%dT%H:%M:%S.%fZ") for k in
     ("TCPOpenRequestTime", "TCPOpenResponseTime", "ROMTime", "BOMTime", "EOMTime")}
assert t["TCPOpenRequestTime"] <= t["TCPOpenResponseTime"] <= t["ROMTime"] <= t["BOMTime"] <= t["EOMTime"], t
assert int(o["PeriodOfFullLoading"]) > 0
EOF
echo "   $(out TestBytesReceived) bytes, $(out ROMTime) .. $(out EOMTime)"
run "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:$HTTP/stream" TimeBasedTestDuration=1
[ "$(out Status)" = Complete ] && [ "$(out TestBytesReceived)" -gt 100000 ] || fail "a time based download: $(cat "$T/out.json")"
run "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:$HTTP/missing"
[ "$(out Status)" = Error_TransferFailed ] || fail "a 404: $(cat "$T/out.json")"
run "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:1/x"
[ "$(out Status)" = Error_InitConnectionFailed ] || fail "nothing listening: $(cat "$T/out.json")"
agctl -j dm do "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="https://127.0.0.1/" > "$T/bad.json" || true
python3 -c "import json,sys; o=json.load(open(sys.argv[1])); sys.exit(0 if o['ok'] and o['out']['Status'] == 'Error_InitConnectionFailed' else 1)" "$T/bad.json" ||
	fail "https: not supported, said so: $(cat "$T/bad.json")"
# one at a time
agctl -j dm do "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:$HTTP/stream" TimeBasedTestDuration=3 > "$T/long.json" &
L=$!
sleep 1
agctl -j dm do "Device.IP.Diagnostics.DownloadDiagnostics()" DownloadURL="http://127.0.0.1:$HTTP/bytes/10" > "$T/busy.json" || true
grep -q "is running" "$T/busy.json" || fail "a second download while one runs: $(cat "$T/busy.json")"
wait "$L" || fail "the long download"

echo "== UploadDiagnostics"
run "Device.IP.Diagnostics.UploadDiagnostics()" UploadURL="http://127.0.0.1:$HTTP/up" TestFileLength=300000
[ "$(out Status)" = Complete ] && [ "$(out TestBytesSent)" -gt 300000 ] || fail "an upload: $(cat "$T/out.json")"
grep -q "^300000 length" "$T/put.log" || fail "the server should have got 300000 bytes: $(cat "$T/put.log")"
run "Device.IP.Diagnostics.UploadDiagnostics()" UploadURL="http://127.0.0.1:$HTTP/up" TimeBasedTestDuration=1
[ "$(out Status)" = Complete ] && grep -q "chunked" "$T/put.log" || fail "a time based upload: $(cat "$T/out.json") $(cat "$T/put.log")"

echo "== NSLookupDiagnostics: the gateway's own DNS"
run "Device.DNS.Diagnostics.NSLookupDiagnostics()" HostName=example.com NumberOfRepetitions=2
[ "$(out Status)" = Complete ] && [ "$(out SuccessCount)" = 2 ] && [ "$(out Result.1.Status)" = Success ] &&
	[ "$(out Result.1.IPAddresses)" = 192.0.2.7 ] && [ "$(out Result.2.DNSServerIP)" = 127.0.0.1 ] &&
	[ "$(out Result.1.AnswerType)" = NonAuthoritative ] || fail "NSLookup example.com: $(cat "$T/out.json")"
run "Device.DNS.Diagnostics.NSLookupDiagnostics()" HostName=unknown.example
[ "$(out SuccessCount)" = 0 ] && [ "$(out Result.1.Status)" = Error_HostNameNotResolved ] || fail "NSLookup NXDOMAIN: $(cat "$T/out.json")"

echo "== TR-069's way: inputs and DiagnosticsState written, then run as icwmp runs it"
P=Device.IP.Diagnostics.IPPing
agctl dm set $P.Host 127.0.0.1 $P.NumberOfRepetitions 2 $P.DiagnosticsState Requested > /dev/null || fail "set the IPPing inputs"
[ "$(dmv $P.DiagnosticsState)" = Requested ] || fail "DiagnosticsState should read Requested"
agctl dm set $P.DiagnosticsState Complete > /dev/null 2>&1 && fail "DiagnosticsState Complete must not be writable"
# icwmp reads the object and runs the command with what it found
run "Device.IP.Diagnostics.IPPing()" Host="$(dmv $P.Host)" NumberOfRepetitions="$(dmv $P.NumberOfRepetitions)"
[ "$(dmv $P.DiagnosticsState)" = Complete ] && [ "$(dmv $P.SuccessCount)" = 2 ] || fail "the results in the object: $(agctl dm get $P.)"
agctl dm set $P.Host localhost > /dev/null
[ "$(dmv $P.DiagnosticsState)" = None ] && [ "$(dmv $P.SuccessCount)" = 0 ] || fail "an input changed: DiagnosticsState None"
agctl dm set $P.NumberOfRepetitions 0 > /dev/null 2>&1 && fail "0 repetitions must not be writable"

echo "== agctl test"
agctl test ping 127.0.0.1 2 | tee "$T/ping.txt"
grep -q "2 of 2 answered" "$T/ping.txt" || fail "agctl test ping"
agctl test download "http://127.0.0.1:$HTTP/bytes/1000000" | tee "$T/dl.txt"
grep -q "received 10" "$T/dl.txt" && grep -q "Mbit/s" "$T/dl.txt" || fail "agctl test download"
agctl test dns example.com | tee "$T/ns.txt"
grep -q "192.0.2.7" "$T/ns.txt" || fail "agctl test dns"
agctl test ping no-such-host.invalid > "$T/bad.txt" 2>&1 && fail "agctl test should fail when the test fails"
grep -q Error_CannotResolveHostName "$T/bad.txt" || fail "agctl test ping, unknown name: $(cat "$T/bad.txt")"
echo "TR143 TEST OK"
