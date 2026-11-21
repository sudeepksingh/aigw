# Smarter mesh Wi-Fi: AIGW over a standard EasyMesh controller

Most homes an ISP serves now have an extender or two. The Wi-Fi Alliance's
**EasyMesh** standard makes the gateway and the extenders one network: an
EasyMesh **controller** (on the gateway) runs **agents** (the gateway itself
and each extender) over IEEE 1905.1. **prplMesh** is the reference controller
and ships in prplOS, which is where an ISP compares AIGW OS with prplOS and
RDK-B most directly.

AIGW OS does not bring another 1905.1 stack. It is the **intelligence over a
standard controller**:

- It reads the mesh through the controller's standard northbound, the Wi-Fi
  Alliance **Data Elements** model (TR-181 `Device.WiFi.DataElements.`).
- It judges the mesh with what the gateway already learns about the home: Self-Healing
  Wi-Fi's loss for each device, calls and games in progress, the devices' names and kinds,
  each device's own history of moves.
- It acts through the controller's own requests (client steering, channel selection), as
  decisions with a trial and a way back.

```
$ agctl mesh
Mesh Wi-Fi: 2 access points (1 extender, wireless backhaul, one weak), 3 devices on the mesh; AIGW moved 3, kept 1, moved back 1; 1 channel change.
  read      1s ago from the controller 02:00:00:00:00:01 (handler; steering on, channel plan on, act.mode enforce)

  the gateway, fake gateway (02:00:00:00:00:01)
    radio     02:00:00:00:01:00 2.4 GHz channel 1/20 MHz, 40% busy
              BSS 02:00:00:00:01:01 home
    radio     02:00:00:00:02:00 5 GHz channel 36/80 MHz, 29% busy
              BSS 02:00:00:00:02:01 home, 02:00:00:00:02:02 home-bh (backhaul)

  the extender 'bedroom', fake extender (02:00:00:00:00:02)
    backhaul  Wi-Fi 5 GHz, -78 dBm, 54 Mbit/s to 02:00:00:00:00:01
    radio     02:00:00:00:03:00 2.4 GHz channel 6/20 MHz, 20% busy
              BSS 02:00:00:00:03:01 home
    radio     02:00:00:00:04:00 5 GHz channel 36/80 MHz, 20% busy
              BSS 02:00:00:00:04:01 home

  devices
    living-room-tv     the gateway 2.4 GHz, -82 dBm, 5 Mbit/s, 35% sent again; 11v yes, 11k yes
                       protected (act.protect): AIGW never moves it
    sara-laptop        the extender 'bedroom' 5 GHz, -58 dBm, 300 Mbit/s, 3% sent again; 11v yes, 11k yes; moved 1 (kept 1, back 0, missed 0)
    dads-phone         the extender 'bedroom' 5 GHz, -79 dBm, 20 Mbit/s, 5% sent again; 11v yes, 11k yes; moved 1 (kept 0, back 1, missed 0)
                       no access point hears it clearly better

  backhaul  the extender 'bedroom' has a weak wireless backhaul to the gateway (-78 dBm, 54 Mbit/s, 5 GHz); 2 devices are behind it. It hurts more than it helps.
            Move it a room closer to the gateway (about halfway is best), or connect it with a cable (Ethernet, powerline or MoCA).

  last      decision 4 (Channel, Kept, 2s ago): channel 1 -> 6 kept: no device on it to measure; the plan itself removes the clash
```

