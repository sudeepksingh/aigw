#!/bin/sh
# Fetches and builds icwmp (icwmpd, BSD-3-Clause, iopsys), the TR-069 client
# AIGW OS is tested with, at a pinned commit. It is used unmodified: its data
# model comes from aigw-cwmp-dm over ubus. The OpenWRT package
# (platform/openwrt/icwmp) builds the same commit.
#
#   scripts/build-icwmp.sh [dir]    default ~/.cache/aigw-cwmp/icwmp-<commit>
#
# Prints the binary's path on its last line (give it to tests/cwmp-test.sh as
# AIGW_ICWMPD). Needs: git, cmake, pkg-config, libubox, ubus and uci
# (scripts/build-deps.sh), and the development files of json-c, curl, mxml,
# uuid, openssl and zlib.
set -eu

COMMIT=0579a5181579396c2eded021a93c94ffe67d4eec
REPO=https://dev.iopsys.eu/bbf/icwmp.git
DIR=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-cwmp/icwmp-$(echo $COMMIT | cut -c1-12)}
# icwmp builds with -Werror; these warnings depend on the compiler and libc, not on icwmp
NOERR="-Wno-error=unused-result -Wno-error=format-truncation -Wno-error=unused-function -Wno-error=stringop-truncation -Wno-error=format-overflow -Wno-error=deprecated-declarations"

die() { printf 'build-icwmp: %s\n' "$*" >&2; exit 1; }
for t in git cmake make pkg-config; do
	command -v $t >/dev/null || die "missing $t (Ubuntu: sudo apt install git cmake pkg-config)"
done
for p in libcurl mxml uuid json-c openssl zlib; do
	pkg-config --exists $p || die "missing $p development files (Ubuntu: sudo apt install libcurl4-openssl-dev libmxml-dev uuid-dev libjson-c-dev libssl-dev zlib1g-dev)"
done

if [ ! -d "$DIR/.git" ]; then
	mkdir -p "$(dirname "$DIR")"
	git clone -q "$REPO" "$DIR"
fi
cd "$DIR"
if [ "$(git rev-parse HEAD)" != "$COMMIT" ]; then
	git fetch -q origin
	git -c advice.detachedHead=false checkout -q "$COMMIT"
	rm -rf build
fi
if [ ! -x build/icwmpd ]; then
	# BBF_VENDOR_PREFIX stays empty: it only names vendor firmware file types
	cmake -S src -B build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="$NOERR -I/usr/local/include" -DCMAKE_EXE_LINKER_FLAGS="-L/usr/local/lib" > build.log 2>&1 &&
	cmake --build build -j"$(nproc)" >> build.log 2>&1 || { tail -30 build.log >&2; die "build failed ($DIR/build.log)"; }
fi
echo "icwmp ($COMMIT) built: $DIR/build/icwmpd" >&2
echo "$DIR/build/icwmpd"
