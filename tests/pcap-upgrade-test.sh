#!/bin/sh
# The PC as AP upgrade path: the latest release (or FROM=<tag>) installed with its
# own platform/pc-ap/install.sh, a home set up on it, then this checkout's
# install.sh run over it, as a person upgrades. Checks (upgrade_console.py's,
# as on OpenWRT): the settings word for word, Wi-Fi settings (pc-ap.conf) and
# every file in /etc/aigw, the remote phone and the gateway's key, devices with
# their names and zone, the block in effect, the care history, every data model
# path; agd restarted on the new version; /etc/config/aigw root's alone (0.43).
#
# It installs AIGW into this system (packages, /usr/local, systemd units): CI
# only, never on a machine that runs an AIGW access point.
#
#   sudo CI=true FROM=v0.41.0 tests/pcap-upgrade-test.sh
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
FROM=${FROM:-latest}
RELEASES=${RELEASES:-https://github.com/sudeepksingh/aigw/releases}
T=$(mktemp -d /var/tmp/pcap-upgrade.XXXXXX)
fail() { echo "PC AP UPGRADE TEST FAIL: $*" >&2; journalctl -u aigw-agd -n 30 --no-pager >&2 2>/dev/null || true; exit 1; }
[ "$(id -u)" = 0 ] || fail "run as root"
[ "${CI:-}" = true ] || fail "this installs AIGW into the system: CI only (CI=true)"
[ -d /run/systemd/system ] || fail "needs systemd"
if [ -e /etc/aigw/pc-ap.conf ] && [ -z "${AIGW_UPGRADE_TEST_FRESH:-}" ]; then
	fail "AIGW is installed here already (/etc/aigw/pc-ap.conf): a fresh system only"
fi
export PATH="/usr/local/bin:/usr/local/sbin:$PATH"

TV=$(sed -n 's/^project(aigw VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
if [ "$FROM" = latest ]; then
	FROM=$(sh "$REPO/tests/latest-release.sh" "$TV") || fail "cannot tell the latest release"
fi
echo "== AIGW OS $FROM, installed with its own install.sh"
git -C "$REPO" fetch -q --depth 1 origin tag "$FROM" 2>/dev/null || true
mkdir -p "$T/old"
git -C "$REPO" archive "$FROM" | tar -x -C "$T/old" || fail "no $FROM in the repository"
BUILD_DIR="$T/b-old" sh "$T/old/platform/pc-ap/install.sh" > "$T/install-old.log" 2>&1 ||
	{ tail -n 30 "$T/install-old.log"; fail "install.sh of $FROM"; }

wait_agd() {
	i=0
	until agctl status >/dev/null 2>&1; do
		i=$((i + 1)); [ $i -gt 60 ] && fail "agd did not answer"; sleep 1
	done
}

echo "== a home on $FROM"
ip link add aigwlan0 type dummy 2>/dev/null || true
ip addr replace 192.168.77.1/24 dev aigwlan0
ip link set aigwlan0 up
mkdir -p /var/lib/aigw-test
t=$(( $(date +%s) + 86400 ))
printf '%s\n' "$t 02:00:00:00:66:01 192.168.77.66 Living-room-TV *" \
	"$t 02:00:00:00:66:02 192.168.77.67 Annas-laptop *" > /var/lib/aigw-test/leases
uci -q batch <<'EOF'
set aigw.main.log_level='debug'
set aigw.main.leases='/var/lib/aigw-test/leases'
set aigw.lan.iface='aigwlan0'
set aigw.act.mode='enforce'
set aigw.act.ttl='7200'
add_list aigw.act.protect='02:00:00:00:66:09'
set aigw.dns.cache='2000'
set aigw.mgmt.vendor_prefix='X_EXAMPLE-COM_'
set aigw.mgmt.manufacturer='Example ISP'
set aigw.my_iot=policy
set aigw.my_iot.alert='new-destination'
set aigw.my_iot.action='throttle'
set aigw.my_iot.rate='500'
set aigw.my_iot.zone='iot'
delete aigw.wifi_busy
set aigw.bridge=plugin
set aigw.bridge.url='https://cortex.example.net'
set aigw.bridge.token='upgrade-test-token-0123456789'
commit aigw
EOF
sed -i 's/^SSID=.*/SSID=Upgrade-Test-Home/' /etc/aigw/pc-ap.conf
systemctl start aigw-ubusd aigw-agd
wait_agd
sleep 2
ubus send ag.dns '{"client":"192.168.77.66","qname":"www.youtube.com"}'
ubus send ag.dns '{"client":"192.168.77.67","qname":"www.wikipedia.org"}'
sleep 2
agctl zone Living-room-TV iot || fail "zone"
agctl block blocked.example.com 1d || fail "block"
timeout 120 agctl care --check > /dev/null 2>&1 || echo "    (care check: $?)"
agctl remote on 203.0.113.10 || fail "remote on"
agctl remote add "Anna's phone" > /dev/null || fail "remote add"
systemctl restart aigw-agd      # what agd keeps is what it saved
wait_agd

snap() {
	d=$T/$1
	wait_agd
	sleep 3
	mkdir -p "$d"
	uci show aigw > "$d/uci.txt"
	for c in status devices remote decisions care; do agctl -j $c > "$d/$c.json" 2>&1; done
	agctl dm get Device. > "$d/dm.txt" 2>&1
	systemctl is-active aigw-agd >/dev/null && echo enabled > "$d/service.txt"
	(cd /etc/aigw && find . -type f | sort | while read -r f; do
		case $f in ./agd-state.json|./agd-upgrade.json) echo "$f" ;; *) md5sum "$f" ;; esac
	done) > "$d/files.txt"
}
snap before

echo "== this checkout's install.sh over it ($TV)"
BUILD_DIR="$T/b-new" sh "$REPO/platform/pc-ap/install.sh" > "$T/install-new.log" 2>&1 ||
	{ tail -n 30 "$T/install-new.log"; fail "install.sh of this checkout"; }
snap after
python3 "$REPO/platform/openwrt/upgrade_console.py" compare "$T/before" "$T/after" "$TV" --pc ||
	fail "something was lost or reset"
[ "$(stat -c %a /etc/config/aigw)" = 600 ] || fail "/etc/config/aigw is $(stat -c %a /etc/config/aigw), not 600"
echo "    PASS config-private: /etc/config/aigw is root's alone (600)"
grep -q "Upgrade-Test-Home" /etc/aigw/pc-ap.conf || fail "pc-ap.conf lost its SSID"
systemctl stop aigw-agd aigw-ubusd 2>/dev/null || true
echo "PC AP UPGRADE TEST OK ($FROM -> $TV)"
