#!/bin/sh
# Builds the OpenWRT libraries AIGW uses (libubox, ubus, uci) for a regular
# Linux host. On OpenWRT they are already installed as packages.
#
#   sudo ./scripts/build-deps.sh            # installs into /usr/local
#   PREFIX=$HOME/.local ./scripts/build-deps.sh
#
# Versions are pinned so every clone builds the same thing.
set -eu

PREFIX=${PREFIX:-/usr/local}
WORK=${WORK:-/tmp/aigw-deps}
STAMP="$PREFIX/share/aigw/deps.stamp"

LIBUBOX_REF=a396f38a8568a1d670c240465cd8d24fdfdf6463
UBUS_REF=db20eff5535fe3126c05dc0edc029caf0e82fe10
UCI_REF=74f6277aabffc943d026f406df57c22595134c42
WANT="libubox=$LIBUBOX_REF ubus=$UBUS_REF uci=$UCI_REF"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ] && [ "${FORCE:-0}" != 1 ]; then
	echo "OpenWRT libraries already installed in $PREFIX (FORCE=1 to rebuild)"
	exit 0
fi

for t in git cmake make cc pkg-config; do
	command -v $t >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done
[ -f /usr/include/json-c/json.h ] || pkg-config --exists json-c || {
	echo "missing json-c development files (Debian/Ubuntu: apt install libjson-c-dev)" >&2; exit 1; }

mkdir -p "$WORK"
JOBS=$(nproc 2>/dev/null || echo 2)

build() {
	name=$1 ref=$2; shift 2
	echo "==> $name @ $(echo "$ref" | cut -c1-12)"
	if [ ! -d "$WORK/$name/.git" ]; then
		git clone -q "https://git.openwrt.org/project/$name.git" "$WORK/$name"
	fi
	git -C "$WORK/$name" fetch -q origin "$ref" 2>/dev/null || git -C "$WORK/$name" fetch -q origin
	git -C "$WORK/$name" checkout -q "$ref"
	cmake --no-warn-unused-cli -S "$WORK/$name" -B "$WORK/$name/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_BUILD_TYPE=Release -DBUILD_LUA=OFF \
		-DBUILD_EXAMPLES=OFF "$@" >/dev/null
	cmake --build "$WORK/$name/build" -j "$JOBS" >/dev/null
	cmake --install "$WORK/$name/build" >/dev/null
}

build libubox "$LIBUBOX_REF"
build ubus "$UBUS_REF"
build uci "$UCI_REF"

command -v ldconfig >/dev/null && ldconfig 2>/dev/null || true
mkdir -p "$(dirname "$STAMP")"
echo "$WANT" > "$STAMP"
echo "OpenWRT libraries installed in $PREFIX"
