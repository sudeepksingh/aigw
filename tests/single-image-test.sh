#!/bin/sh
# Firmware rollback on a router with one firmware image: OpenWRT's handler
# (platform/openwrt/aigw/files/firmware) on a fake flash. Nothing real is
# flashed or rebooted: sysupgrade, df, fwtool, uclient-fetch, uci and reboot
# are shims in front of PATH, a directory stands in for / (the overlay),
# files stand in for the partition, and a list stands in for the free space.
#
#   the handler   a copy of the running image kept on the overlay (carried
#                 over sysupgrade in its archive, this once) and listed as
#                 image 3; written back with its old settings and agd's
#                 record; no room: refused in words (require) or upgraded
#                 without (auto); USB storage before the overlay, never RAM;
#                 the image fetched again from the URL it came from; a
#                 sysupgrade that fails undoes the keep; RAM checked before
#                 going back
#   agd (root)    a bad upgrade on this handler: worse, rolled back through
#                 image 3 by itself, back on 1.0 with the record carried
#                 over, confirmed; Upgrade.RollbackMethod KeptCopy
#
#   tests/single-image-test.sh [build-dir]       (as root for the agd part)
set -eu

BIN=$(cd "${1:-build}" 2>/dev/null && pwd || echo "")
. "$(dirname "$0")/feature-lib.sh"
needs "$BIN" upgrade
REPO=$(cd "$(dirname "$0")/.." && pwd)
FW=$REPO/platform/openwrt/aigw/files/firmware
T=$(mktemp -d)
PIDS=""
cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	sleep 0.3; rm -rf "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
fail() {
	echo "SINGLE-IMAGE TEST FAIL: $*" >&2
	echo "--- flash log" >&2; cat "$T/flash/log" >&2 2>/dev/null || true
	tail -n 40 "$T/agd.log" >&2 2>/dev/null || true
	exit 1
}

R=$T/root F=$T/flash W=$T/www
mkdir -p "$T/bin" "$F" "$W" "$T/tmp"
export AIGW_FW_ROOT=$R AIGW_FW_DIR=$T/tmp/aigw-firmware AIGW_FW_MOUNTS=$T/mounts FAKE=$T FAKE_FLASH=$F FAKE_WWW=$W
export AIGW_FW_BOOT_ID=$T/boot_id     # a new one at every (fake) reboot
export PATH="$T/bin:$PATH"

