# Getting started: OpenWRT router

AIGW installs as one package on OpenWRT 23.05 or newer. It runs next to the
standard firewall (fw4) and dnsmasq and changes neither: removing the package
puts the router back exactly as it was.

## 1. Find your router's target

On the router:

```bash
cat /etc/openwrt_release | grep -E 'DISTRIB_RELEASE|DISTRIB_TARGET|DISTRIB_ARCH'
```

Example: `DISTRIB_TARGET='mediatek/filogic'`, `DISTRIB_ARCH='aarch64_cortex-a53'`.

## 2. Build the package

On any Linux PC (or WSL2), as a normal user:

```bash
sudo apt install build-essential curl rsync unzip file gawk python3 python3-setuptools zstd libncurses-dev
git clone https://github.com/sudeepksingh/aigw.git
cd aigw
TARGET=mediatek/filogic VERSION=24.10 platform/openwrt/build-sdk.sh
```

The script downloads the official OpenWRT SDK for that target and version
(cached in `~/.cache/aigw-sdk`), builds the package and puts it in
`dist/openwrt/<version>/<arch>/`. Use the same major version as the router
(`VERSION=24.10`, `VERSION=25.12`, ...).

Packages are per CPU architecture, so one build serves every router with the
same `DISTRIB_ARCH`.

| Common routers | `TARGET` | Architecture |
|----------------|----------|--------------|
| OpenWRT One, Filogic Wi-Fi 6/7 routers | `mediatek/filogic` | aarch64_cortex-a53 |
| MT7621 routers (many Wi-Fi 5 / 6) | `ramips/mt7621` | mipsel_24kc |
| Qualcomm Atheros (older) | `ath79/generic` | mips_24kc |
| PCs, VMs | `x86/64` | x86_64 |

Already have an OpenWRT build tree? Add the feed instead:

```
src-link aigw /path/to/aigw/platform/openwrt
```

then `./scripts/feeds update aigw && ./scripts/feeds install aigw` and select
`Network → aigw` in `make menuconfig`.

## 3. Install on the router

```bash
scp dist/openwrt/24.10.*/aarch64_cortex-a53/aigw_*.ipk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
```

On the router:

```bash
opkg update
opkg install /tmp/aigw_*.ipk          # OpenWRT 25.12+: apk add --allow-untrusted /tmp/aigw-*.apk
/etc/init.d/aigw enable
/etc/init.d/aigw start
```

The package pulls in libubox, ubus and uci from the official feeds (they are
already on almost every image).

## 4. Use it

```bash
agctl status
agctl devices
agctl watch
```

The same data is on ubus for LuCI, scripts and other tools:

```bash
ubus call ag.device list
ubus listen ag.dns
```

## How it fits into OpenWRT

```
LAN client ──DNS :53──► nft "inet ag" redirect ──► agdns :5300 ──► dnsmasq :53 ──► upstream
                                                      │
                                                      └─ ubus event ag.dns ──► agd (brain)
```

| Piece | Where |
|-------|-------|
| Services | `/etc/init.d/aigw` (procd, respawned on crash) |
| Settings | `/etc/config/aigw` (`uci show aigw`) |
| Kernel rules | table `inet ag`, loaded by `aigw-nft`, kept across firewall reloads by the fw4 include `firewall.aigw` |
| Logs | `logread -e agd -e agdns` |

The LAN interface defaults to `br-lan`. Change it with
`uci set aigw.lan.iface='br-lan br-guest' && uci commit aigw && /etc/init.d/aigw restart`.

Turn off DNS interception (AIGW then sees only clients that use the router as
resolver): `uci set aigw.dns.intercept=0`.

## Remove

```bash
opkg remove aigw     # or: apk del aigw
```

The removal script stops the services, deletes the `inet ag` table and the
firewall include.
