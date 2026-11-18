#!/bin/sh
# Builds the AIGW OS demo image (docs/try-it.md) with OpenWRT's ImageBuilder:
# OpenWRT x86-64 with AIGW OS, LuCI, the bridge, and aigw-demo (a simulated
# home and a simulated fleet in cortex, on only in a QEMU virtual machine).
#
#   VERSION=24.10 TARGET=x86/64 platform/openwrt/build-sdk.sh    # first: the packages
#   platform/openwrt/build-demo-image.sh
#
# Writes dist/demo/aigw-os-demo-x86-64.img.gz and its .sha256; run it with
# scripts/try-aigw.sh --image dist/demo/aigw-os-demo-x86-64.img.gz. The
# ImageBuilder and OpenWRT's own packages come from downloads.openwrt.org
# (MIRROR), as for the SDK. Run as a normal user.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-24.10}
MIRROR=${MIRROR:-https://downloads.openwrt.org}
CACHE=${IB_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-ib}
OUT="$REPO/dist/demo"
NAME=aigw-os-demo-x86-64.img.gz
# the image's budget: OpenWRT (about 12 MB) with LuCI, python3 for cortex and AIGW OS
MAX_MB=${DEMO_MAX_MB:-40}

# OpenWRT's packages beside AIGW's: LuCI, curl for the bridge, the parts of
# python3 cortex uses (http.server needs email and urllib; rollouts statistics,
# which needs decimal; the store sqlite3; hashlib and ssl openssl)
EXTRA="luci curl python3-light python3-sqlite3 python3-openssl python3-email python3-urllib python3-decimal"

die() { printf 'build-demo-image: %s\n' "$*" >&2; exit 1; }
for t in curl tar make gzip sha256sum; do command -v $t >/dev/null || die "missing tool: $t"; done

case $VERSION in
*.*.*) ;;
*)
	VERSION=$(curl -fsSL --retry 3 --retry-all-errors "$MIRROR/releases/" | grep -oE "href=\"$VERSION\.[0-9]+/\"" |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
	[ -n "$VERSION" ] || die "no OpenWRT release found for that version"
	;;
esac

PKGS="$REPO/dist/openwrt/$VERSION/x86_64"
V=$(sed -n 's/^PKG_VERSION:=//p' "$REPO/platform/openwrt/aigw/Makefile")
OURS=""
for p in aigw aigw-bridge aigw-demo luci-app-aigw; do
	f=$(ls "$PKGS/${p}_$V"-r*.ipk 2>/dev/null | tail -1)
	[ -n "$f" ] || die "no ${p}_$V ipk in $PKGS (VERSION=$VERSION TARGET=x86/64 platform/openwrt/build-sdk.sh)"
	OURS="$OURS $f"
done

URL="$MIRROR/releases/$VERSION/targets/x86/64"
IB_FILE=$(curl -fsSL --retry 3 --retry-all-errors "$URL/" | grep -oE 'openwrt-imagebuilder-[^"<>]+\.tar\.(zst|xz)' | head -1)
[ -n "$IB_FILE" ] || die "no ImageBuilder found at $URL"
IB="$CACHE/${IB_FILE%.tar.*}"
echo "==> OpenWRT $VERSION x86/64, ImageBuilder $IB_FILE"
mkdir -p "$CACHE"
if [ ! -d "$IB" ]; then
	curl -fL --http1.1 --retry 5 --retry-all-errors --retry-delay 5 -C - --progress-bar \
		-o "$CACHE/$IB_FILE" "$URL/$IB_FILE"
	case $IB_FILE in
	*.zst) tar --zstd -xf "$CACHE/$IB_FILE" -C "$CACHE" ;;
	*.xz) tar -xJf "$CACHE/$IB_FILE" -C "$CACHE" ;;
	esac
	rm -f "$CACHE/$IB_FILE"
fi

# AIGW's packages in the ImageBuilder's local feed (only this version's)
rm -f "$IB"/packages/aigw*.ipk "$IB"/packages/luci-app-aigw*.ipk
# shellcheck disable=SC2086
cp $OURS "$IB/packages/"

echo "==> image: aigw aigw-bridge aigw-demo luci-app-aigw $EXTRA"
LOG="$CACHE/image-$VERSION.log"
rm -rf "$IB/bin/targets"
# no time servers: the demo VM has no network outside (QEMU's restricted network)
if ! make -C "$IB" image PROFILE=generic EXTRA_IMAGE_NAME=aigw-demo DISABLED_SERVICES="sysntpd" \
	PACKAGES="aigw aigw-bridge aigw-demo luci-app-aigw $EXTRA" > "$LOG" 2>&1; then
	tail -40 "$LOG" >&2
	die "ImageBuilder failed (full log: $LOG)"
fi
IMG=$(find "$IB/bin/targets" -name '*aigw-demo*generic-ext4-combined.img.gz' | head -1)
[ -n "$IMG" ] || die "no ext4 combined image in $IB/bin/targets (log: $LOG)"

mkdir -p "$OUT"
cp "$IMG" "$OUT/$NAME"
(cd "$OUT" && sha256sum "$NAME" > "$NAME.sha256")
mb=$(( ($(wc -c < "$OUT/$NAME") + 1048575) / 1048576 ))
echo "==> $OUT/$NAME: $mb MB (budget $MAX_MB MB), AIGW OS $V on OpenWRT $VERSION"
[ "$mb" -le "$MAX_MB" ] || die "the demo image is $mb MB, over its budget of $MAX_MB MB"