# ---- the shims: never the real sysupgrade or reboot
cat > "$T/bin/sysupgrade" <<'EOF'
#!/bin/sh
# a fake sysupgrade: / is $AIGW_FW_ROOT, the firmware partition is $FAKE_FLASH/firmware.bin
R=$AIGW_FW_ROOT F=$FAKE_FLASH
backup() {   # the settings: /etc/config, and what /lib/upgrade/keep.d lists
	set -- etc/config
	for k in "$R"/lib/upgrade/keep.d/*; do
		[ -f "$k" ] || continue
		while read -r p; do p=${p#/}; p=${p%/}; [ -e "$R/$p" ] && set -- "$@" "$p"; done < "$k"
	done
	tar -czf "$out" -C "$R" "$@"
}
case ${1:-} in
-T) head -n 1 "$2" | grep -q '^FAKE-OPENWRT '; exit ;;
-b) out=$2; backup; exit ;;
esac
cfg=
[ "${1:-}" = -f ] && { cfg=$2; shift 2; }
img=$1
if [ -f "$F/fail" ]; then echo "failed $(head -n 1 "$img")" >> "$F/log"; echo "sysupgrade: failed (fake)" >&2; exit 1; fi
set -- $(head -n 1 "$img")
[ "${1:-}" = FAKE-OPENWRT ] || exit 1
if [ -z "$cfg" ]; then out=$F/sysupgrade.tgz; backup; cfg=$out; how=kept; else how="-f $(basename "$cfg")"; fi
cp "$img" "$F/firmware.bin"
# the overlay is wiped (USB storage under mnt/ is not the flash), the image's own files come back
find "$R" -mindepth 1 -maxdepth 1 ! -name mnt -exec rm -rf {} +
mkdir -p "$R/etc/config" "$R/lib/upgrade/keep.d"
printf "DISTRIB_RELEASE='%s'\nDISTRIB_REVISION='%s'\n" "$2" "$3" > "$R/etc/openwrt_release"
echo /etc/aigw/ > "$R/lib/upgrade/keep.d/aigw"
tar -xzf "$cfg" -C "$R"
echo "flashed $2-$3 settings $how" >> "$F/log"
date +%s%N > "$AIGW_FW_BOOT_ID"        # and it reboots
# a real one is often ended by its own last stage's TERM: FAKE_RC=143
exit "${FAKE_RC:-0}"
EOF
cat > "$T/bin/df" <<'EOF'
#!/bin/sh
# df -Pk PATH: the free space listed in $FAKE/free ("<mount point> <KB>") of the longest mount point holding PATH
for p; do :; done
best= kb=0
while read -r mp f; do
	case $p/ in "$mp"/*) [ ${#mp} -gt ${#best} ] && { best=$mp; kb=$f; } ;; esac
done < "$FAKE/free"
[ -n "$best" ] || exit 1
echo "Filesystem 1024-blocks Used Available Capacity Mounted on"
echo "fake 999999 0 $kb 0% $best"
EOF
cat > "$T/bin/fwtool" <<'EOF'
#!/bin/sh
# fwtool -q -i META IMAGE: the version from the image's first line ("nometa": none, as x86's .img.gz)
m=$3
set -- $(head -n 1 "$4")
[ "$1" = FAKE-OPENWRT ] && [ "${4:-}" != nometa ] || exit 1
printf '{"version":{"version":"%s","revision":"%s"}}\n' "$2" "$3" > "$m"
EOF
cat > "$T/bin/jsonfilter" <<'EOF'
#!/bin/sh
# jsonfilter -i FILE -e '@.version.version' | '@.version.revision'
case $4 in
*.revision) sed 's/.*"revision":"\([^"]*\)".*/\1/' "$2" ;;
*) sed 's/.*"version":{"version":"\([^"]*\)".*/\1/' "$2" ;;
esac
EOF
cat > "$T/bin/uclient-fetch" <<'EOF'
#!/bin/sh
# uclient-fetch -q -O OUT [--user=U --password=P] http://fake/NAME: $FAKE_WWW/NAME
out= url=
while [ $# -gt 0 ]; do
	case $1 in -O) out=$2; shift ;; -q|--user=*|--password=*) ;; *) url=$1 ;; esac
	shift
