# AIGW — AI-Native Gateway Architecture

Status: design **v0.3** (2026-11-07) · implementation **v0.7**, phases P0–P4 built (see [roadmap](#16-roadmap)) · Working name: **AIGW** (daemons `agd`, `agdns`)
Hosts: **OpenWRT router** and **PC as AP** (same packages, same API) · Visual version: [architecture.html](architecture.html)

> **The gateway that understands the home, not just the network.**
> One small AI core turns any OpenWRT router, or any PC acting as a Wi-Fi access point, into a
> gateway that knows who is home, what every device and IoT gadget does, what is normal, and what
> to do about it. It works fully inside the home, gets smarter with optional cloud services outside,
> and everything around the core is a plug-in.

| < 1 MB | < 16 MB | < 2 ms | 0 | 2 hosts |
|---|---|---|---|---|
| flash for the core | RAM, fits 128 MB routers | AI decision per new connection | cloud services required | OpenWRT router · PC as AP |

---

## 1. Vision and design rules

Every component is either a **sensor** (feeds facts to the AI) or an **actuator** (carries out its
decisions). The intelligence lives in one place; everything else plugs into it.

| # | Rule | Meaning |
|---|------|---------|
| R1 | **AI-centric** | Components publish typed events and expose actions; none hard-codes policy |
| R2 | **Plug and play** | Sensors, models, actions, integrations are plug-ins that wire themselves in; zero config on first boot |
| R3 | **Inside first, outside optional** | Everything essential runs on the gateway; cloud adds fleet intelligence, never a dependency |
| R4 | **Never break the home** | Fail-open: if the AI stops, traffic and DNS keep working with the last safe policy |
| R5 | **Small and portable** | C core, fixed-size tables, same code and API on OpenWRT and on a PC acting as AP |
| R6 | **Explainable and reversible** | Every decision carries reasons, confidence, a time limit and an undo |
| R7 | **Observe → advise → enforce** | New capabilities watch first, then suggest, and act only when trusted |
| R8 | **Private by design** | Raw data stays home; only consented, anonymized features leave |

---

## 2. Runs anywhere

| Mode | What it is | Install | Best for |
|------|-----------|---------|----------|
| **OpenWRT router** | The router itself becomes the AI gateway (OpenWRT 23.05+, ≥ 128 MB RAM) | `opkg install aigw` | Homes, ISPs, router makers, small offices |
| **PC as AP** | Linux PC (or Windows + WSL2) with a USB Wi-Fi adapter becomes the access point and forwards to the existing router | `agctl host setup --ap wlan1` | Developers, labs, demos, offices with a spare PC; heavy models can run on the same machine |
| **Router + companion** | OpenWRT router runs the core; a PC, NAS or cloud runs the cortex | `agctl cortex pair https://…` | Power users, managed services |

### Shared stack — only the platform layer differs

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Plug-ins · models · skills · connectors      (same packages on both)     │
├──────────────────────────────────────────────────────────────────────────┤
│ AIGW core: agd · agdns · ag-bridge · libag   (identical C code + API)    │
├──────────────────────────────────────────────────────────────────────────┤
│ libubox · ubus · UCI · libnftnl · libnetfilter_*  (built for PC too)     │
├────────────────────────────────────┬─────────────────────────────────────┤
│ Platform layer (PC as AP)          │ Platform layer (OpenWRT)            │
│ hostapd · dnsmasq · systemd · NAT  │ netifd · fw4 · procd · odhcpd · LuCI│
├────────────────────────────────────┴─────────────────────────────────────┤
│ Linux kernel: nftables · conntrack · tc · nl80211 · eBPF                 │
├────────────────────────────────────┬─────────────────────────────────────┤
│ x86 PC + USB Wi-Fi (RTL8821CU,mt76)│ Router SoC (Filogic, Qualcomm, NPU) │
└────────────────────────────────────┴─────────────────────────────────────┘
```

### PC as AP topology

```
Phones · laptops · IoT ──Wi-Fi 10.42.0.0/24──► USB Wi-Fi (AP) ─► Linux / WSL2
                                                 hostapd · dnsmasq (DHCP) · agdns · agd · nft (inet ag + NAT)
                                               ─► host network ──Ethernet──► existing home router ─► Internet
```

The home router does not change. Every device joining the PC's Wi-Fi passes through AIGW.

| Concern | PC as AP | OpenWRT |
|---------|----------|---------|
| Access point | hostapd on the USB adapter | netifd + `/etc/config/wireless` |
| DHCP | dnsmasq `port=0` | dnsmasq `port=0` or odhcpd |
| Firewall | `inet ag` + own NAT table | `inet ag` alongside fw4 |
| QoS | HTB + fq_codel | CAKE / HTB |
| Services | systemd | procd |
| Build | CMake, native gcc | OpenWRT SDK package (CMake) |
| Storage | `/var/lib/ag` | RAM `/tmp/ag` + hourly compact snapshot |

---

## 3. Use cases

**Inside** = runs on the gateway, works offline. **Outside** = opt-in cloud services. 🟢 = IoT.

### Inside the home
| Use case | What it does | Built from |
|----------|--------------|-----------|
| Know every device | Names and classifies every device, even with random MACs | disc · wifi · dns → device model |
| 🟢 IoT lockdown | Learns each IoT device's normal destinations, builds its allow-list, isolates on deviation | dns · flow → baseline → nft quarantine |
| Malware & phishing shield | Stops DGA, tunnelling and lookalike domains before the connection starts | agdns risk model → sinkhole |
| 🟢 Privacy X-ray | Shows which trackers/clouds each TV, speaker, camera talks to; one-tap block | dns · flow → explanations |
| Person-aware priority | Video calls get priority on whichever device the person uses | dns pre-label · flow app model → tc |
| Family schedules | Bedtime, homework, focus modes per person and category | policy · person → dns + nft |
| 🟢 Camera-free presence | Room-level presence and activity from Wi-Fi signals | wifi RSSI/CSI → presence CNN |
| Daily routine care | Alerts family when an older relative's usual routine doesn't happen | presence · routines → notify |
| Self-healing Wi-Fi | Channel choice, band steering, failing-link prediction | wifi stats → hostapd |
| Faster browsing | Predictive DNS prefetch | dns sequence model |
| 🟢 One-scan IoT onboarding | QR join with a per-device key, auto-placed in IoT zone | hostapd DPP · disc → zone |
| Ask your home, locally | Local LLM on NPU gateways answers "why was Wi-Fi slow?" | ubus tools → local LLM |

### Outside (cloud, opt-in)
| Use case | What it does | Built from |
|----------|--------------|-----------|
| Collective threat intelligence | A threat seen in one home protects all; federated, no raw data | bridge → cortex → signed model |
| Model training & OTA | Feedback retrains models; signed models roll out to fleets | feedback → training → `.agm` OTA |
| Ask your home, anywhere | Chat from a phone; LLM proposes, gateway checks and confirms | MCP tools → guardrails |
| ISP proactive support | Separates Wi-Fi vs line vs website problems with evidence | sys · wifi → operator portal |
| 🟢 IoT vulnerability alerts | Device model/firmware matched against CVE feeds; isolates exposed devices | device id → CVE feed → quarantine |
| Care & insurance services | Consented wellbeing/presence signals to a provider | presence → consented connector |
| 🟢 Energy & tariff optimization | Shifts EV charging and heavy loads via Home Assistant / Matter | presence · tariffs → connector |
| Small-business reports | Security/usage reports, guest policies, compliance evidence | decision log → cortex |

---

## 4. See it decide (scenarios)

**A. Smart camera talks to a strange server**

| Time | Component | What happens |
|------|-----------|--------------|
| t+0 ms | agdns (sensor) | Camera asks for `x7f3k9q2.top`, a name it never used |
| t+0.3 ms | risk model (brain) | 0.94 "machine-generated"; not in the camera's 3-domain baseline |
| t+0.4 ms | dns policy (actuator) | IoT zone in enforce mode → sinkhole answer; event to agd |
| t+2 s | flow (sensor) | Camera connects straight to `203.0.113.50` with no DNS lookup |
| t+2.001 s | agd (brain) | `R_NO_DNS_BEFORE_CONN` + `R_NEW_DOMAIN_FOR_DEVICE` → quarantine 1 h |
| t+2.002 s | nft (kernel) | MAC into `ag_quarantine`; dropped at packet speed except vendor cloud |
| t+3 s | notify (you) | "Front-door camera tried to reach an unknown server. Isolated for 1 hour." Keep / Undo |
| later | cortex | Anonymized indicator joins the fleet model (with consent) |

**B. A video call starts** — agdns labels the meeting service's IPs "real-time" → the first packet
gets the priority mark in the kernel → flow model confirms "video call" (0.97) → the brain marks the
person's other bulk traffic → tc gives low latency → marks expire with the connection.

**C. A new smart plug joins** — QR join with its own key → DHCP/mDNS/vendor fingerprints →
"smart plug" (0.91), IoT zone, observe → 24 h baseline (3 domains, ~40 KB/h) → "Lock it down?"
one tap → allow-list enforced → optional daily CVE check.

---

## 5. System overview

```
                 ┌──────────── OUTSIDE · CORTEX (cloud / PC / NAS, optional) ───────────┐
                 │ training · fleet intelligence · LLM agent (MCP) · history · connectors│
                 └──────────────▲───────────────────────────────────┬───────────────────┘
                   features (opt-in)                    signed models, proposals
┌──────────────────────────── INSIDE · ON THE GATEWAY ──────────────────────────────────┐
│  SENSORS                    agd · BRAIN                              ACTUATORS        │
│  agdns   (intent)   ──►  ingest + identity                    ──►  nft  sets · marks  │
│  flow    (behaviour)──►  Context Graph (DNS ⇄ flow)           ──►  tc   QoS classes   │
│  wifi    (radio)    ──►  feature engine (streaming)           ──►  dns policy         │
│  disc    (identity) ──►  inference runtime (.agm)             ──►  wifi / iot actions │
│  iot     (MQTT, Matter, Zigbee) ──► policy engine + guardrails ──► notify (app)       │
│  plug-ins (yours)   ──►  decision log · ubus ag.*                                     │
│                                                                                        │
│  KERNEL DATA PLANE: Wi-Fi AP · conntrack · nftables "inet ag" · tc · eBPF (later)     │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

The brain never edits firewall rules; it only writes **labels** (connection marks, set entries)
that fixed kernel rules act on.

| Tier | Latency | Does | Never |
|------|---------|------|-------|
| **Kernel** | µs per packet | Looks up the connection's label, applies the action | Thinks |
| **Brain · inside** | 0.1–5 ms per new query/flow | `agd` + `agdns`: identify, classify, score, decide, label | Per-packet work on established flows |
| **Cortex · outside** | seconds–hours, async | Training, fleet intelligence, LLM, history, connectors | Sits in the packet path |

---

## 6. Plug and play

| Kind | Role | Examples |
|------|------|----------|
| **Sensor** | Feeds events | Zigbee dongle, smart meter, camera motion feed |
| **Actuator** | Carries out actions | VLAN switch, smart lock, Matter command |
| **Model** | Signed `.agm` that turns features into scores | domain risk, device type |
| **Skill** | Bundle: policies + models + app card | "Elder care", "IoT lockdown" |
| **Connector** | Links outside | Home Assistant, ISP portal, care provider, threat feed |

**Manifest**

```json
{
  "name":    "ag-sensor-zigbee",
  "kind":    "sensor",
  "version": "0.3.0",
  "emits":   ["iot.zigbee.attr", "iot.zigbee.join"],
  "needs":   ["usb:serial"],
  "caps":    [],
  "budget":  { "ram_kb": 900, "cpu_pct": 2 },
  "hosts":   ["openwrt", "linux"]
}
```

**On install:** announce (`ag.plugin.<name>` on ubus) → check (signature, permissions, RAM/CPU
budget) → wire (events into the Context Graph, actions into the actuator list) → activate (models
whose inputs are now available switch on; the app shows the capability).

```
opkg install ag-skill-iot-lockdown         # OpenWRT router
agctl plugin add ag-skill-iot-lockdown     # either host
```

**Zero configuration — first 48 hours:** 0 min installed, observe mode · 10 min devices identified
and grouped into people · 24 h baselines learned · 48 h first suggestions ("Lock down 6 IoT
devices?") · ongoing learning from every Undo/Keep.

---

## 7. IoT support

```
Wi-Fi devices ─┐
Matter/Thread ─┤                ┌──────────── AIGW IoT lifecycle ────────────┐     Home Assistant
Zigbee · BLE  ─┼──► events ──►  │ 1 Onboard → 2 Identify → 3 Zone            │ ──► Matter controllers
MQTT telemetry ┘                │ 6 Automate ← 5 Enforce ← 4 Learn           │     Cloud (CVE, energy)
                                └── drift → back to Learn · firmware watch ──┘
```

| Capability | How | Built on |
|------------|-----|----------|
| Onboarding | QR join (Wi-Fi Easy Connect); per-device Wi-Fi key, revocable individually | hostapd DPP, per-device PSK file |
| Identification | DHCP, mDNS, vendor prefix, 802.11 fingerprints → device-type model | `disc`, `wifi` |
| Zones | IoT network/VLAN, client isolation, per-device allow-lists; honors MUD (RFC 8520) | hostapd, nft sets |
| Telemetry as context | Door, temperature, power readings become events for presence/routine models | MQTT broker plug-in |
| Matter / Thread | Border router plug-in; Matter devices profiled like any other | OpenThread border router |
| Automation | Learned routines → suggested automations | connectors |
| Lifecycle | Firmware/vulnerability watch, end-of-support warnings, auto-isolation | cortex (outside) |

---

## 8. Components

### Processes
| Process | Lang | Role | Why separate |
|---------|------|------|--------------|
| `agdns` | C | AI DNS (sensor + actuator) | Critical path, untrusted input, sandboxed, survives brain restarts |
| `agd` | C | Brain + built-in modules + plug-in host | Shared memory, one event loop, lowest RAM |
| `agctl` | C | CLI: setup, plug-ins, debugging | Scripting |
| `ag-bridge` | C | Optional MQTT/TLS link to cortex | Internet-facing code isolated |
| `cortex` | Python | Training, fleet intelligence, LLM, connectors | Cloud/PC/NAS, never on the router |
| App / UI | LuCI · web · mobile | Status, decisions, feedback | ubus only |

### `agdns` — AI DNS

```
client :53 → parse (strict, fuzzed) → identify (IP→dev_id) → policy snapshot → cache
          → risk model (DGA · tunnel · lookalike) → upstream (unbound/DNSSEC or DoT/DoH) → answer
          ↳ cache hit: p99 < 1 ms        ↳ blocked: sinkhole / redirect
          ↳ every answer: label IPs in nft sets (with TTL) + events → agd
```

- Forwarder, not a recursor; recursion + DNSSEC delegated.
- Keeps the last policy snapshot if `agd` is down.
- Predictive prefetch; bypass handling (port-53 redirect, `use-application-dns.net` canary, DoH/DoT detection, `dns.bypass`).
- Fuzzed parser, ASan in CI, only `CAP_NET_BIND_SERVICE`, `ujail` + seccomp on OpenWRT.

### `agd` — brain pipeline
1. **Ingest** — one uloop loop; built-in modules call directly, agdns/plug-ins via Unix socket
2. **Identity** — MAC/IP/fingerprints → `dev_id`, `person_id`
3. **Context Graph** — entity tables, including the DNS ⇄ flow join
4. **Features** — streaming, fixed memory (EWMA, count-min, HyperLogLog, bloom, windows)
5. **Inference** — `.agm` models, ≤ 2 ms per new flow
6. **Policy** — scores + user policies → action from the ladder
7. **Guardrails** — local checks even cloud/LLM proposals must pass
8. **Decision log** — reasons, `explain`, `revert`

Signals only the DNS ⇄ flow join can see: connection with no prior DNS lookup · far more data to a
domain than baseline · resolving via another resolver · traffic shape contradicting domain category.

Escalation ladder: `observe → notify → deprioritize → throttle → quarantine → block`

Guardrails: never touch the gateway, DHCP/DNS to the gateway, or protected devices · TTL + auto-revert
on every enforcing action · per-device/hour rate limits · block needs the highest confidence ·
observe/advise never touch traffic.

### Built-in sensors and actuators
| Module | Type | Source / mechanism |
|--------|------|--------------------|
| flow | sensor | conntrack events, nft counters; opt-in NFQUEUE first packets (SNI, JA4, sizes) with fail-open `bypass`; eBPF later |
| wifi | sensor | hostapd events, nl80211 station stats, RSSI/CSI presence sensing |
| disc | sensor | DHCP leases + fingerprints, passive mDNS / SSDP / LLDP |
| sys | sensor | interface counters, WAN latency/jitter, CPU/memory |
| act.nft | actuator | set elements and ct marks in `inet ag` |
| act.tc | actuator | HTB + fq_codel or CAKE classes by ct mark |
| act.dns | actuator | policy snapshot to `agdns` |
| act.wifi | actuator | hostapd band steering, disassociate, per-station limits |
| act.notify | actuator | app/UI notifications, push via bridge |

### Kernel data plane

```nft
table inet ag {
  set ag_quarantine { type ether_addr; timeout 1h; }
  set ag_dst_risky  { type ipv4_addr; flags timeout; }
  map ag_action { type mark : verdict; elements = {
      0x01 : accept, 0x02 : jump prio, 0x03 : jump bulk,
      0x04 : jump throttle, 0x05 : jump quarantine, 0x06 : drop } }

  chain forward {
    type filter hook forward priority filter - 5;           # before fw4 on OpenWRT
    ether saddr @ag_quarantine jump quarantine
    ip daddr @ag_dst_risky ct mark set ct mark | 0x0700     # DNS pre-label
    ct mark & 0xff == 0 ct packets < 6 queue num 0 bypass   # opt-in first-packet look, fail-open
    meta mark set ct mark
    ct mark & 0xff vmap @ag_action
  }
}
```

### Cortex · outside
- **Bridge (C, gateway):** MQTT/TLS with per-device certificate; exports features and decisions, never raw packets; offline buffer with cap; accepts only ed25519-signed (usign-compatible) models and proposals.
- **Services (Python):** fleet history/training data (ClickHouse) · training → `.agm` → sign → OTA · federated threat intelligence · LLM agent as MCP server (write tools only propose) · connectors (Home Assistant, ISP portals, care providers, feeds).

---

## 9. Shared data model

```
 Person 1──* Device 1──* Iface(MAC) ──* Address(IP)              Policy ──produces──► Decision
                │                                                  (subject: device|flow|domain|person)
                ├──* Flow ──to──► Endpoint(IP) ◄══ DNS answer binds IP → name ══ Domain
                ├──* Query(DNS) ──for──────────────────────────────────────────► Domain
                ├── Baseline (sketches)
                └── Zone / Room (IoT zone · presence)
```

| Entity | Key fields | Size |
|--------|-----------|------|
| Device | `dev_id` (stable across MAC randomization), MACs, IPs, name, vendor, type, owner, trust, zone | 256 |
| Person | `person_id`, name, devices, presence | 16 |
| Domain | name, category, risk, first seen in home, popularity | 16k LRU |
| Endpoint | IP → {domain, client, expiry} | 32k LRU |
| Flow | 5-tuple, device, domain, app label, bytes/pkts, timing, ct mark, verdict | 4096 LRU |
| Baseline | domain set, bytes/hour, peers, active hours | 1 per device |
| Decision | subject, action, reasons, confidence, model ver, mode, TTL, revert | ring 1024 |
| Policy | subject, condition, action, mode, schedule, origin | 128 |

**Events:** libubox `blobmsg` everywhere — `ts_ms · src · type · dev_id? · severity · body{...}`.
Namespaces: `dns.*`, `flow.*`, `wifi.*`, `disc.*`, `iot.*`, `sys.*`, `decision.*`, `user.feedback`;
plug-ins add their own.

**Connection mark (AI → kernel contract):**

```
 31        24 23        16 15         8 7          0
┌───────────┬────────────┬────────────┬────────────┐
│ flags/ver │ zone/trust │ app class  │  action    │
└───────────┴────────────┴────────────┴────────────┘
action: 0 unlabeled · 1 allow · 2 priority · 3 bulk · 4 throttle · 5 quarantine · 6 block · 7 mirror
```

Reason codes (`R_DGA_HIGH`, `R_NO_DNS_BEFORE_CONN`, `R_VOLUME_SPIKE`, …) plus feature values travel
with every decision, so explanations are never invented after the fact.

---

## 10. On-device AI runtime

| Op family | Covers | Size |
|-----------|--------|------|
| Tree ensembles | device type, flow app, domain risk | ~5 KB |
| Linear / logistic | quick scorers, calibration | ~1 KB |
| MLP (int8) | combined risk | ~5 KB |
| 1D-CNN (int8) | presence, char-level domains | ~15 KB |

`.agm` = header (op family, input-schema hash, quantization) + weights + ed25519 signature. Schema
hash mismatch → refused. NPU gateways can register an accelerated back-end for the same op families.

| Starter model | Inputs | Output | Size |
|---------------|--------|--------|------|
| Domain risk | char n-grams, entropy, length, TLD, age in home | DGA / tunnel / lookalike | ≤ 100 KB |
| Device type | DHCP, vendor, 802.11 fp, mDNS, domains | type + confidence | ≤ 100 KB |
| Flow app | first-N sizes/directions/gaps, SNI category | app class | ≤ 150 KB |
| Device anomaly | baseline vs current sketches | score (statistical) | 0 |
| Presence / activity | RSSI / CSI windows | room, activity | ≤ 100 KB |

---

## 11. Policies

UCI on both hosts, or drafted from plain language by the LLM agent; each compiles into DNS rules,
nft set membership and a QoS class.

```
config policy 'kids_bedtime'
	option subject 'person:kids'
	option when    'daily 21:00-07:00'
	option match   'category:video category:social category:games'
	option action  'block'
	option mode    'enforce'

config policy 'iot_lockdown'
	option subject 'zone:iot'
	option match   'domain:!baseline'
	option action  'quarantine'
	option mode    'advise'
```

"Turn off the kids' games at 9 pm on school nights" → agent drafts → gateway validates → you approve.

---

## 12. For developers

- **Write once:** plug-ins built on `libag` run on OpenWRT and on a PC acting as AP.
- **Test without a home:** record and replay event streams deterministically.
- **AI-native API:** every `ag.*` method is also an MCP tool.
- **Open core:** core, SDK and starter models open source; signed plug-ins can be shared or sold.

```c
/* Illustrative draft API */
#include <ag/plugin.h>

static void on_query(const struct ag_event *ev, void *ctx)
{
    const char *name = ag_ev_str(ev, "qname");
    if (ag_domain_in(name, "my-watchlist"))
        ag_emit(ctx, "custom.watch", AG_SEV_WARN,
                "dev_id", ag_ev_dev(ev), "qname", name);
}

AG_PLUGIN("watchlist", AG_KIND_SENSOR)
{
    ag_subscribe(plugin, "dns.query", on_query);
}
```

```python
from aigw.cortex import skill

@skill("weekly-family-report")
def weekly(home):
    usage = home.flows.top(by="bytes", days=7, group="person")
    alerts = home.decisions.list(days=7)
    return home.llm.summarize(usage, alerts, tone="friendly")
```

```
ubus call ag.device list
ubus call ag.decision explain '{"id": 4182}'
agctl plugin add ag-sensor-zigbee
```

| Object | Methods |
|--------|---------|
| `ag.device` | list · get · set (name/owner/trust) · merge |
| `ag.person` | list · set |
| `ag.flow` | top · get |
| `ag.dns` | stats · log · allow · block |
| `ag.decision` | list · explain · revert · confirm |
| `ag.policy` | list · add · validate · set_mode |
| `ag.model` | list · load · rollback |
| `ag.plugin` | list · add · remove · status |
| `ag.feedback` | label |
| events | subscribe `ag.event.*` |

### Repository layout
```
aigw/
  include/ag/           public headers: events, DNS parser, mark layout (plugin API later)
  core/libag/           logging, UCI config, DNS parser (later: entity tables, sketches, .agm runtime)
  agd/                  brain (later: modules/{flow,wifi,disc,sys,act})
  agdns/                AI DNS
  agctl/                CLI
  plugins/              first-party plug-ins (mqtt connector, iot-guard skill) and examples
  scripts/              aigw-nft (kernel rules), build-deps.sh (OpenWRT libs for PCs)
  config/aigw           default UCI configuration, shared by every host
  platform/pc-ap/       installer, aigw-ap, systemd units, WSL launcher
  platform/openwrt/     package feed (Makefile, procd init, uci-defaults), SDK build script
  tests/                unit tests, runtime smoke test (later: fuzz, event and pcap replay)
  docs/                 architecture (md + html), getting-started guides
  -- planned --
  bridge/               ag-bridge (MQTT)            P5
  cortex/               Python: training, LLM/MCP   P5
```

---

## 13. For businesses

**Why now:** Wi-Fi 7 router chips ship NPUs and 1–2 GB RAM · IoT growth (Matter) with nobody
managing device behaviour · LLMs make networks explainable · privacy favors local processing.

| Adopter | How they use AIGW | Value |
|---------|-------------------|-------|
| Internet providers | White-label AI gateway on OpenWRT-based CPE (incl. prplOS-style stacks) | Fewer support calls, premium tier, lower churn |
| Router makers | Ship AIGW as the "smart" feature set | Differentiation without building an AI stack |
| Security vendors | Models and threat feeds as signed plug-ins | Reach many gateways through one SDK |
| IoT makers | Verified device profiles and MUD files | Safer devices, fewer returns, "works with AIGW" |
| Care / insurance | Consented presence and routine signals | Camera-free monitoring services |
| IT service providers | Managed small-office gateways + reports | Recurring revenue |
| Developers | Build and sell plug-ins and skills | Marketplace on every gateway |

**Business models:** open core · cortex subscription (fleet, federated intel, remote access) ·
plug-in/skill marketplace revenue share · OEM/operator licensing and certification.

| | Typical smart router | AIGW |
|--|---------------------|------|
| Where AI runs | Vendor cloud | On the gateway, cloud optional |
| Extensible | Closed | Plug-in SDK, MCP |
| IoT protection | Blocklists | Learned per-device allow-lists |
| Explains decisions | Rarely | Every decision, with reasons |
| Hardware | Vendor-locked | Any OpenWRT device or a PC |
| Presence sensing | Motion yes/no, if any | Room and activity |

---

## 14. Footprint budget

| Item | Flash | RAM |
|------|-------|-----|
| `agd` | ~300 KB | ~6 MB |
| state tables | — | ~4 MB |
| `agdns` | ~150 KB | ~3 MB |
| `ag-bridge` (optional) | ~60 KB | ~1 MB |
| models | ~300 KB | ~0.5 MB |
| plug-in headroom | — | ~1.5 MB |
| **Total** | **< 1 MB** | **< 16 MB** |

Profiles: **lite** (128 MB routers: no first-packet inspection, no Wi-Fi sensing) · **standard**
(256 MB+) · **pro** (NPU gateways or PC as AP: local LLM, heavy models).
Targets: DNS cache hit p99 < 1 ms added · new-connection decision < 2 ms · zero per-packet cost on
established connections · forwarding throughput unchanged.

---

## 15. Security and privacy

- **Least privilege:** `agdns` binds port 53 only; `agd` holds only `CAP_NET_ADMIN`; plug-ins get what their manifest declares; `ujail` on OpenWRT.
- **Signed everything:** models, plug-ins, remote policies (ed25519); unsigned refused.
- **Propose only:** cloud and LLM propose; local guardrails and the user decide.
- **Local-first:** only consented features leave, MACs/IPs hashed.
- **Household consent:** "what AIGW sees" page, per-person opt-out for wellbeing features.

---

## 16. Roadmap

Every phase ships on both hosts; CI builds Linux x86-64 and the OpenWRT SDK from day one.

| Phase | Deliverable | Exit test | Status |
|-------|-------------|-----------|--------|
| **P0 Foundation** | libag, event schema, mark layout; PC-as-AP bring-up; OpenWRT packages in CI | A phone joins the PC's AIGW Wi-Fi; same packages install on an OpenWRT x86 VM | v0.1 |
| **P1 Observe** | agdns, flow/disc/wifi/sys, Context Graph, agctl, first app screens | Live "who talks to what" per device and person | v0.2 (no app screens yet) |
| **P2 Understand** | `.agm` runtime, domain-risk + device-type models, baselines, DNS ⇄ flow signals | Correct device types; explained alerts on test cases | v0.3, v0.4 (device type is rule-based) |
| **P3 Act** | Policy engine, guardrails, nft/tc/dns/wifi actuators, TTL + revert | Quarantine/priority work and auto-revert; DNS/DHCP never blocked | v0.5, completed in v0.6 |
| **P4 Plug & IoT** | Manifest + auto-wiring, skills, QR onboarding, per-device keys, MQTT, Home Assistant | A third-party plug-in installs and activates with no config | v0.7 |
| **P5 Outside** | Bridge, fleet history, training loop, OTA, LLM agent via MCP, remote access | A retrained model reaches a fleet and hot-loads | |
| **P6 Hardware** | LuCI app, procd tuning, OpenWRT One + Wi-Fi 7 boards, footprint validation | Same test suite passes on routers within budget | |
| **P7 Advanced** | eBPF fast path, QUIC SNI, prefetch, CSI presence, band steering, federated learning, NPU back-end | Measured latency and detection gains | |

Real-router testing of every phase so far is planned for the end of the project (P6).

**As built in v0.6 (P3).** Alerts flow through `config policy` sections to *decisions*
(`ag.decision`: list · request · approve · reject · revert). Modes: `advise` (default; proposals wait
for a person) · `enforce` (automatic, at most `max_auto_per_hour`) · `off`. The whole escalation
ladder has an actuator:

| Rung | Action | Actuator |
|------|--------|----------|
| notify | alert | `ag.alert` event, plain-language reason |
| (boost) | priority | IP in nft set `priority4/6`, DSCP CS5 → WMM video queue |
| deprioritize | deprioritize | IP in nft set `bulk4/6`, DSCP CS1 → WMM background queue |
| throttle | throttle | IP in nft set `throttle4/6` + per-device `limit rate` rules in chain `throttle`, both directions. nft policing instead of tc: no extra kernel modules, and each rule checks the timed set, so it goes inert when the element expires |
| quarantine | quarantine | MAC in `inet ag quarantine` (routed) and `bridge ag quarantine` (LAN-to-LAN through a bridge; needs kmod-nft-bridge); forwarding only, so DHCP/DNS to the gateway keep working |
| block | block | agdns NXDOMAIN for a name and its subdomains, UDP and TCP, home-wide or for one device |
| (Wi-Fi) | disconnect | hostapd deny list + deauthenticate, on every AP interface |

Guardrails: never the gateway, never off-LAN or protected devices, TTL 10 s – `max_ttl` on every
action with expiry enforced by the kernel and agdns themselves (Wi-Fi bans by agd, also across
restarts), blocking never automatic, only well-formed MACs/IPs reach nft and hostapd. Decisions
persist and are re-applied after restarts, firewall reloads and hostapd restarts. Verified by
`tests/act-test.sh` against real nft rules in network namespaces, with a bridged LAN and a fake
hostapd. Not covered: Wi-Fi clients talking directly through the same radio (use `disconnect`
or AP isolation); band steering needs a multi-band AP and moves to P7 with the rest of steering.

**As built in v0.7 (P4).** The plug-in host in `agd` (`plugin.c`) makes the section 6 lifecycle real:

| Step | As built |
|------|----------|
| Discover | `plugins.dir` (packages, trusted) and `plugins.user_dir` (`agctl plugin add`) scanned every 5 s; a changed manifest reloads, a removed folder stops the plug-in |
| Check | manifest (name = folder, kind, lists, actions, policies, budget), `hosts`, safe file permissions for added plug-ins, `needs` (`bin:`, `file:`, `tcp:host:port` with `${config}` values, `event:<type>` from another running plug-in): unmet → *waiting*, re-checked every scan |
| Wire | JSON lines on stdin/stdout. In: `emit` (types from the manifest, never `ag.*`; a `mac`/`ip` field attaches telemetry to the device), `alert` (kinds from the manifest → policies), `propose`, `request` (acts for a person: privileged), `done`, `query` (`status`, `devices`/`decisions`: privileged), `log`. Out: `hello` (config from the manifest and `config plugin` UCI sections, granted/denied capabilities), subscribed `event`s (`ag.dns`/`ag.flow`: privileged), `act`/`undo` for its actions, `reply` |
| Act | a plug-in action `<plugin>.<action>` is a decision like any other: guardrails, TTL, undo, policies; re-sent when the plug-in restarts |
| Skill | policies (alert → action, optional `zone`) join the policy table after your own; a skill you added only proposes until allowed |
| Run | child process as `plugins.user` (`nobody`) in its own process group, minimal environment; crash → restart with 1–32 s back-off, 5 crashes in 10 min → failed; RSS above `ram_kb` or CPU above `cpu_pct` (3 checks in a row) → stopped |

Trust is simple: packages are trusted (the package manager installed them as root);
for plug-ins you added, privileged capabilities wait for `agctl plugin allow`. Signed
side-loaded plug-ins (ed25519) arrive with model signing in P5; `ujail` sandboxing with P6.

IoT onboarding (`keys.c`): `agctl key add <name> [zone]` → a fresh passphrase with a `keyid`
in hostapd's `wpa_psk_file` + `RELOAD_WPA_PSK` + a `WIFI:` QR code (encoder in `libag`). The
station's `keyid` (hostapd `STA` output) binds the key to the first MAC that uses it; the device
takes the key's name and zone; `revoke` removes the line and deauthenticates. Policies take
`option zone`. Connector: `ag-mqtt` (MQTT 3.1.1, QoS 0, own client) with Home Assistant
discovery (device tracker + Quarantine switch per device, alerts and waiting-decision sensors),
`aigw/sensor/#` → `iot.mqtt` events, `aigw/cmd/<action>` → requests. Shipped as package
`aigw-mqtt`. Verified by `tests/plug-test.sh` (fake hostapd, private mosquitto).
Not yet: Wi-Fi Easy Connect (DPP) bootstrapping, IoT zone network isolation (VLAN / client
isolation; the zone is a label policies use today), Matter/Thread, Zigbee.

---

## 17. Status and risks

Development PC checked 2026-11-06 (WSL2 kernel 6.18.54, RTL8821CU):

| Capability | Status | Note |
|------------|--------|------|
| nf_tables, conntrack + events, NAT | built in | |
| nfnetlink_queue, nft_queue, nft_ct | module | first-packet inspection, ct marks |
| sch_htb, sch_fq_codel | module | QoS on the PC |
| sch_cake | **missing** | HTB + fq_codel on PC; CAKE on OpenWRT |
| BPF syscall, XDP sockets | built in | eBPF fast path possible |
| rtw88_8821cu | module | interface needs the firmware workaround |
| hostapd, dnsmasq, conntrack tools | **not installed** | install in P0 |

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Regulatory DB not loaded on PC | 5 GHz AP may be refused | load wireless-regdb, set country; 2.4 GHz fallback |
| USB Wi-Fi AP stability (rtw88 over USB/IP) | brief disconnects | hostapd watchdog; mt76 adapters recommended |
| One adapter can't be AP and sensor | no presence sensing while AP | second USB adapter; routers have several radios |
| DNS bypass | partial DNS view | port-53 redirect, canary, DoH detection, flow signals |
| False positives | lost trust | observe/advise first, TTL, one-tap feedback |
| C parsing untrusted data | gateway compromise | fuzzing, ASan, sandboxing |
| Third-party plug-ins | misbehaving code | signatures, declared permissions, budgets, ujail |

**Open decisions:** product name · second (mt76) adapter for the PC · `agdns` default upstream
(local unbound vs encrypted provider) · first reference router for P6 · open-core licence.
