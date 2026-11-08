#!/bin/sh
# Phase 5 "outside" integration test. Runs a private ubusd, agd with the
# bridge plug-in (as "nobody", talking HTTP through curl) and a private
# cortex (Python) on a random port. Safe next to a live AIGW: own config,
# sockets, ports, model and key directories.
#
# Checks:
#   exit test   a person's answer on the gateway reaches cortex as a label; cortex
#               retrains with it, signs the model, and the gateway fetches it and
#               hot-loads it: same agd process, new model version in use
#   trust       updates signed by a key the gateway doesn't trust, changed after
#               signing, or not signed at all are refused; the running model stays
#   rollback    goes back to the previous update and holds the newer version off,
#               across a restart; --force installs it anyway
#   proposals   a suggestion made through the MCP server reaches the gateway and
#               waits for a person, even in enforce mode; cortex learns how it went
#   mcp         initialize, tools/list, read tools answer from the reports
#   intel       names people confirmed become a signed threat list that every
#               gateway loads; a lookup under a listed name raises "known-threat"
#               and the built-in policy acts; false alarms stay off the list
#   signed      a plug-in folder signed with cortex's key is trusted without
#               "agctl plugin allow"; changing a file takes the trust away
#   privacy     reports carry state, not traffic; "devices anonymous" drops names,
#               addresses and reasons
# Needs root, python3, curl; numpy + scikit-learn for a real retrain (otherwise
# the packaged model is republished, and the test says so). AIGW_TRAIN_PY picks
# the Python that has them.
#
#   sudo tests/outside-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
PORT=$((20000 + R % 20000))
TRAIN_PY=${AIGW_TRAIN_PY:-python3}
PIDS=""
MAC=02:00:00:00:97:10

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	[ -n "${AGD:-}" ] && kill "$AGD" 2>/dev/null || true
	sleep 0.3
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

fail() { echo "OUTSIDE TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 60 "$T"/*.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

agctl() { "$BIN/agctl/agctl" "$@"; }
cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }

