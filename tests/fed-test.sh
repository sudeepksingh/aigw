#!/bin/sh
# Federated learning test: three homes (three agd instances, each with its
# own ubusd, config, state and bridge plug-in running as "nobody") and a
# private cortex. Safe next to a live AIGW: everything lives in a temp dir.
#
#   home A   a Polish home: a dga alert on bydgoszcz.pl (few vowels, long
#            consonant runs) is acted on automatically; you undo it (a false
#            alarm), and label two more Polish names safe
#   home B   labels three other Polish names safe
#   home C   labels three machine-generated names as threats
#
# (CloudFront hashes like d2xqm7kjv0bh4f.cloudfront.net are a false alarm the
# layer can't learn safely: in the 12 features they look like a generated
# name under a subdomain, so every gateway refuses such a layer. They belong
# on an allow list.)
#
# Checks:
#   rounds    each home trains on its own answers and sends 14 noisy numbers;
#             cortex averages three homes and publishes a signed layer; every
#             home checks it (anchor names) and uses it without a restart
#   learning  the Polish false alarms score lower in every home, also a Polish
#             name nobody answered about; real threats still alert, also under
#             a subdomain
#   privacy   no answered name reaches cortex (database, published files, log);
#             with alert_names 0 reports carry no alert subjects or reasons;
#             each home reports the privacy it spent (epsilon)
#   guards    a change for a closed round, a malformed one and a second one from
#             the same home are refused; a layer that would stop flagging the
#             anchors is refused by the gateway
#
#   sudo tests/fed-test.sh [build-dir]
set -eu

BIN=$(cd "${1:-build}" && pwd)
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

fail() { echo "FED TEST FAIL: $*" >&2; echo "--- logs" >&2; tail -n 40 "$T"/*.log "$T"/h*/agd.log >&2 2>/dev/null || true; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
for t in python3 curl; do command -v $t >/dev/null || fail "missing $t"; done
id nobody >/dev/null 2>&1 || fail "no 'nobody' user"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)

