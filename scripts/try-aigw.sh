#!/bin/sh
# Try AIGW OS in five minutes (docs/try-it.md): download the demo image, boot
# it in QEMU, and print where to look. The image is OpenWRT (x86-64) with
# AIGW OS and a simulated home and fleet: demo data, labelled as such.
#
#   sh try-aigw.sh                     the latest release's demo image
#   sh try-aigw.sh --version 0.41.0    a given release
#   sh try-aigw.sh --image FILE        an image you built (platform/openwrt/build-demo-image.sh)
#   sh try-aigw.sh --check             boot, check that the demo answers, stop (CI)
#
# Needs qemu-system-x86_64, curl and gzip: on Debian/Ubuntu
#   sudo apt install qemu-system-x86 curl
# on macOS (Homebrew)
#   brew install qemu
# Windows: in WSL2 (Ubuntu), as on Linux; the browser on Windows reaches it.
#
# On this computer only (127.0.0.1): the dashboard on DASH_PORT (8090), the
# router's LuCI on LUCI_PORT (8080), its shell on SSH_PORT (2222). The VM has
# no network outside: QEMU's restricted user network, nothing leaves it. It
# runs from a snapshot: nothing is kept when it stops (Ctrl+C).
set -eu

REPO=sudeepksingh/aigw
NAME=aigw-os-demo-x86-64.img.gz
CACHE=${AIGW_DEMO_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-demo}
DASH_PORT=${DASH_PORT:-8090}
LUCI_PORT=${LUCI_PORT:-8080}
SSH_PORT=${SSH_PORT:-2222}
MEM=${MEM:-512}
BOOT_S=${BOOT_S:-600}           # how long to wait for the demo (emulated, without KVM or HVF, is slow)

IMAGE="" VERSION="" CHECK=0
while [ $# -gt 0 ]; do
	case $1 in
	--image) IMAGE=${2:?--image FILE}; shift 2 ;;
	--version) VERSION=${2:?--version X.Y.Z}; VERSION=${VERSION#v}; shift 2 ;;
	--check) CHECK=1; shift ;;
	-h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "try-aigw: unknown option $1 (--help)" >&2; exit 2 ;;
	esac
done

die() { echo "try-aigw: $*" >&2; exit 1; }
say() { echo "==> $*"; }
for t in qemu-system-x86_64 curl gzip; do
	command -v $t >/dev/null || die "missing $t (Debian/Ubuntu: sudo apt install qemu-system-x86 curl; macOS: brew install qemu)"
done
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

mkdir -p "$CACHE"
RAW="$CACHE/aigw-os-demo-x86-64.img"

# ---- the image
if [ -n "$IMAGE" ]; then
	[ -f "$IMAGE" ] || die "no such image: $IMAGE"
	GZ=$IMAGE
else
	if [ -n "$VERSION" ]; then BASE="https://github.com/$REPO/releases/download/v$VERSION"
	else BASE="https://github.com/$REPO/releases/latest/download"; fi
	say "the demo image's checksum (${VERSION:-latest release})"
	curl -fsSL --retry 3 -o "$CACHE/$NAME.sha256.new" "$BASE/$NAME.sha256" ||
		die "could not fetch $BASE/$NAME.sha256 (is there a release with the demo image?)"
	WANT=$(cut -d' ' -f1 < "$CACHE/$NAME.sha256.new")
	GZ="$CACHE/$NAME"
	if [ ! -f "$GZ" ] || [ "$(sha256 "$GZ")" != "$WANT" ]; then
		say "downloading $NAME (about 30 MB)"
		curl -fL --retry 3 --progress-bar -o "$GZ.part" "$BASE/$NAME"
		[ "$(sha256 "$GZ.part")" = "$WANT" ] || die "the download's checksum doesn't match: try again"
		mv "$GZ.part" "$GZ"
	fi
	mv "$CACHE/$NAME.sha256.new" "$CACHE/$NAME.sha256"
