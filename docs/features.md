# Choosing features

AIGW OS is one code base. Whoever builds it, an ISP, a router maker or you, picks which
features it has. A feature that is not built in takes no flash. New features come as
options too, so they don't grow every image.

Four profiles, each with all of the one before: **minimal** < **small** < **full** < **max**.

- **full** is the default. It is what most routers have room for, and it stays within 1024 KB
  of installed flash on every architecture.
- **max** has every feature, within 2048 KB, for routers with 16 MB of flash or more (most
  current OpenWRT routers) and for a PC as access point (`platform/pc-ap/install.sh` builds max).

### The rule for new features

A new feature goes into **max**. It joins **full** only when full still fits its 1024 KB, with
headroom, on every architecture CI measures. It joins **small** only when an ISP needs it on the
smallest routers. In `cmake/features.cmake`, a feature's last column names the smallest profile it
is in; leaving it empty means max. A feature leaving full for max is said in the release notes,
and an upgrade of the full packages loses its data model (the upgrade test knows which ones).

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
other features need it, they go too. They were measured before agctl became a link to agd
(v0.51.1), which made every build smaller; the profiles' totals below are today's.

| Feature | What it is | Needs | In | Off saves |
|---|---|---|---|---|
| `WIFI_HEAL` | Self-Healing Wi-Fi, band steering, channel healing, neighbour-aware channels | | small | 60 KB |
| `MESH` | Smarter mesh Wi-Fi over an EasyMesh controller ([mesh.md](mesh.md)) | | full | 52 KB |
| `COVERAGE` | Coverage placement advice ([coverage.md](coverage.md)) | `WHY` | full | 20 KB |
| `TR143` | TR-143 diagnostics: IPPing, TraceRoute, Download, Upload, NSLookup | | small | 168 KB with what needs it (about 36 on its own) |
| `CARE` | Proactive care: checks, early warning, the experience score, the speed test | `TR143` | small | 132 KB with what needs it (about 40 on its own) |
| `UPGRADE` | Firmware upgrades that check themselves and roll back | `CARE` | small | 24 KB |
| `WHY` | Why was it slow: the home's timeline | | small | 152 KB with what needs it (about 32 on its own) |
| `OFFERS` | Signals for the ISP (a faster plan, mesh Wi-Fi, and more) | `WHY`, `CARE` | full | 16 KB |
| `SQM` | Self-tuning smart queueing | `WHY` | small | 32 KB with `CALLS` (about 12 on its own) |
| `CALLS` | Calls and games first | `SQM` | full | 20 KB |
| `BRIEF` | The help-desk brief | `CARE`, `WHY` | small | 52 KB with `PROACTIVE` (about 24 on its own) |
| `PROACTIVE` | Proactive care before the call: tickets to the ISP | `BRIEF` | small | 28 KB |
| `AREA` | Area outages: fixes held while many homes fail at once | | full | 8 KB |
| `FED` | Federated learning | | full | 16 KB (with `FLEETLEARN`, which needs it, 44 KB) |
| `FLEETLEARN` | Fleet learning: fixes ranked by the fleet's outcomes, warnings from day one ([fleet-learning.md](fleet-learning.md)); it needs an ISP-side collector | `FED` | **max** (since v0.51.1) | 28 KB |
| `PLUGINS` | Plug-ins and skills: MQTT, Home Assistant, the bridge to cortex | | full | 104 KB |
| `SNI` | Connection names from TLS and QUIC handshakes | | full | 8 KB |
| `METER` | Per-device traffic meter (eBPF) | | full | 4 KB |
| `FASTPATH` | Kernel fast path (flowtable) | | full | under 4 KB |
| `REMOTE` | Remote access over WireGuard | | full | 8 KB (and the `aigw-remote` package) |
| `KEYS` | Per-device Wi-Fi keys | | full | 8 KB |
| `PROVEN` | Proven self-repair: every fix predicted, measured, and undone when it didn't help ([proven-repair.md](proven-repair.md)) | `CARE` | small | 44 KB (aarch64 36, mipsel 36) |

"In" is the smallest profile that has the feature: small's are in full and max too, full's in max.

Some features work better together without needing each other. Coverage advice uses the mesh
when it is built, and Self-Healing Wi-Fi's loss when that is built. The help-desk brief mentions
smart queueing, the mesh and area outages when they are built.

## Profiles

| Profile | Features | Budget | x86-64 | aarch64 | mipsel |
|---|---|---|---|---|---|
| `max` | every feature: for 16 MB of flash or more, and a PC | 2048 KB | 972 KB | 956 KB | 800 KB |
| `full` (default) | all but max's own: today `FLEETLEARN` | 1024 KB | 944 KB | 932 KB | 780 KB |
| `small` | for 8 or 16 MB of flash: `WIFI_HEAL`, `TR143`, `CARE`, `UPGRADE`, `WHY`, `BRIEF`, `PROACTIVE`, `SQM`, `PROVEN` | 1024 KB | 652 KB | 664 KB | 560 KB |
| `minimal` | the core only | 1024 KB | 356 KB | 352 KB | 308 KB |