(This is from the end-to-end test, `tests/mesh-test.sh`, over a simulated
controller; see [Testing](#testing).)

## What AIGW adds over a plain EasyMesh controller

A controller already steers devices and picks channels by itself. AIGW adds:

| | A plain EasyMesh controller | With AIGW OS over it |
|---|---|---|
| **When to move a device** | When its signal crosses an RCPI threshold (`RCPISteeringThreshold`) or the channel is busy | Also when it **loses traffic** at a good signal: the gateway's own probes (Self-Healing Wi-Fi) and the resends in the Data Elements counters. And only after it has struggled for a while (`mesh.hold`), not on a blip |
| **Where to** | The best RCPI it knows of | A target **measured** to hear the device clearly better (an 802.11k report, or the target's unassociated-station measurement), by a margin (hysteresis). AIGW never moves a device on a guess, except across bands of the same box, where the spot is the same. A target behind a weak backhaul counts for less, and a radio over 80% busy is never a target |
| **Never at the wrong time** | Unaware of what the device is doing | **Never during a call or a game** (calls first knows). Never a protected device (`act.protect`). Never a device without 802.11v unless allowed (`mesh.legacy`), because blocklist steering drops it for a moment |
| **Proof** | Fire and forget | Each move is a **trial**. The device's link (signal, rate, resends, loss) is measured `mesh.trial` seconds after the move against just before. AIGW keeps the move when the device does clearly better and nothing got worse. Otherwise it moves the device back and avoids that move for a day |
| **Learning per device** | — | A device whose steers don't land (it ignores BTM requests) is paused after two misses, doubling each time. A move that didn't help is avoided for a day. This history survives restarts |
| **Never fighting the controller** | — | A device the controller (or the device itself) moved in the last `mesh.quiet` seconds is left alone, and so is a device AIGW moved in the last `mesh.cooldown` seconds. AIGW makes one change at a time |
| **Channels** | Each radio's own channel selection | A plan across **all** radios of the home: no extender on the same air as the gateway where it is avoidable. A radio carrying a wireless backhaul stays with its parent. The gateway keeps its channels, and only the extenders move |
| **The backhaul** | Picks one | Says when an extender **hurts more than it helps**: a weak or slow wireless backhaul with devices behind it. Also a 2.4 GHz backhaul where 5 or 6 GHz would do, and a 5 GHz backhaul where both ends have 6 GHz |
| **Act modes** | Always acts | `act.mode`: **advise** proposes each change, and a person (or the ISP over USP/TR-069) says yes. **Enforce** applies it within the hourly limit. **Off** only watches. No change during an [area outage](area-outages.md) |
| **Explained** | Counters | The [why timeline](why-slow.md), the [help-desk brief](help-desk.md) ("the extender 'bedroom' has a weak wireless backhaul"), a [proactive ticket](before-the-call.md) with a note for the customer, `agctl mesh`, the data model and cortex's fleet dashboard |

## How it works

### Reading the mesh

Every `mesh.poll` seconds (30), agd reads `Device.WiFi.DataElements.Network.`
from a backend:

- **The mesh handler** (`mesh.handler`, default
  `/usr/libexec/aigw/mesh-prplmesh`): `get <file>` writes the model as JSON,
  either path-keyed (`ubus call Device.WiFi.DataElements.Network _get
  '{"depth":"10"}'`, the way prplMesh's own tools read it) or nested. It can also
  write `path=value` lines (`ubus-cli`, `ba-cli`, obuspa).
- **A snapshot** (`mesh.source`): a file in either form. This is for tests, and for a
  controller whose northbound is elsewhere: anything that can write the file can feed
  AIGW. AIGW can't act without a handler.

What AIGW uses from the model, by its standard names:

| Object | Parameters |
|---|---|
| `Network.` | `ID`, `ControllerID` (which agent is the gateway) |
| `Network.Device.{i}.` | `ID` (the AL MAC), `Manufacturer`, `ManufacturerModel` |
| `...Device.{i}.MultiAPDevice.Backhaul.` | `LinkType` (Wi-Fi, Ethernet, MoCA, ...), `BackhaulMACAddress`, `BackhaulDeviceID`, `MACAddress`; `Stats.SignalStrength`, `Stats.LastDataDownlinkRate`, `Stats.LinkUtilization`; `CurrentOperatingClassProfile.{i}.Class/Channel` |
| `...Device.{i}.Radio.{i}.` | `ID` (RUID), `Utilization`, `Noise`; `CurrentOperatingClassProfile.{i}.Class/Channel` (the primary channel, and the width from the widest class) |
| `...Radio.{i}.BSS.{i}.` | `BSSID`, `SSID`, `BackhaulUse`, `FronthaulUse` |
| `...BSS.{i}.STA.{i}.` | `MACAddress`, `SignalStrength`, `LastDataDownlinkRate`, `LastDataUplinkRate`, `EstMACDataRateDownlink`, `PacketsSent`, `RetransCount`, `ErrorsSent`, `LastConnectTime`, `ClientCapabilities` (802.11v and k, from the association request's elements); `MultiAPSTA.SteeringSummaryStats.LastSteerTime`, `BTMAttempts`, `BTMFailures`; `MeasurementReport.{i}.BSSID/RCPI` (802.11k) |
| `...Radio.{i}.UnassociatedSTA.{i}.` | `MACAddress`, `SignalStrength` |

Signal strengths are RCPI (802.11 Table 9-176): dBm = RCPI / 2 − 110. RCPI 0
and 221–255 mean "not known". Instance numbers may also be aliases.

### Moving a struggling device

A device is **struggling** when its signal is at or below `mesh.weak_dbm` (−75 dBm), or when it loses
traffic: the gateway's probes lose `10%`, or `20%` of its packets are sent
again. It must struggle for `mesh.hold` seconds (60) before AIGW acts.

Then AIGW skips the device when any of these holds:

- it is protected (`act.protect`);
- a call or a game is on;
- its last steers didn't land (it is paused);
- the controller moved it in the last `mesh.quiet` seconds (300);
- AIGW itself moved it in the last `mesh.cooldown` seconds (900);
- it joined or roamed in the last `mesh.quiet` seconds;
- it has no 802.11v and `mesh.legacy` is 0.

Otherwise the target is the BSS of the same network that:

- hears it at −68 dBm or better, and at least `mesh.hysteresis` dB (8) better than where it is;
- is on a radio below 80% busy;
- is not a move that failed in the last day.

The target counts 6 dB less when it is behind a weak backhaul, and 3 dB more on 5 or 6 GHz.

The move is the decision `meshsteer <device>@<BSSID>`. The handler's `steer <device>
<BSSID> <btm|mandate>` carries it out. With prplMesh that is
`Network.ClientSteering()`, which sends a BTM request, or uses prplMesh's
blocklist for a device without 802.11v.

### The trial

AIGW waits for the device to show up on the target. If it doesn't within three
reads (at least 10 s), that is a **miss**: the decision ends as `tried`
("missed"), the device is not moved back, and the cooldown starts again. After
two misses in a row the device is paused for an hour, and each further miss
doubles that. Once the device lands, AIGW measures it for `mesh.trial` seconds
(120), then compares it with just before:

- **kept** when something is clearly better and nothing is clearly worse:
  - the signal by half the hysteresis;
  - the rate by 20%;
  - loss 5 points less;
  - resends 10 points less.
- **moved back** otherwise. The device goes back to where it was, and that move is
  avoided for a day.

A person can undo a kept move (`agctl undo <id>`), and the device goes back too.

### The channel plan

For 2.4 and 5 GHz, AIGW looks for radios of the home on the same air: overlapping
2.4 GHz channels, or the same 20/40/80 MHz block on 5 GHz. The gateway keeps its
channels. An extender's radio that clashes moves to a channel that clashes with no
other radio of the home: 1, 6 or 11, or outside DFS on 5 GHz (36 or 149 at 80 MHz).
A radio that carries a wireless backhaul stays with its parent, because moving it
would cut the extender off. A tri-band extender's second 5 GHz radio is free to move.
When every channel is taken, the clash is unavoidable and nothing is proposed.

The decision is `meshchan <RUID>:<channel>`. The handler's `channel <RUID> <channel>
<opclass> <centre> <width>` carries it out. It is judged on the devices on that radio
(their mean rate and resends). With none, it is judged on the radio's busy time. A
change that didn't help is not tried again for a day.

### The backhaul

For each extender on a wireless backhaul:

| Finding | When | Advice |
|---|---|---|
| **Weak** | signal at or below −72 dBm | a room closer to the gateway, or a cable (Ethernet, powerline, MoCA) |
| **Slow** | below 100 Mbit/s | closer, or a cable |
| **Busy** | its link 80% or more in use | a cable, or a faster band for the backhaul |
| **On24** | on 2.4 GHz while both ends have 5 or 6 GHz | closer so it can use 5 GHz, or a cable |
| **Try6** | on 5 GHz at −65 dBm or better while both ends have 6 GHz | a 6 GHz backhaul (the controller's backhaul preference) |

A weak or slow backhaul with devices behind it **hurts more than it helps**.
That is what the help-desk brief says (code `MESH_BACKHAUL`, fault `HOME_WIFI`,
no technician visit), what proactive care raises a ticket for, and what
cortex's fleet dashboard counts. Meanwhile, AIGW moves the devices that hear the
gateway well enough off that extender. Phase 1 gives advice only. It does not
switch the backhaul itself (see [Follow-ups](#follow-ups)).

### Naming the extenders

An extender is named by `list name '<AL MAC>=<name>'` in the `mesh` section,
otherwise by its DHCP hostname. The brief then says "the extender 'bedroom'". A
DHCP hostname, like any device name, reaches the ISP only by kind, unless `brief.names` is on.

## What the ISP sees

The data model is version **1.19**. It has `Device.X_AIGW_.Mesh.` (`Status`,
`ControllerID`, `Agents`, `Extenders`, `Stations`, `WirelessBackhauls`,
`WeakBackhauls`, `HurtingExtenders`, `Steers`, `SteersKept`, `SteersUndone`,
`SteerMisses`, `ChannelChanges`, `ChannelsKept`, `ChannelsUndone`,
`Proposed`, the last decision and a `Summary`) and the event
**`MeshDecision!`** (`Decision`, `Kind` Steer/Channel, `State`
Proposed/Applied/Kept/Undone/Missed/Ended, `Station`, `From`, `To`, `Reason`,
`Before`, `After`). Device names reach the ISP only by kind (as with
`brief.names 0`). The decisions themselves are in `Device.X_AIGW_.Decision.{i}.`
like every other, so the ISP can approve a proposed move over USP or TR-069.

Cortex gets counts only from the bridge: access points, extenders, wireless,
weak and hurting backhauls with each finding's kind, signal and rate, and AIGW's
moves. The fleet report and the dashboard show **homes whose extenders hurt
more than they help**, worst first. Each one is a lead for a visit, a cable or a
better extender.

## Settings (`/etc/config/aigw`, section `mesh`)

| Option | Default | |
|---|---|---|
| `enabled` | 1 | Read the controller and decide. With no controller nothing happens: AIGW looks every 5 minutes |
| `handler` | `/usr/libexec/aigw/mesh-prplmesh` | The controller's handler (see [Another controller](#another-controller)) |
| `source` | — | A Data Elements snapshot to read instead (JSON or `path=value`) |
| `poll` | 30 | Seconds between reads |
| `steer` | 1 | Move struggling devices (as `act.mode` allows) |
| `channel` | 1 | Plan channels across the gateway and extenders (as `act.mode` allows) |
| `trial` | 120 | Seconds a move or a channel change is measured before it is judged |
| `hold` | 60 | Seconds a device must struggle first |
| `weak_dbm` | −75 | A device at or below this is struggling |
| `hysteresis` | 8 | dB a target must hear it better |
| `cooldown` | 900 | Seconds after AIGW moved a device before it moves it again |
| `quiet` | 300 | Seconds after the controller, or the device itself, moved it |
| `legacy` | 0 | Also steer devices without 802.11v |
| `name` (list) | — | `<AL MAC>=<name>`: the home's names for its extenders |

`act.mode` stays as it is: advise by default, so every move is only proposed
until a person or the ISP says yes.

## Pairing it with prplMesh on OpenWRT

1. **Run prplMesh's controller on the gateway.** prplOS ships it. On OpenWRT, build
   prplMesh from its feed (`https://gitlab.com/prpl-foundation/prplmesh/prplMesh`, with
   Ambiorix on ubus), then start the controller and the local agent. The extenders
   run prplMesh's agent, or any certified EasyMesh agent.
2. **Check its northbound is on ubus:**
   ```sh
   ubus list Device.WiFi.DataElements.Network
   /usr/libexec/aigw/mesh-prplmesh get /tmp/de.json && head -c 600 /tmp/de.json
   ```
3. **Read it from AIGW:** `agctl mesh --run`. Expect `status Ok`, the access points,
   each extender's backhaul and the devices.
4. **Decide who steers what.** prplMesh steers too: its optimal-path task, and its
   channel selection tasks (`X_PRPLWARE-COM_Controller.Configuration.ChannelSelectionTaskEnabled`,
   `DynamicChannelSelectionTaskEnabled`). AIGW never fights them: a device the controller just moved is left
   alone. But one owner per job is clearer:
   - leave prplMesh's own steering on (AIGW only adds moves it can prove);
   - give channels to one side: turn prplMesh's dynamic channel selection off and keep
     `mesh.channel 1`, or set `mesh.channel 0`.
5. **Turn off AIGW's single-AP Wi-Fi features** on radios the controller manages:
   `wifi.steer 0` (band steering through hostapd) and `wifi.chan_heal 0`. Self-Healing
   Wi-Fi's measuring (`wifi.heal`) stays useful: it gives AIGW each device's loss.
6. **Name the extenders:** `uci add_list aigw.mesh.name='<AL MAC>=bedroom'; uci commit
   aigw`, then restart agd.

prplMesh takes channel requests only through its BML (`beerocks_cli -c
"bml_trigger_channel_selection <RUID> <channel> <MHz>"`); its northbound has no
channel request yet. The handler uses BML, so it needs `beerocks_cli` on the
gateway.

### Another controller

A vendor's controller plugs in with its own handler, with the same three commands.
Point `mesh.handler` at it:

```
<handler> get <file>                                      the Data Elements model, JSON or path=value
<handler> steer <station MAC> <target BSSID> <btm|mandate>
<handler> channel <RUID> <channel> <opclass> <centre channel> <width MHz>
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | done |
| 2 | usage |
| 3 | no controller here |
| 1 | refused |

The last line printed says why. agd checks every value before calling the handler: MAC
addresses, channels AIGW uses, numbers. A controller with the TR-181 2.17+ commands
maps directly:

- `steer` is `Device.WiFi.DataElements.Network.Device.{i}.Radio.{i}.BSS.{i}.STA.{i}.ClientSteer(TargetBSSID,
  RequestMode=Steering_Mandate, ...)`.
- `channel` is `Device.WiFi.DataElements.Network.Device.{i}.Radio.{i}.ChannelSelectionRequest()`
  (2.19), with the target as preference 15 and the others lower.

## Testing

- **Unit tests** (`tests/test_mesh.c`, ctest `mesh_wifi`): reading the model, including
  RCPI, operating classes and widths, aliases, and 802.11v/k from the association
  request; steering (hysteresis, hold, protected, calls, cooldown, the controller's own
  steers, legacy devices, misses and pauses, moves avoided for a day, retries); trials;
  the channel plan (a wired or wireless extender, a tied radio, unavoidable clashes); and
  each backhaul finding. The brief and proactive care have their own cases
  (`test_brief.c`, `test_proactive.c`).
- **End to end** (`tests/mesh-test.sh`, in CI): agd against
  **`tests/mesh/fakectl.py`**, a fake controller that answers like prplMesh's ubus
  `_get`, simulates where each device hears each access point, and records every
  request. It covers advise mode (proposed, approved, kept), enforce mode (moved, worse,
  moved back), a miss, the controller's own steering left alone, the channel plan, a
  weak backhaul (the timeline, the data model, a proactive ticket, the brief), state
  over a restart, a `path=value` snapshot, and no controller. Run it with
  `sudo tests/mesh-test.sh build`.
- **prplMesh's own simulation** (Docker containers exchanging real 1905.1 frames) is
  too heavy for CI, which is why the fake controller exists.

### The real multi-AP check (on hardware)

This can't be simulated faithfully: real radios, real BTM behaviour. With a
gateway and one extender running prplMesh:

1. Do steps 1–5 above. `agctl mesh` lists both access points and the extender's
   backhaul (Wi-Fi or Ethernet, signal and rate for Wi-Fi).
2. Keep `act.mode advise`. Walk a phone from the extender's room toward the gateway
   (or the other way) until it is below −75 dBm. Within a minute or two `agctl
   decisions` shows a proposed `meshsteer`. `agctl mesh` says why, or why not:
   "no access point hears it clearly better", "in a call or game", ...
3. `agctl approve <id>`. Within `mesh.trial` (2 min) the decision is `kept` or
   `tried` (moved back), with the before and after in its note. `agctl watch decisions`
   follows it live.
4. Repeat with a device that ignores BTM requests: two misses, then "left alone".
5. Put the extender's 2.4 GHz on the gateway's channel (in prplMesh), with `mesh.channel
   1`: AIGW proposes another channel.
6. Move the extender far away (or behind walls) so its backhaul drops below −72 dBm
   with a device on it. `agctl mesh` shows the weak backhaul, `agctl brief --no-check`
   names it, and `agctl care proactive` raises `MESH_BACKHAUL`.
7. Report anything that differs, especially whether prplMesh's `ClientSteering()`
   lands, and how long it takes, on your devices.

## Follow-ups

Phase 1 is the read, steering and channel plan with trials, and the backhaul as
advice. Still to come:

- **Switching the backhaul.** `Network.SetPreferredBackhauls()` (TR-181 2.15): a 6 GHz
  or 5 GHz backhaul, or another parent, as a trial like the others. Today it is advice.
- **The 6 GHz channel plan**, and DFS channels with their CAC (from the radios'
  `CACCapability`).
- **Placement.** v0.46 does part of this: [coverage advice](coverage.md) finds the
  weak spots of the home, says which access point hears each one best, and where an
  extender would go (about halfway). Still to do: where to move an extender that is there.
- **Neighbours.** The controller's channel scan results (`ScanResult`), so the plan
  avoids the neighbours' networks as well as the home's own radios.
- **Moving a device off a hurting extender**, even when the gateway hears it
  only a little better, and a learned per-device margin from its kept and undone
  moves.
- **The ISP's view per extender**: `Device.X_AIGW_.Mesh.Extender.{i}.` with each
  backhaul, beyond the counts.
- **prplMesh channel requests through its northbound**, once it has one (today: BML).
- **The handler's `ubus-cli`/`ba-cli` fallback** is untested on prplOS hardware. The
  ubus `_get` path is the one prplMesh's own tools use.