fi
SUM=$(sha256 "$GZ")
if [ ! -f "$RAW" ] || [ "$(cat "$RAW.from" 2>/dev/null)" != "$SUM" ]; then
	say "unpacking"
	gzip -dc "$GZ" > "$RAW.part" 2>/dev/null || true        # OpenWRT's images end in "trailing garbage"
	[ -s "$RAW.part" ] || die "could not unpack $GZ"
	mv "$RAW.part" "$RAW"
	echo "$SUM" > "$RAW.from"
fi

# ---- the ports (on this computer only)
in_use() {     # curl: 7 is "nothing listening there"
	curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$1/" 2>/dev/null
	[ $? -ne 7 ]
}
for p in $DASH_PORT $LUCI_PORT $SSH_PORT; do
	if in_use "$p"; then
		die "port $p on 127.0.0.1 is in use (another demo still running?): set DASH_PORT, LUCI_PORT or SSH_PORT"
	fi
done

# ---- the accelerator: KVM (Linux, WSL2), HVF (Intel Macs), else emulated
ACCEL="-accel tcg"
case $(uname -s) in
Linux) # /dev/kvm can be there without working KVM (WSL2 without nested virtualization): QEMU falls back to tcg
	if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-accel kvm -accel tcg -cpu max"
	else say "no KVM (/dev/kvm): emulated, so it starts slowly (a few minutes)"; fi ;;
Darwin) if [ "$(uname -m)" = x86_64 ] && [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]; then ACCEL="-accel hvf -cpu host"
	else say "Apple silicon: an x86-64 VM is emulated, so it starts slowly (a few minutes)"; fi ;;
esac

# ---- boot: the LAN is QEMU's restricted network as 192.168.1.0/24 (the router is .1, this computer
# .2), with this computer's ports forwarded to it; the WAN is another restricted network (no internet)
LOG="$CACHE/console.log"
: > "$LOG"
LAN="user,id=lan,restrict=on,net=192.168.1.0/24,host=192.168.1.2,dhcpstart=192.168.1.200"
LAN="$LAN,hostfwd=tcp:127.0.0.1:$DASH_PORT-192.168.1.1:8090"
LAN="$LAN,hostfwd=tcp:127.0.0.1:$LUCI_PORT-192.168.1.1:80,hostfwd=tcp:127.0.0.1:$SSH_PORT-192.168.1.1:22"
say "booting OpenWRT with AIGW OS in QEMU (console: $LOG)"
# shellcheck disable=SC2086
qemu-system-x86_64 $ACCEL -m "$MEM" -smp 2 -snapshot -drive "file=$RAW,format=raw" \
	-netdev "$LAN" -device e1000,netdev=lan -netdev user,id=wan,restrict=on -device e1000,netdev=wan \
	-display none -serial "file:$LOG" -monitor none &
QEMU=$!
trap 'kill $QEMU 2>/dev/null; wait $QEMU 2>/dev/null' EXIT
trap 'exit 130' INT TERM

i=0
TOKEN=""
while [ -z "$TOKEN" ]; do
	kill -0 $QEMU 2>/dev/null || die "QEMU stopped; see $LOG"
	grep -q "demo mode stays off" "$LOG" && die "$(grep "demo mode stays off" "$LOG" | tail -n 1)"
	TOKEN=$(sed -n 's/.*AIGW-DEMO dashboard token \([A-Za-z0-9_-]*\).*/\1/p' "$LOG" | tail -n 1)
	i=$((i + 2)); [ $i -le "$BOOT_S" ] || die "the demo didn't start within $BOOT_S s; see $LOG"
	sleep 2
