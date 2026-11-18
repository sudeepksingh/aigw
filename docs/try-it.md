# Try AIGW OS in five minutes

For an engineer at an ISP or a router maker who wants to see what AIGW OS
does before putting it on a router. One command downloads a demo image
(OpenWRT 24.10, x86-64, with AIGW OS), boots it in QEMU on your computer and
prints where to look. The image carries a **simulated home** and a
**simulated fleet** of 24 homes, so the operator dashboard, "why was it slow
last night?", the help desk's brief, smart queueing, calls first, the upgrade
check and the TR-181 data model all show real-looking content without
hardware. Everything simulated is labelled as demo data.

You need Linux, macOS, or Windows with WSL2, about 1 GB of disk, and QEMU.
The virtual machine has no network outside your computer.

## Five steps

**1. Install QEMU** (and curl, if you don't have it).

```bash
sudo apt install qemu-system-x86 curl          # Debian, Ubuntu, WSL2
```

```bash
brew install qemu                              # macOS (Homebrew)
```

**2. Run the demo.**

```bash
curl -fsSLO https://github.com/sudeepksingh/aigw/releases/latest/download/try-aigw.sh && sh try-aigw.sh
```

It downloads the image (about 30 MB, checked against its SHA-256), boots
it, and waits until the demo is up: under a minute with KVM (Linux, WSL2) or
HVF (Intel Macs), a few minutes emulated (Apple silicon, no KVM). Then it
prints three addresses on `127.0.0.1`. Ctrl+C stops it; nothing is kept.

**3. Open the dashboard**, with the link it printed
(`http://127.0.0.1:8090/#token=...`). This is what an ISP's operations team
sees of a fleet: the experience score, problems AIGW found and fixed by
itself, an outage across four homes of one town (the ISP's network, not the
homes), the homes to call first, signals for sales and care, and two staged
firmware rollouts, one of them stopped by itself after rollbacks. Your VM is
one of the homes: **This demo VM**, reported by its own bridge every minute.

**4. Ask the router why it was slow.**

```bash
ssh -p 2222 root@127.0.0.1                     # no password
agctl why --from "yesterday 20:00" --to "yesterday 21:00"
agctl brief "yesterday 20:00-21:00"
```

Yesterday at 20:00 the TV streamed in 4K and a laptop started a big game
download: the line was full, and everything else waited (the internet's delay
went from 12 ms to about 190 ms; a work call stuttered). AIGW found the
bufferbloat, proposed smart queueing, someone approved it at 20:38, and the
delay fell to about 22 ms while the line stayed full; the next call was
smooth. `agctl why` tells that story from the home's own timeline, in order;
`agctl brief` is what a help desk would hear: not a fault, the line was full,
AIGW fixed what could be fixed, no technician needed.

**5. Look at it as the ISP's management does.**

```bash
agctl dm get Device.X_AIGW_.SQM. Device.X_AIGW_.CallsFirst.Last.
agctl dm get Device.DeviceInfo.
```

The same answers, in TR-181, as a USP controller or a TR-069 ACS reads them
([device management](device-management.md)). Or open the router's web
interface, LuCI (`http://127.0.0.1:8080/`, user root, no password), menu
**AIGW OS**.

## What to look at

| Where | What it shows | More |
|---|---|---|
| `agctl why` | The slow evening explained against the home's normal: the line full, who downloaded, the rough call, AIGW's change, all in time order. Try `--at "yesterday 20:15"` or `--last 3h`. | [Why was it slow](why-slow.md) |
| `agctl brief` | A help desk's answer in one screen: fault or not, the evidence, what AIGW did, what to tell the customer, whether to send a technician. | [Help desk](help-desk.md) |
| `agctl sqm` | Smart queueing: found on bufferbloat, approved, judged by a trial (the delay while the line was full, before and after) and kept. | [Smart queueing](smart-queueing.md) |
| `agctl calls` | Calls and games first: the call on the full line that went first in the queue and stayed smooth. | [Calls first](calls-first.md) |
| `agctl upgrade` | The upgrade to this version two days ago: the home recorded before, watched for 24 h after, kept. | [Upgrade check](upgrade-check.md) |
| `agctl care` | Proactive care: the experience score, one problem this week, fixed by AIGW, the nightly speed test. | [Proactive care](proactive-care.md) |
| `agctl devices` | The home's nine devices, named and typed. | |
| The dashboard | The fleet: health, the outage, the homes to call first, rollouts (try **Pause**), signals, smart queueing and calls first across homes, each gateway's details. | [Dashboard](dashboard.md), [fleet report](fleet-report.md), [staged rollout](staged-rollout.md) |

## Demo data stays demo data

- **Labelled everywhere.** `agctl` prints `DEMO  a simulated home` first,
  `ag status` has `"demo": true`, LuCI shows a banner, the dashboard says
  "Demo data", and the data model's model name is "AIGW OS demo (simulated
  home)".
