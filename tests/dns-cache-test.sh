#!/bin/sh
# agdns cache test, against a fake upstream that takes 50 ms per answer:
#
#   client (python) ── agdns 127.0.0.1:15391 ── fake upstream 127.0.0.1:15392
#
# Checks: answers from the cache are fast and counted down, the client's
# spelling of the name is kept (0x20), "no such name" is cached, a name in use
# is refreshed before it expires (no client waits for upstream), and an
# expired answer stands in when upstream stops answering or fails (RFC 8767).
# Prints the measured answer times. Needs root: ubusd lets only root
# publish an object (ag.dnscache) without an ACL file.
#
#   sudo tests/dns-cache-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() { echo "DNS CACHE TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log >&2 2>/dev/null || true; exit 1; }
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)

cat > "$T/upstream.py" <<'EOF'
# A tiny authoritative server: <anything>.ttl<N>.test -> A 192.0.2.x with TTL N,
# nx.* -> NXDOMAIN with a SOA. $T/mode: ok | drop | servfail.
import os, re, socket, struct, sys, time
port, T = int(sys.argv[1]), sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
log = open(T + "/upstream.log", "a", buffering=1)
while True:
    d, a = s.recvfrom(4096)
    o, labels = 12, []
    while d[o]:
        labels.append(d[o + 1:o + 1 + d[o]].decode()); o += 1 + d[o]
    q, name = d[12:o + 5], ".".join(labels).lower()
    log.write(name + "\n")
    mode = open(T + "/mode").read().strip() if os.path.exists(T + "/mode") else "ok"
    if mode == "drop":
        continue
    time.sleep(0.05)
    rd = d[2] & 1
    if mode == "servfail":
        r = d[:2] + bytes([0x80 | rd, 0x82]) + struct.pack(">HHHH", 1, 0, 0, 0) + q
    elif name.startswith("nx."):
        soa = b"\x02ns\x00\x05admin\x00" + struct.pack(">IIIII", 1, 60, 60, 60, 60)
        rr = b"\xc0\x0c" + struct.pack(">HHIH", 6, 1, 60, len(soa)) + soa
        r = d[:2] + bytes([0x80 | rd, 0x83]) + struct.pack(">HHHH", 1, 0, 1, 0) + q + rr
    else:
        m = re.search(r"ttl(\d+)", name)
        ttl = int(m.group(1)) if m else 300
        rr = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, ttl, 4) + bytes([192, 0, 2, sum(name.encode()) % 250 + 1])
        r = d[:2] + bytes([0x80 | rd, 0x80]) + struct.pack(">HHHH", 1, 1, 0, 0) + q + rr
    s.sendto(r, a)
EOF

mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config dns 'dns'
	option listen '127.0.0.1'
	option port '15391'
	option upstream '127.0.0.1#15392'
	option cache '1000'

config act 'act'
	option dns_policy '$T/dns-policy'
EOF
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
python3 "$T/upstream.py" 15392 "$T" > "$T/fake.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
"$BIN/agdns/agdns" > "$T/agdns.log" 2>&1 & PIDS="$PIDS $!"
sleep 1
grep -q "cache: 1000 answers" "$T/agdns.log" || fail "agdns did not start with a cache"

python3 - "$T" <<'EOF' || fail "cache checks"
import os, random, socket, statistics, struct, sys, time
T = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
problems = []

def ask(name):
    qid = random.randrange(65536)
    q = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    q += b"".join(bytes([len(l)]) + l.encode() for l in name.split(".")) + b"\x00" + struct.pack(">HH", 1, 1)
    t0 = time.monotonic()
    s.sendto(q, ("127.0.0.1", 15391))
    while True:
        d = s.recv(4096)
        if d[:2] == q[:2]:
            break
    ms = (time.monotonic() - t0) * 1000
    qlen = len(q) - 12
    ttl = struct.unpack(">I", d[12 + qlen + 6:12 + qlen + 10])[0] if len(d) >= 12 + qlen + 10 else None
    return ms, d[3] & 15, ttl, d[12:12 + qlen] == q[12:]

