#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build Ambiorix (prpl Foundation's data model framework, BSD-2-Clause-Patent),
# the parts aigw-amx needs, at pinned versions, into one private root:
#
#   scripts/build-ambiorix.sh [root]       default ~/.cache/aigw-amx/root
#
# then: cmake -DAIGW_AMX_ROOT=<root> ...  (aigw-amx, the plug-in for prplOS)
#
# On prplOS these libraries are part of the system; this is for building and
# testing on a PC. Needs: git, make, gcc, flex, bison, pkg-config, and the
# development files of yajl, uriparser, libcap-ng, libevent, libubox, ubus.
set -eu

ROOT=${1:-${AMX_ROOT:-$HOME/.cache/aigw-amx/root}}
SRC=${AMX_SRC:-$(dirname "$ROOT")/src}
BASE=https://gitlab.com/prpl-foundation/components/ambiorix

# component  path  tag
COMPONENTS="
libamxc libraries/libamxc v2.9.0
libamxj libraries/libamxj v1.4.0
libamxp libraries/libamxp v2.14.1
libamxd libraries/libamxd v6.13.0
libamxb libraries/libamxb v4.19.0
libamxs libraries/libamxs v0.11.0
libamxo libraries/libamxo v5.9.0
libamxrt libraries/libamxrt v0.13.0
amxrt applications/amxrt v2.8.0
amxb_ubus modules/amxb_backends/amxb_ubus v3.13.0
"

mkdir -p "$ROOT" "$SRC"
# libubox / ubus from /usr/local when built from source (scripts in this repo do that)
export CFLAGS="${CFLAGS:-} -I/usr/local/include -Wno-error"
export LDFLAGS="${LDFLAGS:-} -L/usr/local/lib -Wl,-rpath,$ROOT/lib"
export PKG_CONFIG_PATH="$ROOT/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "$COMPONENTS" | while read -r name path tag; do
	[ -n "$name" ] || continue
	if [ -f "$ROOT/.built-$name-$tag" ]; then
		echo "== $name $tag (built)"
		continue
	fi
	echo "== $name $tag"
	rm -rf "${SRC:?}/$name"
	git -c advice.detachedHead=false clone -q --depth 1 -b "$tag" "$BASE/$path.git" "$SRC/$name"
	# the version comes from git describe: a shallow clone has the tag
	make -C "$SRC/$name" STAGINGDIR="$ROOT" PREFIX=/ LIBDIR=/lib INCLUDEDIR=/include BINDIR=/bin >"$SRC/$name.log" 2>&1 ||
		{ tail -n 30 "$SRC/$name.log" >&2; exit 1; }
	make -C "$SRC/$name" install STAGINGDIR="$ROOT" DEST="$ROOT" PREFIX=/ LIBDIR=/lib INCLUDEDIR=/include BINDIR=/bin \
		>>"$SRC/$name.log" 2>&1 || { tail -n 30 "$SRC/$name.log" >&2; exit 1; }
	: > "$ROOT/.built-$name-$tag"
done
echo "$ROOT"
