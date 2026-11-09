# AIGW OS — Architecture

Status: design **v0.3** (2026-11-07) · implementation **v0.16**, phases P0–P5 built, P6 waiting for real routers, P7 under way (see [roadmap](#16-roadmap)) · Name: **AIGW OS** (daemons `agd`, `agdns`; packages and paths `aigw`)
Hosts: **OpenWRT router** and **PC as AP** (same packages, same API) · Visual version: [architecture.html](architecture.html)

> **The AI-powered operating system for home gateways.**
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
│  KERNEL DATA PLANE: Wi-Fi AP · conntrack · nftables "inet ag" · flowtable · tc · eBPF │
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
| `ag-bridge` | C | Optional HTTPS link to cortex (a connector plug-in, TLS by curl) | Internet-facing code isolated, unprivileged |
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
- **Bridge (C, gateway):** a connector plug-in, unprivileged; HTTPS through curl with a per-gateway token; exports state, alerts and decisions, never raw packets or DNS logs; each report carries recent history, so a missed one is covered by the next; models it fetches go to `agd`, which accepts only ed25519-signed (usign-compatible) ones; proposals wait for a person.
- **Services (Python):** fleet history and training data (SQLite; a column store for large fleets later) · training → `.agm` → sign → OTA · federated threat intelligence · LLM agent as MCP server (write tools only propose) · connectors (Home Assistant, ISP portals, care providers, feeds).

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
hash mismatch → refused. NPU gateways can register an accelerated back-end for the same op families (since v0.16: [model-backends.md](model-backends.md)).

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
  include/ag/           public headers: events, DNS parser, .agm models, QR, signatures
  core/libag/           logging, UCI config, DNS parser, .agm runtime, domain features, QR,
                        SHA-512 + Ed25519 verification
  agd/                  brain: devices, sensors, alerts, baselines, decisions, plug-in host,
                        Wi-Fi keys, models (signed updates, hot-load, rollback), threat list,
                        remote access
  agdns/                AI DNS
  agctl/                CLI
  plugins/              first-party plug-ins (mqtt and bridge connectors, iot-guard skill), examples
  cortex/aigw_cortex/   Python: fleet history, labels, retrain/sign/publish, bridge API, MCP server
  cortex/training/      Python training pipelines (domain-risk)
  models/               packaged .agm models
  scripts/              aigw-nft (kernel rules), aigw-remote (WireGuard), build-deps.sh (OpenWRT libs for PCs)
  config/aigw           default UCI configuration, shared by every host
  platform/pc-ap/       installer, aigw-ap, systemd units, WSL launcher
  platform/openwrt/     package feed (Makefile, procd init, uci-defaults), SDK build script
  tests/                unit tests, smoke test, namespace tests, plug-in test, outside test
                        (later: fuzz, event and pcap replay)
  docs/                 architecture (md + html), getting-started guides
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
| *measured v0.10, idle (QEMU)* | *476 KB* | *2.5 MB* |
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
- **Local-first:** nothing leaves until you link a cortex (your own, by default); then state, not traffic. `devices anonymous` replaces MACs, IPs and names with salted hashes.
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
| **P5 Outside** | Bridge, fleet history, training loop, OTA, LLM agent via MCP, remote access | A retrained model reaches a fleet and hot-loads | v0.8, completed in v0.9 |
| **P6 Hardware** | LuCI app, procd tuning, OpenWRT One + Wi-Fi 7 boards, footprint validation | Same test suite passes on routers within budget | v0.10: LuCI, procd, self-test, OpenWRT in QEMU; real routers next |
| **P7 Advanced** | eBPF fast path, QUIC SNI, prefetch, CSI presence, band steering, federated learning, NPU back-end | Measured latency and detection gains | v0.11: DNS cache with prefetch and stale answers, TLS/QUIC connection names; v0.12: dual-band PC AP, band steering, presence; v0.13: channel self-healing; v0.14: federated learning; v0.15: fast path (flowtable), eBPF traffic meter; v0.16: model back-ends (NPU interface, built-in flat back-end) |

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
side-loaded plug-ins (ed25519, same trusted keys as models) arrived in v0.9; `ujail`
sandboxing with P6.

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

**As built in v0.8 (P5, first part).** The training loop runs end to end:

| Step | As built |
|------|----------|
| Answer | a person rejects a proposal, or undoes an automatic action, that came from a `dga` alert → false alarm; approves one → confirmed. Nothing is asked twice: the answer is the decision itself |
| Report | `ag-bridge` (connector plug-in, `nobody`, curl over HTTPS, token in a mode-600 file it alone can read) posts status, models, alerts, decisions and devices every `interval`; `devices names \| anonymous \| none` (anonymous: a salted hash instead of MAC/IP/name, no alert reasons). Never packets or DNS logs |
| Learn | cortex (`cortex/aigw_cortex`, Python, SQLite) turns decisions into labels; `retrain` passes them to the trainer, where each answer weighs as much as 50 synthetic examples; the model records how many answers it saw and how far it agrees with them |
| Publish | stamp `version` into the `.agm` META section, sign with cortex's usign key (Ed25519; `cryptography` or a built-in pure-Python signer) |
| Deliver | the bridge sees a newer version, downloads `.agm` + `.sig` into its own state directory and offers them to agd (`{"model": …}`, capability `model.offer`, paths must be inside that directory) |
| Check | `models.c`: signature by a key in `models.keys` (`agctl trust`; verified by libag's own SHA-512/Ed25519, RFC 8032 vectors in the unit test), parses, same feature schema and count, sane scores on probe names, newer than the version in use and than any rolled-back one |
| Load | written to `models.dir` (previous update kept as `.prev`), swapped in between two lookups: same process, an `ag.model` event, `agctl models` shows version, signer and origin. A refused offer leaves the running model and is shown with its reason |
| Undo | `agctl model rollback` returns to `.prev` (or the packaged model) and holds the newer version off, across restarts; `agctl model install --force` overrides |

Suggestions travel the same path the other way: the MCP server (`python3 -m aigw_cortex mcp`,
stdio, tools for gateways, devices, alerts, decisions, models; one write tool, `propose_action`)
queues a proposal; the bridge hands it to agd as a plug-in proposal, which waits for a person
even in enforce mode, and reports back how it was taken. Plug-ins also gained `$AIGW_STATE`
(an own writable directory), `config:<key>` needs and `agctl plugin config`. Verified by
`tests/outside-test.sh` (private cortex, real retrain with scikit-learn, tampered and untrusted
models, rollback across a restart, MCP session, anonymous reports).

**As built in v0.9 (P5, complete).**

| Piece | As built |
|-------|----------|
| Remote access | `remote.c` + `aigw-remote`: a WireGuard interface (`agwg`, `10.77.0.0/24`, UDP 51820) with one peer per phone. `agctl remote add` makes the peer's key pair with `wg`, keeps only the public key, and returns a compact config that fits a QR code (version 10, ECC L). Full tunnel by default. Table `inet ag_remote` accepts the port, redirects tunnel DNS to agdns, forwards and masquerades; on OpenWRT a firewall zone and a port rule are added. The tunnel joins `lan.iface` at run time, so its clients are devices (named after their peer) for every sensor, alert and decision. `wg syncconf` keeps phones connected across agd restarts; revoke removes the peer at once. Package `aigw-remote` (wireguard-tools, kmod-wireguard) |
| Shared threat intelligence | cortex `publish-intel`: names people confirmed (any home), or flagged `dga` in ≥ N homes, minus anything called a false alarm, as a signed `# aigw-list threat-intel` with a version. `models.c` treats signed lists like models (same keys, versions, refusal, rollback, hold-off; no packaged version). `intel.c` matches a lookup and its parent domains (binary search); alert `known-threat`, built-in policy `known_threat` → quarantine |
| Signed plug-ins | `plugin.sig` = usign signature over "sha512  path" lines for every file of the folder (sorted). Checked when a plug-in is found and before every start; a trusted signature grants privileged capabilities without `allow`, a changed file revokes it (it then runs as any added plug-in). cortex `sign-plugin` |

Verified by `tests/remote-test.sh` (two namespaces, a real WireGuard tunnel, DNS through agdns, restart, revoke, off) and the extended `tests/outside-test.sh`.
Not covered yet: IPv6 inside the tunnel, the OpenWRT firewall integration on a real router (P6). Federated *learning* (training across homes without sharing labels) arrived in v0.14.

**As built in v0.10 (P6, first part).** Everything a router needs, checked on real OpenWRT userspace before real hardware:

| Piece | As built |
|-------|----------|
| procd | agd: respawn 3600/5/5, stdout+stderr to logd, no core dumps, nofile 2048, restarted on `/etc/config/aigw` changes. agdns: respawn without limit, in a **ujail** when the router has one (`main.jail`), with only ubus, the log, its config, resolvers and the block list. |
| `aigw-selftest` | PASS/FAIL per check, exit code = failures. It checks: the services and procd instances; the jail; `table inet ag`; the fw4 include (and a reload with `--full`); `agctl selftest` (SHA-512 and Ed25519 vectors, the packaged model's scores, a QR code, a real DNS query through agdns: all on this CPU); RAM per daemon and in total against the budget; installed flash; the bridge; the LuCI ACL through an rpcd session. `--full` adds: agd killed and back under procd, remote access on → zone, rule, peer → off, closed again. |
| QEMU harness | `platform/openwrt/qemu-test.sh` boots the official OpenWRT x86-64 image (KVM in CI, software emulation otherwise). It fetches the built packages from the host and installs them with opkg, so dependencies come from the real feeds. Then it runs `aigw-selftest --full` over the serial console. A CI job does this for every build. `KEEP=1` leaves it running with LuCI on :8080. |
| `luci-app-aigw` | LuCI JS views over rpcd (`acl.d/luci-app-aigw.json`). Pages: overview; devices (quarantine, priority, zone); alerts and decisions (approve, reject, undo); Wi-Fi keys and remote access with QR codes (agd adds `qr_rows`, LuCI draws SVG); plug-ins (on/off, allow) and models (rollback). |
| Packaging | `aigw-remote` prerm closes the tunnel, the port and the zone. `luci-app-aigw` reloads rpcd. `build-sdk.sh` retries flaky mirror downloads. |

QEMU result (OpenWRT 24.10.8 x86-64, 256 MB): all 25 self-test checks and all 12 LuCI checks pass, including the ujail, procd respawn and remote access through fw4. Measured footprint, idle with the bridge waiting:

| | Measured | Budget |
|--|---------|--------|
| agd RAM | 1.6 MB | 10 MB |
| agdns RAM | 0.8 MB | 4 MB |
| AIGW RAM in total | 2.5 MB | 16 MB |
| installed flash (core, remote, self-test, models, plug-ins) | 476 KB | 1 MB |

The same run also happens on emulated router CPUs (v0.10.1), with the very packages routers get:

| Run | Packages | Result | RAM (agd + agdns) | Flash |
|-----|----------|--------|-------------------|-------|
| x86-64 (KVM in CI) | `x86_64` | 25/25 + 12/12 LuCI | 2.5 MB | 476 KB |
| malta, MIPS 24Kc little-endian | `mipsel_24kc` (MT7621 build) | 25/25 + 12/12 LuCI | 3.1 MB | 488 KB |
| armsr, ARM Cortex-A53 | `aarch64_cortex-a53` (Filogic build) | 25/25 + 12/12 LuCI | 3.0 MB | 616 KB |

The model's scores and the signature checks come out identical on all three CPUs. One real problem only showed up under the slow ARM emulation: `agctl` waited 3 s for agd, and `remote on` (which reloads the firewall) can take longer. agctl now waits 30 s.

The LuCI pages are also rendered in a headless browser (`platform/openwrt/luci_screenshots.py`, against a `KEEP=1` VM seeded with devices, an alert and a decision). This found that LuCI's `E()` parses plain strings as HTML, so a device that named itself `<svg/onload=…>` would have run script in the admin's browser. Every value from the network or agd now goes in as a text node, and the check fails if seeded markup becomes an element or a dialog opens.

Busy homes (hundreds of devices and endpoints, plug-ins running) still need measuring on real routers. The checklist is [testing-on-routers.md](testing-on-routers.md).

**As built in v0.11 (P7, first part).** Faster DNS, and a name for every HTTPS connection:

| Piece | As built |
|-------|----------|
| DNS cache (`agdns`) | Up to `dns.cache` answers (1000), keyed by question, EDNS, DO and CD bits; served with the client's ID, RD bit and spelling of the name (0x20), TTLs counted down. Only clean answers: one question, not truncated, NOERROR or NXDOMAIN, records with a TTL; negative answers at most 300 s. Every answer, cached or not, still reaches `agd` as an `ag.dns` event (`cached: 1`). Blocks are checked before the cache. |
| Prefetch | A name asked for at least twice, with a TTL of 10 s or more, is asked for again when under a tenth of its TTL is left. Busy names never wait for the upstream. |
| Stale answers | RFC 8767: an expired answer (kept up to `dns.stale`, 1 day) is served with a 30 s TTL when the upstream answers SERVFAIL or REFUSED, or has not answered after 1.8 s. The upstream's answer still refreshes the cache when it comes. |
| `ag.dnscache` | ubus `stats`: entries, hits and misses, average answer time from the cache and from upstream, prefetches, stale answers. `agctl dns cache`; LuCI overview. |
| Connection names (`agd/sni.c`) | `aigw-nft` adds table `inet ag_sni`: from the LAN, TCP 443 packets 3–6 and UDP 443 packets 1–3 of each connection are copied (`log group`, 2 KB) to NFLOG group `flow.sni_group`. A copy, never a verdict: forwarding never waits. `agd` puts the ClientHello back together (TCP by sequence number; QUIC CRYPTO frames by offset, across Initial packets, in any order), and `libag sni.c` reads the server name and ALPN. QUIC Initial protection uses keys derived from the packet itself (RFC 9001 §5, v1 and v2), so any on-path box can read it; SHA-256, HKDF, AES-128 and AES-GCM are in `libag`, checked against FIPS 197, RFC 5869 and RFC 9001 vectors. |
| Flow sensor | An HTTPS connection without a DNS name waits up to 3 s for its handshake before it counts as "no DNS". A handshake name replaces a missing name, a "(nearby)" guess or a different DNS name (an address shared by many sites); a DNS name stays when the handshake only shows an Encrypted Client Hello's outer name. Flows carry `named_by` (`dns`, `nearby`, `sni`); `agctl flows` notes "name from handshake". |
| Alerts | A handshake name is judged like a lookup: DNS-over-HTTPS servers → `dns-bypass`, threat-intel lists → `known-threat`, the domain-risk model → `dga`. Names behind an Encrypted Client Hello are not judged. |
| Packaging | `aigw` depends on `kmod-nfnetlink-log`. Where the kernel lacks NFLOG, `aigw-nft` leaves the table out and connections are named from DNS only; the fw4 include always loads. `aigw-selftest` checks the cache (a repeated lookup is a hit), the sensor and its table. |

Measured by `tests/dns-cache-test.sh` against an upstream that takes 50 ms per answer:

| | Result |
|--|--------|
| Answer time, median of 20 names | 51 ms from upstream, 0.3 ms from the cache (about 150× faster) |
| A 10 s name asked every 0.5 s for 14 s | slowest answer after the first 1.4 ms; the upstream was asked twice (once by prefetch) |
| Upstream silent | the expired answer after 1.9 s, TTL 30 |
| Upstream answering SERVFAIL | the expired answer after 52 ms, TTL 30 |

`tests/sni-test.sh` (network namespaces, a real `openssl s_server`, NFLOG in the kernel): HTTPS without a lookup through AIGW (`curl --resolve`, as an app using DNS over HTTPS or a hard-coded address) is named from its ClientHello. A machine-generated name raises `dga`, and `dns.google` raises `dns-bypass`, both from the handshake alone. A QUIC Initial split over two datagrams in reverse order is named. A TLS connection to a bare address still counts as no-DNS, and a DNS name stays.

Not covered yet: TCP ClientHellos split into segments that arrive out of order (waits for retransmission, which the copy window may miss); with fw4 flow offloading, the second segment of a post-quantum ClientHello can bypass the copy, so a name late in it is missed. ECH hides the real site name by design.

**As built in v0.12 (P7, second part).** Both bands, and who is home:

| Piece | As built |
|-------|----------|
| Dual-band PC AP | `aigw-ap` with `BAND=dual`: a 5 GHz access point on one adapter and a 2.4 GHz one on another, same name and password, both in bridge `aigwbr0` (one network: a device keeps its address when it changes band). `WIDTH` 20/40/80 MHz with fallback to narrower widths, `bss_transition=1` on every access point. On WSL it attaches every shared USB Wi-Fi adapter through `usbipd.exe` and reloads the driver when fewer interfaces than adapters came up; `aigw-wsl.ps1` shares new adapters (with `--force` where a USB filter driver is installed). `aigw-ap conf` prints a hostapd config exactly as `up` writes it. |
| Wi-Fi sensor | Each access point's band, channel, BSSID and name from hostapd `STATUS`; each station's abilities from its `STA` entry: 802.11v BSS transition (extended capabilities bit 19) and 5 GHz (operating classes 115+, VHT, or seen there). A smoothed signal. A device found on another access point has `roamed`; listed by two for a moment, the newer join counts. |
| Band steering (`steer.c`) | Needs a 2.4 and a 5 GHz access point with one name. Up: on 2.4 GHz, 5 GHz-capable, smoothed signal ≥ `steer_up` (−60 dBm); down: on 5 GHz, ≤ `steer_down` (−75 dBm); held for `steer_hold` (30 s). With 802.11v: `BSS_TM_REQ` naming the other access point (operating class, channel, PHY type); without: refused on its band for 30 s (`DENY_ACL`) and disconnected, unless `steer_kick 0` or it is already refused by a "disconnect" action. Checked 15 s later (`steered` / `steer-missed`). Guardrails: never `act.protect` devices, 20 s settling on an access point, `steer_gap` (300 s) per device, `steer_rate` (2) moves a minute for the home, three misses in a row → left alone for a day. `ag.wifi radios`, `agctl wifi`, `agctl watch wifi`. |
| Presence (`presence.c`) | Present while on Wi-Fi or seen in the last `presence_away` (600 s; phones drop Wi-Fi to save power). `ag.presence` events `arrived` (with time away) and `left` (with time home); `home` in `ag status`; near / mid / far from the smoothed signal. |

Verified by `tests/steer-test.sh` (a fake dual-band hostapd: a phone moved up by 802.11v, a laptop without it moved up by refusal and let back in after 30 s, a far device moved down without ping-pong, a 2.4 GHz-only plug and a protected TV never asked, a device that ignores requests left alone after three, a visitor's departure) and `tests/hwsim-test.sh` in CI (real hostapd with `aigw-ap`'s configs and a real `wpa_supplicant` on `mac80211_hwsim` radios: the client joins on 2.4 GHz, agd's 802.11v request moves it to 5 GHz).

Not covered yet: steering between more than two access points (mesh), and room-level presence or motion from signal patterns.

**As built in v0.13 (P7, third part).** Channel self-healing:

| Piece | As built |
|-------|----------|
| Measure (`chan.c`) | Every `wifi.chan_poll` (30 s), per access point, the radio's survey of the channel in use (`iw dev <ap> survey dump`): air used by others = (busy − own transmit) / active, smoothed. Shown by `agctl wifi`; a radio that reports no channel time says so. |
| Decide | Busier than `wifi.chan_busy` (60 %) for `wifi.chan_hold` (30 min): one neighbour scan (`iw dev <ap> scan ap-force`, at most every `wifi.chan_scan_gap`, 6 h: the radio leaves its channel briefly). Each candidate channel (2.4 GHz 1/6/11; 5 GHz per width: 36/149 at 80 MHz, 36/44/149/157 at 40, the non-radar 20 MHz channels) scores the networks on or overlapping it, a strong one 3, a weak one 1; channels the radio may not use here (`iw phy`: disabled, no-IR, radar) are left out. One scoring at most half of the current raises `wifi-busy` with the numbers. |
| Act | Policy `wifi_busy` (built in, and in the shipped config) → decision `channel <ap>:<channel>`, a day by default: a proposal in advise mode, automatic in enforce mode within `max_auto_per_hour`. One channel decision per access point at a time. hostapd `CHAN_SWITCH` keeps the width (center frequency and secondary channel worked out) and announces the switch, so devices follow without dropping; the minute's re-check does nothing when it is already there. Where the radio has no channel switch announcement, the PC restarts that access point on the new channel (`aigw-ap channel`). Undo, or the time limit, switches back to the channel it had. `agctl channel <ap|2.4|5> <ch> [time]` by hand. |

Verified by `tests/chan-test.sh` (a fake hostapd and a stand-in `iw`: an 80 % busy channel 6 with 6 strong networks, channel 1 proposed, approval sends `CHAN_SWITCH 5 2412`, the re-check doesn't switch again, undo goes back, a manual switch, channel 14 refused) and in `tests/hwsim-test.sh` (CI): a real hostapd moves the 5 GHz access point from 36 to 44 and the real client follows the announcement.

On the PC's RTL8821CU (`rtw88`) the driver reports no survey data and no channel switch announcement: the channel can't be judged there, and a switch restarts the access point (devices rejoin within seconds). Router radios (mt76, ath10k/ath11k) report both.

**As built in v0.14 (P7, fourth part).** Federated learning: homes improve the domain-risk judgement together, and the names they answered about never leave.

| Piece | As built |
|-------|----------|
| The layer (`fed.c`) | 14 numbers on top of the trees: score = sigmoid(w0·logit(trees) + Σ wi·(featureᵢ − centerᵢ)/scaleᵢ + b). w0 = 1, the rest 0, is the trees' own score. A signed `# aigw-adapter domain-adapter` file, delivered and rolled back like a model (`models.c` slot, no packaged version). `agctl score` shows the trees' score next to the final one. |
| Examples | Kept on the gateway (state file), at most 200: a dga alert whose decision you rejected, or undid after it was taken automatically, is "safe"; one you approved is "threat"; `agctl label <name> safe|threat`; `agctl fed forget`. |
| A round | The bridge (capability `fed.train`) fetches `GET /v1/fed` (round, center, scale, weights, lr, epochs, clip, noise) and hands it to agd (`{"fed": …}`). With at least `alerts.fed_min_examples` (3) answers, agd runs logistic regression from the shared weights on its examples plus fixed public anchors (machine-generated names, also under `www.`/`cdn.` and a two-part suffix; popular sites with and without `www.`), threats weighing double. The change is clipped to length C and Gaussian noise σ·C is added to each number, σ = max(cortex's, `alerts.fed_noise`, default 1.0). Only `{round, delta[14], n, noise, clip}` goes to `POST /v1/fed/update`. |
| Cortex (`fed.py`) | One change per home per round. When `--fed-min-homes` (3) are in and the round is `--fed-gap` (a day) old: new = old + mean(changes), every home weighing the same. A layer whose trees' weight falls outside 0.2–5 is not published. Otherwise signed and listed in `/v1/models`; the next round starts from it. `aigw_cortex fed` shows rounds; `fed aggregate` closes one early. |
| Gateway check | Before use: every anchor the trees flag must stay flagged, no machine-generated anchor may score lower than the trees give it (so a layer can't learn "more labels = safe"), popular sites stay below the threshold, and names you said are threats stay flagged. A refusal is reported without naming your names. |
| Privacy | Names, features and verdicts stay home. Each round is a Gaussian mechanism over the whole home's data; rounds add up as zCDP (ρ = Σ 1/(2σ²)), shown as ε at δ = 1e-5 by `agctl fed`: 5.3 for one round at σ = 1, 20 after ten. `alerts.fed_budget` stops taking part before ε would pass it. The bridge's `alert_names 0` keeps alert subjects, reasons and block targets out of reports too, so cortex then learns only through rounds. |

Verified by `tests/fed-test.sh` (three agd instances with their own bridges, a private cortex): a Polish home's false alarms (bydgoszcz.pl, pszczyna.pl, …, few vowels and long consonant runs) and a home's threats train three rounds; the published layer is loaded in every home without a restart; the false alarms score lower everywhere, also szczebrzeszyn.pl which nobody answered about (0.98 → 0.2–0.8); generated names still alert, also under `www.`; none of the answered names reaches cortex's database, files or log; a layer that drops a home's own threat is refused there without naming it; a budget stops a home after three rounds; malformed, late and repeated changes are refused. `tests/test_cortex.py` checks the averaging and publishing.

Limits, measured: with σ = 1 and three homes the noise outweighs what the homes learned (the test uses σ = 0.1, which gives no meaningful ε); useful learning at σ ≈ 1 needs many homes. A false alarm that looks like a generated name under a subdomain in the 12 features (CloudFront hashes such as d2xqm7kjv0bh4f.cloudfront.net) can't be learned safely: every gateway refuses such a layer, and those belong on an allow list. The trees themselves scored generated names lower under a subdomain (www.xkqjzvbwptmr.com 0.56 against 0.99): fixed by the v0.16.1 retraining below.

**As built in v0.15 (P7, fifth part).** Fast path and eBPF traffic meter:

| Piece | As built |
|-------|----------|
| Fast path (`aigw-nft`, `fastpath.c`) | With `fastpath.enabled 1`, table `inet ag` gets flowtable `fast` (devices: `fastpath.ifaces`, default the LAN interfaces and the default-route interface; `offload 1` adds `flags offload` for the switch chip, falling back to software where the hardware can't) and chain `fastpath` (forward, filter + 5, after the action chains): established connections past 8 packets in the original direction (so handshake copies for connection names are taken first), with no connection-mark action and neither end in the quarantine, priority, bulk or throttle sets, are offloaded (`flow add @fast`). Skipped with a note where the kernel has no `nf_flow_table` (the WSL kernel). |
| Actions | Offloaded packets skip netfilter, so an action that starts on a device must take its connections back: right after the nft change, agd deletes that device's offloaded conntrack entries (status `IPS_OFFLOAD`, either address); their next packets build a new entry that goes through the rules and, the device now being in a set, stays on the slow path. A no-op on the minute's re-assert. After undo, the connections are offloaded again. |
| Counts | The flowtable keeps the connection's counters (`counter`), so per-device bytes from conntrack stay right. `agctl status`: connections offloaded (by the switch chip), sent back for actions. `aigw-selftest` fails when fw4's own `flow_offloading` is on (it would also offload devices AIGW acts on). |
| Meter (`meter.c`) | A 46-instruction eBPF program, written out as instructions in agd (no clang, libbpf or BPF toolchain in the build), loaded with `bpf(BPF_PROG_LOAD)` and attached with `BPF_LINK_CREATE` to each LAN interface's tcx ingress (key: source MAC, "up") and egress (destination MAC, "down"). It adds `skb->len` and 1 to an LRU hash map (1024 devices) with atomic adds and returns "next": it never changes a packet. agd reads the map every second: rate per device (`meter` in `ag.device list`, "right now" in `agctl device`). The links are agd's file descriptors, so the meter disappears when agd stops. It sees fast-path traffic (tc runs before the flowtable on ingress and in the transmit path on egress), not what the switch chip forwards. `meter.enabled 0` turns it off; it stays off with a reason where the kernel has no BPF or tcx (Linux < 6.6). |

Verified by `tests/fastpath-test.sh` (CI, network namespaces, on a kernel with `nf_flow_table`): a long download is offloaded and still counted for the device, and the meter sees it; throttling sends it back to the rules and keeps it there; after undo it is offloaded again; quarantine cuts off a connection that was offloaded before; with the fast path off there is no flowtable. `tests/meter-test.sh` (on the PC and in CI): a download's rate down matches curl's own speed, an upload's rate up, the kernel's byte counts cover the transfers, rates fall to zero afterwards; `meter.enabled 0` attaches nothing. Inside OpenWRT 24.10 in QEMU, `aigw-selftest` reports the meter running on x86_64, mipsel (MT7621's CPU) and aarch64.

Not measured yet: the gain on a real router (the test's veth pair runs on one CPU, so its speed figures say nothing about a router) and hardware offload, which needs a router with a switch chip. The fast path stays off by default until then.

**As built in v0.16 (P7, sixth part).** Model back-ends: NPU gateways register an accelerated back-end for the same models.

| Piece | As built |
|-------|----------|
| Interface (`include/ag/backend.h`) | `struct ag_backend { abi, name, prepare(view), predict(handle, x, count, raw), release }`; the model view exposes the trees (node: feature, value, left, right; leaves feature −1), `base`, schema. A library exports `ag_backend_entry()`. On OpenWRT the header is installed for other packages (`Build/InstallDev`). |
| Built in (`agm.c`, `agm_flat.c`) | `interp`: the reference interpreter. `flat`: every tree depth-first in one array of 12-byte nodes (left child next, right by index), thresholds as floats rounded down, so for float inputs every decision equals the reference's and the scores are bit-identical (checked on 20,000 inputs, including inputs on each threshold and the floats either side). |
| Loading (`agd/backend.c`) | `*.so` files in `models.backend_dir` (`/usr/lib/aigw/backends`); the directory and the file must be root's and writable by nobody else; `dlopen`, ABI 1, unique name. |
| Checking and choosing | On every model load: each back-end scores 300 probes (known names, and generated names of every kind, some under `www.`); it must give the reference's verdict at the alert threshold and be within 0.005 of its probability on each. Timed per score. `models.backend auto` takes the fastest that passed; a name takes that one if it passed, otherwise the reference. `agctl models` lists each one's result and speed. |

Verified by `tests/test_agm.c` (flat equals interp bit for bit; speed on this PC: 2.9 against 3.5 µs per score), and `tests/backend-test.sh` with `plugins/examples/backend/example_backend.c`:
- the example library computes in 32-bit floats, passes, and can be chosen;
- a broken build of it is refused, naming the probe it got wrong, and asking for it falls back to the reference;
- verdicts are the same under every back-end;
- a library writable by others, or a directory writable by others, loads nothing.

Not done: a back-end for real NPU hardware (each vendor's SDK; none available here), and the local LLM ("ask your home") those gateways could run.

**v0.16.1: the domain-risk model retrained for names under a subdomain.** Every generated training name used to be a bare `label.tld` (2 labels), while popular sites include `bbc.co.uk`-style names, so the trees learned that more labels mean safe. A malware author needed only `www.` in front: on held-out generated names under a subdomain the old model caught **0.00%**. Training now puts 35% of both kinds under a common host (`www.`, `mail.`, `cdn.`, …, `--subdomain-share`), so the label count says nothing about risk; features and schema are unchanged, so gateways, the federated layer and back-ends keep working. `train.py --compare old.agm` reports both models on the same held-out names:

| At score ≥ 0.98 | false alarms on popular sites | generated names caught |
|-----------------|------------------------------|------------------------|
| old, names as listed | 0.151 % | 68.35 % |
| old, under a subdomain | 0.000 % | 0.00 % |
| new, names as listed | 0.159 % | 68.24 % |
| new, under a subdomain | 0.156 % | 68.15 % |

The golden test data now includes `www.`, `cdn.` and `mail.` names, so the C feature extractor and runtime are checked on them too. Still weak, as before and by design: pronounceable generated names (syllables family, 11 % caught) and dictionary-word generators, which need behaviour (a never-seen domain, no history) rather than the name alone.

**v0.16.2: AIGW OS, and a flow sensor race fixed.** The project is named **AIGW OS**, *the AI-powered operating system for home gateways*, with a logo and brand guide in `docs/brand/`; packages, paths and commands keep the short name `aigw`. The flow sensor had a race: the counter refresh (a conntrack dump) could read a connection before agd read its "new" event, and agd then took it for a connection older than itself, so it was not counted, never checked for a missing DNS lookup and raised no alerts. A "new" event for such a connection now completes its start. Forcing the race (events read 300 ms late, a refresh every 20 ms), the old code counted 1 of 4 connections in flow-test and the new code all 4, with the `no-dns` alert.

**As built in v0.17: lookup bursts, behaviour instead of names.** Malware with a name generator tries many names until one answers. Most get "no such name", and none was ever used in the home. The name model can't see the generators whose names look like words. Measured on 1,000 names per family with the shipped model at 0.98:

| Generator | flagged by the name model |
|-----------|---------------------------|
| base32 | 99.6 % |
| hex | 93.2 % |
| random letters and digits | 74.6 % |
| random letters | 67.0 % |
| letters, then digits | 61.8 % |
| pronounceable syllables | 12.7 % |
| dictionary words (`rivergarden.net`) | 0.0 % |

| Piece | As built |
|-------|----------|
| Home memory (`burst.c`) | Every registrable domain that resolved for any device (rcode 0 with an address), in two 8 KB Bloom filter generations (k = 3, 4,000 sites each, about 0.5 % false "used" per generation). A site in use is re-added to the newer one, so turning over (the newer becomes the older, the older is dropped) forgets only sites nobody uses any more. Saved in the state file (`home_sites`). |
| Per device | A ring of its last 32 lookups of never-used registrable domains, each counted once (a retry is not another name): time, hash, NXDOMAIN or not, and whether the name model flagged it. Not saved. |
| `dga-burst` | Within `alerts.burst_window` (600 s), at least `alerts.burst_min` (8) of them got NXDOMAIN, and they are at least `alerts.burst_share` (75 %) of its never-used names then; one alert per window. The reason gives the count, how many the name model flagged, and "at an hour it is normally idle" when the baseline says so. A never-used name that answers within 15 s of a failed one is the likely control server: named in the alert, or in a second alert (subject: that domain) if it comes later in the window, with the `agctl block` command. Built-in policy `dga_burst`: quarantine, 1 h (a proposal in advise mode). |
| Not counted | Names AIGW blocked (`blocked` in agdns's event), `0.0.0.0` / `::` answers (filtering resolvers), empty answers (NODATA), server failures, local names (no registrable domain). Names that don't exist or that AIGW blocked are no longer learned into the device's baseline either: a burst would otherwise fill its Bloom filter with junk. |

Verified by `tests/burst-test.sh` (four devices behind a bridge, a fake DNS that answers its list, `0.0.0.0` for ad names and NXDOMAIN otherwise): an infected laptop tries 25 generated names, half pronounceable and half dictionary words, of which the 14th was registered. The name model flags 3 of the 25; the burst alert comes at the 8th failed lookup, a quarantine is proposed, and the 14th name is named as the control server. No alert for a phone visiting 40 new sites with 4 typos, a TV retrying one dead name 30 times, a tablet behind a filtering resolver, or names AIGW blocks. The laptop's baseline learns none of the names that don't exist. The home memory is the same after an agd restart, and known sites aren't learned again.

Limits: a generator that makes fewer than 8 attempts in 10 minutes (slow, or most of its names registered or sinkholed so they answer) stays below the line; lookups that bypass AIGW (DNS over HTTPS) are not seen here (`dns-bypass` says so). The first days after installing, the home memory is empty: every working site counts as never-used, which only makes a burst harder to reach (the share), never easier.

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
| DNS bypass | partial DNS view | port-53 redirect, DoH detection, flow signals; since v0.11 connections are named from their TLS / QUIC handshakes |
| False positives | lost trust | observe/advise first, TTL, one-tap feedback |
| C parsing untrusted data | gateway compromise | fuzzing, ASan, sandboxing |
| Third-party plug-ins | misbehaving code | signatures, declared permissions, budgets, ujail |

**Open decisions:** product name · second (mt76) adapter for the PC · `agdns` default upstream
(local unbound vs encrypted provider) · first reference router for P6 · open-core licence.
