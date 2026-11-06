# AIGW: AI-native home gateway

[![ci](https://github.com/sudeepksingh/aigw/actions/workflows/ci.yml/badge.svg)](https://github.com/sudeepksingh/aigw/actions/workflows/ci.yml)

**The gateway that understands the home, not just the network.**

AIGW is a small AI core that turns an **OpenWRT router**, or **any Linux PC
acting as a Wi-Fi access point**, into a gateway that knows which devices are
on the network, what each one does and what is normal, and acts on it.
Everything runs on the gateway itself. Cloud services are optional, and
everything around the core is a plug-in.

- C core, under 1 MB, built for 128 MB routers
- The same code, API and plug-ins on OpenWRT and on a PC acting as AP
- Fail-open: if AIGW stops, your network keeps working

Read the design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · or open
[docs/architecture.html](docs/architecture.html) in a browser for the
illustrated version.

> **Status: v0.1 "observe".** AIGW runs the access point, sees every device
> and every DNS query, and serves that over its API. Models, policies and
> enforcement come next. See the [roadmap](docs/ARCHITECTURE.md#16-roadmap).

---

## Quick start: PC as AP

You need a Debian/Ubuntu PC (WSL2 works), an internet connection on Ethernet
or another adapter, and a Wi-Fi adapter that supports AP mode (most USB
adapters with Realtek rtw88 or MediaTek mt76 chips do).

```bash
git clone https://github.com/sudeepksingh/aigw.git
cd aigw
sudo ./install.sh
sudo aigw-ap up
```

The installer prints the Wi-Fi name and password. Join it from a phone, then:

```bash
agctl devices      # every device and what it talks to
agctl watch        # DNS activity, live
```

**On Windows (WSL2):** install inside WSL as above, then start everything from
Windows with `platform\pc-ap\wsl\aigw-wsl.cmd`. It hands the USB adapter to
WSL and brings the access point up.

Full guide: [docs/getting-started-pc-ap.md](docs/getting-started-pc-ap.md)

## Quick start: OpenWRT router

OpenWRT 23.05 or newer. Build the package for your router's CPU (on any Linux
PC, takes about 10 minutes the first time):

```bash
git clone https://github.com/sudeepksingh/aigw.git
cd aigw
TARGET=mediatek/filogic platform/openwrt/build-sdk.sh   # your router's target
```

Copy the package to the router and install it:

```bash
scp dist/openwrt/*/*/aigw* root@192.168.1.1:/tmp/
ssh root@192.168.1.1 'opkg install /tmp/aigw_*.ipk && /etc/init.d/aigw enable && /etc/init.d/aigw start'
```

On OpenWRT 25.12 and newer use `apk add --allow-untrusted /tmp/aigw-*.apk`.

Full guide: [docs/getting-started-openwrt.md](docs/getting-started-openwrt.md)

## What you get in v0.1

| Component | What it does |
|-----------|--------------|
| `agdns` | DNS sensor: forwards client DNS (UDP + TCP) and reports every answered query to the brain. Keeps working if the brain stops. |
| `agd` | The brain: builds the device table (DHCP leases, ARP, DNS activity) and DNS log, and serves them over ubus |
| `agctl` | Command line: `status`, `devices`, `dns`, `watch` |
| `aigw-nft` | Kernel rules (`inet ag` table): DNS interception, quarantine set, connection-mark actions |
| `aigw-ap` | PC as AP: hostapd, DHCP, NAT and the services |

API (same on every host):

```bash
ubus call ag status
ubus call ag.device list
ubus call ag.dns log '{"limit": 20}'
ubus listen ag.dns
```

## Repository layout

```
include/ag/          public headers (events, DNS parser, connection-mark layout)
core/libag/          shared library: logging, config (UCI), DNS parser
agd/                 the brain
agdns/               the DNS sensor
agctl/               command line
scripts/             aigw-nft (kernel rules), build-deps.sh (OpenWRT libs for PCs)
config/aigw          default configuration (UCI, used on every host)
platform/pc-ap/      PC as AP: installer, aigw-ap, systemd units, WSL launcher
platform/openwrt/    OpenWRT package (feed) and SDK build script
tests/               unit tests and a runtime smoke test
docs/                architecture and getting-started guides
```

## Develop

```bash
sudo ./scripts/build-deps.sh          # libubox, ubus, uci (once)
cmake -S . -B build && cmake --build build
ctest --test-dir build                # unit tests
sudo tests/smoke.sh build             # runs agd + agdns + DNS queries on loopback
```

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0, see [LICENSE](LICENSE).
