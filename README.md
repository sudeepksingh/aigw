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

> **Status: v0.3, phase 2 "understand" (advise mode).** AIGW knows every
> device (identity, Wi-Fi, lookups, connections, traffic) and now has its
> first on-device AI: a domain-risk model that recognises machine-generated
> names used by malware. It explains in plain words what deserves attention:
> machine-generated lookups, connections without DNS, and devices hiding
> their DNS. Nothing is blocked yet; policies and enforcement are phase 3.
> See the [roadmap](docs/ARCHITECTURE.md#16-roadmap).

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
agctl devices          # every device: type, Wi-Fi signal, lookups, traffic
agctl device <name>    # one device in detail
agctl flows            # recent connections, with the name each one belongs to
agctl alerts           # what AIGW thinks you should know, and why
agctl score <domain>   # ask the domain-risk model about any name
agctl watch all        # DNS, connections, Wi-Fi joins and alerts, live
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

## On-device AI

The domain-risk model (`models/domain-risk.agm`, 52 KB, 150 small decision
trees) runs inside `agd` through a tiny C interpreter. No ML framework is
needed on the router. It is trained by
[`cortex/training/domain_risk/train.py`](cortex/training/domain_risk/train.py)
on popular sites (Tranco list) and synthetic machine-generated names.

| Held-out test, alert at score >= 0.98 | |
|---|---|
| False alarms on popular sites | 0.15% |
| Generated names caught, per name | 68% overall (hex 95%, base32 99%, random letters 66%, pronounceable 11%) |

Malware using a name generator looks up dozens of such names, so an infected
device is flagged quickly even though single names can slip through.
Pronounceable and dictionary-word generators are a known weak spot; spotting
those needs per-device behaviour baselines (next step).

Retrain: `python3 cortex/training/domain_risk/train.py` (needs `numpy`,
`scikit-learn`). It also writes golden data that the C test uses to check
it computes exactly the same scores.

## What you get in v0.3

| Component | What it does |
|-----------|--------------|
| `agdns` | DNS sensor: forwards client DNS (UDP + TCP) and reports every answered query to the brain. Keeps working if the brain stops. |
| `agd` | The brain. Joins all sensors into one device table and saves it across restarts. **Identity:** DHCP fingerprint, MAC, hostname, type guess. **Wi-Fi:** signal and link rate from hostapd. **Flows:** every connection from conntrack, labelled with the DNS name it belongs to, bytes up/down, and a flag for connections made without any DNS lookup. |
| Alerts | Advise mode: `dga` (machine-generated lookup), `no-dns` (connection without a lookup), `dns-bypass` (device using its own encrypted DNS). Each has a plain-language reason; repeats are counted, not repeated. |
| `agctl` | Command line: `status`, `devices`, `device`, `dns`, `flows`, `alerts`, `score`, `watch` |
| `aigw-nft` | Kernel rules (`inet ag` table): DNS interception, quarantine set, connection-mark actions |
| `aigw-ap` | PC as AP: hostapd, DHCP (with fingerprint hook), NAT and the services |

API (same on every host):

```bash
ubus call ag status
ubus call ag.device list
ubus call ag.dns log '{"limit": 20}'
ubus call ag.flow recent '{"limit": 20}'
ubus call ag.alert list
ubus call ag.domain score '{"name": "example.com"}'
ubus listen ag.dns ag.flow ag.wifi ag.alert
```

## Repository layout

```
include/ag/          public headers (events, DNS parser, connection-mark layout)
core/libag/          shared library: logging, config (UCI), DNS parser
agd/                 the brain: devices, endpoints (DNS<->flow join), flow and Wi-Fi sensors
agdns/               the DNS sensor
agctl/               command line
scripts/             aigw-nft (kernel rules), aigw-dhcp-event (DHCP hook), build-deps.sh
config/aigw          default configuration (UCI, used on every host)
platform/pc-ap/      PC as AP: installer, aigw-ap, systemd units, WSL launcher
platform/openwrt/    OpenWRT package (feed) and SDK build script
models/              trained on-device models (.agm)
cortex/training/     Python training pipelines that produce those models
tests/               unit tests, model parity test, smoke test, flow test (network namespaces)
docs/                architecture and getting-started guides
```

## Develop

```bash
sudo ./scripts/build-deps.sh          # libubox, ubus, uci (once)
cmake -S . -B build && cmake --build build
ctest --test-dir build                # unit tests
sudo tests/smoke.sh build             # runs agd + agdns + DNS queries on loopback
sudo tests/flow-test.sh build         # client/server namespaces: DNS<->flow join, bytes, no-DNS flag
```

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0, see [LICENSE](LICENSE).
