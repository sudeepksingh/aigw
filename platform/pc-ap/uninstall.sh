#!/bin/sh
# Removes AIGW from a PC. Settings in /etc/aigw and /etc/config/aigw are kept
# unless --purge is given. The OpenWRT libraries in /usr/local stay (other
# software may use them).
#
#   sudo ./platform/pc-ap/uninstall.sh [--purge]
set -eu

PREFIX=${PREFIX:-/usr/local}
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

[ -x "$PREFIX/sbin/aigw-ap" ] && "$PREFIX/sbin/aigw-ap" down 2>/dev/null || true
if [ -d /run/systemd/system ]; then
	systemctl disable --now aigw-cwmp aigw-usp aigw aigw-agdns aigw-agd aigw-ubusd 2>/dev/null || true
	rm -f /etc/systemd/system/aigw.service /etc/systemd/system/aigw-*.service
	systemctl daemon-reload
fi
rm -f "$PREFIX/sbin/agd" "$PREFIX/sbin/agdns" "$PREFIX/sbin/aigw-nft" "$PREFIX/sbin/aigw-ap" "$PREFIX/bin/agctl"
rm -f "$PREFIX/sbin/aigw-usp-run" "$PREFIX/sbin/obuspa" "$PREFIX/sbin/aigw-cwmp-run" "$PREFIX/sbin/aigw-cwmp-dm" "$PREFIX/sbin/icwmpd"
rm -rf "$PREFIX/lib/aigw/usp"
rm -f /usr/share/acl.d/aigw-*.json
rm -rf "$PREFIX/libexec/aigw"
if [ "${1:-}" = --purge ]; then
	rm -rf /etc/aigw /etc/config/aigw /var/lib/aigw
fi
echo "AIGW removed"
