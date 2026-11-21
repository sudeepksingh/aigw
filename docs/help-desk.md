# AIGW help-desk assistant: one answer for the ISP's support agent

A customer calls: "bad internet tonight?". The support agent has a few
minutes and a script. AIGW OS already knows a lot about that home: the why
timeline, proactive care's open problem and early warning, each access
point's Wi-Fi health, smart queueing, calls and games, the last firmware
upgrade, and its own decisions. Until now the agent had to read each one.

The help-desk assistant brings it together into **one short answer**. It can
first ask the home: a fresh quiet Diagnose (a few seconds) and, if wanted, the
speed test. It needs no AI model: the rules are plain code, and unit-tested.

```
$ agctl brief laptop tonight 20:00-22:00
20:00 to 21:30 (checked just now)
  fault     HOME_WIFI (WIFI_COVERAGE, medium confidence)
  verdict   Wi-Fi in the home, earlier: laptop's Wi-Fi signal was weak for 40 min of it (-82 dBm): too far from the access point, or walls in the way (WIFI_COVERAGE).
  evidence
    21:30     Checked just now: Everything the gateway can measure is fine: ... (OK) [internet 18 ms, 0% lost; next hop 2 ms; DNS 9 ms]
    20:12     Timeline 20:12-20:52: laptop's Wi-Fi signal was weak for 40 min of it (-82 dBm) ... (WIFI_COVERAGE)
  tell      The Wi-Fi signal is weak where the device is: move it closer to the gateway, or a mesh node or extender would help.
  fix       No remote fix: a mesh node or extender, or the device closer.
  visit     No: Wi-Fi in the home is fixed remotely or by moving things, not by a line repair.
```

(An example evening; `tests/brief-test.sh` makes the same answer in seconds.)
The ISP sees the same answer, with **device kinds instead of names**: "a
laptop's Wi-Fi signal was weak". The call is answered. No technician is sent.

## What is in a brief