done
echo "fetch $url" >> "$FAKE_FLASH/log"
cp "$FAKE_WWW/${url##*/}" "$out" 2>/dev/null || { rm -f "$out"; exit 1; }
EOF
printf '#!/bin/sh\nexit 1\n' > "$T/bin/uci"
printf '#!/bin/sh\necho "reboot (fake)" >> "$FAKE_FLASH/log"\n' > "$T/bin/reboot"
printf '#!/bin/sh\necho "firstboot (fake)" >> "$FAKE_FLASH/log"\n' > "$T/bin/firstboot"
chmod +x "$T"/bin/*
[ "$(command -v sysupgrade)" = "$T/bin/sysupgrade" ] && [ "$(command -v reboot)" = "$T/bin/reboot" ] ||
	fail "the shims must come first in PATH"

image() {   # image NAME VERSION REVISION: a fake image of about 300 KB
	{ echo "FAKE-OPENWRT $2 $3"; head -c 300000 /dev/zero; } > "$W/$1"
}
image fw-1.0.img 1.0 r1; image fw-2.0.img 2.0 r2; image fw-3.0.img 3.0 r3
{ echo "FAKE-OPENWRT 1.0 r1 nometa"; head -c 300000 /dev/zero; } > "$W/fw-1.0-nometa.img"
echo "not an image" > "$W/bad.img"

# a router on 1.0, settings marked, with the overlay $1 KB free, RAM $2 KB, and the mounts given after
router() {
	rm -rf "$R" "$F"/* "$T/tmp"; mkdir -p "$R/etc/config" "$R/etc/aigw" "$R/lib/upgrade/keep.d" "$T/tmp"
	date +%s%N > "$AIGW_FW_BOOT_ID"
	printf "DISTRIB_RELEASE='1.0'\nDISTRIB_REVISION='r1'\n" > "$R/etc/openwrt_release"
	echo /etc/aigw/ > "$R/lib/upgrade/keep.d/aigw"
	echo "option marker 'on 1.0'" > "$R/etc/config/aigw"
	printf '%s %s\n%s %s\n' "$R" "$1" "$T/tmp" "$2" > "$T/free"
	printf 'overlayfs:/overlay %s overlay rw 0 0\ntmpfs %s tmpfs rw 0 0\n' "$R" "$T/tmp" > "$T/mounts"
	shift 2
	while [ $# -ge 3 ]; do
		mkdir -p "$R/mnt/$1"
		echo "$R/mnt/$1 $3" >> "$T/free"
		echo "/dev/fake $R/mnt/$1 $2 rw 0 0" >> "$T/mounts"
		shift 3
	done
}
fw() { "$FW" "$@"; }
running() { fw version; }
img3() { fw list | awk '$1 == 3 { print $2, $5 }'; }
flashes() { cat "$F/log" 2>/dev/null | grep -c '^flashed' || true; }
fetch2() {   # a file in /tmp is moved (RAM): download a copy
	u=$1
	case $u in file://*) cp "${u#file://}" "$T/dl.img"; u=file://$T/dl.img ;; esac
	fw download 2 "$u" > /dev/null || fail "download $1"
}

echo "== a router on 1.0 with room on its flash: nothing kept yet"
router 4000 8000
[ "$(running)" = 1.0-r1 ] || fail "the running version: $(running)"
fw list | grep -q '^3 NoImage 0 0 - kept$' || fail "image 3, empty: $(fw list)"
fw rollback | grep -q '^copy no copy is kept now; at the next upgrade, none can be kept on the flash (overlay): nothing to copy 1.0-r1 from' ||
	fail "rollback, nothing to copy from: $(fw rollback)"
fw keep "$W/bad.img" 2> "$T/err" && fail "keep should refuse what isn't an image"
grep -q "not 1.0-r1" "$T/err" || fail "keep a bad image: $(cat "$T/err")"
fw keep "$W/fw-2.0.img" 2> "$T/err" && fail "keep should refuse a copy of another version"
# an image without metadata (as x86's .img.gz) is taken for the version it is given as
fw keep "file://$W/fw-1.0-nometa.img" | grep -q "a copy of 1.0-r1 is kept on the flash (overlay)" ||
	fail "keep the running image (no metadata)"
[ "$(cat "$R/etc/aigw-rollback/current.version")" = 1.0-r1 ] || fail "the copy is 1.0-r1"
fw rollback | grep -q "the copy of 1.0-r1 on the flash (overlay) becomes image 3" || fail "rollback, a copy ready: $(fw rollback)"
fw download 3 "file://$W/fw-2.0.img" 2> "$T/err" && fail "download into image 3 should be refused"
grep -q "kept for a rollback" "$T/err" || fail "download into 3: $(cat "$T/err")"
echo "   kept a copy of the running image by hand; image 3 is not a download slot"

echo "== a sysupgrade that fails: the keep is undone"
fetch2 "file://$W/fw-2.0.img"
fw list | grep -q '^2 Available 0 0 2.0-r2 next$' || fail "image 2: $(fw list)"
touch "$F/fail"
fw activate 2 > "$T/out" 2>&1 && fail "activate should fail when sysupgrade does"
rm -f "$F/fail"
[ "$(running)" = 1.0-r1 ] && [ "$(img3)" = "NoImage -" ] && [ ! -e "$R/lib/upgrade/keep.d/aigw-rollback" ] &&
	[ -f "$R/etc/aigw-rollback/current.img" ] || fail "after a failed sysupgrade: $(img3); $(ls -R "$R")"
echo "   still on 1.0, its copy back where it was, nothing left to carry"

echo "== upgrade 1.0 -> 2.0: 1.0 kept on the overlay, over the flash"
fw activate 2 > "$T/out" 2>&1 || fail "activate 2: $(cat "$T/out")"
grep -q "kept a copy of 1.0-r1 on the flash (overlay) (image 3)" "$T/out" || fail "the words: $(cat "$T/out")"
[ "$(running)" = 2.0-r2 ] && [ "$(flashes)" = 1 ] || fail "should run 2.0: $(running)"
grep -q "flashed 2.0-r2 settings kept" "$F/log" || fail "flashed with the settings kept"
[ "$(img3)" = "Available 1.0-r1" ] || fail "image 3 should be the kept 1.0: $(fw list)"
[ ! -e "$R/lib/upgrade/keep.d/aigw-rollback" ] || fail "the copy is carried once, not on every sysupgrade"
grep -q "on 1.0" "$R/etc/config/aigw" || fail "the settings survived the upgrade"
[ ! -e "$R/etc/aigw-rollback/current.img" ] || fail "on the flash only the old copy is carried (not 2.0 too: the archive would double)"
fw rollback | grep -q '^copy a copy of 1.0-r1 is kept on the flash (overlay) (image 3)' || fail "rollback: $(fw rollback)"
echo "   on 2.0; image 3 = 1.0 (kept on the overlay, carried over the flash)"

echo "== the check fails: image 3 written back, with 1.0's settings and agd's record"
echo "option marker 'changed on 2.0'" > "$R/etc/config/aigw"
echo '{"state": "rolling back"}' > "$R/etc/aigw/agd-upgrade.json"
cp "$T/free" "$T/free.save"
sed -i "s|^$T/tmp .*|$T/tmp 600|" "$T/free"
AIGW_FW_CARRY=$R/etc/aigw/agd-upgrade.json fw activate 3 > "$T/out" 2>&1 && fail "going back should be refused without RAM for it"
grep -q "no room in RAM" "$T/out" && [ "$(running)" = 2.0-r2 ] && [ "$(img3)" = "Available 1.0-r1" ] ||
	fail "no RAM: $(cat "$T/out"); $(fw list)"
cp "$T/free.save" "$T/free"
AIGW_FW_CARRY=$R/etc/aigw/agd-upgrade.json fw activate 3 > "$T/out" 2>&1 || fail "activate 3: $(cat "$T/out")"
[ "$(running)" = 1.0-r1 ] || fail "should run 1.0 again: $(running)"
grep -q "flashed 1.0-r1 settings -f rollback-config.tgz" "$F/log" || fail "written back with its own settings"
grep -q "on 1.0" "$R/etc/config/aigw" || fail "1.0's settings: $(cat "$R/etc/config/aigw")"
grep -q "rolling back" "$R/etc/aigw/agd-upgrade.json" || fail "agd's record carried over"
[ "$(img3)" = "NoImage -" ] || fail "nothing kept after going back: $(fw list)"
echo "   back on 1.0 with its settings, agd's record carried"

echo "== no room on the flash"
router 400 8000
fw keep "file://$W/fw-1.0.img" 2> "$T/err" && fail "keep without room"
grep -q "no room on the flash" "$T/err" || fail "keep without room: $(cat "$T/err")"
fetch2 "file://$W/fw-2.0.img"
AIGW_FW_KEEP=require fw activate 2 > "$T/out" 2>&1 && fail "require: no upgrade without a copy"
grep -q "not upgraded: no copy of 1.0-r1 was kept: no room on the flash: 0.4 MB free" "$T/out" || fail "require: $(cat "$T/out")"
[ "$(running)" = 1.0-r1 ] && [ "$(flashes)" = 0 ] || fail "require: nothing written"
fw activate 2 > "$T/out" 2>&1 || fail "auto: $(cat "$T/out")"
grep -q "upgrading without a way back" "$T/out" && [ "$(running)" = 2.0-r2 ] && [ "$(img3)" = "NoImage -" ] ||
	fail "auto: $(cat "$T/out"); $(fw list)"
fw rollback | grep -q "Last upgrade: no copy of 1.0-r1 was kept: no room on the flash" || fail "rollback says why: $(fw rollback)"
AIGW_FW_KEEP=off fw list | grep -q '^3 ' && fail "firmware_keep off: no image 3"
AIGW_FW_KEEP=off fw rollback | grep -q '^none copies are not kept' || fail "off: $(AIGW_FW_KEEP=off fw rollback)"
echo "   refused in words (require), or upgraded without a way back (auto), and said so"

echo "== x86 and EFI: sysupgrade leaves its archive on the small boot partition"
router 40000 80000
mkdir -p "$R/boot"
echo "$R/boot 6000" >> "$T/free"; echo "/dev/fake1 $R/boot ext4 rw 0 0" >> "$T/mounts"
fw keep "file://$W/fw-1.0.img" > /dev/null || fail "keep with room on the boot partition"
sed -i "s|^$R/boot .*|$R/boot 300|" "$T/free"
fetch2 "file://$W/fw-2.0.img"
AIGW_FW_KEEP=require fw activate 2 > "$T/out" 2>&1 && fail "require: the copy can't ride over the flash"
grep -q "no room on the boot partition" "$T/out" && [ "$(flashes)" = 0 ] || fail "the boot partition: $(cat "$T/out")"
echo "   no room on the boot partition: said so, nothing written"

echo "== USB storage: kept there, not on the full flash, never in RAM; fetched again from where it came"
router 400 8000 usb ext4 6000 ram tmpfs 99999
fw rollback | grep -q "USB storage (/mnt/usb/aigw-rollback)" || fail "USB storage chosen, not tmpfs: $(fw rollback)"
fetch2 "http://fake/fw-2.0.img"
fw activate 2 > "$T/out" 2>&1 || fail "1.0 -> 2.0: $(cat "$T/out")"
grep -q "nothing to copy 1.0-r1 from" "$T/out" && [ "$(running)" = 2.0-r2 ] || fail "1.0 -> 2.0: $(cat "$T/out")"
fw rollback | grep -q "a copy of 2.0-r2 is fetched again" || fail "2.0's URL remembered: $(fw rollback)"
fetch2 "http://fake/fw-3.0.img"
# sysupgrade ended by the TERM of its own last stage, as on a real router: not a failure
FAKE_RC=143 AIGW_FW_WAIT=0 fw activate 2 > "$T/out" 2>&1 || fail "2.0 -> 3.0: $(cat "$T/out")"
grep -q "kept a copy of 2.0-r2 on USB storage" "$T/out" || fail "2.0 -> 3.0: $(cat "$T/out")"
grep -q "fetch http://fake/fw-2.0.img" "$F/log" || fail "2.0 fetched again from its URL"
[ "$(running)" = 3.0-r3 ] && [ "$(img3)" = "Available 2.0-r2" ] && [ -f "$R/mnt/usb/aigw-rollback/kept.img" ] &&
	[ ! -e "$R/mnt/ram/aigw-rollback" ] && [ ! -e "$R/etc/aigw-rollback" ] || fail "kept on USB: $(fw list); $(ls -R "$R/mnt")"
[ "$(cat "$R/mnt/usb/aigw-rollback/current.version")" = 3.0-r3 ] && [ ! -e "$R/mnt/usb/aigw-rollback/current.pending" ] ||
	fail "on USB, 3.0 is kept too, named after the boot"
fw activate 3 > "$T/out" 2>&1 || fail "back to 2.0: $(cat "$T/out")"
[ "$(running)" = 2.0-r2 ] && [ "$(img3)" = "NoImage -" ] || fail "back on 2.0: $(running); $(fw list)"
fw rollback | grep -q "the copy of 2.0-r2 on USB storage (/mnt/usb/aigw-rollback) becomes image 3" ||
	fail "the USB copy stays, ready for the next upgrade: $(fw rollback)"
echo "   2.0 fetched again and kept on USB; back on it, its copy stays there"

if [ "$(id -u)" != 0 ] || [ -z "$BIN" ] || [ ! -x "$BIN/agd/agd" ]; then
	echo "   (agd part skipped: needs root and a build directory)"
	echo "SINGLE-IMAGE TEST OK"
	exit 0
fi

echo "== agd on this handler: a bad upgrade rolled back through image 3, by itself"
command -v python3 >/dev/null || fail "missing python3"
UBUSD=$(command -v ubusd || echo /usr/local/sbin/ubusd)
UBUS=$(command -v ubus || echo /usr/local/bin/ubus)
router 4000 8000
fw keep "file://$W/fw-1.0.img" > /dev/null || fail "keep 1.0"
ALL="02:00:00:00:78:01 02:00:00:00:78:02 02:00:00:00:78:03 02:00:00:00:78:04"
echo "$ALL" > "$T/stations"
mkdir -p "$T/hostapd"
python3 - "$T/hostapd" "$T/stations" > "$T/fakehapd.log" 2>&1 <<'EOF' &
import os, socket, sys, time
d, stations = sys.argv[1:3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(os.path.join(d, "wlan0"))
start = time.time()
def present():
    return sorted(open(stations).read().split())
def sta(m):
    return "%s\nflags=[AUTH][ASSOC][AUTHORIZED][WMM][HT]\nsignal=-50\nconnected_time=%d\ntx_rate_info=1440\n" % (
        m, int(time.time() - start))
while True:
    data, addr = s.recvfrom(4096)
    cmd = data.decode().strip()
    w = cmd.split()
    if cmd == "STATUS":
        rep = "state=ENABLED\nfreq=2437\nchannel=6\nbssid[0]=02:aa:00:00:00:24\nssid[0]=AIGW-TEST\n"
    elif cmd == "STA-FIRST":
        p = present(); rep = sta(p[0]) if p else ""
    elif w[0] == "STA-NEXT":
        p = present(); i = p.index(w[1]) + 1 if w[1] in p else len(p)
        rep = sta(p[i]) if i < len(p) else ""
    else:
        rep = "UNKNOWN COMMAND\n"
    if addr:
        s.sendto(rep.encode(), addr)
EOF
PIDS="$PIDS $!"
mkdir -p "$T/config"
cat > "$T/config/aigw" <<EOF
config aigw 'main'
	option log_level 'debug'
	option leases '$T/leases'
	option state_file '$T/state.json'
	option model_dir '$REPO/models'

config dns 'dns'
	option listen '127.0.0.1'
	option port '15358'

config flow 'flow'
	option enabled '0'

config meter 'meter'
	option enabled '0'

config wifi 'wifi'
	option ctrl_dir '$T/hostapd'
	option keys '$T/wifi-keys'
	option steer '0'

config care 'care'
	option enabled '0'

config act 'act'
	option dns_policy '$T/dns-policy'

config plugins 'plugins'
	option enabled '0'

config models 'models'
	option dir '$T/models'
	option keys '$T/keys.d'

config mgmt 'mgmt'
	option firmware_handler '$FW'

config upgrade 'upgrade'
	option watch '45'
	option reconnect '15'
	option report_delay '3'
	option file '$R/etc/aigw/agd-upgrade.json'

config why 'why'
	option enabled '0'
	option file '$T/why.bin'

config lan 'lan'
	list iface 'lo'
EOF
: > "$T/leases"
export AIGW_UCI_CONFDIR="$T/config" AIGW_UBUS="$T/ubus.sock"
"$UBUSD" -s "$AIGW_UBUS" > "$T/ubusd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5
start_agd() { "$BIN/agd/agd" >> "$T/agd.log" 2>&1 & AGD=$!; PIDS="$PIDS $AGD"; sleep 1; }
stop_agd() { kill "$AGD"; wait "$AGD" 2>/dev/null || true; }
agctl() { "$BIN/agctl/agctl" "$@"; }
call() { "$UBUS" -s "$AIGW_UBUS" call ag.dm "$@"; }
js() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
up() { agctl -j upgrade | js "$1"; }
wait_up() {
	i=0
	until up "sys.exit(0 if $2 else 1)" 2>/dev/null; do
		i=$((i + 1)); [ $i -le $(($1 * 2)) ] || fail "$3: $(agctl -j upgrade)"; sleep 0.5
	done
}
dmget() { agctl dm get "$1" | sed 's/^[^=]*= //'; }
start_agd
i=0
until [ "$(agctl -j wifi | js "print(sum(len(r['stations']) for r in d['radios']))" 2>/dev/null)" = 4 ]; do
	i=$((i + 1)); [ $i -le 30 ] || fail "the 4 devices should be seen: $(agctl wifi)"; sleep 0.5
done
[ "$(dmget Device.DeviceInfo.SoftwareVersion)" = 1.0-r1 ] || fail "agd should see 1.0-r1"
[ "$(dmget Device.X_AIGW_.Upgrade.RollbackMethod)" = KeptCopy ] || fail "RollbackMethod: $(agctl dm get Device.X_AIGW_.Upgrade.)"
dmget Device.X_AIGW_.Upgrade.RollbackNote | grep -q "the copy of 1.0-r1 on the flash (overlay) becomes image 3" ||
	fail "RollbackNote: $(dmget Device.X_AIGW_.Upgrade.RollbackNote)"
[ "$(dmget Device.DeviceInfo.FirmwareImageNumberOfEntries)" = 3 ] && [ "$(dmget Device.DeviceInfo.FirmwareImage.3.Status)" = NoImage ] ||
	fail "three images, the third empty: $(agctl dm get Device.DeviceInfo.FirmwareImage.)"
agctl upgrade | grep -q "way back  one firmware image, and a kept copy" || fail "agctl upgrade: $(agctl upgrade)"

# on 2.0, 3 of the 4 stay away: hidden already, as agd here outlives the (fake) flash and
# would see them on 2.0 at once (they still count as connected before: seen minutes ago)
echo "02:00:00:00:78:01" > "$T/stations"
call operate "{\"command\":\"Device.DeviceInfo.FirmwareImage.2.Download()\",\"args\":{\"URL\":\"file://$W/fw-2.0.img\",\"AutoActivate\":\"true\"}}" |
	js "sys.exit(0 if d['ok'] else 1)" || fail "download and activate 2.0"
i=0
until [ "$(flashes)" = 1 ] && grep -q "reboot (fake)" "$F/log"; do
	i=$((i + 1)); [ $i -le 40 ] || fail "2.0 should be written and the reboot asked for"; sleep 0.5
done
stop_agd                                                # the reboot into 2.0
start_agd
wait_up 20 "d['state'] == 'Watching' and d['to'] == '2.0-r2' and len(d['devices']) == 4" "watching 2.0, 4 devices before"
[ "$(dmget Device.DeviceInfo.FirmwareImage.3.Status)" = Available ] && [ "$(dmget Device.DeviceInfo.FirmwareImage.3.Version)" = 1.0-r1 ] ||
	fail "image 3 = the kept 1.0: $(agctl dm get Device.DeviceInfo.FirmwareImage.)"
wait_up 40 "d['state'] in ('RollingBack', 'RolledBack')" "worse: rolling back"
i=0
until [ "$(flashes)" = 2 ]; do
	i=$((i + 1)); [ $i -le 40 ] || fail "1.0 should be written back"; sleep 0.5
done
grep -q "flashed 1.0-r1 settings -f rollback-config.tgz" "$F/log" || fail "written back with its own settings"
sleep 1
stop_agd                                                # the reboot back into 1.0
echo "$ALL" > "$T/stations"
start_agd
wait_up 20 "d['state'] == 'RolledBack'" "back on 1.0 (agd's record carried over the flash)"
[ "$(dmget Device.DeviceInfo.SoftwareVersion)" = 1.0-r1 ] || fail "should run 1.0 again"
wait_up 20 "d.get('confirmed') == 'yes'" "the devices came back on 1.0: confirmed"
up "sys.exit(0 if d['held'] == '2.0-r2' and 'Rolled back to 1.0-r1 (image 3)' in d['summary'] else 1)" || fail "the record: $(agctl -j upgrade)"
agctl upgrade

echo "== a kept copy that appears a moment after agd listed the images (a USB disk mounted late)"
K=$R/etc/aigw-rollback                                  # where this router keeps it (the flash)
[ "$(dmget Device.DeviceInfo.FirmwareImage.3.Status)" = NoImage ] || fail "image 3 should be empty on 1.0"
# within agd's listing cache (FW_CACHE): Activate() must look again, not refuse on what it listed
mkdir -p "$K"; cp "$W/fw-3.0.img" "$K/kept.img"; echo 3.0-r3 > "$K/kept.version"
fw list | grep -q '^3 Available 0 0 3.0-r3 kept$' || fail "image 3 = the copy put there: $(fw list)"
call operate '{"command":"Device.DeviceInfo.FirmwareImage.3.Activate()"}' > "$T/out" 2>&1
js "sys.exit(0 if d['ok'] else 1)" < "$T/out" || fail "Activate() on a copy that just appeared: $(cat "$T/out")"
i=0
until [ "$(flashes)" = 3 ]; do
	i=$((i + 1)); [ $i -le 40 ] || fail "3.0 should be written"; sleep 0.5
done
[ "$(running)" = 3.0-r3 ] || fail "should run 3.0: $(running)"
echo "   activated at once, on the handler's word, not agd's listing of a moment before"
echo "SINGLE-IMAGE TEST OK"
