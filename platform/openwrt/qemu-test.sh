#!/bin/sh
# Install the built packages on a real OpenWRT x86-64 system in QEMU and run
# aigw-selftest --full there: procd, fw4, opkg dependencies, ubus, rpcd and
# the firewall integration, as on a router, before any router is touched.
#
#   platform/openwrt/build-sdk.sh                   # first: build the packages
#   platform/openwrt/qemu-test.sh                   # VERSION=24.10 by default
#   KEEP=1 platform/openwrt/qemu-test.sh            # leave it running, LuCI on :8080 (LUCI_PORT)
#
# Checks: aigw-selftest --full inside OpenWRT, then LuCI from the host as the
# AIGW pages use it (rpcd ACL, pages, menu, remote-access QR code).
#
# Uses KVM when /dev/kvm is usable (CI), plain emulation otherwise (slower).
# Needs qemu-system-x86_64, python3, curl, gzip.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-24.10}
MIRROR=${MIRROR:-https://downloads.openwrt.org}
CACHE=${QEMU_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-qemu}
PORT=${PORT:-$((20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 20000))}
LUCI_PORT=${LUCI_PORT:-8080}

die() { echo "qemu-test: $*" >&2; exit 1; }
for t in qemu-system-x86_64 python3 curl gzip; do command -v $t >/dev/null || die "missing $t"; done

case $VERSION in
*.*.*) ;;
*)
	VERSION=$(curl -fsSL --retry 3 --retry-all-errors "$MIRROR/releases/" | grep -oE "href=\"$VERSION\.[0-9]+/\"" |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
	[ -n "$VERSION" ] || die "no OpenWRT release found"
	;;
esac
PKGS="$REPO/dist/openwrt/$VERSION/x86_64"

IMG_GZ="openwrt-$VERSION-x86-64-generic-ext4-combined.img.gz"
mkdir -p "$CACHE"
if [ ! -f "$CACHE/$IMG_GZ" ]; then
	echo "==> downloading $IMG_GZ"
	curl -fL --http1.1 --retry 5 --retry-all-errors -C - --progress-bar -o "$CACHE/$IMG_GZ.part" \
		"$MIRROR/releases/$VERSION/targets/x86/64/$IMG_GZ"
	mv "$CACHE/$IMG_GZ.part" "$CACHE/$IMG_GZ"
fi
W=$(mktemp -d)
trap 'kill $HTTP 2>/dev/null; rm -rf "$W"' EXIT INT TERM
gzip -dc "$CACHE/$IMG_GZ" > "$W/openwrt.img" 2>/dev/null || true    # "trailing garbage" is normal
[ -s "$W/openwrt.img" ] || die "could not unpack $IMG_GZ"

# only this version's packages (older builds stay in dist/)
V=$(sed -n 's/^PKG_VERSION:=//p' "$REPO/platform/openwrt/aigw/Makefile")
mkdir -p "$W/pkgs"
for f in "$PKGS"/*aigw*[_-]"$V"-r*; do
	[ -f "$f" ] && cp "$f" "$W/pkgs/"
done
ls "$W/pkgs" | grep -q . || die "no packages of version $V in $PKGS (run platform/openwrt/build-sdk.sh)"
(cd "$W/pkgs" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
HTTP=$!

ACCEL="-accel tcg"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; fi
echo "==> OpenWRT $VERSION in QEMU (${ACCEL#-accel }), packages: $(ls "$W/pkgs" | tr '\n' ' ')"
LOG="$REPO/dist/openwrt/qemu-$VERSION.log"
: > "$LOG"
set +e
# shellcheck disable=SC2086
python3 "$REPO/platform/openwrt/qemu_console.py" "$LOG" "$W/pkgs" "$PORT" "$LUCI_PORT" ${KEEP:+--keep} -- \
	qemu-system-x86_64 $ACCEL -m 256 -smp 2 -display none -serial stdio -monitor none -no-reboot \
	-drive file="$W/openwrt.img",format=raw \
	-netdev user,id=lan -device e1000,netdev=lan \
	-netdev user,id=wan,hostfwd=tcp:127.0.0.1:$LUCI_PORT-:80 -device e1000,netdev=wan
rc=$?
set -e
echo "==> console log: $LOG"
[ $rc -eq 0 ] && echo "QEMU OPENWRT TEST OK" || echo "QEMU OPENWRT TEST FAILED ($rc)"
exit $rc