done
api() { curl -fsS --max-time 10 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$DASH_PORT$1"; }
i=0
until api /api/overview >/dev/null 2>&1; do
	kill -0 $QEMU 2>/dev/null || die "QEMU stopped; see $LOG"
	i=$((i + 2)); [ $i -le "$BOOT_S" ] || die "the dashboard didn't answer within $BOOT_S s; see $LOG"
	sleep 2
done

SSH="ssh -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
if [ $CHECK = 1 ]; then
	bad=0
	ok() { echo "    PASS $1"; }
	no() { echo "    FAIL $1"; bad=$((bad + 1)); }
	say "checking the demo"
	api /api/overview | grep -q '"demo": true' && ok "the dashboard says demo data" || no "no demo flag in /api/overview"
	curl -fsS "http://127.0.0.1:$DASH_PORT/" | grep -q 'Demo data' && ok "the page has its demo banner" ||
		no "no demo banner on the page"
	n=$(api /api/gateways | grep -o '"name": "home-' | wc -l | tr -d ' ')
	[ "$n" -ge 24 ] && ok "a simulated fleet of $n homes" || no "the simulated fleet has $n homes"
	api /api/rollouts | grep -q '"state": "stopped"' && ok "a rollout stopped by itself" || no "no stopped rollout"
	# this VM's own home, reported by its bridge (once a minute)
	j=0
	until api /api/gateways | grep -q '"name": "This demo VM", "group": "riverside", "aigw": "[0-9]'; do
		j=$((j + 5)); [ $j -le 300 ] || break
		sleep 5
	done
	if api "/api/gateways/This%20demo%20VM" | grep -q '"action": "sqm"'; then
		ok "this VM's home reported by its bridge, smart queueing in its decisions"
	else
		no "this VM's home didn't report (or without its decisions): $(api "/api/gateways/This%20demo%20VM" | head -c 300)"
	fi
	code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$LUCI_PORT/cgi-bin/luci/") || true
	[ "$code" = 200 ] || [ "$code" = 403 ] && ok "LuCI answers ($code)" || no "LuCI: HTTP $code"
	if command -v ssh >/dev/null; then
		out=$($SSH -o BatchMode=yes -o ConnectTimeout=15 root@127.0.0.1 \
			'agctl why --from "yesterday 20:00" --to "yesterday 21:00"; agctl sqm; agctl -j status' 2>&1) || true
		echo "$out" | sed 's/^/      | /' | head -n 40
		echo "$out" | grep -q '^DEMO ' && ok "agctl labels it demo data" || no "agctl has no DEMO label"
		echo "$out" | grep -q 'LINE_FULL' && ok "agctl why explains the slow evening" || no "agctl why: no LINE_FULL"
		echo "$out" | grep -q 'verdict   kept' && ok "smart queueing kept" || no "agctl sqm: not kept"
		echo "$out" | grep -q '^  why not ' && no "smart queueing unavailable: $(echo "$out" | grep '^  why not ')" ||
			ok "smart queueing is available on this kernel"
		echo "$out" | grep -q '"demo": true' && ok "ag status says demo" || no "ag status: no demo"
	else
		no "no ssh client to check agctl"
	fi
	[ $bad = 0 ] && { say "DEMO CHECK OK"; exit 0; }
	say "DEMO CHECK FAILED ($bad); console: $LOG"
	tail -n 30 "$LOG"
	exit 1
fi

cat <<EOF

  AIGW OS demo is running: a simulated home and a simulated fleet (demo data, not real).

  The fleet, as an ISP sees it (the dashboard):
      http://127.0.0.1:$DASH_PORT/#token=$TOKEN

  The router (LuCI; user root, no password), menu AIGW OS:
      http://127.0.0.1:$LUCI_PORT/

  Its shell, and what AIGW explains there:
      ssh -p $SSH_PORT root@127.0.0.1          (no password)
      agctl why --from "yesterday 20:00" --to "yesterday 21:00"
      agctl brief "yesterday 20:00-21:00"
      agctl sqm; agctl calls; agctl upgrade

  What to look at: https://github.com/$REPO/blob/main/docs/try-it.md
  Ctrl+C stops it (nothing is kept).

EOF
wait $QEMU
