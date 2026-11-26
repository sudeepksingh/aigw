#!/bin/sh
# Fleet learning test: three homes that share (three agd instances, each with
# its own ubusd, config, state and bridge plug-in running as "nobody"), a
# fourth new home that doesn't share, and a private cortex running the
# reference aggregator (k 3). Safe next to a live AIGW: everything lives in a
# temp dir. Outcomes are fed through the test hook (AIGW_TEST_OUTCOMES), as act.c
# and proactive.c feed them for real (proactive-test.sh covers that path).
#
#   homes A-C  a phone hopping between the bands: keeping it on 2.4 GHz helped
#              4 times in 20, waiting cleared it 14 times in 20; home A alone
#              also has smart queueing outcomes (one home: below k)
#   home D     new, shares nothing: gets the priors all the same
#
# Checks:
#   sharing    each sharing home's summary reaches cortex through the bridge,
#              nothing from home D (fleet.share 0, the default)
#   priors     cortex adds them up, publishes signed fleet-priors; every home
#              checks the signature and loads it; agctl fleet and the data model
#              say what was shared and received
#   blend      fleet rank shows waiting above 2.4 GHz for the hopping phone, from
#              the fleet in home D (nothing of its own) and blended in home A
#   k          home A's own smart queueing bucket is not in the priors
#   privacy    no MAC, IP or device name reaches cortex; a summary carrying a MAC
#              or an unknown field is refused (400)
#   cold start normals other homes sent: home D (learning) judges warnings
#              against the fleet's normal (ColdStart)
#   tamper     a priors file changed after signing, and a signed one with a
#              bucket below its own k, are refused
#
#   sudo tests/fleet-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" fleetlearn plugins
REPO=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
chmod 755 "$T"
R=$(od -An -N2 -tu2 /dev/urandom | tr -d ' \n')
PORT=$((20000 + R % 20000))
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() { echo "FLEET TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 30 "$T"/*.log "$T"/h*/agd.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }
h() { H=$1; shift; AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" "$BIN/agctl/agctl" "$@"; }
hcheck() {   # hcheck <home> <agctl args...> -- <python expression over j> <message>
	home=$1; shift
	args=""
	while [ "$1" != "--" ]; do args="$args $1"; shift; done
	shift
	# shellcheck disable=SC2086
	h "$home" -j $args > "$T/j.json" 2>&1 || true
	python3 - "$T/j.json" "$1" <<'EOF' || { cat "$T/j.json" >&2; fail "$home: $2"; }
import json, sys
j = json.load(open(sys.argv[1]))
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
hwait() {
	n=$1; shift
	i=0
	while :; do
		if ( hcheck "$@" ) > /dev/null 2>&1; then return 0; fi
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && hcheck "$@"
		sleep 0.5
	done
}
cx() {
	PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$1" <<'EOF'
import json, sys
from aigw_cortex.store import Store
s = Store(sys.argv[1])
sys.exit(0 if eval(sys.argv[2]) else 1)
EOF
}
cx_wait() {
	i=0
	while ! cx "$2" 2>/dev/null; do
		i=$((i + 1))
		[ $i -ge $(($1 * 2)) ] && fail "$3"
		sleep 0.5
	done
}
outcome() {   # outcome <home> <code> <action> <result> <times> [mac]
	"$UBUS" -s "$T/$1/ubus.sock" call ag.fleet outcome \
		"{\"code\":\"$2\",\"action\":\"$3\",\"result\":\"$4\",\"times\":$5,\"mac\":\"${6:-}\"}" |
		grep -q '"ok": true' || fail "$1: outcome $2 $3 $4"
}
sign() {
	PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$1" <<'EOF'
import sys
from aigw_cortex import usign
from aigw_cortex.__main__ import signing_key
sec, _ = signing_key(sys.argv[1])
open(sys.argv[2] + ".sig", "w").write(usign.sign(sec, open(sys.argv[2], "rb").read()))
EOF
}

mkdir -p "$T/sys/bridge" "$T/cortex"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"

echo "== cortex: the reference aggregator, k 3"
cortex serve --port "$PORT" --quiet --no-fed --fleet-k 3 --fleet-gap 0 > "$T/cortex.log" 2>&1 &
PIDS="$PIDS $!"
cortex pubkey > /dev/null 2> "$T/enroll.log"
PUB="$T/cortex/keys/cortex.pub"

N=0
for H in hA hB hC hD; do
	N=$((N + 1))
	SHARE=1; [ $H = hD ] && SHARE=0
	CARE=0; [ $H = hD ] && CARE=1
	mkdir -p "$T/$H/config" "$T/$H/user"
	cat > "$T/$H/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/$H/leases'
	option state_file '$T/$H/state.json'
	option model_dir '$REPO/models'

config lan 'lan'
	option iface 'aigw-none0'

config flow 'flow'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/$H/hostapd'
	option keys '$T/$H/wifi-keys'

config diag 'diag'
	option targets '127.0.0.1'

config care 'care'
	option enabled '$CARE'
	option interval '86400'
	option speed_hour '25'

config fleet 'fleet'
	option share '$SHARE'
	option k '3'
	option interval '1'
	option line 'fibre'

config act 'act'
	option dns_policy '$T/$H/dns-policy'

config plugins 'plugins'
	option dir '$T/sys'
	option user_dir '$T/$H/user'
	option user 'nobody'
	option state_dir '$T/$H/pstate'

config models 'models'
	option dir '$T/$H/models'
	option keys '$T/$H/keys.d'

config plugin 'bridge'
	option interval '2'
	option devices 'anonymous'
	option alert_names '0'
	option fed '0'
EOF
	printf '1999999999 02:00:00:00:77:1%s 192.0.2.1%s pixel-%s *\n' "$N" "$N" "$H" > "$T/$H/leases"
	"$UBUSD" -s "$T/$H/ubus.sock" > "$T/$H/ubusd.log" 2>&1 & PIDS="$PIDS $!"
	sleep 0.3
	( export AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" AIGW_NFT=/bin/true AIGW_TEST_OUTCOMES=1
	  exec "$BIN/agd/agd" >> "$T/$H/agd.log" 2>&1 ) & PIDS="$PIDS $!"
	TOKEN=$(cortex enroll "$H" 2>> "$T/enroll.log")
	sleep 1
	h $H trust "$PUB" > /dev/null || fail "$H: trust"
	h $H bridge "http://127.0.0.1:$PORT" > /dev/null || fail "$H: agctl bridge"
	echo "$TOKEN" | h $H bridge token > /dev/null || fail "$H: bridge token"
done
for H in hA hB hC hD; do
	hwait 15 $H plugin bridge -- "j['plugins'][0]['state'] == 'running' and \
		'fleet.share' in j['plugins'][0].get('granted', ['fleet.share'])" "bridge should run with fleet.share"
done
hcheck hD fleet -- "j['enabled'] and not j['share'] and not j['received']['loaded']" "home D: on, not sharing, nothing yet"
h hD fleet | grep -q "nothing leaves this home" || fail "agctl fleet should say home D shares nothing"

echo "== outcomes: in homes A-C waiting clears a hopping phone far more often than 2.4 GHz does"
for H in hA hB hC; do
	mac=$(cut -d' ' -f2 "$T/$H/leases")
	outcome $H WIFI_HOPPING stay24 helped 4 "$mac"
	outcome $H WIFI_HOPPING stay24 no-change 14 "$mac"
	outcome $H WIFI_HOPPING stay24 worse 2 "$mac"
	outcome $H WIFI_HOPPING wait helped 14 "$mac"
	outcome $H WIFI_HOPPING wait no-change 6 "$mac"
done
outcome hA LINE_FULL sqm helped 9
hcheck hA fleet -- "j['outcomes'] == 49 and any(c['code'] == 'WIFI_HOPPING' and c['action'] == 'wait' and \
	c['bucket'].split('/')[3] == 'phone' and c['bucket'].endswith('/fibre') and c['tried'] == 20 for c in j['local'])" \
	"home A's own outcomes, bucketed (a phone, fibre)"

echo "== a summary from each sharing home reaches cortex; priors come back signed"
cx_wait 30 "len(s.q('SELECT * FROM fleet_summaries')) == 3" "three summaries (A-C) should reach cortex"
cx "not s.q(\"SELECT 1 FROM fleet_summaries f JOIN gateways g ON g.id = f.gateway WHERE g.name = 'hD'\")" ||
	fail "home D shares nothing (fleet.share 0)"
cx_wait 20 "[m for m in s.latest_models() if m['name'] == 'fleet-priors']" "cortex should publish fleet-priors"
for H in hA hB hC hD; do
	hwait 30 $H models -- "any(m['name'] == 'fleet-priors' and m['loaded'] and m['source'] == 'update' and \
		len(m['signer']) == 16 for m in j['models'])" "fleet-priors should be loaded, signed"
	hwait 10 $H fleet -- "j['received']['loaded'] and j['received']['buckets'] > 0 and j['received']['k'] == 3" \
		"agctl fleet should say what was received"
done
hcheck hA fleet -- "j['shared']['at'] > 0 and j['shared']['buckets'] >= 3" "home A says what it shared"
h hA dm get Device.X_AIGW_.FleetLearning. > "$T/dm.txt" || fail "dm get FleetLearning."
grep -q "FleetLearning.Share = true" "$T/dm.txt" && grep -q "FleetLearning.PriorsVersion = [1-9]" "$T/dm.txt" ||
	{ cat "$T/dm.txt"; fail "the data model should show sharing and the priors"; }

echo "== the blend: waiting first for the hopping phone, from the fleet (D) and blended (A)"
hcheck hD fleet rank WIFI_HOPPING pixel-hD -- "{a['action']: a for a in j['actions']}['wait']['score'] > {a['action']: a for a in j['actions']}['stay24']['score'] + 0.2 and \
	{a['action']: a for a in j['actions']}['wait']['from_fleet'] and \
	{a['action']: a for a in j['actions']}['wait']['local_tried'] == 0" "home D: waiting ranks above 2.4 GHz from the fleet"
hcheck hA fleet rank WIFI_HOPPING pixel-hA -- "0 < {a['action']: a for a in j['actions']}['wait']['fleet_share'] < 1 and \
	{a['action']: a for a in j['actions']}['wait']['local_tried'] == 20" "home A: the fleet's and its own blended"
h hD fleet rank WIFI_HOPPING pixel-hD | grep -q "fleet:" || fail "agctl fleet rank in words"

echo "== k-anonymity: home A's smart queueing (one home) is not in the priors"
P=$(ls "$T/cortex/models/"fleet-priors-*.priors | sort -V | tail -1)
grep -q "LINE_FULL" "$P" && fail "one home's bucket was published"
hcheck hA fleet rank LINE_FULL -- "all(not a['from_fleet'] for a in j['actions'])" "home A: LINE_FULL only its own"

echo "== privacy: nothing that names a home or a device reached cortex"
for H in hA hB hC; do
	for n in "pixel-$H" "192.0.2.1"; do
		grep -raqF -- "$n" "$T/cortex/models" "$T/cortex.log" && fail "'$n' reached cortex"
	done
done
cx "not [r for r in s.q('SELECT body FROM fleet_summaries') if '02:00:00' in r['body'] or 'pixel' in r['body'] or '192.0.2' in r['body']]" ||
	fail "a summary carried a MAC, a name or an address"
TOK=$(cortex enroll intruder 2>> "$T/enroll.log")
post() { curl -s -o "$T/post.out" -w '%{http_code}' -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
	--data-binary "$1" "http://127.0.0.1:$PORT/v1/fleet/summary"; }
[ "$(post '{"v":1,"line":"fibre","outcomes":[["WIFI_HOPPING","stay24","5/80+/high/02:00:00:00:77:11/evening/fibre",4,1,0,0]]}')" = 400 ] ||
	fail "a MAC in a bucket should be refused"
[ "$(post '{"v":1,"line":"fibre","outcomes":[],"hostname":"pixel"}')" = 400 ] || fail "an unknown field should be refused"
curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary '{}' "http://127.0.0.1:$PORT/v1/fleet/summary" | grep -q 401 ||
	fail "no token: 401"

echo "== cold start: three more homes' normals; home D, still learning, warns against the fleet's normal"
for x in n1 n2 n3; do
	TOK=$(cortex enroll "$x" 2>> "$T/enroll.log")
	[ "$(post '{"v":1,"line":"fibre","outcomes":[],"normals":[[1,0,18,3],[1,1,20,3],[1,2,22,3],[1,3,30,4],[3,0,12,3],[3,1,12,3],[3,2,13,3],[3,3,15,4]]}')" = 200 ] ||
		{ cat "$T/post.out"; fail "$x: a summary of normals"; }
done
hwait 30 hD fleet -- "j['received']['normals'] >= 8 and j['received']['cold_start']" \
	"home D should judge warnings against the fleet's normal while learning"
h hD dm get Device.X_AIGW_.FleetLearning.ColdStart | grep -q "= true" || fail "ColdStart in the data model"
hcheck hD care -- "j['early_warning']['fleet_normal']" "agctl care: warnings use the fleet's normal"
h hD fleet | grep -q "judged against the fleet's normal" || fail "agctl fleet should say warnings use the fleet's normal"
P=$(ls "$T/cortex/models/"fleet-priors-*.priors | sort -V | tail -1)
grep -q "^n fibre 1 3 3 30 " "$P" || { cat "$P"; fail "the evening internet delay normal of three homes"; }

echo "== tamper: a changed priors file, and a signed one below its own k, are refused"
sed 's/^o WIFI_HOPPING wait \(\S*\) \([0-9]*\) \([0-9]*\) [0-9]* /o WIFI_HOPPING wait \1 \2 \3 0 /' "$P" > "$T/t.priors"
cmp -s "$P" "$T/t.priors" && fail "the tamper step changed nothing"
cp "$P.sig" "$T/t.priors.sig"
sed -i 's/^# version [0-9]*/# version 990/' "$T/t.priors"
h hA model install "$T/t.priors" > "$T/inst.out" 2>&1 && fail "a changed priors file was accepted"
grep -q "does not match" "$T/inst.out" || { cat "$T/inst.out"; fail "refusal should say the signature doesn't match"; }
printf '# aigw-priors fleet-priors\n# version 991\n# k 3\n# homes 9\no WIFI_HOPPING wait */*/*/*/*/* 1 20 20 0 0\n' > "$T/k.priors"
sign "$T/k.priors"
h hA model install "$T/k.priors" > "$T/inst.out" 2>&1 && fail "a bucket of one home was accepted"
grep -q "fewer than its k" "$T/inst.out" || { cat "$T/inst.out"; fail "refusal should say why"; }
hcheck hA fleet -- "j['received']['version'] < 990" "the fleet's own priors stay in use"

echo "FLEET TEST OK"