Measured in v0.51.1 (OpenWRT 24.10.8 SDK). Full has 80 KB of headroom on x86-64, 92 on aarch64
and 244 on mipsel. The installed flash is what `aigw-selftest` counts on the router: the programs,
the scripts, the model and the plug-ins. CI's OpenWRT 24.10 jobs print it for every profile against
its budget, and fail when one is over (`platform/openwrt/flash-kb.py`).

`agctl` is a link to `agd` (since v0.51.1): one program, so libag and the libraries are in flash
once. That saved 36 to 56 KB in every profile on every architecture.

## How to pick

**CMake** (a PC, or your own build system):

```sh
cmake -S . -B build -DAIGW_PROFILE=max                      # a profile: minimal, small, full (default), max
cmake -S . -B build -DAIGW_FEATURE_MESH=OFF                 # full, without mesh Wi-Fi
cmake -S . -B build -DAIGW_FEATURE_FLEETLEARN=ON            # full, with fleet learning (that is max)
cmake -S . -B build -DAIGW_PROFILE=minimal -DAIGW_FEATURE_TR143=ON   # the core and TR-143
```

Pick a profile in a new build directory: a feature already in the CMake cache keeps its value.
A feature that needs one that is off stops the build, and says which one to turn on.

**OpenWRT** (`make menuconfig`): Network > aigw > AIGW OS features. Choose a profile, or
"Custom" to pick each feature. A feature whose needs are off can't be picked. The SDK script
takes a profile too:

```sh
AIGW_PROFILE=max platform/openwrt/build-sdk.sh      # packages in dist/openwrt-max/
```

**Releases** have the full profile's packages under their usual names, and, for each target, the
max profile's `aigw` package as `aigw-max_<version>_<arch>.ipk` (`.apk` on 25.12): the other
packages (LuCI, the bridge, MQTT, USP, TR-069) are the same in every profile. That is one file
more per target, four in all. It installs as the `aigw` package, in place of full's (the same
version: `opkg install --force-reinstall ./aigw-max_*.ipk`; full's back the same way). The demo
image is built from max.

Only the files of the features built are shipped. For example, `aigw-sqm` comes with smart
queueing, the mesh handler with mesh Wi-Fi, and the Wi-Fi keys and remote access page in LuCI
with either of them. The `aigw-mqtt` and `aigw-bridge` packages need `PLUGINS`, and
`aigw-remote` needs `REMOTE`.

## What a build says about itself

- `agd --features` and `agctl version` list the features built in.
- `agd --profile` and `agctl version` say the profile (`custom` when the features are not exactly
  a profile's) and its flash budget: that of the smallest profile with everything built (a custom
  build with a max feature has 2048 KB). `aigw-selftest` checks the installed flash against it.
- `Device.X_AIGW_.Features` (data model 1.22 and later) has the same list, so the ISP and cortex know
  what each gateway can do.
- A command of a feature that is not built in says so: `agctl mesh: not built in
  (AIGW_FEATURE_MESH)`.
- The data model has only what is built in. USP and TR-069 serve the schema agd gives them,
  so the ISP sees only what exists.
- A config section for a feature that is not built in is ignored, and agd's log says so once
  at start: `config: mesh (AIGW_FEATURE_MESH): not built in, ignored`.

## Adding a feature

1. Add a row to `AIGW_FEATURE_TABLE` in `cmake/features.cmake`: the name, what it is, what it needs, and
   no profile (it is in max). In `platform/openwrt/aigw/Makefile`, its Kconfig option defaults to y only
   with `AIGW_PROFILE_MAX || AIGW_PROFILE_CUSTOM`, and its help says its KB. Moving it into full later
   is a change of its own, with CI's flash table showing headroom on every architecture.
2. List its sources with `aigw_feature_sources()` in `agd/CMakeLists.txt` and `core/libag/CMakeLists.txt`.
3. In `agd/agd.h`, put its functions inside `#if AIGW_FEATURE_<NAME>`. Under `#else`, add small inline stubs
   only for what the core or an optional partner calls.
4. Put its ubus methods (`agd/main.c`), data model objects (`agd/dm.c`) and agctl commands inside the same
   `#if`, and add its commands to agctl's `features[]` table.
5. Give its shell test `needs "$BIN" <name>` (`tests/feature-lib.sh`), and its unit test an `if()` in
   `tests/CMakeLists.txt`.
6. Build the minimal profile, the full one, and max minus your feature: all build with no warnings.
