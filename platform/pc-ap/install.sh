#!/bin/sh
# Installs AIGW on a Debian/Ubuntu PC (including WSL2) in "PC as AP" mode:
# packages, OpenWRT libraries, AIGW itself, configuration and services.
# Safe to run again: it updates the build and keeps your settings.
#
#   sudo ./install.sh
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=${PREFIX:-/usr/local}
BUILD=${BUILD_DIR:-$REPO/build}

step() { printf '\n==> %s\n' "$*"; }
die() { printf 'install: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run with sudo: sudo ./install.sh"
command -v apt-get >/dev/null || die "this installer supports Debian/Ubuntu. For other distributions see docs/getting-started-pc-ap.md"

step "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
	build-essential cmake pkg-config git ca-certificates libjson-c-dev libnetfilter-conntrack-dev \
	hostapd dnsmasq-base nftables iw iproute2 wireless-regdb kmod procps \
	zstd xz-utils dnsutils curl wireguard-tools >/dev/null

step "Building OpenWRT libraries (libubox, ubus, uci)"
PREFIX=$PREFIX sh "$REPO/scripts/build-deps.sh"

USP_OPT=""
if [ "${AIGW_USP:-0}" = 1 ]; then
	step "Building OB-USP-Agent (management by USP / TR-369)"
	apt-get install -y -qq --no-install-recommends autoconf automake libtool libssl-dev libsqlite3-dev \
		zlib1g-dev libmosquitto-dev >/dev/null
	OBUSPA_SRC=$(sh "$REPO/scripts/build-obuspa.sh" "${OBUSPA_DIR:-/usr/local/src/obuspa}" | tail -1)
	install -m 0755 "$OBUSPA_SRC/obuspa" "$PREFIX/sbin/obuspa"
	USP_OPT="-DAIGW_OBUSPA_SRC=$OBUSPA_SRC"
fi
if [ "${AIGW_CWMP:-0}" = 1 ]; then
	step "Building icwmp (management by TR-069)"
	apt-get install -y -qq --no-install-recommends libcurl4-openssl-dev libmxml-dev uuid-dev libssl-dev \
		zlib1g-dev >/dev/null
	ICWMPD=$(sh "$REPO/scripts/build-icwmp.sh" "${ICWMP_DIR:-/usr/local/src/icwmp}" | tail -1)
	install -m 0755 "$ICWMPD" "$PREFIX/sbin/icwmpd"
fi

step "Building AIGW"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release $USP_OPT >/dev/null
cmake --build "$BUILD" -j "$(nproc)" >/dev/null
(cd "$BUILD" && ctest --output-on-failure) >/dev/null || die "unit tests failed (run: cd $BUILD && ctest --output-on-failure)"
cmake --install "$BUILD" >/dev/null
install -m 0755 "$REPO/platform/pc-ap/aigw-ap" "$PREFIX/sbin/aigw-ap"
ldconfig

step "Configuring"
mkdir -p /etc/config /etc/aigw /etc/aigw/plugins /etc/aigw/keys.d /etc/aigw/models /var/lib/aigw
if [ ! -f /etc/config/aigw ]; then
	install -m 0644 "$REPO/config/aigw" /etc/config/aigw
fi
if [ ! -f /etc/aigw/pc-ap.conf ]; then
	pass=$(tr -dc 'a-km-zA-HJ-NP-Z2-9' < /dev/urandom | head -c 12)
	tag=$(tr -dc 'A-Z0-9' < /dev/urandom | head -c 4)
	cc=$(iw reg get 2>/dev/null | awk '/^country/ { sub(":", "", $2); print $2; exit }')
	[ "$cc" = 00 ] && cc=""
	sed -e "s|^SSID=.*|SSID=AIGW-$tag|" \
	    -e "s|^PASSPHRASE=.*|PASSPHRASE=$pass|" \
	    -e "s|^COUNTRY=.*|COUNTRY=$cc|" \
	    "$REPO/platform/pc-ap/pc-ap.conf.example" > /etc/aigw/pc-ap.conf
	chmod 600 /etc/aigw/pc-ap.conf
fi

# Let the user who ran the installer use agctl without sudo (read-only:
# status, devices, DNS log, live DNS events). More users: AIGW_USERS="a b".
mkdir -p /usr/share/acl.d
for u in ${AIGW_USERS:-${SUDO_USER:-}}; do
	[ "$u" = root ] && continue
	id "$u" >/dev/null 2>&1 || continue
	sed "s|@USER@|$u|" "$REPO/platform/pc-ap/ubus-acl.json.in" > "/usr/share/acl.d/aigw-$u.json"
	chown root:root "/usr/share/acl.d/aigw-$u.json"
	chmod 0644 "/usr/share/acl.d/aigw-$u.json"
	acl_users="${acl_users:-}$u "
done

if [ -d /run/systemd/system ]; then
	for u in "$REPO"/platform/pc-ap/systemd/*.service; do
		sed "s|@SBINDIR@|$PREFIX/sbin|g" "$u" > "/etc/systemd/system/$(basename "$u")"
	done
	systemctl daemon-reload
	# pick up new binaries and permission files if AIGW is already running
	for s in aigw-ubusd aigw-agd aigw-agdns aigw-usp aigw-cwmp; do
		systemctl try-restart "$s" 2>/dev/null || true
	done
else
	echo "systemd not running: aigw-ap will start the daemons directly"
fi

. /etc/aigw/pc-ap.conf
step "Done"
cat <<EOF
AIGW OS $(agd --version | cut -d' ' -f2) is installed.

  Wi-Fi name   $SSID
  Password     $PASSPHRASE
  Settings     /etc/aigw/pc-ap.conf (country, band, channel, adapter)

Start the access point:   sudo aigw-ap up
Start it at every boot:   sudo systemctl enable --now aigw
See what it sees:         agctl devices   |   agctl watch
EOF
if [ -n "${acl_users:-}" ]; then
	echo "agctl works without sudo for: $acl_users"
fi
if grep -qi microsoft /proc/version 2>/dev/null; then
	cat <<EOF

You are on WSL2: \`sudo aigw-ap up\` attaches the USB Wi-Fi adapter from
Windows by itself once it is shared. To share it (one time, administrator
approval) and keep WSL running, run platform\\pc-ap\\wsl\\aigw-wsl.cmd from
Windows. It also attaches the adapter and starts the access point for you.
EOF
fi