- **Only in QEMU.** The simulation is the `aigw-demo` package, which is in the
  demo image and nowhere else (it is not released as a package). Its start-up
  script runs only in a QEMU virtual machine (`/sys/class/dmi/id/sys_vendor`);
  anywhere else it says so and does nothing. It never replaces the state of a
  home it didn't write. The image is x86-64 only.
- **No network outside.** Both of the VM's networks are QEMU's restricted user
  networks: only the three forwarded ports on your computer's `127.0.0.1`
  reach it, and nothing leaves it. The demo turns off time servers and checks
  the line against QEMU's own gateway.
- **Nothing kept.** The VM runs from a snapshot; each boot simulates the last
  three days anew, up to now.

The simulated home is a three-day timeline (a sample a minute), nine
devices, two weeks of proactive care and an upgrade record, written before
agd starts ([demo/aigw-demo.c](../demo/aigw-demo.c)). The verdicts and their
words come from AIGW's own rules, and agd explains the timeline itself when
asked, exactly as on a router. The fleet is 24 homes as their bridges would
report them ([cortex/aigw_cortex/demo.py](../cortex/aigw_cortex/demo.py)).

## Only the dashboard, without QEMU

With Python 3 and nothing else:

```bash
git clone https://github.com/sudeepksingh/aigw.git
cd aigw/cortex
python3 -m aigw_cortex demo                    # http://127.0.0.1:8090/
```

It seeds a simulated fleet in a temporary directory (never your cortex
data) and serves the dashboard until Ctrl+C.

## From the demo to a real router

1. **Install AIGW OS on a router.** Each release has packages for x86-64,
   MediaTek Filogic (aarch64) and MT7621 (MIPS), for OpenWRT 24.10 (.ipk) and
   25.12 (.apk), or build them for any target with the SDK:
   [Getting started on OpenWRT](getting-started-openwrt.md). What to check on
   real hardware (memory, flash, Wi-Fi drivers): [Testing on
   routers](testing-on-routers.md).
2. **Let it learn.** On a real home the devices appear within minutes; the
   timeline fills a sample a minute (72 hours kept); proactive care learns the
   home's normal in three days, and smart queueing needs the line's speed
   (`care.speed_mbps`, or the nightly speed test with `care.speed_url`).
3. **Connect it to cortex**, on your own server:
   `python3 -m aigw_cortex enroll <name> --group <town>`, then on the router
   `agctl bridge <cortex URL>` and `agctl bridge token`, then
   `python3 -m aigw_cortex dashboard` ([dashboard](dashboard.md)).
4. **Manage it as an ISP**: USP (TR-369) or TR-069 with your controller or
   ACS, under your vendor prefix and names (`mgmt.vendor_prefix`,
   `mgmt.manufacturer`, ...): [Device management](device-management.md).
5. **Build it into your firmware**: the same packages go into an OpenWRT or
   prplOS image; [ARCHITECTURE.md](ARCHITECTURE.md) has the parts and
   their budgets.

## If something goes wrong

| | |
|---|---|
| "port 8090 is in use" | Another demo, or something else, is on that port: `DASH_PORT=8190 LUCI_PORT=8180 SSH_PORT=2322 sh try-aigw.sh`. |
| It takes minutes to start | No KVM or HVF: the VM is emulated. On Linux, check `/dev/kvm` (your user in group `kvm`); on WSL2, nested virtualization gives `/dev/kvm`. `BOOT_S=1200` waits longer. |
| WSL2 | Run it in the Linux side (Ubuntu); open the printed link in a browser on Windows: WSL2 forwards `127.0.0.1`. |
| ssh asks about a host key | Each run is a new VM: `ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@127.0.0.1`. |
| Anything else | The VM's console is in `~/.cache/aigw-demo/console.log`. |

A release's image is attached to it as `aigw-os-demo-x86-64.img.gz` (with
its `.sha256`); `sh try-aigw.sh --version 0.41.0` runs a given release, and
`sh try-aigw.sh --image FILE` one you built
(`platform/openwrt/build-demo-image.sh`). CI builds the image for every
change and boots it with `try-aigw.sh --check`, which checks the dashboard,
the demo labels, this VM's home reported through its bridge, and `agctl`
over ssh.
