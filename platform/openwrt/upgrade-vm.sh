#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# The in-VM half of platform/openwrt/upgrade-test.sh (busybox sh on OpenWRT).
#
#   upgrade-vm.sh setup          a home on the old release: settings that aren't the
#                                defaults, a policy of its own and a default one removed,
#                                devices (one in a zone), a block in effect, a care
#                                check, remote access with a phone
#   upgrade-vm.sh snap <name> <url>   what must survive, as files, posted to <url>
#                                as /tmp/snap-<name>.tgz (the host compares them)
set -u

wait_agd() {
	i=0
	until agctl status >/dev/null 2>&1; do
		if [ $i -gt 180 ]; then
			echo "agd did not answer"
			ls /etc/rc.d/ | grep -i aigw; /etc/init.d/aigw enabled && echo "aigw enabled" || echo "aigw NOT enabled"
			ps w | grep -E '[a]gd|[u]bus'
			logread 2>/dev/null | grep -iE 'agd|aigw|procd' | tail -n 20
			return 1
		fi
		i=$((i + 1)); sleep 1
	done
}

case "${1:-}" in
setup)
	uci -q batch <<-'EOF'
		set aigw.main.log_level='debug'
		set aigw.act.mode='enforce'
		set aigw.act.ttl='7200'
		set aigw.act.throttle_kbps='1500'
		add_list aigw.act.protect='02:00:00:00:66:09'
		set aigw.dns.cache='2000'
		set aigw.dns.stale='3600'
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
	/etc/init.d/aigw restart
	wait_agd || exit 1
	t=$(( $(date +%s) + 86400 ))
	printf '%s\n' "$t 02:00:00:00:66:01 192.168.1.66 Living-room-TV *" \
		"$t 02:00:00:00:66:02 192.168.1.67 Annas-laptop *" >> /tmp/dhcp.leases
	sleep 6
	ubus send ag.dns '{"client":"192.168.1.66","qname":"www.youtube.com"}'
	ubus send ag.dns '{"client":"192.168.1.67","qname":"www.wikipedia.org"}'
	sleep 2
	agctl zone Living-room-TV iot || exit 1
	agctl block blocked.example.com 1d || exit 1
	timeout 120 agctl care --check >/dev/null 2>&1 || echo "(care check: $?)"
	agctl remote on 203.0.113.10 || exit 1
	agctl remote add "Anna's phone" >/dev/null || exit 1
	# what agd keeps is what it saved: a restart first, so the snapshot holds only that
	/etc/init.d/aigw restart
	wait_agd
	;;
snap)
	name=$2 url=$3 d=/tmp/snap-$2
	wait_agd || exit 1
	sleep 3
	rm -rf "$d"; mkdir -p "$d"
	uci show aigw > "$d/uci.txt"
	uci show firewall | grep aigw > "$d/firewall.txt"
	for c in status devices remote decisions care; do agctl -j $c > "$d/$c.json" 2>&1; done
	agctl dm get Device. > "$d/dm.txt" 2>&1
	/etc/init.d/aigw enabled && echo enabled > "$d/service.txt"
	opkg list-installed | grep -E 'aigw' > "$d/packages.txt"
	# the files that hold the home (state changes as agd runs: listed, not compared)
	(cd /etc/aigw && find . -type f | sort | while read -r f; do
		case $f in ./agd-state.json|./agd-upgrade.json) echo "$f";; *) md5sum "$f";; esac
	done) > "$d/files.txt"
	ls /etc/config/ > "$d/configs.txt"
	tar czf "$d.tgz" -C /tmp "snap-$name"
	uclient-fetch -O /dev/null --post-file="$d.tgz" "$url" 2>&1 | tail -n 2
	# a failed upload shows in its own words above; the host checks that the file arrived

	echo "snapshot $name posted"
	;;
*)
	echo "usage: upgrade-vm.sh setup | snap <name> <url>" >&2; exit 2 ;;
esac
