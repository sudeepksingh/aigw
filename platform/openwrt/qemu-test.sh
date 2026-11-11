#!/bin/sh
# Install the built packages on a real OpenWRT system in QEMU and run
# aigw-selftest --full there: procd, fw4, opkg dependencies, ubus, rpcd and
# the firewall integration, as on a router, before any router is touched.
#
#   platform/openwrt/build-sdk.sh                       # first: build the packages
#   platform/openwrt/qemu-test.sh                       # x86-64, VERSION=24.10 by default
#   ARCH=mipsel_24kc platform/openwrt/qemu-test.sh      # MT7621 packages on malta (MIPS)
#   ARCH=aarch64_cortex-a53 platform/openwrt/qemu-test.sh   # Filogic packages on armsr (ARM64)
#   KEEP=1 platform/openwrt/qemu-test.sh                # leave it running, LuCI on :8080 (LUCI_PORT)
#
# Checks: aigw-selftest --full inside OpenWRT, then LuCI from the host as the
# AIGW pages use it (rpcd ACL, pages, menu, remote-access QR code). On MIPS
# and ARM the CPU-level checks (signatures, model scores) run on that CPU.
# Then management by an ISP (MGMT=0 skips): OB-USP-Agent with the plug-in
# answers on the device, icwmp informs an ACS on the host and answers it; on
# x86-64 the ACS upgrades the firmware for real (sysupgrade, reboot, the
# configuration kept, TransferComplete).
# x86-64 uses KVM when /dev/kvm is usable; the others are emulated (slower).
# Needs qemu-system-x86_64 / -mipsel / -aarch64, python3, curl, gzip.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-24.10}
ARCH=${ARCH:-x86_64}
MIRROR=${MIRROR:-https://downloads.openwrt.org}
CACHE=${QEMU_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aigw-qemu}
PORT=${PORT:-$((20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 20000))}
LUCI_PORT=${LUCI_PORT:-8080}

die() { echo "qemu-test: $*" >&2; exit 1; }
for t in python3 curl gzip; do command -v $t >/dev/null || die "missing $t"; done

case $VERSION in
*.*.*) ;;
*)
	VERSION=$(curl -fsSL --retry 3 --retry-all-errors "$MIRROR/releases/" | grep -oE "href=\"$VERSION\.[0-9]+/\"" |
		grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
	[ -n "$VERSION" ] || die "no OpenWRT release found"
	;;
esac

# what to boot for each package architecture
NET="-netdev user,id=lan -netdev user,id=wan,hostfwd=tcp:127.0.0.1:$LUCI_PORT-:80"
ADD_ARCH=""
case $ARCH in
x86_64)
	TGT=x86/64 FILE="openwrt-$VERSION-x86-64-generic-ext4-combined.img.gz" QEMU=qemu-system-x86_64
	ACCEL="-accel tcg"
	if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; fi
	BOOT="$ACCEL -m 256 -smp 2 -drive file=@IMG@,format=raw $NET -device e1000,netdev=lan -device e1000,netdev=wan"
	;;
mipsel_24kc)
	TGT=malta/le FILE="openwrt-$VERSION-malta-le-vmlinux-initramfs.elf" QEMU=qemu-system-mipsel
	BOOT="-M malta -m 256 -kernel @IMG@ $NET -device pcnet,netdev=lan -device pcnet,netdev=wan"
	;;
aarch64_cortex-a53)
	# armsr packages are aarch64_generic; the Filogic build runs on any ARMv8
	TGT=armsr/armv8 FILE="openwrt-$VERSION-armsr-armv8-generic-initramfs-kernel.bin" QEMU=qemu-system-aarch64
	BOOT="-M virt -cpu cortex-a53 -m 256 -kernel @IMG@ $NET -device virtio-net-pci,netdev=lan -device virtio-net-pci,netdev=wan"
	ADD_ARCH=aarch64_cortex-a53
	;;
*)
	die "ARCH: x86_64, mipsel_24kc or aarch64_cortex-a53"
	;;
