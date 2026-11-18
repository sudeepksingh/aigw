#!/bin/sh
# Upgrade an older AIGW OS release to this build on OpenWRT x86-64 in QEMU, and
# check that nothing is lost or reset: settings, policies, devices, decisions,
# care history, remote access, the data model; agd running the new version,
# also after a reboot (platform/openwrt/upgrade_console.py says what is compared).
#
#   platform/openwrt/build-sdk.sh                         # first: this build's packages
#   FROM=v0.33.1 platform/openwrt/upgrade-test.sh         # a release tag, or "latest"
#   FROM=v0.33.1 TO=v0.41.0 platform/openwrt/upgrade-test.sh   # release to release (no build)
#
# The old release's packages come from its GitHub release (aigw, aigw-remote,
# aigw-bridge, luci-app-aigw); OpenWRT's own from its feeds. Uses KVM when
# /dev/kvm is usable. Needs qemu-system-x86_64, python3, curl, gzip.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-24.10}
FROM=${FROM:-latest}
MIRROR=${MIRROR:-https://downloads.openwrt.org}
RELEASES=${RELEASES:-https://github.com/sudeepksingh/aigw/releases}
CACHE=${QEMU_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-qemu}
PORT=${PORT:-$((20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 20000))}
PKGS="aigw aigw-remote aigw-bridge luci-app-aigw"

die() { echo "upgrade-test: $*" >&2; exit 1; }
for t in python3 curl gzip qemu-system-x86_64; do command -v $t >/dev/null || die "missing $t"; done

case $VERSION in
*.*.*) ;;
*)
	VERSION=$(curl -fsSL --retry 3 --retry-all-errors "$MIRROR/releases/" | grep -oE "href=\"$VERSION\.[0-9]+/\"" |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
	[ -n "$VERSION" ] || die "no OpenWRT release found"
	;;
esac
TV=$(sed -n 's/^PKG_VERSION:=//p' "$REPO/platform/openwrt/aigw/Makefile")
if [ "$FROM" = latest ]; then
	# the newest release older than this build (once this version is released, the one before)
	FROM=$(sh "$REPO/tests/latest-release.sh" "$TV") || die "cannot tell the latest release"
fi
FV=${FROM#v}
[ "$FV" != "$TV" ] || die "$FROM is this build's version: nothing to upgrade from"
NEW="$REPO/dist/openwrt/$VERSION/x86_64"

FILE="openwrt-$VERSION-x86-64-generic-ext4-combined.img.gz"
mkdir -p "$CACHE"
if [ ! -f "$CACHE/$FILE" ]; then
	echo "==> downloading $FILE"
	curl -fL --http1.1 --retry 5 --retry-all-errors -C - --progress-bar -o "$CACHE/$FILE.part" \
		"$MIRROR/releases/$VERSION/targets/x86/64/$FILE"
	mv "$CACHE/$FILE.part" "$CACHE/$FILE"
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
gzip -dc "$CACHE/$FILE" > "$W/boot.img" 2>/dev/null || true    # "trailing garbage" is normal
[ -s "$W/boot.img" ] || die "could not unpack $FILE"
mkdir -p "$W/old" "$W/new"
release() {   # release <tag> <dir>: the packages of a GitHub release
	for p in $PKGS; do
		case $p in aigw-remote|luci-app-aigw) arch=all ;; *) arch=x86_64 ;; esac
		f="${p}_${1#v}-r1_${arch}.ipk"
		curl -fsSL --retry 5 --retry-all-errors -o "$2/$f" "$RELEASES/download/$1/$f" || die "no $f in release $1"
	done
}
release "$FROM" "$W/old"
if [ -n "${TO:-}" ]; then
	# trying the harness without a build: from one release to another
	TV=${TO#v}
	release "$TO" "$W/new"
else
	for p in $PKGS; do
		n=$(ls "$NEW"/"${p}"_"$TV"-r*.ipk 2>/dev/null | head -1)
		[ -n "$n" ] || die "no ${p}_$TV in $NEW (build-sdk.sh for 24.10 x86/64 first)"
		cp "$n" "$W/new/"
	done
fi
cp "$REPO/platform/openwrt/upgrade-vm.sh" "$W/"

ACCEL="-accel tcg"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; fi
LOG="$REPO/dist/openwrt/upgrade-$FV-to-$TV.log"
mkdir -p "$(dirname "$LOG")"
: > "$LOG"
echo "==> AIGW OS $FROM -> $TV on OpenWRT $VERSION x86-64 in QEMU"
set +e
# shellcheck disable=SC2086
python3 "$REPO/platform/openwrt/upgrade_console.py" "$LOG" "$W" "$PORT" "$FV" "$TV" -- \
	qemu-system-x86_64 $ACCEL -m 256 -smp 2 -drive "file=$W/boot.img,format=raw" \
	-netdev user,id=lan -netdev user,id=wan -device e1000,netdev=lan -device e1000,netdev=wan \
	-display none -serial stdio -monitor none
rc=$?
set -e
echo "==> console log: $LOG"
[ $rc -eq 0 ] && echo "UPGRADE TEST OK ($FROM -> $TV)" || echo "UPGRADE TEST FAILED ($FROM -> $TV: $rc)"
exit $rc