# jcheck <agctl args...> -- <python expression over j> <message>
jcheck() {
	args=""
	while [ "$1" != "--" ]; do args="$args $1"; shift; done
	shift
	# shellcheck disable=SC2086
	agctl -j $args > "$T/j.json" 2>&1 || true
	python3 - "$T/j.json" "$1" <<'EOF' || { cat "$T/j.json" >&2; fail "$2"; }
import json, sys
j = json.load(open(sys.argv[1]))
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
wait_for() { # wait_for <seconds> <agctl args...> -- <python expression> <message>
	n=$1; shift
	i=0
	while :; do
		if ( jcheck "$@" ) > /dev/null 2>&1; then return 0; fi
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && jcheck "$@"
		sleep 0.5
	done
}
# cx_wait <seconds> <python expression over s (the store)> <message>
cx_wait() {
	n=$1
	i=0
	while :; do
		if PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$2" <<'EOF' 2>/dev/null; then return 0; fi
import sys
from aigw_cortex.store import Store
s = Store(sys.argv[1])
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && fail "$3"
		sleep 0.5
	done
}
model_is() { # model_is <version> <source> [seconds]
	wait_for "${3:-15}" models -- "j['models'][0]['version'] == $1 and j['models'][0]['source'] == '$2'" \
		"model should be version $1 ($2)"
}

mkdir -p "$T/sys/bridge" "$T/user" "$T/config" "$T/cortex"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"

echo "== cortex: enroll a gateway, serve"
TOKEN=$(cortex enroll home 2> "$T/enroll.log")
[ -n "$TOKEN" ] || fail "no token from enroll"
cortex serve --port "$PORT" --quiet > "$T/cortex.log" 2>&1 & PIDS="$PIDS $!"
cortex pubkey > /dev/null 2>> "$T/enroll.log"     # make the signing key now
PUB="$T/cortex/keys/cortex.pub"

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

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'

config act 'act'
	option mode 'enforce'
	option max_auto_per_hour '10'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/user'
	option user 'nobody'
	option state_dir '$T/pstate'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config plugin 'bridge'
	option interval '2'
EOF
printf '1999999999 %s 192.0.2.10 smoke-host *\n1999999999 02:00:00:00:97:11 192.0.2.11 tablet *\n' "$MAC" > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock" AIGW_NFT=/bin/true

"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; sleep 1.5; }
stop_agd() { kill "$AGD"; while kill -0 "$AGD" 2>/dev/null; do sleep 0.2; done; AGD=""; }
start_agd
AGD_PID=$AGD

echo "== the bridge waits for its settings, then starts as nobody"
wait_for 10 plugin bridge -- "j['plugins'][0]['state'] == 'waiting' and 'url' in j['plugins'][0]['why']" \
	"bridge should wait for its url"
agctl bridge "http://127.0.0.1:$PORT" > /dev/null || fail "agctl bridge <url>"
echo "$TOKEN" | agctl bridge token > /dev/null || fail "agctl bridge token"
wait_for 10 plugin bridge -- "j['plugins'][0]['state'] == 'running'" "bridge should run once configured"
pid=$(agctl -j plugin bridge | python3 -c "import json,sys; print(json.load(sys.stdin)['plugins'][0]['process']['pid'])")
[ "$(ps -o user= -p "$pid" | tr -d ' ')" = nobody ] || fail "bridge does not run as nobody"
[ "$(stat -c '%U %a' "$T/pstate/bridge")" = "nobody 700" ] || fail "bridge state dir should be nobody's, mode 700"
[ "$(stat -c '%a' "$T/pstate/bridge/curl.conf")" = 600 ] || fail "curl.conf (token) should be mode 600"
grep -q "$TOKEN" "$T/pstate/bridge/curl.conf" || fail "token not in curl.conf"
ps -o args= -p "$pid" | grep -q "$TOKEN" && fail "token visible on a command line"

echo "== the gateway checks in: state, not traffic"
cx_wait 15 "s.gateway('home')['last_seen'] > 0" "no report reached cortex"
cx_wait 10 "any(d.get('hostname') == 'smoke-host' for d in s.snapshot('home').get('devices', []))" \
	"report lacks the device list"
cx_wait 5 "set(s.snapshot('home')) <= {'bridge', 'status', 'models', 'alerts', 'decisions', 'devices'}" \
	"report carries more than state"
cx_wait 5 "s.snapshot('home')['models'][0]['source'] == 'package'" "report lacks the model in use"

echo "== an update signed by an untrusted key is refused"
cortex publish "$REPO/models/domain-risk.agm" --notes "before the key is trusted" > /dev/null
wait_for 15 models -- "\"doesn't trust\" in j['models'][0]['refused']" "untrusted update not refused"
model_is 0 package 1

echo "== a person's answer becomes a label"
"$UBUS" -s "$AIGW_UBUS" send ag.dns '{"client":"192.0.2.10","qname":"xkqjzvbwptmr.com","qtype":1}'
wait_for 10 decisions -- "any(d['action'] == 'quarantine' and d['state'] == 'active' and d['automatic'] \
	for d in j['decisions'])" "dga alert should quarantine automatically (enforce)"
id=$(agctl -j decisions | python3 -c "import json,sys; print([d['id'] for d in json.load(sys.stdin)['decisions'] if d['action'] == 'quarantine'][0])")
agctl undo "$id" > /dev/null || fail "undo"
cx_wait 15 "any(l['subject'] == 'xkqjzvbwptmr.com' and l['label'] == 0 for l in s.labels('dga'))" \
	"undoing an automatic action should reach cortex as a false-alarm label"

echo "== exit test: retrained with that answer, signed, fetched, hot-loaded"
agctl trust "$PUB" > /dev/null || fail "agctl trust"
if "$TRAIN_PY" -c "import numpy, sklearn" 2>/dev/null; then
	cortex retrain --benign-file "$REPO/tests/data/benign-sample.txt" --trees 20 --python "$TRAIN_PY" \
		> "$T/retrain.log" 2>&1 || fail "retrain"
	grep -q "1 answers from people" "$T/retrain.log" || { cat "$T/retrain.log"; fail "retrain did not use the label"; }
	echo "   retrained with numpy/scikit-learn"
else
	cortex publish "$REPO/models/domain-risk.agm" --notes "no scikit-learn here: republished" > /dev/null
	echo "   NOTE: numpy/scikit-learn missing; republished the packaged model instead of retraining"
fi
model_is 2 update 20
[ "$AGD" = "$AGD_PID" ] && kill -0 "$AGD" || fail "agd restarted"
jcheck models -- "j['models'][0]['origin'] == 'plug-in bridge' and len(j['models'][0]['signer']) == 16" \
	"model should come from the bridge, signed"
jcheck score example.com -- "'score' in j or 'verdict' in j" "the new model should answer"
jcheck status -- "j['model_versions']['domain-risk'] == 2" "status should show version 2"
cx_wait 10 "s.snapshot('home')['models'][0]['version'] == 2" "cortex should see version 2 in use"

echo "== a model changed after signing is refused"
cortex publish "$REPO/models/domain-risk.agm" --notes "tampered" > /dev/null
python3 - "$T/cortex/models/domain-risk-3.agm" <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
b[-3] ^= 1
open(p, "wb").write(bytes(b))
EOF
wait_for 15 models -- "'does not match' in j['models'][0]['refused']" "tampered update not refused"
model_is 2 update 1

echo "== unsigned and wrong-model files are refused locally too"
cp "$REPO/models/domain-risk.agm" "$T/unsigned.agm"
agctl model install "$T/unsigned.agm" > "$T/inst.out" 2>&1 && fail "unsigned model accepted"
grep -q "not signed" "$T/inst.out" || { cat "$T/inst.out"; fail "unsigned refusal should say why"; }
head -c 100 /dev/urandom > "$T/junk.agm"
agctl model install "$T/junk.agm" "$T/cortex/models/domain-risk-2.agm.sig" > "$T/inst.out" 2>&1 && fail "junk accepted"

echo "== next version loads, rollback goes back one step and holds it off"
cortex publish "$REPO/models/domain-risk.agm" --notes "v4" > /dev/null
model_is 4 update 15
agctl model rollback > /dev/null || fail "rollback"
model_is 2 update 2
jcheck models -- "j['models'][0]['held_back'] == 4" "version 4 should be held back"
sleep 5
model_is 2 update 1
stop_agd
start_agd
model_is 2 update 5
jcheck models -- "j['models'][0]['held_back'] == 4 and j['models'][0]['origin'] == 'installed earlier'" \
	"update and hold should survive a restart"
agctl model install "$T/cortex/models/domain-risk-4.agm" --force > /dev/null || fail "install --force"
model_is 4 update 2

echo "== MCP: an assistant reads the home and proposes; a person decides"
cx_wait 10 "s.snapshot('home')['models'][0]['version'] == 4" "cortex should see version 4"
cat > "$T/mcp.in" <<'EOF'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_devices","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"propose_action","arguments":{"action":"quarantine","target":"smoke-host","reason":"it talks to a known scam site"}}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"propose_action","arguments":{"action":"format-disk","target":"smoke-host","reason":"x"}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"model_status","arguments":{}}}
EOF
cortex mcp < "$T/mcp.in" > "$T/mcp.out" 2>> "$T/cortex.log" || fail "mcp server"
python3 - "$T/mcp.out" <<'EOF' || { cat "$T/mcp.out" >&2; fail "MCP answers"; }
import json, sys
r = {m["id"]: m for m in map(json.loads, open(sys.argv[1]))}
assert set(r) == {1, 2, 3, 4, 5, 6}, "one answer per request, none for the notification"
assert r[1]["result"]["serverInfo"]["name"] == "aigw-cortex"
tools = {t["name"]: t for t in r[2]["result"]["tools"]}
assert {"list_devices", "propose_action", "gateway_status"} <= set(tools)
assert [n for n, t in tools.items() if not t["annotations"]["readOnlyHint"]] == ["propose_action"]
assert "smoke-host" in r[3]["result"]["content"][0]["text"]
assert not r[4]["result"]["isError"] and "approve" in r[4]["result"]["content"][0]["text"]
assert r[5]["result"]["isError"], "unknown action must be refused"
assert '"version": 4' in r[6]["result"]["content"][0]["text"]
EOF
wait_for 15 decisions -- "any(d['state'] == 'proposed' and d['origin'] == 'plug-in bridge' and \
	'AI assistant' in d['reason'] for d in j['decisions'])" "MCP proposal should wait on the gateway (enforce mode)"