cortex() { PYTHONPATH="$REPO/cortex" python3 -m aigw_cortex --data "$T/cortex" "$@"; }
# h <home> <agctl args...>
h() { H=$1; shift; AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" "$BIN/agctl/agctl" "$@"; }

# hcheck <home> <agctl args...> -- <python expression over j> <message>
hcheck() {
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
hwait() { # hwait <seconds> <home> <agctl args...> -- <expr> <message>
	n=$1; shift
	i=0
	while :; do
		if ( hcheck "$@" ) > /dev/null 2>&1; then return 0; fi
		i=$((i + 1))
		[ $i -ge $((n * 2)) ] && hcheck "$@"
		sleep 0.5
	done
}
cx() { # cx <python expression over s (the store)>: exit status
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
score() { # score <home> <name>: the score agd gives now
	h "$1" -j score "$2" | python3 -c "import json,sys; print(json.load(sys.stdin)['score'])"
}

mkdir -p "$T/sys/bridge" "$T/cortex"
cp "$REPO/plugins/bridge/manifest.json" "$BIN/plugins/ag-bridge" "$T/sys/bridge/"

echo "== cortex: three gateways, rounds of three homes, noise 0.1"
cortex serve --port "$PORT" --quiet --fed-min-homes 3 --fed-gap 0 --fed-noise 0.1 > "$T/cortex.log" 2>&1 &
PIDS="$PIDS $!"
cortex pubkey > /dev/null 2> "$T/enroll.log"
PUB="$T/cortex/keys/cortex.pub"

N=0
for H in hA hB hC; do
	N=$((N + 1))
	mkdir -p "$T/$H/config" "$T/$H/user"
	# home A: a privacy budget that allows three rounds at noise 0.1 (epsilon 98, 168, 233, then 296)
	BUDGET=0; [ $H = hA ] && BUDGET=250
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

config alerts 'alerts'
	option dga_threshold '0.85'
	option fed_noise '0.1'
	option fed_budget '$BUDGET'

config act 'act'
	option mode 'enforce'
	option max_auto_per_hour '10'
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
EOF
	printf '1999999999 02:00:00:00:98:1%s 192.0.2.1%s laptop-%s *\n' "$N" "$N" "$H" > "$T/$H/leases"
	"$UBUSD" -s "$T/$H/ubus.sock" > "$T/$H/ubusd.log" 2>&1 & PIDS="$PIDS $!"
	sleep 0.3
	( export AIGW_UCI_CONFDIR="$T/$H/config" AIGW_UBUS="$T/$H/ubus.sock" AIGW_NFT=/bin/true
	  exec "$BIN/agd/agd" >> "$T/$H/agd.log" 2>&1 ) & PIDS="$PIDS $!"
	TOKEN=$(cortex enroll "$H" 2>> "$T/enroll.log")
	sleep 1
	h $H trust "$PUB" > /dev/null || fail "$H: trust"
	h $H bridge "http://127.0.0.1:$PORT" > /dev/null || fail "$H: agctl bridge"
	echo "$TOKEN" | h $H bridge token > /dev/null || fail "$H: bridge token"
done
for H in hA hB hC; do
	hwait 15 $H plugin bridge -- "j['plugins'][0]['state'] == 'running' and \
		'fed.train' in j['plugins'][0].get('granted', ['fed.train'])" "bridge should run with fed.train"
	hcheck $H fed -- "j['enabled'] and j['examples'] == 0 and j['rounds'] == 0" "fed status before any answer"
done

CF1=bydgoszcz.pl       # A: alert, undone
CF2=pszczyna.pl        # A: labelled
CF3=krzyzowka.pl       # A: labelled
CF4=chrzaszcz.pl       # B
CF5=brzeszczot.pl      # B
CF6=przyczyna.pl       # B
CF7=szczebrzeszyn.pl   # nobody: does the layer generalise?
TH1=tvqkrzmn.com; TH2=xjfghtwq.org; TH3=jj3k9mq2zv.io   # C: threats

for n in $CF1 $CF7; do
	s=$(score hA $n)
	python3 -c "import sys; sys.exit(0 if $s >= 0.85 else 1)" || fail "$n should alert with the packaged trees (score $s)"
done
BEFORE_CF1=$(score hA $CF1)
BEFORE_CF7=$(score hA $CF7)

echo "== home A: a Polish name raises a dga alert; undoing the action makes it an example"
"$UBUS" -s "$T/hA/ubus.sock" send ag.dns "{\"client\":\"192.0.2.11\",\"qname\":\"$CF1\",\"qtype\":1}"
hwait 10 hA decisions -- "any(d['action'] == 'quarantine' and d['state'] == 'active' and d['automatic'] \
	for d in j['decisions'])" "dga alert should quarantine automatically"
id=$(h hA -j decisions | python3 -c "import json,sys; print([d['id'] for d in json.load(sys.stdin)['decisions'] if d['action'] == 'quarantine'][0])")
h hA undo "$id" > /dev/null || fail "undo"
hcheck hA fed -- "any(e['name'] == '$CF1' and e['label'] == 'safe' and e['source'] == 'decision' for e in j['list'])" \
	"undoing an automatic action should become a 'safe' example"
h hA label $CF2 safe > /dev/null && h hA label $CF3 safe > /dev/null || fail "label (A)"
h hA label "not a name" safe > /dev/null 2>&1 && fail "a non-name should be refused"
h hB label $CF4 safe > /dev/null && h hB label $CF5 safe > /dev/null && h hB label $CF6 safe > /dev/null || fail "label (B)"
h hC label $TH1 threat > /dev/null && h hC label $TH2 threat > /dev/null && h hC label $TH3 threat > /dev/null ||
	fail "label (C)"
for H in hA hB hC; do hcheck $H fed -- "j['examples'] == 3" "three examples each"; done

echo "== a round: three homes train here, cortex averages and publishes a signed layer"
cx_wait 30 "any(r['state'] == 'published' and r['homes'] == 3 for r in s.q('SELECT * FROM fed_rounds'))" \
	"cortex should publish a layer from three homes"
for H in hA hB hC; do
	hwait 20 $H models -- "any(m['name'] == 'domain-adapter' and m['loaded'] and m['version'] >= 1 and \
		m['source'] == 'update' and len(m['signer']) == 16 for m in j['models'])" "the layer should be loaded, signed"
	hcheck $H fed -- "j['rounds'] >= 1 and isinstance(j['epsilon'], float) and 0 < j['epsilon'] < 1e6 and \
		j['adapter']['loaded']" "fed status should count rounds and the privacy spent"
done
h hA fed > "$T/fed.txt" || fail "agctl fed"
grep -q "ε = " "$T/fed.txt" || { cat "$T/fed.txt"; fail "agctl fed should show epsilon"; }

echo "== learning: the false alarms score lower in every home, threats still alert"
# (qpwzkxvtrm.net: a generated name no home answered about, also tried under www.)
sleep 6       # a couple more rounds
for H in hA hB hC; do
	a=$(score $H $CF1); g=$(score $H $CF7)
	python3 -c "import sys; sys.exit(0 if $a < $BEFORE_CF1 - 0.03 else 1)" ||
		fail "$H: $CF1 should score lower (before $BEFORE_CF1, now $a)"
	python3 -c "import sys; sys.exit(0 if $g < $BEFORE_CF7 - 0.03 else 1)" ||
		fail "$H: $CF7 (nobody's answer) should score lower too (before $BEFORE_CF7, now $g)"
	# anchors, and a generated name nobody answered about, alert everywhere; the
	# names home C called threats are protected in home C (its gateway refuses a
	# layer that would let them through; other homes don't know them)
	names="xkqjzvbwptmr.com a8f3k2l9q0zx.info qpwzkxvtrm.net"
	[ $H = hC ] && names="$names $TH1 $TH2 $TH3"
	for n in $names; do
		t=$(score $H $n)
		python3 -c "import sys; sys.exit(0 if $t >= 0.85 else 1)" || fail "$H: $n should still alert (score $t)"
	done
	hcheck $H score www.qpwzkxvtrm.net -- "j['score'] >= j.get('trees', j['score']) - 0.05" \
		"a generated name under www. should not score lower than the trees give it"
	echo "   $H: $CF1 $BEFORE_CF1 -> $a, $CF7 (unanswered) $BEFORE_CF7 -> $g"
done
hcheck hA score $CF1 -- "'trees' in j and j['trees'] > j['score']" "score should show the trees' own score too"

echo "== home A's privacy budget: it stops taking part after three rounds"
hwait 20 hA fed -- "j['rounds'] == 3 and j['epsilon'] < 250" "home A should take part in exactly three rounds"
grep -q "privacy budget reached" "$T/hA/agd.log" || fail "home A should say its budget is reached"

echo "== a layer that would let home C's own threat through is refused there, without naming it"
sign() {
	PYTHONPATH="$REPO/cortex" python3 - "$T/cortex" "$1" <<'EOF'
import sys
from aigw_cortex import usign
from aigw_cortex.__main__ import signing_key
sec, _ = signing_key(sys.argv[1])
open(sys.argv[2] + ".sig", "w").write(usign.sign(sec, open(sys.argv[2], "rb").read()))
EOF
}
# longer names up, shorter down: anchors (12+ letters) keep alerting, tvqkrzmn (8) drops
cat > "$T/short.adapter" <<'EOF'
# aigw-adapter domain-adapter
# version 998
center 10 3 0.05 0.35 3 0.1 2.2 -3 -4.5 0.5 0.85 0.05
scale 4 0.6 0.15 0.15 2.5 0.5 0.5 0.6 1.2 2 0.12 0.25
weights 1 2 0 0 0 0 0 0 0 0 0 0 0 0
EOF
sign "$T/short.adapter"
h hC model install "$T/short.adapter" > "$T/inst.out" 2>&1 && fail "home C accepted a layer that drops its own threat"
grep -q "a name you said is a threat" "$T/inst.out" || { cat "$T/inst.out"; fail "refusal should say why"; }
grep -q "$TH1" "$T/inst.out" && fail "the refusal (reported to cortex) should not name the threat"
sleep 5        # the next reports carry the refusal

echo "== privacy: no answered name reached cortex"
for n in $CF1 $CF2 $CF3 $CF4 $CF5 $CF6 $TH1 $TH2 $TH3 laptop-h; do
	grep -raqF -- "$n" "$T/cortex" "$T/cortex.log" && fail "'$n' reached cortex"
done
cx "all('subject' not in a and 'reason' not in a for g in ('hA', 'hB', 'hC') for a in s.snapshot(g).get('alerts', []))" ||
	fail "alert_names 0: reports should carry no alert subjects or reasons"
cx "all(len(json.loads(u['delta'])) == 14 for u in s.q('SELECT * FROM fed_updates'))" ||
	fail "an update should be 14 numbers"
cx "all(u['noise'] >= 0.1 for u in s.q('SELECT * FROM fed_updates'))" || fail "updates should say their noise"

echo "== guards"
TOK=$(cortex enroll intruder 2>> "$T/enroll.log")
post() { curl -s -o "$T/post.out" -w '%{http_code}' -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
	--data-binary "$1" "http://127.0.0.1:$PORT/v1/fed/update"; }
open=$(curl -s -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/v1/fed" | python3 -c "import json,sys; print(json.load(sys.stdin)['round'])")
Z='[0,0,0,0,0,0,0,0,0,0,0,0,0,0]'
[ "$(post "{\"round\":1,\"delta\":$Z,\"n\":3,\"noise\":1,\"clip\":1}")" = 409 ] || fail "a closed round should be refused"
[ "$(post "{\"round\":$open,\"delta\":[1,2],\"n\":3,\"noise\":1,\"clip\":1}")" = 400 ] || fail "a short delta should be refused"
[ "$(post "{\"round\":$open,\"delta\":$Z,\"n\":0,\"noise\":1,\"clip\":1}")" = 400 ] || fail "n = 0 should be refused"
[ "$(post "{\"round\":$open,\"delta\":$Z,\"n\":3,\"noise\":1,\"clip\":1}")" = 200 ] || { cat "$T/post.out"; fail "a good change"; }
[ "$(post "{\"round\":$open,\"delta\":$Z,\"n\":3,\"noise\":1,\"clip\":1}")" = 409 ] || fail "a second change should be refused"
curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/fed" | grep -q 401 || fail "no token: 401"

# a layer that stops flagging machine-generated names is refused by the gateway
cat > "$T/bad.adapter" <<'EOF'
# aigw-adapter domain-adapter
# version 999
center 10 3 0.05 0.35 3 0.1 2.2 -3 -4.5 0.5 0.85 0.05
scale 4 0.6 0.15 0.15 2.5 0.5 0.5 0.6 1.2 2 0.12 0.25
weights 1 0 0 0 0 0 0 0 0 0 0 0 0 -8
EOF
sign "$T/bad.adapter"
h hA model install "$T/bad.adapter" > "$T/inst.out" 2>&1 && fail "a layer that flags nothing was accepted"
grep -q "would no longer flag" "$T/inst.out" || { cat "$T/inst.out"; fail "refusal should say why"; }

echo "== forgetting an answer"
h hB fed forget $CF4 > /dev/null || fail "forget"
hcheck hB fed -- "j['examples'] == 2" "forget one example"

echo "FED TEST OK"
