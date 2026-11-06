#!/bin/sh
# AIGW installer.
#
#   PC as AP (Debian/Ubuntu, incl. WSL2):   sudo ./install.sh
#   OpenWRT router:                         see docs/getting-started-openwrt.md
set -eu
cd "$(dirname "$0")"

if [ -f /etc/openwrt_release ]; then
	cat >&2 <<EOF
This is an OpenWRT device. Install the AIGW package instead of building here:
  opkg install aigw_*.ipk     (OpenWRT 24.10 and older)
  apk add --allow-untrusted aigw-*.apk   (OpenWRT 25.12 and newer)
See docs/getting-started-openwrt.md
EOF
	exit 1
fi

exec sh ./platform/pc-ap/install.sh "$@"
