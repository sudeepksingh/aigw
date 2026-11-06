#!/bin/sh
# Builds the AIGW OpenWRT package with the official OpenWRT SDK.
#
#   platform/openwrt/build-sdk.sh                         x86/64, latest 24.10.x
#   TARGET=mediatek/filogic platform/openwrt/build-sdk.sh
#   VERSION=25.12 TARGET=ramips/mt7621 platform/openwrt/build-sdk.sh
#
# Packages land in dist/openwrt/<version>/<arch>/ (.ipk for 24.10, .apk for
# 25.12+). Packages are per CPU architecture, so one build serves every
# router with that architecture. Run as a normal user (not root).
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TARGET=${TARGET:-x86/64}
VERSION=${VERSION:-24.10}
MIRROR=${MIRROR:-https://downloads.openwrt.org}
# the SDK must live on a Linux filesystem (not /mnt/c on WSL)
CACHE=${SDK_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-sdk}

die() { printf 'build-sdk: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" != 0 ] || [ "${FORCE_UNSAFE_CONFIGURE:-0}" = 1 ] || \
	die "do not run as root (the OpenWRT build refuses it)"
for t in curl tar make gcc g++ python3 rsync unzip file gawk; do
	command -v $t >/dev/null || die "missing tool: $t (Ubuntu: sudo apt install build-essential curl rsync unzip file gawk python3 zstd)"
done

# "24.10" -> newest 24.10.x release
case $VERSION in
*.*.*) ;;
*)
	VERSION=$(curl -fsSL "$MIRROR/releases/" | grep -oE "href=\"$VERSION\.[0-9]+/\"" |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
	[ -n "$VERSION" ] || die "no OpenWRT release found for that version"
	;;
esac

URL="$MIRROR/releases/$VERSION/targets/$TARGET"
SDK_FILE=$(curl -fsSL "$URL/" | grep -oE 'openwrt-sdk-[^"<>]+\.tar\.(zst|xz)' | head -1)
[ -n "$SDK_FILE" ] || die "no SDK found at $URL"
SDK_DIR="$CACHE/${SDK_FILE%.tar.*}"

echo "==> OpenWRT $VERSION, target $TARGET"
mkdir -p "$CACHE"
if [ ! -d "$SDK_DIR" ]; then
	echo "==> downloading $SDK_FILE"
	curl -fL --progress-bar -o "$CACHE/$SDK_FILE" "$URL/$SDK_FILE"
	case $SDK_FILE in
	*.zst) tar --zstd -xf "$CACHE/$SDK_FILE" -C "$CACHE" ;;
	*.xz) tar -xJf "$CACHE/$SDK_FILE" -C "$CACHE" ;;
	esac
	rm -f "$CACHE/$SDK_FILE"
fi

cd "$SDK_DIR"
# only the base feed (libubox, ubus, uci, json-c) plus this repository
grep -E '^src-git(-full)? base ' feeds.conf.default > feeds.conf
echo "src-link aigw $REPO/platform/openwrt" >> feeds.conf

echo "==> feeds"
./scripts/feeds update -a > /dev/null 2>&1
./scripts/feeds install aigw > /dev/null 2>&1

echo "==> building"
make defconfig > /dev/null 2>&1
if ! make package/aigw/compile -j"$(nproc)" > "$CACHE/build-$VERSION.log" 2>&1; then
	make package/aigw/compile V=s -j1 2>&1 | tail -60
	die "build failed (full log: $CACHE/build-$VERSION.log)"
fi

ARCH=$(sed -n 's/^CONFIG_TARGET_ARCH_PACKAGES="\(.*\)"/\1/p' .config)
OUT="$REPO/dist/openwrt/$VERSION/$ARCH"
mkdir -p "$OUT"
find bin/packages -type f \( -name 'aigw*.ipk' -o -name 'aigw*.apk' \) -exec cp {} "$OUT/" \;
echo "==> done:"
ls -l "$OUT"