cx_wait 10 "any(p['state'] == 'waiting for approval' and p['decision'] > 0 for p in s.proposals())" \
	"cortex should learn the proposal is waiting"

echo "== shared threat intelligence: a confirmed name protects every home"
cortex label evil-cnc.example risky > /dev/null
cortex label kofamuveliza.top risky > /dev/null
cortex publish-intel > "$T/intel.out" || fail "publish-intel"
grep -q "with 2 names" "$T/intel.out" || { cat "$T/intel.out"; fail "intel should hold the 2 confirmed names \
(and not xkqjzvbwptmr.com, a false alarm)"; }
wait_for 15 models -- "any(m['name'] == 'threat-intel' and m['version'] == 1 and m['entries'] == 2 and \
	m['source'] == 'update' for m in j['models'])" "threat list not loaded"
"$UBUS" -s "$AIGW_UBUS" send ag.dns '{"client":"192.0.2.11","qname":"www.evil-cnc.example","qtype":1}'
wait_for 10 alerts -- "any(a['kind'] == 'known-threat' and a['subject'] == 'evil-cnc.example' for a in j['alerts'])" \
	"a lookup under a listed name should raise a known-threat alert"
wait_for 5 decisions -- "any(d['action'] == 'quarantine' and d['origin'] == 'policy known_threat' and \
	d['device'] == 'tablet' for d in j['decisions'])" \
	"the built-in known_threat policy should act"
