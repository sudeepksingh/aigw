# Choosing features

AIGW OS is one code base. Whoever builds it, an ISP, a router maker or you, picks which
features it has. A feature that is not built in takes no flash. New features come as
options too, so they don't grow every image.

The **full** build has every feature. It is the default, and it behaves exactly as AIGW OS
always has.

## The core

The core is always built. It is what makes AIGW OS a managed gateway:

- devices (DHCP, ARP, Wi-Fi stations), DNS and connection sensing, the baseline of each device;
- alerts, with the domain-risk model and signed model updates;
- decisions (`act`): guardrails, time limits, undo, `act.mode`;
- the data model, with USP (TR-369) and TR-069;
- Wi-Fi settings and firmware upgrades, as a management system changes them;
- Diagnose ("my internet is slow");
- taking the home's names out of what the ISP sees.

## The features

Sizes are for x86-64. They are agd, agctl, agdns and the plug-ins, stripped, in 4 KB blocks,
built as OpenWRT's SDK builds them. "Off saves" is what turning the feature off saves; when
other features need it, they go too. The core alone is 316 KB of programs; everything is 832 KB.

| Feature | What it is | Needs | Off saves |
|---|---|---|---|
| `WIFI_HEAL` | Self-Healing Wi-Fi, band steering, channel healing, neighbour-aware channels | | 60 KB |
| `MESH` | Smarter mesh Wi-Fi over an EasyMesh controller ([mesh.md](mesh.md)) | | 52 KB |
| `COVERAGE` | Coverage placement advice ([coverage.md](coverage.md)) | `WHY` | 20 KB |
| `TR143` | TR-143 diagnostics: IPPing, TraceRoute, Download, Upload, NSLookup | | 168 KB with what needs it (about 36 on its own) |
| `CARE` | Proactive care: checks, early warning, the experience score, the speed test | `TR143` | 132 KB with what needs it (about 40 on its own) |
| `UPGRADE` | Firmware upgrades that check themselves and roll back | `CARE` | 24 KB |
| `WHY` | Why was it slow: the home's timeline | | 152 KB with what needs it (about 32 on its own) |
| `OFFERS` | Signals for the ISP (a faster plan, mesh Wi-Fi, and more) | `WHY`, `CARE` | 16 KB |
| `SQM` | Self-tuning smart queueing | `WHY` | 32 KB with `CALLS` (about 12 on its own) |
| `CALLS` | Calls and games first | `SQM` | 20 KB |
| `BRIEF` | The help-desk brief | `CARE`, `WHY` | 52 KB with `PROACTIVE` (about 24 on its own) |
| `PROACTIVE` | Proactive care before the call: tickets to the ISP | `BRIEF` | 28 KB |
| `AREA` | Area outages: fixes held while many homes fail at once | | 8 KB |
| `FED` | Federated learning | | 16 KB (with `FLEETLEARN`, which needs it, 44 KB) |
| `FLEETLEARN` | Fleet learning: fixes ranked by the fleet's outcomes, warnings from day one ([fleet-learning.md](fleet-learning.md)) | `FED` | 28 KB |
| `PLUGINS` | Plug-ins and skills: MQTT, Home Assistant, the bridge to cortex | | 104 KB |
| `SNI` | Connection names from TLS and QUIC handshakes | | 8 KB |
| `METER` | Per-device traffic meter (eBPF) | | 4 KB |
| `FASTPATH` | Kernel fast path (flowtable) | | under 4 KB |
| `REMOTE` | Remote access over WireGuard | | 8 KB (and the `aigw-remote` package) |
| `KEYS` | Per-device Wi-Fi keys | | 8 KB |

Some features work better together without needing each other. Coverage advice uses the mesh
when it is built, and Self-Healing Wi-Fi's loss when that is built. The help-desk brief mentions
smart queueing, the mesh and area outages when they are built.

## Profiles

| Profile | Features | Installed flash (`aigw-selftest`) |
|---|---|---|
| `full` | all | x86-64 980 KB, aarch64 972 KB, mipsel 812 KB |
| `small` | for 8 or 16 MB of flash: `WIFI_HEAL`, `TR143`, `CARE`, `UPGRADE`, `WHY`, `BRIEF`, `PROACTIVE`, `SQM` | x86-64 664 KB, aarch64 668 KB, mipsel 564 KB |
| `minimal` | the core only | x86-64 392 KB, aarch64 384 KB, mipsel 340 KB |

The installed flash is what `aigw-selftest` counts on the router: the programs, the scripts,
the model and the plug-ins. CI's OpenWRT jobs print it for each profile
(`platform/openwrt/flash-kb.py`).

## How to pick

**CMake** (a PC, or your own build system):

```sh
cmake -S . -B build -DAIGW_PROFILE=small                    # a profile
cmake -S . -B build -DAIGW_FEATURE_MESH=OFF                 # full, without mesh Wi-Fi
cmake -S . -B build -DAIGW_PROFILE=minimal -DAIGW_FEATURE_TR143=ON   # the core and TR-143
```

Pick a profile in a new build directory: a feature already in the CMake cache keeps its value.
A feature that needs one that is off stops the build, and says which one to turn on.

**OpenWRT** (`make menuconfig`): Network > aigw > AIGW OS features. Choose a profile, or
"Custom" to pick each feature. A feature whose needs are off can't be picked. The SDK script
takes a profile too:

```sh
AIGW_PROFILE=small platform/openwrt/build-sdk.sh    # packages in dist/openwrt-small/
```

Only the files of the features built are shipped. For example, `aigw-sqm` comes with smart
queueing, the mesh handler with mesh Wi-Fi, and the Wi-Fi keys and remote access page in LuCI
with either of them. The `aigw-mqtt` and `aigw-bridge` packages need `PLUGINS`, and
`aigw-remote` needs `REMOTE`.

## What a build says about itself

- `agd --features` and `agctl version` list the features built in.
- `Device.X_AIGW_.Features` (data model 1.22) has the same list, so the ISP and cortex know
  what each gateway can do.
- A command of a feature that is not built in says so: `agctl mesh: not built in
  (AIGW_FEATURE_MESH)`.
- The data model has only what is built in. USP and TR-069 serve the schema agd gives them,
  so the ISP sees only what exists.
- A config section for a feature that is not built in is ignored, and agd's log says so once
  at start: `config: mesh (AIGW_FEATURE_MESH): not built in, ignored`.

## Adding a feature

1. Add a row to `AIGW_FEATURE_TABLE` in `cmake/features.cmake`: the name, what it is, what it needs.
2. List its sources with `aigw_feature_sources()` in `agd/CMakeLists.txt` and `core/libag/CMakeLists.txt`.
3. In `agd/agd.h`, put its functions inside `#if AIGW_FEATURE_<NAME>`. Under `#else`, add small inline stubs
   only for what the core or an optional partner calls.
4. Put its ubus methods (`agd/main.c`), data model objects (`agd/dm.c`) and agctl commands inside the same
   `#if`, and add its commands to agctl's `features[]` table.
5. Give its shell test `needs "$BIN" <name>` (`tests/feature-lib.sh`), and its unit test an `if()` in
   `tests/CMakeLists.txt`.
6. Build the minimal profile and the full one minus your feature: both build with no warnings.