esac
command -v $QEMU >/dev/null || die "missing $QEMU"
PKGS="$REPO/dist/openwrt/$VERSION/$ARCH"

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$FILE" ]; then
	echo "==> downloading $FILE"
	curl -fL --http1.1 --retry 5 --retry-all-errors -C - --progress-bar -o "$CACHE/$FILE.part" \
		"$MIRROR/releases/$VERSION/targets/$TGT/$FILE"
	mv "$CACHE/$FILE.part" "$CACHE/$FILE"
fi
W=$(mktemp -d)
trap 'kill $HTTP 2>/dev/null; rm -rf "$W"' EXIT INT TERM
case $FILE in
*.gz)
	gzip -dc "$CACHE/$FILE" > "$W/boot.img" 2>/dev/null || true    # "trailing garbage" is normal
	;;
*)
	cp "$CACHE/$FILE" "$W/boot.img"
	;;
esac
[ -s "$W/boot.img" ] || die "could not unpack $FILE"

# only this version's packages (older builds stay in dist/); the
# architecture-independent ones may sit in any architecture's folder
V=$(sed -n 's/^PKG_VERSION:=//p' "$REPO/platform/openwrt/aigw/Makefile")
mkdir -p "$W/pkgs"
for f in "$PKGS"/*aigw*[_-]"$V"-r*; do
	[ -f "$f" ] && cp "$f" "$W/pkgs/"
done
# aigw-usp and aigw-cwmp need the clients they serve (their own versions); their libraries come from OpenWRT's feed
for f in "$PKGS"/obuspa[_-]* "$PKGS"/icwmp[_-]*; do
	[ -f "$f" ] && cp "$f" "$W/pkgs/"
done
ls "$W/pkgs" | grep -q . || die "no packages of version $V in $PKGS (build-sdk.sh with that target)"
(cd "$W/pkgs" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
HTTP=$!

# managed by an ISP (MGMT=0 skips): an ACS on the host (tests/cwmp/acs.py), the
# USP agent asked on the device; on x86-64 a real firmware upgrade from the ACS
# (this same image, through icwmp, agd's firmware handler and sysupgrade)
MGMT=${MGMT:-1}
ISP=""
NOREBOOT="-no-reboot"
ACS=""
if [ "$MGMT" = 1 ]; then
	ACS_PORT=$((PORT + 1))
	mkdir -p "$W/acs/files"
	python3 "$REPO/tests/cwmp/acs.py" serve "$W/acs" --port "$ACS_PORT" > "$W/acs.log" 2>&1 &
	ACS=$!
	ISP="--acs=$W/acs:$ACS_PORT"
	if [ "$ARCH" = x86_64 ]; then
		cp "$CACHE/$FILE" "$W/acs/files/$FILE"
		ISP="$ISP --upgrade=$FILE"
		NOREBOOT=""     # the upgrade reboots: QEMU goes on
	fi
fi
trap 'kill $HTTP ${ACS:-} 2>/dev/null; rm -rf "$W"' EXIT INT TERM

echo "==> OpenWRT $VERSION $TGT in QEMU ($QEMU), packages: $(ls "$W/pkgs" | tr '\n' ' ')"
LOG="$REPO/dist/openwrt/qemu-$VERSION-$ARCH.log"
: > "$LOG"
set +e
# shellcheck disable=SC2086
python3 "$REPO/platform/openwrt/qemu_console.py" "$LOG" "$W/pkgs" "$PORT" "$LUCI_PORT" ${KEEP:+--keep} \
	${ADD_ARCH:+--add-arch=$ADD_ARCH} $ISP -- \
	$QEMU $(echo "$BOOT" | sed "s|@IMG@|$W/boot.img|") -display none -serial stdio -monitor none $NOREBOOT
rc=$?
set -e
echo "==> console log: $LOG"
[ $rc -eq 0 ] && echo "QEMU OPENWRT TEST OK ($ARCH)" || echo "QEMU OPENWRT TEST FAILED ($ARCH: $rc)"
exit $rc
