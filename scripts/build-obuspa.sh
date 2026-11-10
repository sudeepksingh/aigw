#!/bin/sh
# Fetches and builds OB-USP-Agent (obuspa, BSD-3-Clause, Broadband Forum) at
# the version AIGW OS is tested with, with the switches the aigw-usp plug-in is
# compiled for: MQTT and Unix domain sockets; no STOMP, CoAP, WebSockets or
# bulk data. The OpenWRT package (platform/openwrt/obuspa) uses the same ones.
#
#   scripts/build-obuspa.sh [dir]   default ~/.cache/aigw-usp/obuspa-<version>
#
# Prints the source directory on its last line: give it to cmake as
# -DAIGW_OBUSPA_SRC=<dir>. The binary is <dir>/obuspa.
# Needs: git, autoconf, automake, libtool, pkg-config, and the development
# files of openssl, sqlite3, zlib and libmosquitto.
set -eu

VERSION=11.0.7
COMMIT=59028beba21471d19bd3842ef2632ceb6ca8c7fc
REPO=https://github.com/BroadbandForum/obuspa.git
DIR=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-usp/obuspa-$VERSION}

die() { printf 'build-obuspa: %s\n' "$*" >&2; exit 1; }
for t in git autoreconf make pkg-config; do
	command -v $t >/dev/null || die "missing $t (Ubuntu: sudo apt install git autoconf automake libtool pkg-config)"
done
for p in openssl sqlite3 zlib libmosquitto; do
	pkg-config --exists $p || die "missing $p development files (Ubuntu: sudo apt install libssl-dev libsqlite3-dev zlib1g-dev libmosquitto-dev)"
done

if [ ! -d "$DIR/.git" ]; then
	mkdir -p "$(dirname "$DIR")"
	git clone -q "$REPO" "$DIR"
fi
cd "$DIR"
if [ "$(git rev-parse HEAD)" != "$COMMIT" ]; then
	git fetch -q origin
	git -c advice.detachedHead=false checkout -q "$COMMIT"
	rm -f obuspa
fi
if [ ! -x obuspa ]; then
	autoreconf -fi > build.log 2>&1 &&
	./configure --disable-stomp --disable-coap --disable-websockets --disable-bulkdata --enable-hardening >> build.log 2>&1 &&
	make -j"$(nproc)" >> build.log 2>&1 || { tail -30 build.log >&2; die "build failed ($DIR/build.log)"; }
fi
echo "obuspa $VERSION built: $DIR/obuspa" >&2
echo "$DIR"