agctl model rollback threat-intel > /dev/null || fail "rolling back the list"
wait_for 5 models -- "any(m['name'] == 'threat-intel' and not m['loaded'] and m['held_back'] == 1 for m in j['models'])" \
	"rolling back the only list should unload it and hold it off"

echo "== a signed plug-in is trusted without 'agctl plugin allow'"
mkdir -p "$T/src/signed-dev"
cat > "$T/src/signed-dev/manifest.json" <<'EOF'
{"name":"signed-dev","kind":"sensor","version":"1.0","exec":"run.sh","caps":["devices.read"]}
EOF
printf '#!/bin/sh\nwhile read -r l; do :; done\n' > "$T/src/signed-dev/run.sh"
chmod 755 "$T/src/signed-dev/run.sh"
cortex sign-plugin "$T/src/signed-dev" > /dev/null || fail "sign-plugin"
agctl plugin add "$T/src/signed-dev" > /dev/null 2>&1 || fail "plugin add"
wait_for 15 plugin signed-dev -- "j['plugins'][0]['state'] == 'running' and j['plugins'][0]['trusted'] and \
	len(j['plugins'][0]['signed_by']) == 16 and not j['plugins'][0]['waiting_for_ok']" "signed plug-in should be trusted"
echo "# changed" >> "$T/user/signed-dev/run.sh"
agctl plugin disable signed-dev > /dev/null && agctl plugin enable signed-dev > /dev/null
wait_for 15 plugin signed-dev -- "j['plugins'][0]['state'] == 'running' and not j['plugins'][0]['trusted'] and \
	'does not match' in j['plugins'][0]['signature'] and 'devices.read' in j['plugins'][0]['waiting_for_ok']" \
	"a changed signed plug-in should lose its trust"

echo "== devices anonymous: no names, addresses or reasons leave the home"
agctl plugin config bridge devices anonymous > /dev/null || fail "plugin config"
sleep 1
cx_wait 15 "all('mac' not in d and 'hostname' not in d and len(d.get('id', '')) == 12 \
	for d in s.snapshot('home')['devices']) and s.snapshot('home')['devices']" "anonymous devices"
cx_wait 5 "all('reason' not in a and 'device' not in a for a in s.snapshot('home')['alerts'])" "anonymous alerts"

echo "OUTSIDE TEST OK"
