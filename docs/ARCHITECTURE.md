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
  cortex/aigw_cortex/   Python: fleet history, labels, retrain/sign/publish, bridge API, MCP server, operator dashboard
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
| Internet providers | White-label AI gateway on OpenWRT-based CPE | Fewer support calls, premium tier, lower churn |
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
| *measured v0.53, installed (SDK 24.10.8, `flash-kb.py`)* | *full 996 KB x86-64, 988 KB aarch64, 828 KB mipsel; small 664 / 668 / 568; minimal 392 / 384 / 340* | — |
| *measured v0.48, installed (QEMU)* | *full 952 KB x86-64, 940 KB aarch64, 788 KB mipsel; small 664 / 668 / 564; minimal 392 / 384 / 340* | — |
| *measured v0.51, installed (SDK packages)* | *full 992 KB x86-64, 980 KB aarch64, 820 KB mipsel; small 708 / 708 / 600; minimal 392 x86-64* | — |
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

Verified by `tests/steer-test.sh` (a fake dual-band hostapd: a phone moved up by 802.11v, a laptop without it moved up by refusal and let back in after 30 s, a far device moved down without ping-pong, a device at the edge of 5 GHz moved up and back down that is then left on 2.4 GHz for an hour (v0.29.2: steering remembers bounces, and leaves a hopping device be), a 2.4 GHz-only plug and a protected TV never asked, a device that ignores requests left alone after three, a visitor's departure) and `tests/hwsim-test.sh` in CI (real hostapd with `aigw-ap`'s configs and a real `wpa_supplicant` on `mac80211_hwsim` radios: the client joins on 2.4 GHz, agd's 802.11v request moves it to 5 GHz).

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
| Fast path (`aigw-nft`, `fastpath.c`) | With `fastpath.enabled 1`, table `inet ag` gets flowtable `fast` (devices: `fastpath.ifaces`, default the LAN interfaces and the default-route interface; `offload 1` adds `flags offload` for the switch chip, falling back to software where the hardware can't) and chain `fastpath` (forward, filter + 5, after the action chains): established connections past 8 packets in the original direction (so handshake copies for connection names are taken first), with no connection-mark action and neither end in the quarantine, priority, bulk or throttle sets, are offloaded (`flow add @fast`), judged only on a packet that came in from the LAN (`lan.iface`). That packet is the device's own, so the quarantine check (by MAC) sees it. Otherwise a packet from the internet could offload a quarantined device's connection again after agd sent it back, and the device's packets would then skip the drop (v0.29.2). Skipped with a note where the kernel has no `nf_flow_table` (the WSL kernel). |
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

**As built in v0.18: managed by an ISP over USP (TR-369).** One data model, in agd, for every management protocol; the protocol clients only translate. Details and how to extend it: [device-management.md](device-management.md).

| Piece | As built |
|-------|----------|
| Data layer (`agd/dm.c`, ubus `ag.dm`) | A table of TR-181 paths (at most one `{i}` each), with type, flags (writable, notify), table and getter / setter; commands with inputs, outputs and a handler; events with arguments. Calls: `schema`, `instances`, `get` (a path ending in `.` is the whole subtree; unknown paths and gone instances under `errors`), `set` (every value checked, then all applied, or none), `operate`; event `ag.dm.event`. Protocol-neutral error codes. 54 parameters, 5 commands, 2 events in version 1.0 (56 parameters in 1.1, v0.19). |
| Instance numbers | Devices from their MAC, access points from their interface, saved with the state (`dm_instances`) and never reused; alerts and decisions use their ids. The same `Device.Hosts.Host.3` for every protocol and after a restart. |
| Who may do what | `origin` `local` (agctl) or the adapter's (`usp`). A management system may read, and approve / reject / undo decisions; quarantine, block and `Mode` need `mgmt.actions 1`; alert subjects and reasons and blocked domains need `mgmt.alert_details 1`. Actions go through `act_request` / `act_approve` / `act_reject` / `act_revert`, so every guardrail holds; decisions record who (`approved by usp`). `Mode` is saved in `/etc/config/aigw` (`ag_config_set`). |
| USP adapter (`usp/aigw_usp.c`, package `aigw-usp`) | A plug-in for an unmodified OB-USP-Agent v11.0.7 (`obuspa -x aigw-usp.so`; BSD-3-Clause, the Broadband Forum's reference agent). At start it reads `ag.dm schema` and registers every path in one group of vendor hooks: get / set / refresh-instances (5 s) / operate become `ag.dm` calls on the agent's thread; its own thread follows `ag.dm.event` and signals USP events; asynchronous commands each run on a thread too. libubus is not thread-safe even with a connection per thread (one process-wide request buffer and reply table), so every libubus call holds one lock and no uloop runs: a waiting thread polls its own socket without the lock. The agent's DeviceInfo stays its own, with `SoftwareVersion` from agd. `aigw-usp-run` writes the agent's settings (MQTT 5 client, controller, topics, TLS) from the `usp` section and rebuilds its database when they change. |
| Packages | `obuspa` (pinned source, MQTT and UDS only; `scripts/build-obuspa.sh` for a PC) and `aigw-usp` (plug-in, launcher, procd service that starts only with `mgmt.enabled 1` and a broker). On a PC: `AIGW_USP=1 install.sh`, systemd `aigw-usp`. |
| Published definition | `docs/datamodel/aigw-dm.xml` (TR-106 DM XML), made by `scripts/dm-xml` from `agctl -j dm schema`; `tests/dm-test.sh` fails when it differs from what agd serves. |

Verified by `tests/dm-test.sh` (the layer alone: schema and XML, subtrees, errors, all-or-nothing set, read-only and invalid values, what a management system may see and do, the `Alert!` event without the name, instance numbers kept and never reused) and `tests/usp-test.sh` (a USP controller in Python over MQTT 5 with the Broadband Forum's message definitions, OB-USP-Agent and the plug-in, in network namespaces): it reads the software version and the devices and lists the commands and events; an alert reaches it as `Device.X_AIGW_.Alert!` without the name; it approves the quarantine AIGW proposed (the laptop's MAC lands in the nft set) and undoes it; with `mgmt.actions 0` quarantine and mode changes are refused (USP 7022, 7006); after an agd restart with `mgmt.actions 1` and alert details on, it sees the name, blocks a domain, sets enforce mode (saved), is refused a protected TV, and the devices keep their numbers.

Not done yet: writable Wi-Fi settings, firmware upgrade and WebSocket transport over USP; the USP test inside OpenWRT in QEMU.

**As built in v0.19: managed over TR-069 too, on the same data layer.** Trial result:

- **icwmp** (iopsys; BSD-3-Clause; maintained; standard OpenWRT libraries only) gets its whole data model through one ubus object.
- **easycwmp** is GPLv2, unchanged since 2019, and its data model is shell scripts.

So icwmp runs unmodified, and a small AIGW daemon answers it.

| Piece | As built |
|-------|----------|
| Data model daemon (`cwmp/aigw_cwmp.c`, `aigw-cwmp-dm`) | Publishes the ubus objects icwmp asks: `bbfdm` (`get`, `schema`, `set`, `add`, `del`, `operate`) and `bbf.config` (`commit`, `revert`). Normally bbfdmd serves these. Paths, values and types come from `ag.dm` with origin `acs`. GetParameterNames is built from the schema and `instances`, with `NextLevel` and USP-style templates (icwmp asks for those too). A SetParameterValues is checked value by value (`ag.dm set` with `check`, new) and applied in one `ag.dm set` at commit, or dropped at revert. CWMP fault codes are 9001 / 9003 / 9005 / 9007 / 9008. |
| What only TR-069 has | Kept in the daemon, not in agd. `Device.ManagementServer.*`: values in icwmp's `/etc/config/cwmp`; changes by the ACS also saved in `/etc/config/aigw` and followed by an icwmp reload. Also `RootDataModelVersion` and `ProvisioningCode`. `Device.X_AIGW_.Request.`: `Command` (a command's name or path), its inputs, `Status`, `Message` and its outputs, made from the schema's commands; it runs at commit. `Device.X_AIGW_.LastEvent.`: `Name`, `Time`, `Count` and the events' arguments; `Count` has active notification from the start, so alerts reach the ACS as `4 VALUE CHANGE`. On a PC it also answers `network.interface.<wan> status`. |
| Identity | `DeviceInfo.SerialNumber` and `ManufacturerOUI` are now in agd (data model 1.1): `mgmt.serial` / `mgmt.oui`, else the machine id and the LAN MAC. icwmp's DeviceId and the USP agent (`USP_BOARD_SERIAL`, `USP_BOARD_OUI`) both use them. |
| Packages | `icwmp`: the client only, pinned commit, unmodified; `scripts/build-icwmp.sh` for a PC. `aigw-cwmp`: the daemon, `aigw-cwmp-run` (writes the AIGW-managed options into icwmp's config; `1 BOOT` once per boot), and a procd service. The service starts only with `mgmt.enabled 1` and an ACS URL, and allows the connection request port from the WAN zone. On a PC: `AIGW_CWMP=1 install.sh`, systemd `aigw-cwmp`. |

Verified by `tests/cwmp-test.sh`, in network namespaces with an ACS on the WAN side.
- **Part 1** asks the daemon the way icwmp asks it:
  - values and types, the identity, and the `ConnectionRequestURL`;
  - names with `NextLevel`, instances and templates;
  - faults 9001 / 9005 / 9007 / 9008;
  - a write applied at commit and dropped at revert, saved in both configs;
  - `LastEvent.` without the name;
  - a quarantine refused with `mgmt.actions 0`;
  - an approved decision that lands in nft and is undone, recorded as "by acs".
- **Part 2** runs icwmp against a Python ACS (`tests/cwmp/acs.py`, SOAP over HTTP, connection requests with digest auth):
  - the boot Inform's DeviceId and parameters come from agd;
  - GetParameterValues and GetParameterNames work;
  - a SetParameterValues on `Mode` fails with 9003 and 9001 for the parameter;
  - a refused command shows in `Request.Status`;
  - `ParameterKey` and the Inform interval change, the interval saved;
  - a new alert comes as `4 VALUE CHANGE`;
  - the ACS approves the proposed quarantine, and it lands in nft.
- icwmp's fixed paths are private to the test (overlays in a mount namespace).

Not done yet: TR-069 Download (firmware) and other RPCs beyond the data model; client certificates in `/etc/config/aigw`; the TR-069 test inside OpenWRT in QEMU.

**As built in v0.20: the vendor prefix and identity are the ISP's or OEM's.** AIGW OS doesn't claim a TR-106 prefix for itself. `mgmt.vendor_prefix` (checked: `X_`, upper case, digits, `-`, `_`; else `X_AIGW_` and a warning) is applied once in `dm_init` to every parameter, object, command and event path, and to the references agd hands out (`Decision`, `Alert`). `ag.dm schema` reports it as `vendor_prefix`; the USP plug-in registers whatever paths the schema lists, `aigw-cwmp-dm` builds `Request.` and `LastEvent.` under it and writes icwmp's notification file itself (`-n`), and `scripts/dm-xml` names it in the published file. `mgmt.manufacturer`, `model_name` and `product_class` set `DeviceInfo.` (and so the TR-069 DeviceId and the USP agent's settings). `tests/dm-test.sh` checks a prefix and names end to end in agd, and `tests/cwmp-test.sh` runs entirely under `X_EXAMPLE-COM_`.

**As built in v0.21: firmware upgrades and Wi-Fi settings, by the ISP.** What an ISP does first with a gateway, through the standard TR-181 objects and over both protocols.

| Piece | As built |
|-------|----------|
| Platform handlers (`agd/handler.c`) | agd decides (what may change, who may change it, which values are valid). Small programs carry it out, so an OEM replaces them without touching agd: `wifi.handler`, and `mgmt.firmware_handler`. They are run without a shell, with secrets in the environment. In the background they report back on the loop; quick questions (`list`, `version`) have a 5 s limit. |
| Wi-Fi (`agd/manage.c`, data model 1.2) | `Device.WiFi.Radio.{i}` / `SSID.{i}` / `AccessPoint.{i}` share one instance number per access point. Writable: `SSID`, `Security.KeyPassphrase` (always reads empty), `Security.ModeEnabled` (WPA2, WPA3, mixed, none; read from hostapd's `GET_CONFIG`), `Channel` and `OperatingChannelBandwidth`. Values are checked in the set's first pass and staged in the second. After the set, one handler call per access point. Reads show the new values until the access point runs with them (90 s at most). `mgmt.wifi` (default 1). Handlers: OpenWRT `wifi-config` (`network.wireless status` → section and radio, `uci`, `wifi reload <radio>`); PC `wifi-config` (`pc-ap.conf`, `aigw-ap reconf`, which also has `SECURITY` wpa2 / wpa3 / wpa2-wpa3 / none now). |
| Firmware | `Device.DeviceInfo.FirmwareImage.{i}` from the handler's `list`, `Active`/`BootFirmwareImage`; `SoftwareVersion` is the running image's version, and `AdditionalSoftwareVersion` is "AIGW OS x.y.z". `Download()` and `Activate()` are the schema's first asynchronous commands: agd defers its ubus reply until the handler exits. Activation reboots after 3 s, except for origin `acs`, because icwmp saves its session and reboots by itself. OpenWRT handler: image 1 running, image 2 a RAM slot; `uclient-fetch`, size, SHA-1/256/512, `sysupgrade -T`, version from `fwtool` metadata; activate = `sysupgrade` keeping the configuration. `mgmt.firmware` (default 1). |
| USP | The plug-in registers asynchronous commands with `USP_REGISTER_AsyncOperation`. Each runs on a thread with its own ubus connection (60 min limit) and ends with `USP_SIGNAL_OperationComplete`. |
| TR-069 | icwmp handles `Download` itself: fetch, `sysupgrade -T`, then `fwbank dump`, `bbfdm operate FirmwareImage.N.Download()`, `fwbank set_bootbank`, reboot. aigw-cwmp-dm now answers `operate` for the firmware commands (faults inside `output`, as icwmp reads them), and publishes `fwbank` unless the platform has one. Before activating, it saves icwmp's session to `/etc/icwmpd`, kept over sysupgrade (`keep.d`). `aigw-cwmp-run` puts it back at boot, so the ACS gets its `TransferComplete`. |

Verified by:
- `tests/dm-test.sh` and `tests/usp-test.sh`, with a two-bank test handler that is also the OEM example (`tests/fw/firmware`): bad images, wrong checksums, the running bank and `mgmt.firmware 0` are refused; over USP, a download with AutoActivate over HTTP ends with OperationComplete and a reboot into the new version.
- `tests/cwmp-test.sh`: the bank calls as icwmp makes them; with real icwmp, a `Download` RPC through to `1 BOOT`, `7 TRANSFER COMPLETE`, the new SoftwareVersion and a `TransferComplete` without a fault. In its mount namespace, `reboot` is a script, so the test can never reboot the machine.
- `tests/hwsim-test.sh`: a new name, passphrase and channel through the data model, hostapd back with them, the client joining with the new passphrase.

Not done yet: turning an access point off, guest networks, `RegulatoryDomain`; firmware time windows and scheduled downloads; a firmware handler for a PC.

**As built in v0.22: ready for a carrier network.**

| Piece | As built |
|-------|----------|
| Reboot, factory reset | `ubus ag.dm system {action, origin}`: agd checks `mgmt.reset`, logs who asked, and runs the firmware handler's `reboot` / `factory-reset` 2 s later (OpenWRT: `firstboot -y`, reboot). USP: the plug-in registers OB-USP-Agent's `reboot_cb` / `factory_reset_cb` hooks; without them `Device.Reboot()` only restarted the agent. Its stop no longer joins the event thread, which could wait forever. TR-069: `defaultreset` (icwmp's FactoryReset; iopsys has one, OpenWRT doesn't) in `/usr/libexec/aigw`, put on icwmp's PATH by `aigw-cwmp-run`. |
| Client certificates | `usp.client_cert` (PEM, certificate and key) to obuspa's `-a`; `cwmp.client_cert` / `client_key` into icwmp's `cpe.client_cert_path` / `client_key_path`. |
| On OpenWRT itself | `qemu-test.sh` (MGMT=1) runs tests/cwmp/acs.py on the host. In the VM: obuspa's CLI reads AIGW's data model through the plug-in, and icwmp informs the ACS and answers a job, on all three architectures. On x86-64, the ACS's Download carries the same release image through icwmp, `sysupgrade -T`, agd's OpenWRT firmware handler (RAM slot, `fwtool` version) and `sysupgrade` keeping the configuration. QEMU runs on through the reboot. The packages are reinstalled, and the ACS must get `1 BOOT` + `7 TRANSFER COMPLETE` and a `TransferComplete` without a fault: icwmp's session was kept by aigw-cwmp-dm and icwmp's `keep.d`, and put back by aigw-cwmp-run. |

Verified also by usp-test (the agent on a mosquitto TLS listener requiring a client certificate; `Device.Reboot()` through to the handler), cwmp-test (an HTTPS ACS requiring the CPE's certificate; `FactoryReset` through `defaultreset`) and dm-test (`mgmt.reset`).

**As built in v0.24: AIGW Self-Healing Wi-Fi, part 1 (measure, learn, diagnose, report).** Where AIGW is meant to beat prplOS and RDK-B, which carry the data model but don't understand the home: the gateway finds out why Wi-Fi is bad, from what devices feel.

| Piece | As built |
|-------|----------|
| Measure (`agd/heal.c`) | A raw ICMP socket on the loop. Once a second, an echo to each Wi-Fi device that is in use: hostapd `STA` says `inactive_msec` < 300, or rx + tx packets grow by 4 or more a second (more than the probe's own). Sleeping devices aren't measured, since power save fakes loss and delay. A ring of 128 probes per device; timeout 1 s; window 2 minutes: loss, median delay. |
| Learn | Each device's usual delay (EWMA while it is not degraded); band changes from wifi.c's "roamed" (kept 10 minutes). |
| Diagnose (`core/libag/heal.c`, pure, `tests/test_heal.c`) | Per access point every 10 s. Degraded device: at least 15 probes and (loss >= `wifi.heal_loss` 5 % or (delay >= `heal_delay` 100 ms and >= 3x usual)). Radio: at least 2 measured, all lost. BandHopping: >= `heal_hops` 4 band changes (named first: fixable). WeakCoverage: most affected at or below `heal_weak` -75 dBm. Device: one affected of at least 2. Otherwise Congestion. Poor at 20 % or 300 ms. A change must hold for 2 looks (3 for Good). |
| Report | Log line, `ag.wifi` event `health`, `ag.wifi radios` `health` (`agctl wifi`), data model 1.3: `Device.X_AIGW_.WiFiHealth.{i}` (same number as its Radio; `AffectedHosts` as Host references) and `WiFiHealthChanged!`. |

Verified by `tests/test_heal.c` (each cause, including the real channel-6 case) and `tests/hwsim-test.sh` (CI: a client in use measured Good, 30 % `netem` loss on its access point read as Congestion in agctl and the data model with the event, Good again after). On the PC access point (two RTL8821CU, real phone): "2.4 GHz (channel 11): OnePlus-Nord-5 loses 6% of its traffic with a weak signal (-77 dBm)", which matches what had been traced by hand. Part 2: acting on it (a measured channel change, the 5 GHz floor) through the guardrails.

**As built in v0.25: AIGW Self-Healing Wi-Fi, part 2 (act on it, and check the fix).** The diagnosis becomes action, always through the guardrails, and every change is measured before it stays.

| Piece | As built |
|-------|----------|
| Channel trial (`agd/heal.c`) | `Congestion` held for `wifi.heal_hold` (300 s) and no trial in `heal_retry` (1800 s): the first of `chan_options()` (the band's candidates `allowed_here`, not the current one) not marked bad, as alert `wifi-congestion` (target `<iface>:<ch>`); policy `wifi_congestion` (built in, ttl 3600) → `channel` decision. Seen active (`act_channel_for`, origin the policy): the trial starts, "before" = the radio's loss and delay then. After `heal_trial` (180 s), a diagnosis over probes sent after start + 20 s only. Kept when measured, loss not higher, and Good, or loss halved, or delay halved: `act_keep` (state `kept`: no time limit, no switch back) and `wifiset_save` (the handler with `AIGW_WIFI_SAVE_ONLY=1`). Else `act_undo_because` (state `tried`: switched back; cortex learns nothing from it), the channel bad for 24 h. Nobody measurable: waits up to 2x the trial, then switches back. Log line and `ag.wifi` event `trial-kept` / `trial-tried`. |
| stay24 (`agd/act.c`, `agd/wifi.c`) | A device action by MAC: `wifi_ban_band(mac, 5)` puts it on hostapd's `DENY_ACL` of the 5 GHz access points only and deauthenticates it there; steer.c skips it (`act_active`). Undo or the time limit lifts it; one that ran out while agd was down is lifted at start. Policy `wifi_hopping` (built in, ttl 86400) on alert `wifi-hopping` (per device, once a day by alerts.c's repeat rule). |
| Advice | New verdicts `WeakCoverage`, `Device` raise `wifi-coverage`, `wifi-device` (info) per affected device; `Radio` raises `wifi-radio` (warning). No built-in policy acts on them. |
| Save only | `platform/pc-ap/wifi-config`: writes `pc-ap.conf`, no `aigw-ap reconf`. `platform/openwrt/.../wifi-config`: `uci commit`, no `wifi reload`. |
| Settings | `wifi.heal_act` (1), `heal_hold`, `heal_trial`, `heal_retry`. Explanations leave out delays under 5 ms. |

Verified by `tests/hwsim-test.sh` (CI, enforce mode, hold 20 s, trial 40 s): `agctl do stay24` moves the client to 2.4 GHz, only the 5 GHz access point refuses it, steering leaves it there; 30 % `netem` loss → Congestion → a `wifi_congestion` channel decision → the loss removed → `kept`, hostapd on the new channel, `CHANNEL` saved in `pc-ap.conf`; Good again; the loss again → a second trial on another channel → still lossy → `tried`, back on the kept channel, which stays saved; undoing stay24 lifts the refusal.

**As built in v0.26: a steady verdict, and the healing report.** The diagnosis stops renaming one problem, and the gateway counts what it found and fixed, in numbers an ISP can collect across homes.

| Piece | As built |
|-------|----------|
| Signal margin (`core/libag/heal.c`) | `heal_weak_step(prev, signal, weak, margin)`: weak at or below `wifi.heal_weak` (-75), fine again only above it + `heal_weak_margin` (3 dB); in the margin, unchanged. agd keeps it per device (`hdev.weak`) and passes it in `heal_sta.weak` (0: judge by signal, as before). |
| Sticky cause (`agd/heal.c`) | While a problem lasts, another cause needs 6 looks (1 min) instead of 2; pending looks match on the cause alone (Degraded / Poor alike). `heal_radio.cause_since` (the cause held since) and `problem_since` (not Good since); the trial's `heal_hold` counts from `cause_since`. |
| Report (`core/libag/heal.c`, pure) | `struct heal_day` per access point and UTC day in a ring of 31 (`heal_report_day`, `heal_report_sum`, `heal_day_problems`, `heal_day_good_pct`). agd adds 10 s per look to the status that held, `found[cause]` on a new problem or a new cause, `kept` (with loss and delay before and after, and seconds since `problem_since`), `tried`, and `band_rules` from act.c's `set_state` (stay24 active, once per decision; on the access point whose BandHopping lists the device, else the one it is on). |
| Kept | `wifi_report` in agd's state file: per access point, one array per day with anything in it; restored at start. |
| Shown | `ag.wifi radios` `health.report.{day,week,month}`; `agctl wifi --report`; data model 1.4: `Device.X_AIGW_.WiFiHealth.{i}.Stats.{Day,Week,Month}.` (17 parameters each, 51 in all). |

Verified by `tests/test_heal.c` (the margin over a -76..-71 dBm walk changes the cause twice; day / week / month sums over 40 days, at most 31 kept), `tests/dm-test.sh` (the published DM XML), and `tests/hwsim-test.sh` (CI: after the kept and the switched-back trial and stay24, `agctl wifi --report` and `Stats.Day` count them, with loss before and after and time to fix, and still do after an agd restart).

**As built in v0.27: Diagnose.** "My internet is slow", answered by the gateway in about 3 seconds, for the home and the ISP's help desk ([diagnose.md](diagnose.md)).

| Piece | As built |
|-------|----------|
| Verdict (`core/libag/diag.c`, `include/ag/diag.h`) | Pure: `diag_judge(input, result)`. Findings with a code, area, severity and text, in order of what explains the problem best (AIGW's own block, the line, DNS, the device away, AIGW slowing it, the line's quality, Wi-Fi, then what is fine); `diag_advice(code)`. |
| Measuring (`agd/diag.c`) | The default route (`/proc/net/route`); 10 echo requests, 200 ms apart, to the next hop and to each `diag.targets` address (a raw ICMP socket of its own; the best target counts); one DNS query to agdns for `diag.name`; Self-Healing Wi-Fi's verdict per access point and today's trials; for a device, `heal_device()` (its probes) and `act_on_device()` (active decisions on it). One at a time; the same question meanwhile shares the answer. |
| Asked | `ag diagnose {host}` (deferred ubus reply); `agctl diagnose [device]`; data model 1.5: `Device.X_AIGW_.Diagnose()` (async; input `Host`; outputs `Code`, `Area`, `Severity`, `Summary`, `Advice`, `Findings`, four numbers), over TR-069 through `Request.`; `LastDiagnosis.` (7 parameters); event `DiagnosisDone!`. |

Verified by `tests/test_diag.c` (every code and the order), `tests/dm-test.sh` (the schema and the published DM XML), and `tests/hwsim-test.sh` (CI: with no DNS server the verdict is `DNS_FAILING`; with the client disconnected by AIGW, `DEVICE_BLOCKED` naming the decision, over `agctl` and the data model, with `LastDiagnosis.` and `DiagnosisDone!`).

**As built in v0.28: proactive care.** The gateway runs Diagnose by itself, fixes what it can, tells the ISP about the rest before the customer calls, and keeps a daily experience score ([proactive-care.md](proactive-care.md)).

| Piece | As built |
|-------|----------|
| Rules (`core/libag/care.c`, `include/ag/care.h`) | Pure: `care_step(state, report, verdict, now, params)`. A problem is a verdict of warning or worse on 2 checks in a row, and is over after 2 fine ones. The line and DNS are the ISP's: reported at once. Wi-Fi is Self-Healing Wi-Fi's: reported after `wifi_grace`; over before that, it is fixed by AIGW if AIGW changed something for it (`care_acted`: a channel trial, stay24), else cleared by itself (v0.29.2). A change of cause is reported again. Day counts for 31 days; `care_score`, `care_main_cause`. |
| Checks (`agd/care.c`) | A uloop timer: `care.interval`, or `care.recheck` while something is wrong; within seconds when a radio turns Poor (`care_on_heal`). `diag_check()` is a quiet Diagnose: no `LastDiagnosis.`, no `DiagnosisDone!`; a Diagnose someone asks for during it is queued, not refused. Saved with agd's state: the days, and an open problem (not reported twice after a restart). |
| Seen | `ag care {check}`; `agctl care [--check]`; ubus event `ag.care`; data model 1.6: `Device.X_AIGW_.Care.` (`Enable`, `Status`, `LastCheck`, `Problem.` 7 parameters, `Stats.Day/Week/Month.` 7 each) and events `ProblemDetected!`, `ProblemCleared!`. |

Verified by `tests/test_care.c` (every rule), `tests/care-test.sh` (found, reported once, remembered over a restart, over; quiet checks; a Diagnose during a check waits), `tests/dm-test.sh` (the object, the schema, the DM XML) and `tests/hwsim-test.sh` (the same with simulated radios).

**As built in v0.29: TR-143, early warning, the fleet report.** The standard diagnostics an ISP runs ([tr143.md](tr143.md)). The home's normal is learned, and a drift is told before it is a fault, with a nightly speed test ([proactive-care.md](proactive-care.md)). Every home's care comes together in cortex ([fleet-report.md](fleet-report.md)).

| Piece | As built |
|-------|----------|
| TR-143 (`agd/tr143.c`, `core/libag/tr143.c`, `include/ag/tr143.h`) | IPPing, TraceRoute (ICMP, growing TTL), DownloadDiagnostics (HTTP GET, TR-143's ROM/BOM/EOM and TCP open times, interface counters, time based), UploadDiagnostics (HTTP PUT, chunked when time based), NSLookupDiagnostics (A queries; default the gateway's own DNS). One of each kind at a time, in a child process (blocking code off agd's loop), killed after what it was given; the result through a pipe. Pure: URLs, HTTP heads, TR-106 times with µs, names. |
| TR-143 in the data model (1.7) | Five asynchronous commands (USP), and the objects with `DiagnosticsState`, writable inputs and the results (TR-069; `RouteHops.{i}.` and `Result.{i}.` as tables); capability parameters. One generic getter and setter from the parameter's path (`ictx.ppath`). aigw-cwmp-dm forwards icwmp's bbfdm operate to agd asynchronously (deferred), and keeps TR-143 out of `Request.`; icwmp itself sends `8 DIAGNOSTICS COMPLETE`. |
| Early warning (`core/libag/care_learn.c`) | Pure: `care_learn_step(learn, sample, hour, day, now, quiet_areas, params)`. Per metric (next hop, internet delay, loss, DNS, speed): 24 hourly normals (EWMA mean and mean absolute deviation, two weeks) shrunk towards the whole day's; a one-sided CUSUM of capped deviations (shift) with a minimum effect; a least-squares line over the days' averages (trend, R², days to the fault level). Nothing learned in an area with a problem open. |
| Proactive care (`agd/care.c`) | Every regular check's measurements feed the normal. Warnings are `EarlyWarning!` and `EarlyWarningCleared!` (BecameProblem), and a problem soon after a warning counts as seen coming (`ProblemsWarnedOf`, lead time). The nightly speed test is a TR-143 download at a random minute of `care.speed_hour`; below `care.speed_min_pct` of `care.speed_mbps` twice: `LINE_SLOW_SPEED`, a second care state, reported. The normal, the warning and the speed are saved with agd's state. |
| Fleet (`cortex/aigw_cortex/fleet.py`, the bridge) | The bridge adds agd's care (the plug-in query `care`: score, problem, warning, speed, the last 8 days' rows; a Wi-Fi summary only with `devices = names`) to each report. cortex stores `care_days` and `care_events` and builds the report: scores and their change, problems found, fixed and reported, causes, warnings and their lead time, speed, outages (problems starting in at least 3 homes and 10% of a group within 15 minutes of each other), and the attention list. As text, JSON, and the MCP tool `fleet_report`. Gateways have a group (`enroll --group`, `group`). |

Verified by:

- **Unit tests:**
  - `tests/test_tr143.c`;
  - `tests/test_warn.c` (weeks of simulated checks: quiet when normal; a shift, a drift and then its trend, loss, DNS and speed warned of; a fault not learned);
  - `tests/test_cortex.py` (the fleet report, outages, attention).
- **CI tests:**
  - `tests/tr143-test.sh` (every test against local servers; TR-069's way through the data model);
  - `tests/warn-test.sh` (speed tests teach the normal; a drop warned of and over; restart; `LINE_SLOW_SPEED`);
  - `tests/usp-test.sh` (IPPing and TraceRoute over USP);
  - `tests/cwmp-test.sh` (a real ACS through icwmp: Requested, `8 DIAGNOSTICS COMPLETE`, the results);
  - `tests/outside-test.sh` (care through the bridge into the fleet report).

**As built in v0.30: firmware upgrades that check themselves.** The home is recorded when an image is activated and compared after the boot. A worse upgrade is rolled back by AIGW itself, confirmed, held off and reported ([upgrade-check.md](upgrade-check.md)).

| Piece | As built |
|-------|----------|
| The verdict (`core/libag/upgrade.c`, `include/ag/upgrade.h`) | Pure: `up_judge(before, after, now, params)`, giving watching, worse (reason bits) or keep, with the text. At once: devices not back (≥ 2 and ≥ 25%) after `reconnect`; no internet on every check (≥ 3) for 15 min, when it worked before; more than 3 starts. At the end of `watch`: delays against the normal at the hours sampled (1.5× and a minimum difference), loss (+2 points), the speed (< 80%), the score (−10 points, both with ≥ 4 h watched). |
| The watch (`agd/upgrade.c`) | `manage.c` calls `upgrade_on_activated` before the handler's `activate`, because OpenWRT's sysupgrade never returns. The record goes to `agd-upgrade.json` beside the state, fsynced, with the care state saved too. On the first tick on ubus after a start, the running version is compared with the record: on the new one it is watching (and each later start counts), back on the old one after a rollback it is rolled back, and on the old one after a reboot it is NotBooted. A handler that switches images without stopping agd (the test handler) is followed while agd runs. Devices are back once their `last_seen` is after the boot (Wi-Fi, traffic, DNS, DHCP; never ARP or leases). Care feeds every check, with the normal for that hour, and speed test; checks come every 2 min for the first hour. Worse leads, with `upgrade.rollback`, to `fw_activate` of a non-running image with the old version (who: "upgrade check", which is never watched), and the version is held for `hold_days`, refused in `fw_activate` and in download-and-activate. Back on the old image, the missing devices (or the internet) are checked again: confirmed or not. |
| Data model (1.10) | `Device.X_AIGW_.Upgrade.` (17 values, `Forget()`), event `UpgradeChecked!`. Verdicts found at start wait `report_delay` and are kept in the record, queued in order, until told. |
| agctl | `agctl upgrade [--forget]`, `agctl watch upgrade` (`ag.upgrade` events). |

Verified by `tests/test_upgrade.c` (the verdict's rules on simulated homes) and `tests/upgrade-test.sh` (CI: the two-bank handler and a fake hostapd; agd stops for each reboot; a bad upgrade rolled back, confirmed, reported and held off; a good one kept over an agd restart). Not yet on a router with A/B flash. OpenWRT's single image had no previous one to go back to, so there it was reported, not rolled back; since v0.36 a kept copy is (below).

**As built in v0.31: "why was it slow?" afterwards.** The home's timeline is a sample every minute and a log of what happened, and any stretch of it is explained with Diagnose's codes ([why-slow.md](why-slow.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/why.c`, `include/ag/why.h`) | Pure: `why_judge(in)`. Each sample is put in a state: a gap (agd not running; the gateway off when a start after a reboot follows), no route, the line down, the ISP down, the line full, the line slow, the ISP slow. A full sample takes the blame for its slowness. Loss, DNS, each access point and the device asked about are judged on their own. A state needs two samples (a blip isn't a finding), and runs are joined over one-sample holes. Slow is judged against the home's normal for those hours (2× and +30 ms, at least 40 ms; +20 ms for the next router). The verdict is the finding with the most weight: its samples ×3 when nothing worked, ×2 for a problem, ×1 for a warning, then Diagnose's order. A finding within 5 min after an AIGW change on Wi-Fi names the change. `why_baseline` gives the normal from the timeline itself (medians, the mean for loss). |
| The recorder (`agd/why.c`) | A ring of `hours × 3600 / interval` samples (90 bytes; at most a week), slotted by time. Each tick ends the sample: the echo requests (3 rounds to the next router and two `diag.targets`, their own raw socket and ident), the WAN's bytes on the default route's interface, DNS counted from agdns's events (not blocked ones), each radio's `heal_for()` and channel (a change is an event), and per device the meter's (else flows') download, for the top two. Events come from `agd_emit` itself (`ag.wifi`, `ag.decision`, `ag.care`, `ag.upgrade`), and the recorder adds a signal turning weak or fine (`wifi.heal_weak`, the margin), channel changes, speed tests and agd's start (the system's uptime tells a reboot from an agd restart). It is saved every 10 min and when agd stops (`why.file`; `/tmp` on OpenWRT: no flash writes), and reloaded by time, so a new length or interval keeps what fits. Asked about a stretch: proactive care's normal for its middle hour, else the timeline's own; the line's speed is `care.speed_mbps`, else the last speed test, else the most seen; for a device, its last state before the stretch. |
| Data model (1.11) | `Device.X_AIGW_.WhySlow(Start, End, Host)`, synchronous (also as `Request.` for TR-069), and `Device.X_AIGW_.Timeline.` (5 values). |
| agctl | `agctl why [device] [--at | --from --to | --last] [--raw]`; `ubus call ag why`. |

Verified by `tests/test_why.c` (18 evenings: a full line, a slow ISP, the line slow, a blip, an outage against a longer slowdown, a power cut against an agd restart, DNS, a poor access point, a laptop on a weak 2.4 GHz signal, a device kept off, a channel change just before a problem, the home's own normal, an unknown line speed, no timeline, loss) and `tests/why-test.sh` (CI: a fake hostapd, a sample every 2 s, the line sampled, a weak laptop's story, kept over an agd restart, the internet down, `WhySlow()`). Not yet run on a router for days. The per-minute echo requests are 9 packets a minute.

**As built in v0.32: signals for the ISP.** What a home would buy (a faster plan, smart queueing, mesh Wi-Fi), why it may leave, and a line to repair first, from the last week, with the evidence and a confidence ([isp-signals.md](isp-signals.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/offer.c`, `include/ag/offer.h`) | Pure: `offer_judge(days, care days, line speed)`. Each signal needs the same thing on 3 of the last 7 days that were watched (6 h of samples or more), so a busy evening isn't a sale. `PLAN_UPGRADE`: full 30 min a day. `SMART_QUEUE`: full and slow 15 min, and half the full time. `MESH_WIFI`: 2 devices weak 30 min, or one and 10 short drops. `RETENTION_RISK`: score < 90, −5 points on the week before, or 4 problems (care's days). `LINE_REPAIR`: 2 slow speed tests, and then no `PLAN_UPGRADE`. Confidence from what is known (the sold speed) and how many days. The evidence has no names; the weakest devices by name go in a separate field. |
| The counts (`agd/offers.c`) | Fed by the timeline (`agd/why.c`): each sample adds its time; full time (`why_full`: 80% of the sold or tested speed, else of the most seen), and of it the evening and the slow part (`why_slow_ms` against care's normal for the hour); the peak; a weak device's time (the day's 16 weakest kept); a leave and rejoin within 2 min is a drop. 31 days in agd's state (`offers`). Judged hourly, and when someone asks. A signal that starts is told once (`OfferFound!`, held until agd is on ubus); what was told is saved, so a restart doesn't repeat it. `offers.share 0`: nothing in the data model, no event, nothing in the bridge's report. |
| Data model (1.12) | `Device.X_AIGW_.Offers.` (3 values, then 5 for each signal), event `OfferFound!`. |
| cortex | The bridge carries the active signals inside proactive care (`care.offers`). `store.offers` keeps one row per signal while it lasts, and closes it when a report no longer has it. The fleet report's `signals` section counts the homes by product and lists them: repairs, then homes at risk, then sales by confidence. |
| agctl | `agctl offers` (with the devices by name), `ubus call ag offers` (and `today`: the day so far). |

Verified by `tests/test_offer.c` (weeks of homes: every signal, its confidence, no pattern from two days, the repair holding back the sale, learning, the window), `tests/offers-test.sh` (CI: a week in agd's state; both signals, named only at home; the event once over a restart; the data model; today counted from samples; share 0) and `tests/test_cortex.py` (the fleet report's signals). Not yet on real homes for a week.

**As built in v0.33: self-tuning smart queueing.** The gateway finds bufferbloat on its timeline, shapes the WAN at the line's measured speed through the guardrails, and keeps it only when the timeline shows that it helped ([smart-queueing.md](smart-queueing.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/sqm.c`, `include/ag/sqm.h`) | Pure. `sqm_bloated`: in the last 60 samples, `detect` full-and-slow ones and half the full ones (full: `why_full`; slow: `why_slow_ms` against the normal). `sqm_rates`: 90% of the sold or tested speed, else 85% of the most seen; the upload from the most seen. `sqm_judge` compares before (the day before) and after (`trial` full samples): kept at ≤ 60% of the delay before (or within 30 ms of normal) with downloads ≥ 75%; else undone; never "better" when not lower at all. |
| The actuator (`scripts/aigw-sqm`) | `apply <iface> <down> <up>`, `remove`, `status`, `check`. cake when the kernel has it (one qdisc: shaping and fairness), else htb + fq_codel. Downloads via ingress → ifb (matchall + mirred). Idempotent; refuses a WAN with a shaping qdisc it didn't set up. A state file per interface says what is AIGW's. |
| The loop (`agd/sqm.c`) | On every timeline sample: no live `sqm` decision and nothing in `hold` → bloat raises alert `bufferbloat` (each sighting its own alert), and the policy (built in when the configuration lacks it) gives an `sqm` decision (`act.c`: guard, actuate, unactuate, through the handler). An active decision from the policy is the trial (before: the day's full samples before it took effect), judged each sample until kept (`act_keep`) or undone (`act_undo_because`). Kept: restored by `act_init`, checked and put back every 10 min. A person's own (`agctl sqm --on`) is never judged. The trial's numbers are saved with agd's state. |
| Elsewhere | The timeline's WAN can be set (`why.wan`). An `sqm` decision is an AIGW change for "why was it slow". `SMART_QUEUE` isn't raised while SQM is on. The data model is 1.13: `Device.X_AIGW_.SQM.` (9 values). |
| agctl | `agctl sqm [--on\|--off]`; `ubus call ag sqm`. |

Verified by `tests/test_sqm.c` (bloat and not; the rates; kept, waiting, undone for no gain or for lost speed; no before) and `tests/sqm-test.sh` (CI: a veth WAN to a namespace, a real 10 Mbit/s sender, netem bufferbloat. In advise mode it is proposed, approved and really shaped; kept when the delay goes; restored over a restart; `--off`. In enforce mode it is tried by itself and undone when no better). The PC access point's WSL kernel has no cake, so there it uses htb + fq_codel. Not yet on a real line.

**As built in v0.34: staged firmware rollouts that stop themselves.** Cortex gives a version to the fleet in waves and opens the next wave only when the last one passed on the homes' own upgrade verdicts (v0.30). It stops by itself on rollbacks or a score drop ([staged-rollout.md](staged-rollout.md)).

| Piece | As built |
|-------|----------|
| The rules (`cortex/aigw_cortex/rollout.py`) | Pure. `plan`: cumulative percentages to wave sizes, at least one home each. `judge` (one wave): stop when more than `max_bad`% of the wave got worse (rolled back, worse, not booted, couldn't install), counted as results come in. Wait while fewer than `min_reports`% reported; stop after `timeout_s`. Stop when the homes that kept it lost more than `max_score_drop` points on average. Otherwise pass. `advance`: any opened wave's stop stops it (also while paused); a running rollout whose open wave passed goes to the next wave, or done. Reasons hold counts, never names. |
| The store | `rollouts` (version, image, via, wave sizes, rules, state, wave, when each opened, reason), `rollout_homes` (wave, state, scores, error), `rollout_log`. A bridge report's `upgrade` updates the home's state, when it is for the rollout's version (or was delivered by the bridge) and activated after its wave opened. Then the rollout steps. `serve` also steps every minute, so a quiet wave times out. |
| Delivery | Always agd's own Download path (`fw_download`, as TR-069 `Download` and USP `Download()` with `AutoActivate`). `acs` (default): the ACS asks `rollout homes` and pushes; results come through the bridges, or `rollout result` for homes without one. `bridge`: `GET /v1/firmware` gives a cleared home its offer (once), and the bridge hands it to agd as `{"firmware": {url, version, checksum, size}}` (capability `firmware.install`, privileged; bridge setting `firmware`, default 0; `mgmt.firmware`). agd downloads into the image not running, activates and reboots. "Already running" counts as fine; a held version, or a failed download, as worse. |
| agd | Plug-in query `upgrade`: the last upgrade's check without the devices' addresses or names (`upgrade_blob(b, 0)`). |
| CLI, MCP, report | `aigw_cortex rollout new\|list\|status\|step\|pause\|resume\|abort\|homes\|result`. MCP `rollout_status`, and `rollout_hold` (pause or abort only). Fleet report section `rollouts`. |

Verified by `tests/test_rollout.py` (the waves; pass; stop on rollbacks, early and with a canary of one; stop on a score drop; wait, then stop on silence; a rollout fed by bridge reports to done; pause, resume, abort; the bridge offer once; ACS results; fleet report and MCP) and `tests/rollout-test.sh` (CI: four homes with agd, bridge and the two-bank handler; planned paused, then resumed. Wave 1 is downloaded through the bridge, kept and passes. In wave 2, one home rolls back by itself, so the rollout stops without names, and wave 3's home never gets the image). Not yet with a real ACS pushing against `rollout homes`.

**As built in v0.35: calls and games first.** Calls and games are noticed by their traffic, go first inside AIGW's own smart queueing (only while that is on), and are proved smooth or rough from the timeline. The ISP sees the proof without names ([calls-first.md](calls-first.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/calls.c`, `include/ag/calls.h`) | Pure. `call_classify`: a UDP connection, judged on its last 2 intervals of counters. Each interval needs at least 5 packets a second in each direction, an average packet of 1300 bytes or less, and 10 Mbit/s or less in each direction. The busier direction carries at most 25 times more packets than the other, and the slowest interval reaches at least 40% of the fastest. DNS, QUIC (443), NTP, discovery and VPN ports are never counted. `call_hint`: known ports (STUN/TURN, Zoom, Meet, FaceTime, Webex, Xbox Live, Steam, Riot, Minecraft) and name suffixes (zoom.us, teams.microsoft.com, whatsapp.net, discord.media, xboxlive.com, riotgames.com, ...). A hint gives the app but never makes a call on its own. `call_quality_of`: over the samples while the call lasted (only the full ones, if any), the median delay to the internet, the jitter (the average change from sample to sample), the echo loss and the median download. `call_verdict`: smooth means 150 ms or less, 30 ms of jitter or less and 1% loss or less. `call_words`: the device, app and top downloader are named at home, and left out for the ISP. |
| The counters (`agd/flow.c`) | The conntrack dump also reads packet counts and hands each UDP connection to `calls_counters`. The dump runs every `calls.poll` (10 s) when that is shorter than `flow.poll`. Ended connections leave the table. |
| The loop (`agd/calls.c`) | 64 UDP connections watched (the oldest non-call is evicted first). A device's real-time connections are one live call (16 at most), starting when its first connection began to look like a call. A call stays a call while it still has packets both ways (muted), and ends after 30 s with nothing. The priority set (the far ends of live calls) is sent to `sqm_prio` whenever it changes, and every 60 s while there are calls, only when `sqm_shaping` finds AIGW's own SQM decision in effect. `sqm_apply` and `sqm_remove` reset it. When a call ends, if it lasted `calls.min_s` or more: the report from `why_samples` and `why_line_now`, a line on the timeline (`WE_CALL`), the ubus event `ag.call`, and `CallReport!` if shared and the call met a full line or was rough. The counts and the last report are saved with agd's state. |
| The actuator (`scripts/aigw-sqm prio`) | Only on AIGW's own shaping. One flower filter (prio 7) for each far end: on the WAN by destination, on the ifb by source. cake: `skbedit priority 1:4`, which selects diffserv4's voice tin; the qdisc is `diffserv4` only while there are calls. htb: `classid 1:5`, a priority-0 class with 30% of the line guaranteed and a ceiling of the whole line. htb now shapes as 1:1 → 1:5 / 1:10 (default). A v0.33 htb without 1:5 is shaped again at the first call. |
| Elsewhere | The timeline event kind `call`. The data model is 1.14: `Device.X_AIGW_.CallsFirst.` (20 values) and `CallReport!`. `calls.share 0` leaves only `Enable` and `Shared`. OpenWRT also depends on `kmod-sched-flower`. |
| agctl | `agctl calls`; `ubus call ag calls`. |

Verified by `tests/test_calls.c` (calls and games by port, name and traits; not calls: one look, QUIC, a VPN, TCP, keep-alives, one way, a transfer, big packets, bursts; smooth, rough, unknown; the words with and without names) and `tests/calls-test.sh` (CI: home, gateway and internet namespaces. A 10 Mbit/s stream on a line sold at 8 Mbit/s, and a 50-packets-a-second call to port 3478. With SQM, the call is noticed and goes first, its packets are counted in htb's 1:5 or cake's voice tin, and it is reported smooth, with names at home and none for the ISP, on the timeline, in the data model and in `CallReport!`. The filters go when the call ends. Without SQM and with netem bufferbloat, nothing is shaped and the call is reported rough. With `share 0` the ISP sees nothing). Not yet on a real call.

**As built in v0.36: firmware rollback on routers with one firmware image.** The v0.30 upgrade check rolls back through the firmware handler. On a single-image OpenWRT router, OpenWRT's handler now keeps a copy of the running image before `sysupgrade`, so there is something to go back to ([upgrade-check.md](upgrade-check.md#routers-with-one-firmware-image)).

| Piece | As built |
|-------|----------|
| The handler (`platform/openwrt/aigw/files/firmware`) | Image 3 is the kept copy (`Available` with its version, or `NoImage`). `activate 2`: room checked (USB storage: the copy; overlay: the copy after a bigger image is written, RAM for `sysupgrade`'s archive, and on x86/EFI the boot partition, where the archive waits for the first boot), the copy taken from an earlier copy of the running version or fetched again from the URL it came from (`/etc/aigw/firmware/source.*`, checked with `sysupgrade -T` and, when its metadata has one, its version), the settings saved (`sysupgrade -b`), the new image kept too on USB storage when it fits (named after the reboot, by boot id); on the overlay, a `keep.d` entry made for this one `sysupgrade` carries the copy. `activate 3`: the copy into RAM, `sysupgrade -f` with the settings it had plus `AIGW_FW_CARRY` (agd's record) and `/etc/icwmpd`. A failed `sysupgrade` undoes the keep. `rollback` says `banks`, `copy` or `none` and why; `keep [url]` keeps one by hand. Never tmpfs: RAM is empty after the reboot. |
| Settings | `mgmt.firmware_keep` (`auto`: without room, upgrade and say so; `require`: refuse; `off`), `mgmt.firmware_keep_dir` (default: USB storage under `/mnt` or `/media`, else `/etc/aigw-rollback`). Read by the handler with `uci`. |
| agd | `fw_rollback()` asks the handler's `rollback` (cached a minute, forgotten after an activation): `TwoBanks`, `KeptCopy`, `None`, `Unknown`. `activate` gets `AIGW_FW_CARRY` (the upgrade record's path). A worse upgrade with nothing to go back to says why in the handler's words. The rollback itself is v0.30's: the image that isn't running, with the old version, `Available`. |
| Data model (1.15) | `Device.X_AIGW_.Upgrade.RollbackMethod`, `RollbackNote`; `FirmwareImage.3.` on single-image OpenWRT. |
| Elsewhere | `agctl upgrade` ("way back"), `aigw-selftest` (`INFO rollback`), `tests/fw/firmware` answers `banks`. |

Verified by `tests/single-image-test.sh` (CI: the handler on a fake flash with shims for `sysupgrade`, `df`, `fwtool`, `uclient-fetch`, `uci` and `reboot`: kept on the overlay and carried once, written back with its settings and agd's record, refused or not without room, USB before overlay and never tmpfs, fetched again from its URL, a failed `sysupgrade` undone, RAM checked before going back; and, as root, agd rolling back through image 3 by itself and confirming it) and the QEMU job on x86-64 (a copy kept on a second disk as USB storage, over a real `sysupgrade`, then written back with its settings). Not yet on a real single-image router.

**As built in v0.37: a help-desk assistant for the ISP.** The support agent asks about one home and a period in plain words and gets one short answer from what AIGW already knows, after a fresh quiet check of the home ([help-desk.md](help-desk.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/brief.c`, `include/ag/brief.h`) | Pure, no AI model: `brief_judge(in)`. The verdict that decides: for a period ending now, a fresh check that found something, else care's open problem, else the timeline's verdict; for a past period, the timeline's. `brief_fault_of(code)` maps Diagnose's and the timeline's codes to `LINE`, `HOME_WIFI`, `DEVICE`, `OUTSIDE`, `NONE`, `UNKNOWN` (`LINE_FULL` is `NONE`; rough calls on a fine line are `OUTSIDE_SERVICE`). Confidence: high when two sources agree (the nightly speed tests count for `LINE_SLOW_SPEED`), low when they disagree, the fault is unknown, or little was recorded. The visit: only for the line itself (`LINE_LOSS` that lasted 30 min, was reported, or agreed; `LINE_SLOW_SPEED` on 2 nights), `IfPersists` for a line down (the customer restarts the modem first), never for the ISP's network, Wi-Fi, a device, or after an upgrade that went worse. The fix: `sqm` for `LINE_FULL` when it is off, `stay24` for a hopping device, approve a waiting `channel`/`stay24`/`sqm` proposal, undo AIGW's block or limit. `brief_period` reads "now", "last 2h", "tonight 20:00-22:00", "since 19:30"; `brief_kind` and `brief_scrub` put device kinds in place of names. |
| The gatherer (`agd/brief.c`) | `brief_start`: one at a time; a quiet `diag_check` and, if asked, `care_speed_now` (up to 90 s), then `why_ask` over the period (its verdict, findings, recorded share since the timeline began, the calls in it), `care_open`, the early warning, the week's score and the low-speed nights, `heal_for` each access point, `sqm_field`, `upgrade_field`, and the decisions made in or the hour before the period, or live. For the ISP (`brief.names 0`), every text is scrubbed of the devices' names, MACs and addresses; a stay24 target is dropped. With `propose`, a new fix becomes `act_propose` (or `sqm_propose`), never applied. Counts by fault, visits, remote fixes and proposals in agd's state (`brief`). |
| Data model (1.16) | `Device.X_AIGW_.SupportBrief(Period, Start, End, Host, Fresh, SpeedTest, Propose)`, asynchronous, also as `Request.` for TR-069. |
| Bridge, cortex | Plug-in query `brief`: the last hour, no fresh check, the ISP's names rule, with the gateway's counts and its `SerialNumber`; the bridge sends it in each report (not when it names devices and the bridge's `devices` isn't `names`). `cortex/aigw_cortex/brief.py`: a home by name, id or serial; the answer from the last report with its age, and where to ask for a fresh one (the bridge takes no requests); from care alone for gateways before 0.37; `--propose` queues `sqm` through the bridge; each answer logged (`briefs`). CLI `aigw_cortex brief`, MCP `support_brief`, fleet report section `help_desk`. |
| agctl | `agctl brief [device] [period] [--no-check] [--speed] [--propose]`; `ubus call ag brief`. |

Verified by `tests/test_brief.c` (16 calls: the line down with three sources agreeing, loss that lasted and a blip, the ISP's network, a full line with SQM off, waiting or on, a hopping laptop, a waiting channel, a quarantined device, rough calls on a fine line, all fine, nothing known, disagreeing sources, a rolled-back upgrade, low speeds on 3 nights, the gateway off, a past period; periods in words; kinds and scrubbing), `tests/brief-test.sh` (CI: agd in a gateway namespace with a veth WAN to an ISP namespace and a fake hostapd; all fine after a fresh check; a weak laptop by name at home and as "a laptop" for the ISP; a full line, with SQM proposed and nothing shaped; the internet down: `LINE`, no technician, also for an ACS; the counts kept over a restart), `tests/test_cortex.py` and `tests/outside-test.sh` (the brief through the bridge, found by serial). Not yet used by a real help desk.
**As built in v0.39: area outages.** Many homes behind one uplink with a line fault in the same few minutes are one network incident, not many home faults ([area-outages.md](area-outages.md)).

| Piece | As built |
|-------|----------|
| Correlation (`cortex/aigw_cortex/outage.py`) | Homes grouped by the operator's id in cortex (`group`), else the gateway's `UplinkID`, else `wan:<next hop>` or `bng:<peer>` (home-network addresses group nothing). Faults: care's line and DNS problems, the brief's current `LINE` verdict, silence (only while cortex hears from others). Faults starting within 15 min of the first, at least 3 homes and 10% of the uplink; later ones join; over when 80% recovered. Incidents kept in `incidents`/`incident_homes` with stable ids; the operator may declare and end one. Settings in the database (`outages settings`). |
| Tags and holds | `brief.answer` overlays "known outage in your area" (no visit, no fix); `/v1/proposals` keeps `sqm`, `stay24`, `channel` queued and `/v1/firmware` offers nothing for homes in an open incident; the report's answer carries `area` for the gateway. |
| Gateway (`agd/area.c`) | The bridge passes `area` on (capability `area.notice`); agd holds smart-queueing trials and judging, band steering, channel changes, stay24 (`act_on_alert`, `steer_tick`, `sqm_on_sample`), never security actions; the notice lapses after `hold_s`. Data model 1.17: `Device.X_AIGW_.AreaOutage.` (writable `UplinkID`), `AreaOutage!`, `AreaOutageCleared!`. The gateway's brief rule says `AREA_OUTAGE`. |
| Operator | Dashboard banners and `/api/outages`, the fleet report's `outages`, MCP `area_outages`, `aigw_cortex outages [declare\|end\|settings]`. |

Verified by `tests/test_cortex.py` (the rules, kept incidents, silence, 14 simulated homes end to end over HTTP), `tests/test_brief.c` and `tests/area-test.sh` (three real gateways with bridges and cortex). Not yet seen on a real ISP's network.

**As built in v0.38: the operator dashboard.** One web page, served by cortex, that shows an ISP the fleet: health, the homes to call first, rollouts with pause and abort, ISP signals, smart queueing, and each gateway ([dashboard.md](dashboard.md)).

| Piece | As built |
|-------|----------|
| The server (`cortex/aigw_cortex/dashboard.py`) | `python3 -m aigw_cortex dashboard [--listen 127.0.0.1:8090]`: the stdlib `ThreadingHTTPServer`, apart from `serve` (gateways' tokens never open it). Static `static/dashboard.html`, `.css`, `.js` (no build step, no CDN) and JSON: `/api/overview` (`fleet.report` plus the call-first reasons in words, `queueing`, groups), `/api/rollouts` (`rollout.status` without homes' names), `/api/gateways`, `/api/gateways/<ref>`, and `POST /api/rollouts/<name>/pause\|abort` with a reason (`rollout.control`, who: "a person (dashboard)"). Resume is refused: it stays at the command line. |
| Access | A token in `<data>/dashboard.token` (0600, made on first use; `--token-file`), compared in constant time, sent as `Authorization: Bearer`. On a loopback address, reads need no token when the `Host` is local (no DNS rebinding); bound elsewhere, every request needs it; writes always do. The start line prints a link with the token after `#`. `--cert/--key` for HTTPS. |
| Escaping | The page builds every element with `textContent`; it has no `innerHTML`. CSP `default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'`, `nosniff`, no referrer. The JSON escapes `<`, `>` and `&`. |
| Privacy | `sharing(snapshot)`: names when the last report's devices carry a MAC or hostname (bridge `devices names`), else anonymous or none. Without names, alerts and decisions lose `device`, `reason`, `ip`, `mac`, `note`; a device action's target becomes "a device" (a block's domain and an `sqm` decision's WAN stay); care problem summaries are dropped, also on the call-first list. |
| Smart queueing, calls first | Counted from each home's last report: `sqm` decisions kept or active (on), proposed (waiting), tried. Calls-first counts from `care.calls.stats` when a bridge sends it; bridges up to 0.38 don't, and the page says so. |

Verified by `tests/test_cortex.py` (the endpoints; reading on localhost without a token, refused for another `Host`, refused elsewhere without one; pause and abort need the token and a reason, resume refused, 409 when not possible; gateway text with HTML in it comes back as JSON with no `<`, and the page's script has no `innerHTML` or `eval`; no device names from a home that doesn't share them; the token file). Looked at in a browser with a seeded fleet, light and dark. Not yet on a real fleet.

**As built in v0.40: proactive care before the call.** The gateway finds the faults a customer would notice, fixes what it can itself as the act mode allows, and raises a ticket to the ISP for the rest, with a note for the customer in plain words ([before-the-call.md](before-the-call.md)).

| Piece | As built |
|-------|----------|
| The rules (`core/libag/proactive.c`, `include/ag/proactive.h`) | Pure. `pro_points`: a warning 1, a problem 2; 10/30 min +1/+2; 2/4 devices +1/+2; the 2nd/4th episode in a day +1/+2; a week under 90% +1. `pro_worth`: at `threshold` (3), after `min_s` (180 s), only `LINE`, `HOME_WIFI`, `DEVICE` faults, `LINE_FULL` only with rough calls. `pro_step`: one ticket per key (code, and `:mac` for a device); how long it lasted comes from the source or the looks' own episode, whichever is longer; a closed ticket back within `cooldown_s` is reopened (a flap) and each flap doubles the time it must stay away (`clear_s << flaps`); `PC_GRACE_OVER` for a watched ticket, an unsuccessful fix, or a quiet flap that stays. `pro_budget`/`pro_notified`: `max_per_day` new tickets in 24 h. `area_hold` (set from agd/area.c's `area_get` before each look): a home in a known area outage gets no ticket, no reopening and no raise after a grace, so no customer note; each fault that would have made one is counted once (`area_held`, TR-181 `HeldForAreaOutage`); news of a ticket the ISP already has goes without a note. `pro_close`: Fixed, Resolved, Passed; calls likely avoided. `pro_note` and `pro_plain`: the customer's note and its jargon check. |
| The loop (`agd/proactive.c`) | Every `proactive.interval`: `care_open`, `why_ask` (the last 3 min for what is current; rough calls over 30 min), `heal_for` each access point (one signal per device for `HC_DEVICE`/`HC_HOPPING`), Wi-Fi leaves per device (3 in an hour: `WIFI_DROPS`), `heal_hopping` (not with stay24 in effect), the week's score. A new ticket: the help-desk brief of its time (`brief_quick`, no fresh check, the ISP's names rule); the fix `stay24` for a hopping device, `sqm` for `LINE_FULL`, else the brief's; `act_auto` (`agd/act.c`: enforce applies within `act.max_auto`, advise proposes) or `sqm_auto`; Wi-Fi trouble Self-Healing Wi-Fi works on is watched for `care.wifi_grace`; the rest raised. `proactive_on_acted` (from `act.c`'s `set_state`) marks a ticket AIGW changed something for; `proactive_on_called` (a help-desk brief) marks open ones "called anyway". Tickets and counts in agd's state (`proactive`). |
| Data model (1.18) | `Device.X_AIGW_.Proactive.` counts and the table `Ticket.{i}.`; event `ProactiveTicket!`. |
| Bridge, cortex | Plug-in query `proactive` (no device targets; texts by `brief.names`), sent with each report. `cortex/aigw_cortex/proactive.py`: the fleet's counts, the period's tickets, the open ones (the ISP's first). Fleet report section `proactive` ("before call"), CLI `aigw_cortex proactive`, MCP `proactive_tickets`, the dashboard's "Before the call" with calls likely avoided, `/api/proactive`. |
| agctl | `agctl care proactive [--run]`; `ubus call ag proactive`. |

Verified by `tests/test_proactive.c` (the points and what is worth acting on; one ticket per fault; two sources, one ticket; over after `clear_s`; a flap reopens the same ticket, waits twice as long, is counted once, and is told only if it stays; short episodes that repeat; the day's limit; the area-outage hold; Self-Healing Wi-Fi's grace; Fixed, Resolved, Passed and "called anyway"; full tables; every customer note, for every code, fixed or not, every hour, in plain words), `tests/proactive-test.sh` (CI: agd in a gateway namespace with a veth WAN and a fake two-band hostapd; the internet down for 3 devices: one Raised ticket and one event, a plain note, no names or MACs, the TR-181 table; Resolved and a call likely avoided; a flap with no word to the ISP; kept over a restart; advise mode only proposes stay24 for a hopping laptop; enforce mode keeps a hopping TV on 2.4 GHz by itself and tells the ISP once it is fixed; the day's limit keeps a phone's ticket at home; a help-desk brief counts as called anyway), `tests/test_cortex.py` (class `Proactive`). Not yet on a real fleet. In a known area outage (a test plug-in's notice, as the bridge passes cortex's on), the internet down makes no ticket and no event, and the ticket comes once the notice lapses (`tests/proactive-test.sh`, `tests/test_proactive.c`).

**As built in v0.41: try it in five minutes.** One command (`scripts/try-aigw.sh`, attached to each release) downloads a demo image, boots it in QEMU and prints the dashboard's link, LuCI and an ssh command: an OEM or ISP engineer sees AIGW OS work without a router ([try-it.md](try-it.md)).

| Piece | As built |
|-------|----------|
| The image (`platform/openwrt/build-demo-image.sh`) | OpenWRT 24.10 x86-64 from OpenWRT's ImageBuilder with `aigw`, `aigw-bridge`, `luci-app-aigw`, LuCI, curl, the parts of python3 cortex needs, and `aigw-demo`; NTP off. The build fails over 40 MB. Attached to releases as `aigw-os-demo-x86-64.img.gz` with its `.sha256`. |
| The simulated home (`demo/aigw-demo.c`) | Writes what agd keeps, as if it had run for three days: `why.file` (4320 samples and the events), `state_file` (nine devices, the bufferbloat alert, the kept `sqm` decision, smart queueing's trial, the calls report, 14 care days, the speed test) and `upgrade.file` (an upgrade watched and kept). The story: yesterday 20:00 a full line (LINE_FULL), bufferbloat, a rough call, smart queueing approved at 20:38, the next call smooth. The verdicts and words come from libag's rules (`sqm_judge`, `call_quality_of`/`call_words`, `up_judge`); `why.h` now shares the file's header with agd. |
| Only in QEMU | The `aigw-demo` package (demo image only, never released) has `/etc/init.d/aigw-demo` (before agd): it runs only when `/sys/class/dmi/id/sys_vendor` is `QEMU`, never replaces a state file it didn't write, re-seeds at every boot, enrolls the VM in a fresh simulated fleet (`aigw_cortex demo seed`), starts cortex (127.0.0.1:8790) and the dashboard (port 8090, token), and leaves `/var/run/aigw/demo`. That mark is only a label: `ag status` `demo`, `agctl`'s first line `DEMO`, a LuCI banner; the data model's names say demo. |
| The VM's network | Two QEMU user networks with `restrict=on`: nothing leaves the VM; the LAN is 192.168.1.0/24 with this computer at .2 and three ports forwarded from its 127.0.0.1 (dashboard, LuCI, ssh). Diagnostics ping QEMU's gateway; the snapshot keeps nothing. |
| The fleet (`cortex/aigw_cortex/demo.py`) | 24 homes in two towns as their bridges report: scores better than the week before, problems fixed by AIGW, an outage across four homes, an early warning, signals, smart queueing and calls-first counts, a rollout running and one stopped by rollbacks. A `DEMO` file in the data directory puts "Demo data" on the dashboard; a directory with a real fleet is refused. `python3 -m aigw_cortex demo` serves it from a temporary directory without QEMU. |

Verified by `tests/demo-test.sh` (CI: refused outside QEMU and over an existing state; the real agd on the seeded files: `agctl why` LINE_FULL with the download, the call and smart queueing in the story, `agctl sqm` kept at 22 ms from 193 ms and put back through the handler, the upgrade kept, the data model's calls-first and SQM, `agctl brief` not a fault, a week of care; the label only with the mark; kept over a restart), `tests/test_cortex.py` (the simulated fleet and the refusal), and the CI job `demo-image`, which builds the image and boots it with `try-aigw.sh --check` (the dashboard with demo data, the VM's own home reported through its bridge, LuCI, `agctl` over ssh).

**As built in v0.42: the ISP's own ticketing.** Area outages (v0.39) and proactive tickets (v0.40) become tickets in the ISP's trouble-ticket system, through the TM Forum TMF621 API or signed webhooks ([isp-ticketing.md](isp-ticketing.md)).

| Piece | As built |
|-------|----------|
| Configuration (`tickets.json` in the data directory, `--tickets`, `$AIGW_CORTEX_TICKETS`) | Off by default; `dry_run`; events per target; TMF621 v4 or v5, OAuth2 client credentials or a bearer token; webhooks, each signed. Secrets only from files or the environment, read when used; inline secrets, unsigned webhooks, URLs with credentials and `http://` beyond loopback are refused. |
| Scan (`tickets.scan`) | Area incidents (`outage.incidents`) and each gateway's proactive tickets (its last report; `told_isp` by default, or `all`) against `isp_tickets`: a fingerprint per ticket gives opened, updated, resolved, reopened. Nothing that was over before ticketing was turned on. Each change becomes rows in `ticket_outbox` (one per target), in the same database, before anything is sent. |
| Send (`tickets.flush`) | In order per (ticket, target), claimed with a lease. TMF621: POST (after a `GET ?externalId=` when an earlier try may have reached the ISP: a mark is kept before each POST), merge-patch PATCH with the ticket as it is then, `status: resolved` with a reason. Exponential backoff for network errors, 5xx, 408, 429 (`Retry-After`); one new OAuth2 token on a 401; other 4xx stop as `failed` until `tickets retry`. Redirects not followed. Webhooks: `X-AIGW-Signature: v1=HMAC-SHA256(secret, "<ts>." + body)`, `X-AIGW-Timestamp`, `X-AIGW-Delivery`. |
| The hub | `tickets hub register` posts the callback to `<url>/hub`; cortex's API takes `TroubleTicket*Event`s at `/tmf621/listener` with its own bearer token: the ISP's status is kept, a ticket the ISP resolved or closed gets no more updates, a declared outage it resolved is ended. |
| Where it runs | `aigw_cortex serve` (a worker every `interval_s`, and right after a report), or `aigw_cortex tickets sync`. Sync state: CLI `tickets`, the dashboard's "ISP tickets" and `/api/tickets`, the fleet report's `isp_tickets`, MCP `isp_tickets` (and `isp_ticket` in `area_outages`, `proactive_tickets`). |

Verified by `tests/test_tickets.py` against `tests/tmf621_mock.py` (TMF621 v4/v5, OAuth2, hub and webhook receiver on 127.0.0.1): create, update and resolve for both kinds with the field mapping; retry after the server was down (refused, 503, 429); no duplicate after a lost reply, a restart, or a crash between POST and keeping the id; failed and retried by hand; a revoked token; v5; dry run; no secrets in any output; hub events and the listener's token; signed webhooks. Not yet against an ISP's production TMF621.

**As built in v0.43: hardened before others try it.** A security review of every way in, written up as the threat model in [security.md](security.md) (what is exposed, to whom, what protects it, what was fixed and what was not, with the reasons), and [SECURITY.md](../SECURITY.md) for reporting through GitHub's private vulnerability reporting.

| Fixed | As built |
|-------|----------|
| Wi-Fi passwords through a read permission | `ag.wifi keys` never returns a passphrase; `ag.wifi key_show` (a write permission in LuCI's ACL, not in the PC's non-root one) does; `agctl key qr` uses it |
| agdns as an open resolver on a PC | `aigw-nft`'s `dns_guard`: agdns's port from loopback, the LAN interfaces and redirected DNS only (`dns.guard`) |
| Secrets readable by every user | the PC's `/etc/config/aigw` 0600 (also on upgrade); cortex's data directory 0700 and `cortex.db` 0600; the firmware source record 0600 |
| A shared `/tmp` name | agd's socket for hostapd's answers in `/var/run/aigw`, 0600 |
| Credentials in messages | the firmware handler never shows a URL's `user:password@` |
| Warnings | cortex's fleet API beyond loopback without TLS; TR-069 without connection request credentials, or over HTTP |
| The demo's escape hatch | `aigw-demo seed --test` never writes a system's files |
| ISP ticketing (v0.42) | secret files and `tickets.json` refused when other users can read or change them (the configuration says where secrets go); `tickets.verify_delivery`: a webhook receiver takes the delivery id and event from the signed body and refuses replays |

Upgrades: the CI job `upgrade` installs v0.29.2, v0.33.1 and the latest release on OpenWRT 24.10 x86-64 in QEMU, sets up a home that isn't the defaults (settings, a policy of its own and a default removed, devices and a zone, a block, care history, a remote phone, the bridge's token), upgrades with `opkg` to the PR's packages and reboots; settings word for word, fw4's rules, the phone and WireGuard key, devices, decisions, care history, every data model path, every file in `/etc/aigw` and agd on the new version must survive (`platform/openwrt/upgrade-test.sh`). The job `pcap-upgrade` does the same for a PC as AP's `install.sh` (`tests/pcap-upgrade-test.sh`). It found that up to 0.42 an `opkg` upgrade turned AIGW off at boot (and removed its fw4 include), turned remote access off and turned aigw-cwmp off at boot: opkg runs the old package's prerm with `upgrade` first, and those prerms did what a removal does. agd kept running until the next reboot. The prerms now do nothing on an upgrade (`PKG_UPGRADE=1`), and the postinsts recognise what an older prerm undid (no fw4 include; remote access off with phones in the peers file and the config written minutes ago; an ACS set) and put it back.

Verified by `tests/security-test.sh` (CI: three network namespaces; DNS from the LAN, loopback and the WAN side, UDP and TCP; the guard off; firmware messages; the TR-069 warnings), `tests/plug-test.sh` (passwords, ACLs, the socket), `tests/test_cortex.py` (class `Security`), `tests/demo-test.sh` and the upgrade jobs.

**As built in v0.44: smarter mesh Wi-Fi over EasyMesh.** AIGW is the intelligence over a standard Wi-Fi Alliance EasyMesh controller with a Data Elements northbound, not another 1905.1 stack. It reads the mesh through the Data Elements model (`Device.WiFi.DataElements.`), judges it with what the gateway already learns, and acts through the controller's own requests ([mesh.md](mesh.md)).

| Piece | As built |
|-------|----------|
| Rules (`core/libag/mesh.c`, `ag/mesh.h`, pure) | `mesh_set`: one Data Elements parameter by its path (with or without `Device.WiFi.DataElements.`; aliases as instances); RCPI to dBm; operating class to band, width and primary channel; 802.11v/k from `ClientCapabilities`. `mesh_steer_pick`: struggling (signal or loss) for `mesh.hold`, then a measured target with hysteresis, never protected, in a call, paused, in cooldown, just moved by the controller or itself, or legacy without `mesh.legacy`. `mesh_trial_judge`: kept when something is clearly better and nothing clearly worse. `mesh_chan_plan`: no extender radio on the gateway's (or another's) air where avoidable; backhaul radios tied. `mesh_backhaul`: Weak, Slow, Busy, On24, Try6; weak or slow with devices behind it "hurts" |
| Reading (`agd/mesh.c`) | Every `mesh.poll` s: the handler's `get <file>` (ubus `_get` on `Device.WiFi.DataElements.Network`, or the local USP agent's `path => value` lines) or the `mesh.source` snapshot, JSON (path-keyed or nested) or `path=value`. Moves by the device or the controller noticed; each station's history (moves, misses, pauses, a move to avoid) kept in the state |
| Acting (`act.c`) | Decisions `meshsteer <station>@<BSSID>` and `meshchan <RUID>:<channel>` through `act_auto`: enforce applies (hourly limit), advise proposes; never during an area outage; one live mesh decision at a time; never re-sent on their own. Undo (a person, the time limit, a trial) moves it back or switches back through the controller |
| The controller (`mesh-dataelements` since v0.47, replaceable: `mesh.handler`) | The standard commands on the instance found in the model, over ubus or the local USP agent (`obuspa -c`): `steer`: `STA.{i}.ClientSteer(TargetBSSID, RequestMode=Steering_Mandate, BTMDisassociationImminent)` (TR-181 2.17); `channel`: `Radio.{i}.ChannelSelectionRequest()` (2.19), the target at preference 15 and the class's other channels at 1. A `Status` other than `Success` is a refusal. A controller without them: its own handler with the same three commands |
| Trials | A steer must land within three reads (a miss: no move back, two in a row pause the device, doubling); then `mesh.trial` s against just before (signal, rate, resends, the gateway's own loss); kept, or moved back and avoided a day. A channel change the same over the radio's devices (or its busy time) |
| Explained | `ag.mesh` events and `WE_MESH` on the timeline; `MESH_BACKHAUL` (HOME_WIFI) in the brief and proactive care, with a plain customer note; `agctl mesh`; data model 1.19 `Device.X_AIGW_.Mesh.` and `MeshDecision!`; the bridge's `mesh` counts; cortex `mesh.py`: the fleet report's and dashboard's homes whose extenders hurt more than they help |

Verified by `tests/test_mesh.c` (the read, the rules, the trial, the plan, the backhaul), `test_brief.c`, `test_proactive.c`, `tests/mesh-test.sh` (CI: agd and `mesh-dataelements` against `tests/mesh/fakectl.py`, a fake controller on ubus and a local USP agent: the handler on its own; advise, approve and kept; enforce, worse and moved back; a miss; the controller's own steering left alone; the channel plan; a weak backhaul to the timeline, the data model, proactive care and the brief; state over a restart; a snapshot; no controller) and `tests/test_cortex.py` (class `Mesh`). Not yet on real multi-AP hardware: [mesh.md](mesh.md) says how.

**As built in v0.45: neighbour-aware channels.** Each channel AIGW may use is learned hour by hour of the day, and the access point moves only when another is clearly better over the home's busy hours ([neighbour-channels.md](neighbour-channels.md)). It extends channel self-healing (`agd/chan.c`) rather than adding a second scanner.

| Piece | As built |
|-------|----------|
| Rules (`core/libag/chanplan.c`, `ag/chanplan.h`, pure) | A table per access point: channel x hour, each cell the air others use, the neighbours' weight, what devices felt (loss, replies, rate), with counts; the home's own use by hour. `cp_nb_weight`: on the channel or overlapping (2.4 GHz less the farther away; 5 GHz the same block), by signal, by BSS Load. `cp_busy_hours`: at least half the busiest hour's use. `cp_choose`: after `wifi.chan_plan_days`, on the air where both were measured in the busy hours, else the neighbours (3 scans at least); clearly better = `chan_plan_better` % and an absolute margin, not much worse in any busy hour. `cp_verdict`: kept when nothing devices feel is clearly worse and something (or the air) clearly better |
| Learning (`agd/chan.c`) | Every `wifi.chan_poll`: the survey of all channels (in use: continuous; others after a scan), devices from Self-Healing Wi-Fi and the link rates. Every 10 min the scan results already there (`iw scan dump`), each scan once. Its own `scan ap-force` at most every `chan_learn_gap`, never right after a start, only when quiet (no call or game, own share of the air under `chan_quiet` %, else devices under 2 Mbit/s), in an hour not learned yet. The table is kept in the state file |
| Acting | Every `chan_plan_every`: the same better channel `chan_plan_hold` looks in a row, at most one move per `chan_plan_gap`, not an avoided channel, no protected device on the access point, at a quiet moment: alert `wifi-plan`, policy `wifi_plan` (built in for older configurations), a `channel` decision through act (mode, hourly limit, area outages). The trial: `chan_plan_trial` s of busy hours on the new channel against the old one's learned busy hours; kept (saved through the Wi-Fi handler) or back and avoided `chan_avoid` s. Self-Healing Wi-Fi's congestion trial now picks the quietest known channel first and shares the avoid list |
| Explained | `agctl channels` (air by hour, neighbours, busy hours, the verdict, the trial) and a line in `agctl wifi`; `WE_PLAN` on the timeline; the brief's congestion fix and evidence; data model 1.20 `Device.X_AIGW_.ChannelPlan.Radio.{i}.` and `ChannelPlan!`; the bridge's `channels` (verdicts and counts); cortex `channels.py` in the fleet report |

Verified by `tests/test_chanplan.c` (weights, busy hours, the field case: 6 not 1, not from one scan; the air by busy hour; the verdicts, the field trial moved back) and `tests/chanplan-test.sh` (CI: a fake hostapd and `iw`, five days seeded: no scan or move while busy; a quiet moment: one scan, 6 proposed; approved, kept on better rates; the data model, events, timeline, state; enforce: worse rates, moved back, avoided, the gap holds), `tests/test_cortex.py` (class `Channels`). Not yet on the real PC access point: [neighbour-channels.md](neighbour-channels.md) says what only it shows.

**As built in v0.46: coverage placement advice.** Which device has weak Wi-Fi, when, where in the home, and what fixes it, from what agd already measures; no floor plan, no model on the router ([coverage.md](coverage.md)).

| Piece | As built |
|-------|----------|
| Rules (`core/libag/coverage.c`, `ag/coverage.h`, pure) | Per device: a signal histogram (2 dB bins), looks and weak looks by local hour, the last 7 days, loss when weak and when fine, rate and resends when weak, 2.4 GHz share; up to 3 weak spots, each a signature (what each access point hears, 2.4 GHz less 7 dB) plus the time of day, merged within 6 dB. The history halves after a week of looks. Kinds: `Sometimes`, `Always`, `WeakTransmitter` (steady and weak, yet low loss where other weak devices lose a lot, or a high rate to it where theirs falls, or a smart-home device in a home that is otherwise fine). The home's spots: the devices' spots with the same signature; shared when two or more suffer there. Verdict `Extender`, `UseExtender` (a mesh extender hears the spot well), `MoveDevice`, `WeakDevice`, `Fine`, `Learning`; the words, and a plain customer note |
| Sampling (`agd/coverage.c`) | Every `coverage.interval` s: each device on the gateway's hostapd (signal, band, rate) and on the mesh (`mesh_seen_at`: the agent it is on, resends, unassociated-station and 802.11k readings of the others); Self-Healing Wi-Fi's loss; drops and roams from `ag.wifi`; the local hour (`AIGW_TEST_HOUR_FILE` in tests). Kept in agd's state |
| Shown | `coverage_advice()` for `WIFI_COVERAGE` (and `WIFI_DROPS` of a weak device) in Diagnose, the timeline, care, the data model's advice fields; the brief's evidence (`coverage`), fix and tell; proactive tickets' next step and customer note; `agctl coverage`; data model 1.21 `Device.X_AIGW_.Coverage.` (+ `Spot.{i}.`), `Hosts.Host.{i}.X_AIGW_Coverage`, `X_AIGW_UsualSignal`; the bridge's `coverage` (kinds unless `brief.names`), cortex `coverage.py`: homes that need an extender, in the fleet report and the dashboard |

Verified by `tests/test_coverage.c` (the history, kinds, spots across bands, a shared spot, weak transmitters by loss and by kind, a mesh's best ear, decay, plain notes), `test_brief.c` (a name before a colon is scrubbed too), `test_cortex.py` (`Coverage`), `tests/coverage-test.sh` (CI: a simulated home with a fake hostapd and a test clock: the phone that moves, the TV that stays, the weak plug, one shared spot; kinds for the ISP; a ticket that says where; kept over a restart).

**As built in v0.47: one standard way to the mesh, and more room in flash.** Nothing the gateway does changes; how it reaches an EasyMesh controller and how its programs are built does.

| Piece | As built |
|-------|----------|
| Mesh handler (`platform/openwrt/aigw/files/mesh-dataelements`) | Replaces the vendor-specific handler. `get`: `Device.WiFi.DataElements.Network.` from ubus `_get`, else the local USP agent (`obuspa -c get`, `path => value` lines). `steer`: the station's instance, found by `MACAddress` in the model, `ClientSteer(TargetBSSID, RequestMode=Steering_Mandate, BTMDisassociationImminent)`. `channel`: the radio's instance, found by `ID`, `ChannelSelectionRequest()` with `Class.1.OpClass`, the target at preference 15 and the class's other channels at 1 (centre channels for 80 and 160 MHz classes). A `Status` other than `Success`, or the bus's error, is exit 1; no controller on either bus, exit 3 |
| Upgrades | `mesh.handler` naming `/usr/libexec/aigw/mesh-prplmesh` (0.46 and older): agd's config reader takes it as the platform's handler (`MESH_HANDLER`); OpenWRT's uci-defaults rewrites the option. The upgrade test sets the old path on the old release and checks the new one after |
| Removed | The data model component for one vendor's bus runtime (`amx/`), its build script, CI job and test |
| Smaller programs | Link-time optimisation in release builds (`AIGW_LTO`, on where CMake finds it supported; OpenWRT's SDK has `gcc-ar`). On aarch64 and MIPS, segments aligned to 4 KB pages (`-z max-page-size=4096`), not 64 KB: on mipsel that padding was 35 to 50 KB in agdns and in each plug-in. The USP plug-in also gets gc-sections and the page size |

Installed flash, as `aigw-selftest` counts it in CI's QEMU jobs (budget 1024 KB):

| Packages | v0.46 | v0.47 | Saved |
|----------|-------|-------|-------|
| `x86_64` | 980 KB | 948 KB | 32 KB |
| `aarch64_cortex-a53` | 980 KB | 940 KB | 40 KB |
| `mipsel_24kc` | 960 KB | 788 KB | 172 KB |

Verified by `tests/mesh-test.sh` (the handler on its own over a fake ubus and a fake local USP agent, then everything before through it; a config with the old handler's path), the upgrade test (`MIGRATIONS` in `upgrade_console.py`), and the full suite on the LTO build.

**As built in v0.50: fleet learning, every home teaches the next** ([fleet-learning.md](fleet-learning.md)). Middleware exposes each home's data; AIGW decides with the fleet's evidence, on the gateway, and says why.

| Piece | As built |
|-------|----------|
| Outcomes (`agd/agd.h` `outcome_report`) | How a fix, or waiting, went for a fault: act.c (trial kept, tried, undone by a person), proactive.c (fixed by AIGW, cleared by itself, still there after the grace time); one per key, a later report replaces the earlier (proven self-repair's receipts use the same call) |
| Buckets (`core/libag/fleet.c`, `ag/fleet.h`) | Fault x action x band/width/congestion/device class/hours/line, at five levels from every dimension to none; the Bayesian blend (a Dirichlet: the fleet's counts as at most `fleet.strength` tries, the home's own added) and the choice (only by `fleet.margin`, only with 5 tries behind each) |
| agd (`agd/fleet.c`) | Cells in the state; the summary (opt-in `fleet.share`: counts capped, noised with keyed repeatable noise, the normal's shape); the `fleet-priors` slot in models.c (usign, version, rollback, k checked); proactive care waits first when the fleet says waiting clears it more often; early warning's normal from the fleet while learning (`care_normal_for`) |
| Transport | The bridge (capability `fleet.share`) posts `/v1/fleet/summary`; priors come back through `/v1/models` like any signed update |
| Cortex (`fleetlearn.py`) | The reference aggregator: a fixed vocabulary (no MAC, address or name can travel), latest summary per home, cap per home and bucket, k-anonymity, medians of normals, published only on change |
| Surfaces | `agctl fleet`, `agctl fleet rank <code> [device]`; DM 1.23 `Device.X_AIGW_.FleetLearning.`, `FleetPriorsReceived!`, `FleetChoice!`; ubus `ag.fleet`, event `ag.fleet` |

Installed flash, full profile: 980/972/812 KB (x86-64/aarch64/mipsel); fleet learning itself 28/32/24 KB.

**As built in v0.48: features chosen when it is built.** The full build does what v0.47 did; whoever builds AIGW OS can leave features out ([features.md](features.md)).

| Piece | As built |
|-------|----------|
| Features (`cmake/features.cmake`) | 20 `AIGW_FEATURE_*` options, on by default, each with what it needs (checked: a missing need stops the build); `AIGW_PROFILE` full, small or minimal. A generated `ag/features.h` (`AIGW_FEATURE_X` 1/0, `AG_FEATURES`), included by `ag/ag.h`. A feature that is off isn't compiled: its agd and libag sources, its unit test, its plug-ins |
| The core | Devices, DNS and connection sensing, alerts and the domain-risk model, decisions, models, the data model with USP and TR-069, Wi-Fi settings and firmware, Diagnose, and taking the home's names out of what the ISP sees (moved into `agd/scrub.c`, `core/libag/scrub.c`; Wi-Fi health names into `core/libag/diag.c`) |
| Hook points | `agd/agd.h`: each feature's functions inside `#if`, with inline stubs under `#else` only for what the core or an optional partner calls (the compiler drops them). `main.c` (ubus methods, objects, inits, ticks), `dm.c` (parameters, instance tables, getters, commands; events by name, `EVT_*`), `plugin.c` and agctl (a `features[]` table: "not built in (AIGW_FEATURE_X)") inside the same `#if` |
| Saying what it has | `agd --features`, `agctl version`, `Device.X_AIGW_.Features` (1.22), the `features` field of `ag status`; a log line for config sections of features not built in |
| OpenWRT | `make menuconfig`: a profile, or Custom with each feature (Kconfig `depends on` its needs), passed to CMake; only the built features' files shipped (`aigw-sqm`, the mesh handler, plug-in manifests, LuCI's access page); `aigw-mqtt`, `aigw-bridge` need `PLUGINS`, `aigw-remote` `REMOTE`. `AIGW_PROFILE=small build-sdk.sh` |
| CI | The Linux job for full, small and minimal (a test for a feature not built says SKIP; dm, cwmp, usp and proactive guard their feature parts); the OpenWRT 24.10 jobs also build small and minimal and print each profile's installed KB (`flash-kb.py`) |

Installed flash, as `aigw-selftest` counts it (budget 1024 KB):

| Profile | `x86_64` | `aarch64_cortex-a53` | `mipsel_24kc` |
|---------|----------|----------------------|---------------|
| full | 952 KB | 940 KB | 788 KB |
| small | 664 KB | 668 KB | 564 KB |
| minimal | 392 KB | 384 KB | 340 KB |

Verified by the full local suite on each profile, every feature built alone with what it needs, and the full build without each feature (each with no warnings).

**As built in v0.51: proven self-repair.** Middleware exposes data; AIGW decides, and now proves what it decided ([proven-repair.md](proven-repair.md)). Feature `PROVEN` (needs `CARE`; full and small profiles).

| Piece | As built |
|-------|----------|
| Before a fix | Every corrective decision (`channel`, `stay24`, `sqm`, `meshsteer`, `meshchan`), proposed or taken, gets a receipt with a prediction: the metric, the device or radio, the value now, the value expected, the deadline. The part that proposes it says what it expects (`repair_expect()` in heal.c, chan.c, sqm.c, mesh.c, proactive.c); otherwise the metric's good value or the home's normal for the hour |
| After it | Measured from `repair.settle` to the deadline, against the value before and the home's normal for that hour (care's normal; for Wi-Fi loss, care's learning rule on the home's loss while all is well); helped, no change, worse, or unknown (the device left, an area outage, a person's undo, too little measured). A fix with a trial of its own (channel healing, planned channels, SQM, mesh) gets the trial's numbers (`repair_trial()`), and the trial decides helped against no change |
| Undo | A fix of AIGW's own without a trial that didn't help is undone through the decision's own undo (`act_undo_because`: the channel before, the band refusal lifted, SQM removed, the device steered back), in enforce mode only; state `tried`, `RepairUndone!` |
| Advise mode | Shadow receipts: the prediction, the predicted gain, a confidence (the fix rate measured here, once 3 are judged); measured without the fix: needed or not needed; never applied, marked so everywhere |
| Evidence | Per fault and kind of fix, by day for 31 days: fix rate (unknowns left out), mean improvement, undo rate, calls likely avoided, shadows; over 1, 7 and 30 days |
| Gate | `repair.gate` (off by default): in enforce mode, act alone only with kinds of fix at `gate_fix_rate` % over `gate_trials` judged trials; others wait for a person, with a note (`repair_gate()` in act.c) |
| Surfaces | `agctl repair` (`--stats`, `--shadow`), lines in `agctl care` and `agctl why` (and the timeline, `WE_REPAIR`), ubus `ag repair`, `ag.repair` events, data model 1.24 `Device.X_AIGW_.Repair.` (`Receipt.{i}.`, `Stats.{i}.`), `Repair!`, `RepairUndone!`, the bridge's `repair` counts; proactive's "fixed by AIGW" and the offers' signals count only proven fixes |
| State | Receipts (64 at most, the oldest judged go first) and the evidence, saved by name in agd's state: kept over restarts and upgrades |

Installed flash, OpenWRT 24.10.8 SDK, as `aigw-selftest` counts it (budget 1024 KB), against v0.50 built the same way:

| Profile | `x86_64` | `aarch64_cortex-a53` | `mipsel_24kc` |
|---------|----------|----------------------|---------------|
| full | 992 KB (+40) | 980 KB (+40) | 820 KB (+32) |
| small | 708 KB (+44) | 708 KB (+40) | 600 KB (+36) |
| minimal | 392 KB (+0) | | |

Verified by `tests/test_repair.c`, `tests/repair-test.sh` and `tests/proactive-test.sh`, the full local suite on the full, small and minimal profiles, `PROVEN` built alone with `CARE` and `TR143`, and the full build without it (no warnings).

**As built in v0.53: a model of the home's Wi-Fi, and Wi-Fi sensing.** Two build options, both in the full profile ([home-model.md](home-model.md), [wifi-sensing.md](wifi-sensing.md)).

| Piece | As built |
|-------|----------|
| Home model (`HOMEMODEL`, needs `COVERAGE`) | `core/libag/home.c`: coverage advice's looks clustered into the home's places (signatures across access points and bands, at most 8); places next to each other from devices walking between them; per place and band the path loss and a distance-like figure (log-distance, exponent 3), the 2.4 GHz offset, the experience per access point; the last 30 days' coverage problems. `agd/home.c` is fed by `agd/coverage.c` |
| What if | Extender (near a place with a good link, or about halfway), GatewayUp, GatewayCentral, Band24, Band5: problems fixed of the last 30 days', places it may make weak, a confidence and its limits; ranked by problems fixed, the free one first when AIGW is fairly sure of both. `ExtenderHelps` holds or backs the ISP's `MESH_WIFI` signal. After an extender appears: predicted against measured |
| Wi-Fi sensing (`SENSING`, opt-in) | `core/libag/sense.c`: per device and band, fresh readings only (the frame counter moved), the spread and change point per 30 s window, a sticky HMM per zone and one for presence, the usual movement per hour (weekdays, weekends) and the check-in. `agd/sense.c`: runs only with `sensing.enabled` and a consent; off keeps nothing, `agctl sense off` forgets; events local, the ISP's only with `share_events`; CSI only through a vendor handler |
| Where it shows | `agctl home`, `agctl home name`, LuCI's Place button, `agctl sense`; data model 1.25 (`Device.X_AIGW_.HomeModel.` with `Place.{i}.` and `Option.{i}.`, `Device.X_AIGW_.Sensing.`); events `HomeAdvice!`, `PresenceChanged!`, `UnusualInactivity!`; ubus `ag.home`, `ag.sense`; cortex gets the home model's counts and best option, never sensing |
| Honesty | Sensing's accuracy is measured on a simulated home with whole days held out (`tests/test_sense.c`), not on real labelled activity; `agd --sense-bench` and `aigw-selftest` check its CPU on each router |

Installed flash, as `aigw-selftest` counts it (budget 1024 KB):

| Profile | `x86_64` | `aarch64_cortex-a53` | `mipsel_24kc` |
|---------|----------|----------------------|---------------|
| full | 996 KB | 988 KB | 828 KB |
| ... of it `HOMEMODEL` | 28 KB | 32 KB | 24 KB |
| ... of it `SENSING` | 12 KB | 16 KB | 12 KB |
| small | 664 KB | 668 KB | 568 KB |
| minimal | 392 KB | 384 KB | 340 KB |

The full profile has 28 KB left on x86-64 and 36 KB on aarch64.

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