def upstream(name):
    return sum(1 for l in open(T + "/upstream.log") if l.strip() == name)

def mode(m):
    open(T + "/mode", "w").write(m)

names = ["n%d.example.test" % i for i in range(20)]
cold = [ask(n)[0] for n in names]
warm = [ask(n)[0] for n in names]
pc, pw = statistics.median(cold), statistics.median(warm)
print("  answer time, median of 20 names: %.1f ms from upstream, %.2f ms from the cache (%.0fx faster)" % (pc, pw, pc / pw))
if pc < 40: problems.append("first lookups did not go upstream (%.1f ms)" % pc)
if pw > 10: problems.append("cached answers are slow (%.1f ms)" % pw)
if sum(upstream(n) for n in names) != 20: problems.append("upstream asked more than once per name")

time.sleep(2.1)
ms, rc, ttl, same = ask("n1.example.test")
print("  TTL after 2 s in the cache: %s (was 300)" % ttl)
if not ttl or ttl > 298: problems.append("TTL not counted down: %s" % ttl)

ms, rc, ttl, same = ask("N2.ExAmPlE.tEsT")
if not same: problems.append("question not echoed in the client's spelling")
if upstream("n2.example.test") != 1: problems.append("mixed-case query missed the cache")

ask("nx.example.test"); ms, rc, ttl, same = ask("nx.example.test")
if rc != 3 or upstream("nx.example.test") != 1: problems.append("no-such-name not cached (rcode %d)" % rc)

# prefetch: a 10 s name asked every 0.5 s for 14 s never waits for upstream again
lat = []
t_end = time.monotonic() + 14
while time.monotonic() < t_end:
    lat.append(ask("pf.ttl10.test")[0]); time.sleep(0.5)
print("  name in use for 14 s with a 10 s TTL: %d lookups, slowest after the first %.1f ms, upstream asked %d times"
      % (len(lat), max(lat[1:]), upstream("pf.ttl10.test")))
if max(lat[1:]) > 30: problems.append("a client waited for upstream: prefetch missing (%.1f ms)" % max(lat[1:]))
if upstream("pf.ttl10.test") < 2: problems.append("name was never refreshed")

# serve stale
ask("st.ttl2.test"); ask("sf.ttl2.test")
time.sleep(3)
mode("drop")
ms, rc, ttl, same = ask("st.ttl2.test")
print("  upstream silent: expired answer after %.0f ms, TTL %s" % (ms, ttl))
if not (1500 < ms < 3000) or ttl != 30 or rc != 0: problems.append("no stale answer when upstream is silent (%.0f ms, TTL %s)" % (ms, ttl))
mode("servfail")
ms, rc, ttl, same = ask("sf.ttl2.test")
print("  upstream failing: expired answer after %.0f ms, TTL %s" % (ms, ttl))
if ms > 500 or ttl != 30 or rc != 0: problems.append("no stale answer on SERVFAIL (%.0f ms, TTL %s, rcode %d)" % (ms, ttl, rc))
ms, rc, ttl, same = ask("never-seen.example.test")
if rc != 2: problems.append("a name never seen should get upstream's SERVFAIL (rcode %d)" % rc)
mode("ok")
for p in problems: print("  -", p)
sys.exit(1 if problems else 0)
EOF

"$BIN/agctl/agctl" dns cache
"$BIN/agctl/agctl" -j dns cache > "$T/stats.json"
python3 - "$T/stats.json" <<'EOF' || fail "stats"
import json, sys
s = json.load(open(sys.argv[1]))
ok = s["hits"] >= 40 and s["prefetches"] >= 1 and s["stale"] >= 2 and s["hit_avg_us"] < s["upstream_avg_us"]
sys.exit(0 if ok else 1)
EOF
echo "DNS CACHE TEST OK"
