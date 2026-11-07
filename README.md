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

> **Status: v0.6, phase 3 "act" complete.** AIGW knows every device (identity, Wi-Fi,
> lookups, connections, traffic), learns what is normal for each one, and
> runs an on-device AI model that recognises machine-generated names used by
> malware. It explains in plain words what deserves attention, and now it can
> act: quarantine a device, prioritise one, or block a domain. Every action
> has a time limit and an undo, guardrails that nothing can override, and by
> default **nothing changes until you approve it**. See the
> [roadmap](docs/ARCHITECTURE.md#16-roadmap).

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
agctl decisions        # actions AIGW proposes, and the ones in effect
agctl score <domain>   # ask the domain-risk model about any name
agctl watch all        # DNS, connections, Wi-Fi joins, alerts and decisions, live
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
Pronounceable and dictionary-word generators are a known weak spot of the
name model; per-device behaviour baselines cover part of that gap.

## Behaviour baselines

Every device gets a baseline, learned continuously in about 1 KB:

- **the sites it uses**, as registrable domains in a Bloom filter
- **how many new sites it picks up per day**
- **its usual upload per hour**
- **the hours of the day it is active**

After 24 hours and 20 sites a device is *steady* and changes raise alerts:

| Alert | When |
|-------|------|
| `new-destination` | A device with a stable pattern (fewer than 2 new sites a day, typical of IoT) uses a site it never used. Phones and laptops never raise this. |
| `upload-spike` | Upload in an hour is 6 standard deviations above the device's usual, and at least 50 MB |
| `unusual-hour` | Traffic at an hour of day the device has never been active in, after 7 days of history |

Thresholds are in `/etc/config/aigw` (`baseline` section).

Retrain: `python3 cortex/training/domain_risk/train.py` (needs `numpy`,
`scikit-learn`). It also writes golden data that the C test uses to check
it computes exactly the same scores.

## Acting: decisions and guardrails

```
alert ──► policy ──► proposal ──(you approve, or enforce mode)──► guardrails ──► actuator
                                                                    every action: time limit + undo
```

From mild to strong:

| Action | Command | How it works |
|--------|---------|--------------|
| Prioritise a device | `agctl prioritize <device> [time]` | Its traffic is marked DSCP CS5, which Wi-Fi (WMM) sends in the video queue |
| Deprioritise a device | `agctl deprioritize <device> [time]` | DSCP CS1: the Wi-Fi background queue, so it yields to everything else |
| Throttle a device | `agctl throttle <device> [rate] [time]` | A per-device speed limit in nft, both directions (`2mbit`, `500kbit`; default 2 Mbit/s) |
| Block a domain | `agctl block <domain> [time] [device]` | `agdns` answers "no such name" for it and every subdomain, over UDP and TCP, for every device or just one |
| Quarantine a device | `agctl quarantine <device> [time]` | Its MAC goes into the nft sets `quarantine` (routed traffic) and `bridge ag quarantine` (other devices on a bridged LAN): it still reaches the gateway (DHCP, DNS), nothing beyond it |
| Disconnect from Wi-Fi | `agctl disconnect <device> [time]` | hostapd kicks it off and refuses it until the time is up |

`<device>` is an IP, MAC or name from `agctl devices`; `time` is like `90s`,
`30m`, `2h` or `1d` (default 1 hour). `agctl decisions` lists what is waiting
for you and what is in effect; `agctl approve <id>`, `agctl reject <id>` and
`agctl undo <id>` answer them. Asking again for an action that is in effect
restarts its time limit (and, for throttle, sets the new rate).

**Modes** (`act.mode` in `/etc/config/aigw`):

- `advise` (default): policies *propose*; nothing changes until you approve.
- `enforce`: policies act on their own, within the guardrails.
- `off`: alerts only.

Built-in policies propose a quarantine for a device that looks up a
machine-generated name (`dga`) and for a stable IoT device that suddenly uses
a new site (`new-destination`). Add your own `config policy` sections.

**Guardrails**, checked for every action whoever asks (a policy, you, later
the cloud):

- never the gateway itself, never a device outside the LAN, never a device
  in `act.protect`;
- always a time limit, at most `max_ttl` (1 day). The kernel (nft timeouts)
  and `agdns` (per-entry expiry) end actions on their own, even if `agd`
  stops (Wi-Fi bans are lifted by `agd`, also after a restart);
- automatic actions only in enforce mode, at most `max_auto_per_hour` (3),
  and blocking a domain is never automatic;
- DNS and DHCP to the gateway are never affected.

Actions are saved and put back after a restart or a firewall reload.

## What you get in v0.6

| Component | What it does |
|-----------|--------------|
| `agdns` | DNS sensor and actuator: forwards client DNS (UDP + TCP), reports every answered query to the brain, and answers blocked names itself. Keeps working if the brain stops. |
| `agd` | The brain. Joins all sensors into one device table and saves it across restarts. **Identity:** DHCP fingerprint, MAC, hostname, type guess. **Wi-Fi:** signal and link rate from hostapd. **Flows:** every connection from conntrack, labelled with the DNS name it belongs to, bytes up/down, and a flag for connections made without any DNS lookup. |
| Decisions | Policies, proposals, approval, guardrails and six actions (prioritize, deprioritize, throttle, block, quarantine, disconnect), each with a time limit and undo. See above. |
| Alerts | `dga` (machine-generated lookup), `no-dns` (connection without a lookup), `dns-bypass` (device using its own encrypted DNS), `new-destination`, `upload-spike`, `unusual-hour` (behaviour baselines). Each has a plain-language reason; repeats are counted, not repeated. A connection to an address next to a name the home looked up (same /24) is labelled "(nearby)" instead of flagged, which keeps apps with their own server lists, such as WhatsApp, quiet. |
| `agctl` | Command line: `status`, `devices`, `device`, `dns`, `flows`, `alerts`, `score`, `watch`, `decisions`, `approve`, `reject`, `undo`, `prioritize`, `deprioritize`, `throttle`, `block`, `quarantine`, `disconnect` |
| `aigw-nft` | Kernel rules: `inet ag` (DNS interception, quarantine, priority, background and throttle sets, connection-mark actions) and `bridge ag` (quarantine inside a bridged LAN, where the kernel supports it) |
| `aigw-ap` | PC as AP: hostapd, DHCP (with fingerprint hook), NAT and the services |

API (same on every host):

```bash
ubus call ag status
ubus call ag.device list
ubus call ag.dns log '{"limit": 20}'
ubus call ag.flow recent '{"limit": 20}'
ubus call ag.alert list
ubus call ag.domain score '{"name": "example.com"}'
ubus call ag.decision list
ubus call ag.decision request '{"action": "block", "target": "example.com", "ttl": 600}'
ubus call ag.decision request '{"action": "throttle", "target": "tv", "rate": 3000}'
ubus call ag.decision approve '{"id": 4}'      # also: reject, revert
ubus listen ag.dns ag.flow ag.wifi ag.alert ag.decision
```

## Repository layout

```
include/ag/          public headers (events, DNS parser, connection-mark layout)
core/libag/          shared library: logging, config (UCI), DNS parser
agd/                 the brain: devices, endpoints (DNS<->flow join), flow and Wi-Fi sensors,
                     alerts, baselines, decisions (act.c: policies, guardrails, actuators)
agdns/               the DNS sensor
agctl/               command line
scripts/             aigw-nft (kernel rules), aigw-dhcp-event (DHCP hook), build-deps.sh
config/aigw          default configuration (UCI, used on every host)
platform/pc-ap/      PC as AP: installer, aigw-ap, systemd units, WSL launcher
platform/openwrt/    OpenWRT package (feed) and SDK build script
models/              trained on-device models (.agm)
cortex/training/     Python training pipelines that produce those models
tests/               unit tests, model parity test, smoke test, network-namespace tests (flow, baseline, act)
docs/                architecture and getting-started guides
```

## Develop

```bash
sudo ./scripts/build-deps.sh          # libubox, ubus, uci (once)
cmake -S . -B build && cmake --build build
ctest --test-dir build                # unit tests
sudo tests/smoke.sh build             # runs agd + agdns + DNS queries on loopback
sudo tests/flow-test.sh build         # client/server namespaces: DNS<->flow join, bytes, alerts
sudo tests/baseline-test.sh build     # behaviour baselines on a fast clock (1 s = 1 hour)
sudo tests/act-test.sh build          # decisions, guardrails and actuators against real nft rules
```

The namespace tests are safe to run on a machine that is itself running
AIGW: they use their own namespaces, bus and configuration.

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Apache License 2.0, see [LICENSE](LICENSE).