| | |
|---|---|
| **Fault** | One word: `LINE` (the ISP's line or network), `HOME_WIFI`, `DEVICE` (one device, not the line), `OUTSIDE` (a service beyond the ISP), `NONE`, or `UNKNOWN` |
| **Code** | The finding that decided it, with Diagnose's and the timeline's codes (`LINE_LOSS`, `WIFI_COVERAGE`, `LINE_FULL`, ...), so the help desk can count them |
| **Confidence** | `high` when two sources agree (the fresh check, the timeline, proactive care, the nightly speed tests), `low` when they disagree or there is little to go on |
| **Verdict** | One sentence |
| **Evidence** | Times and numbers, each with its source: `now` (the fresh check), `timeline`, `care`, `wifi`, `speed`, `calls`, `upgrade` |
| **Done** | What AIGW already did: its own decisions (a channel moved, a device kept on 2.4 GHz, smart queueing), the ISP told about a problem, a bad firmware upgrade rolled back |
| **Tell** | What to tell the customer, in words they understand |
| **Fix** | A remote fix if there is one, and whether it waits for a yes already. It is never applied: see [Fixes need a yes](#fixes-need-a-yes) |
| **Visit** | Whether a technician should go out: `No`, `IfPersists` or `Yes`, with the reason |

### Which source decides

- For a period that ends now: the fresh check, if it found something (it is
  happening now). Else proactive care's open problem. Else the timeline.
- For a period in the past: the timeline. The fresh check is then only
  "and now".
- When nothing is wrong and there is enough data, the fault is `NONE`. When
  calls or games were rough although the line and the Wi-Fi were fine, it is
  `OUTSIDE` (the app or the far end), with low confidence: the gateway cannot
  see the service itself.
- When there is too little to go on (no timeline for that time, no fresh
  check), it is `UNKNOWN`.

### The fault, and the technician

A technician visit costs the ISP money, and a wrong one costs the customer a
day. So the rules are careful:

| Fault | Codes | Visit |
|---|---|---|
| `LINE` | `LINE_LOSS` (the next hop itself loses packets) | `Yes`, only when it lasted 30 minutes, care reported it, or the fresh check and the timeline agree; otherwise `IfPersists` or `No` |
| `LINE` | `LINE_SLOW_SPEED` | `Yes` when the nightly speed test was low on 2 nights or more; once: `IfPersists` |
| `LINE` | `LINE_DOWN`, `NO_ROUTE` | `IfPersists`: first the customer restarts the modem and checks its cable |
| `LINE` | `ISP_DOWN`, `ISP_LOSS`, `ISP_SLOW`, `DNS_FAILING`, `DNS_SLOW` | `No`: the home is fine up to the ISP; this is for the network team |
| `HOME_WIFI` | `WIFI_COVERAGE`, `WIFI_CONGESTION`, `WIFI_HOPPING`, `WIFI_RADIO`; v0.44 `MESH_BACKHAUL` (an extender's weak or slow backhaul with devices behind it, [mesh.md](mesh.md)) | `No` |
| `DEVICE` | `WIFI_DEVICE`, `WIFI_DROPS`, `DEVICE_OFFLINE`, `DEVICE_BLOCKED`, `DEVICE_LIMITED` | `No` |
| `NONE` | `OK`, `LINE_FULL` (the home used its whole line: not a fault) | `No` |
| `OUTSIDE`, `UNKNOWN` | `OUTSIDE_SERVICE`; `GATEWAY_OFF`, `NO_DATA` | `No` |

A firmware upgrade that made the home worse just before always means no
visit: the firmware, not the line, is the likely cause.

## Fixes need a yes

When a fix is possible, the brief offers it. It never applies it.

| Code | Fix |
|---|---|
| `LINE_FULL` | Smart queueing (`sqm`), if it is off and wasn't tried here already |
| `WIFI_HOPPING` | Keep the device on 2.4 GHz for a day (`stay24`) |
| `WIFI_CONGESTION`, `WIFI_COVERAGE` | A channel change, a move to a better mesh access point (`meshsteer`) or `stay24` that AIGW already proposed: approve it. Otherwise, for `WIFI_COVERAGE` (v0.46): the [coverage advice](coverage.md): which device, when, where in the home, and whether moving it, moving the gateway, or an extender about halfway would fix it |
| `MESH_BACKHAUL` | A move off the extender that AIGW already proposed: approve it; for good, the extender closer or on a cable |
| `DEVICE_BLOCKED`, `DEVICE_LIMITED` | Undo AIGW's decision, after checking its reason with the customer |

With `--propose` (agctl) or `Propose` = `true` (the data model), a new fix
becomes a **proposed decision**: it waits, like any proposal, until a person
approves it: the customer at home (`agctl approve <id>`, LuCI), or the agent
with the customer's yes on the phone (`Device.X_AIGW_.Decision.{i}.Approve()`).
A fix that already waits is pointed to, not proposed again. An undo is only
named: it is a person's own action.

## Asking

- **At home:** `agctl brief [device] [period] [--no-check] [--speed]
  [--propose]`. The period is in plain words: `now` (the default: the last 15
  minutes), `last hour`, `last 2h`, `90m`, `tonight 20:00-22:00`,
  `yesterday 19:00-23:00`, `since 19:30`. A clock range later than now is
  yesterday's. `--no-check` skips the fresh check, and `--speed` also runs the
  speed test (`care.speed_url`). `-j` prints JSON. Over ubus:
  `ubus call ag brief '{"period":"last 2h","host":"laptop"}'`.
- **The ISP, over USP:** `Device.X_AIGW_.SupportBrief()`, an asynchronous
  command (the fresh check takes a few seconds).
  - Inputs: `Period` (the words above), or `Start` and `End` (dateTime);
    `Host` (a `Device.Hosts.Host.{i}` path, MAC, IP or name; optional);
    `Fresh` (default `true`), `SpeedTest`, `Propose`.
  - Outputs: `Fault`, `Code`, `Confidence`, `Verdict`, `Evidence` and `Done`
    (one line each, local time first), `Tell`, `RemoteFix`, `Fix`,
    `FixDecision` (the decision's path, when there is one), `Visit`,
    `VisitReason`, `Checked`, `Start`, `End`, `DataAgeSeconds`.
- **The ISP, over TR-069:** the same command through
  `Device.X_AIGW_.Request.` (`Command` = `Device.X_AIGW_.SupportBrief()`).
  An ISP's own ACS gets it with no cortex.
- **prplOS:** the same command through aigw-amx.

`SupportBrief()` reads, and at most proposes, so a management system may run
it without `mgmt.actions`. One brief is made at a time; another one asked
meanwhile is told to ask again in a few seconds.

## cortex: the help desk for the whole fleet

Each bridge sends the gateway's brief of the last hour with its report (no
fresh check, so the report never waits), with device kinds unless
`brief.names` is `1`. With the bridge's `devices` not `names`, a brief with
names is not sent at all.

- `python3 -m aigw_cortex brief <home> [--period ...] [--propose] [--json]`.
  The home is the gateway's name, its id, or the serial number the ACS knows
  (`Device.DeviceInfo.SerialNumber`).
- MCP: the tool `support_brief` (`gateway`, `period`, `propose`).

cortex cannot ask the home again: the bridge reports every few minutes and
takes no requests. So cortex answers from the last report and says how old it
is (`age_s`, `freshness`). For a fresh brief, or another period, it says where
to ask: `SupportBrief()` on the gateway, over USP or TR-069. A gateway older
than 0.37 sends no brief; cortex then answers from proactive care alone, with
low confidence, and never advises a visit.

With `--propose`, cortex sends smart queueing (`sqm`) as a proposal through
the bridge, where it waits for a yes. A fix on one device (`stay24`) needs
the device's address, which stays home: it is proposed from the gateway
(`Propose` = `true`).

**Proof for the ISP.** cortex logs every brief it answers (the fault, the
visit advice). The [fleet report](fleet-report.md) counts them in its
`help_desk` section: the briefs, the fault split, how many advised a
technician, how many had a remote fix and how many were proposed. It also
counts the briefs the gateways answered themselves (over USP, TR-069 or
agctl), from each gateway's own counts.

```
  help desk    120 briefs for 97 homes: HOME_WIFI 51, NONE 38, LINE 19, DEVICE 9, UNKNOWN 3; a technician advised for 4, only if it persists for 6, not needed for 110; a remote fix for 22 (15 proposed)
```

## Privacy

The ISP sees device kinds ("a laptop", "an Android phone"), never a device's
name, MAC or address, unless the home allows it (`brief.names 1`). This is
the same rule as the [ISP signals](isp-signals.md) (`offers.share`): the
home decides what leaves. Every text in a brief (the verdict, the evidence,
what AIGW did, the fix, the visit's reason) is scrubbed of the home's device
names, MACs and IP addresses. The sites behind alerts and blocks become "a
site", and the reasons of AIGW's decisions (which can name one) stay home.
With `calls.share 0`, the calls stay out too.
agctl at home always shows the names.

## Settings (`/etc/config/aigw`, section `brief`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no briefs (agctl, the data model, the bridge) |
| `names` | `0` | `1`: the ISP's brief names devices; `0`: by kind |

## Tests

- `tests/test_brief.c` (unit): the line down now, with the timeline and care
  agreeing (a visit only if it persists after a restart); the next hop losing
  packets for 40 minutes (a visit) and for 3 minutes (none); the ISP's network
  (never a visit); the home filling its line (not a fault; smart queueing
  offered, or a waiting proposal approved, or nothing when it is on); a
  hopping laptop (`stay24`); a busy channel with a quieter one waiting; a
  device AIGW quarantined (undo, with its reason); rough calls on a fine line
  (`OUTSIDE`); all fine (`NONE`); nothing known (`UNKNOWN`); sources that
  disagree; a rolled-back upgrade (no visit); the speed low on 3 nights (a
  visit); the gateway off; a period in the past; periods in plain words; device
  kinds and names taken out.
- `tests/brief-test.sh` (CI): agd in a gateway namespace with its WAN to an
  ISP namespace, a fake hostapd, a sample every 2 s. All fine, after a fresh
  check. A weak laptop: by name at home, as "a laptop" for the ISP over the
  data model, without its name, MAC or address. The ISP fills the line: not a
  fault, and `--propose` leaves smart queueing waiting for a yes, with nothing
  shaped. The internet stops answering: `LINE` now, high confidence, no
  technician, also for an ACS. Every brief counted by fault, kept over a
  restart.
- `tests/test_cortex.py`: a home found by name, id or serial number; how old
  the answer is; another period; a fix proposed only when asked, and never
  twice; an older gateway; the MCP tool; the fleet report's counts.
- `tests/outside-test.sh` (CI): the brief reaches cortex through the bridge,
  without names, and `aigw_cortex brief` finds the home by its serial number.
