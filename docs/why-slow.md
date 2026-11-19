# AIGW "Why was it slow?": the home's timeline, explained afterwards

A help desk rarely gets "my internet is slow *now*". It gets "it was slow last
night at 8", and by the time someone asks, 8 o'clock is gone.
[Diagnose](diagnose.md) can only measure the present, and
[proactive care](proactive-care.md) checks every 10 minutes and only reports
faults. Neither can say why the evening was slow when nothing was broken.

AIGW OS keeps a timeline of the home and explains any stretch of it
afterwards, in plain words and with Diagnose's codes.

```
$ agctl why --at 20:00
19:30 to 20:30: The line was used to its speed for 24 min (the longest from 20:02 to 20:26): 47 of 50 Mbit/s, most of it by TV (38 Mbit/s on average). Nothing was broken: the home used all of it. Everything else was slower meanwhile: the internet answered in 140 ms, normally 18 ms.
  code      LINE_FULL (Internet, warning)
  advice    Nothing to repair: the home used the whole line. Smart queueing (SQM) keeps calls and games responsive while others download; a faster plan if it happens often.
  recorded  60 of 60 samples (one every 60 s; the timeline goes back to Sat 08:12)
  normal    internet 18 ms, next router 2 ms, DNS 10 ms; the line 50 Mbit/s
  what happened
    20:02     TV started downloading (38 Mbit/s)
    20:02     the line was full: TV, until 20:26
  findings
    warning  LINE_FULL        The line was used to its speed for 24 min ...
    info     ...
```

(The numbers are from the unit test's evening.) The help desk now knows that
the line, the ISP and the Wi-Fi were fine, and that the TV filled the line.
No truck is sent, and there is a reason to offer smart queueing or a faster
plan.

## What is kept

**A sample every minute** (`why.interval`), about 90 bytes:

| | How |
|---|---|
| The line | 3 echo requests to the default route's next router, and to the first two `diag.targets` (the best one counts): sent, lost, median delay |
| The WAN | bytes in and out of the default route's interface over the minute: how full the line was |
| DNS | lookups the gateway's DNS (agdns) answered in that minute, those that failed (SERVFAIL), their mean time; names AIGW blocked aren't counted |
| Wi-Fi | each access point: Self-Healing Wi-Fi's verdict (Good, Degraded, Poor) and cause, its probes' loss and delay, its channel, how many devices |
| The busiest devices | the two that downloaded most that minute (the traffic meter, else the flow sensor's counters) |

**What happened**, with the time and the device's name, for up to 512
events:

- a device joined or left Wi-Fi, moved to another band or access point;
  AIGW moved it (band steering)
- a device's signal turned weak (at `wifi.heal_weak`) or was fine again
- an access point's Wi-Fi health changed; its channel changed
- an AIGW decision took effect or ended (quarantine, throttle, a channel...)
- proactive care found a problem, it changed or was over; an early warning
- the nightly speed test, and its result
- a firmware upgrade activated, kept, worse or rolled back
- agd started: after the gateway itself was off (a power cut, a reboot), or
  only agd

`why.hours` (72) of it is kept, about 400 KB of memory. It is saved to
`why.file` every 10 minutes and when agd stops. On OpenWRT that file is in
`/tmp`, so nothing is written to flash, and a reboot is itself what the
timeline shows: the gap, then "The gateway started". On a PC the file is in
`/var/lib/aigw`.

## The verdict

Each sample of the stretch asked about is put in a state, and each state that
lasted at least two samples is a finding. One bad minute is a blip, not a
finding.

| Code | Area | Means |
|---|---|---|
| `GATEWAY_OFF` | All | no samples, and then the gateway started (it was off, or rebooting): no internet for anyone |
| `NO_ROUTE` | Internet | no default route |
| `LINE_DOWN` | Internet | the internet didn't answer, nor did the next router (when it answers at other times) |
| `ISP_DOWN` | Internet | the internet didn't answer while the next router did: beyond the home |
| `LINE_FULL` | Internet | the line was used to its speed (80% of `care.speed_mbps`, else of the last speed test, else of the most this timeline has seen on it), and by whom. With no speed known, the most seen is only a guess at the line's limit (a quiet home's may be far below it), so a moment near it counts as full only when the internet also slowed, as a full line's queue makes it. The delays meanwhile are the full line's, not the ISP's fault |
| `LINE_SLOW` | Internet | the next router answered slowly, and the home wasn't filling the line |
| `ISP_SLOW` | Internet | the internet answered slowly while the next router was fine and the line wasn't full |
| `LINE_LOSS`, `ISP_LOSS` | Internet | 5% or more of the echo requests lost, to the next router or only beyond it |
| `DNS_FAILING`, `DNS_SLOW` | DNS | half or more of the lookups failed while the internet answered; or they took 300 ms or more |
| `WIFI_CONGESTION`, `WIFI_COVERAGE`, ... | WiFi | an access point Degraded or Poor, with Self-Healing Wi-Fi's cause |

With a device asked about (`agctl why laptop`, `Host`), its own events count
too:

| Code | Area | Means |
|---|---|---|
| `DEVICE_BLOCKED`, `DEVICE_LIMITED` | AIGW | an AIGW decision kept it off, or slowed it, for part of the stretch |
| `DEVICE_OFFLINE` | Device | it wasn't on Wi-Fi for half the stretch or more |
| `WIFI_DROPS` | WiFi | it dropped off Wi-Fi twice or more (with when) |
| `WIFI_COVERAGE` | WiFi | its signal was weak, and for how long |
| `DEVICE_ON_24GHZ` | WiFi | it moved to 2.4 GHz (a warning when it stayed there for half the stretch) |

**Slow is the home's own.** A delay is slow at twice the normal and at least
30 ms more for the internet (20 ms more for the next router), and never under 40 ms
(20 ms). The normal is proactive care's for those hours
([early warning](proactive-care.md)), or, while it is still learning, the
median of the rest of the timeline. A satellite line that always answers in
45 ms isn't slow at 60. A fibre line that answers in 8 ms is slow at 50.
Without any normal, Diagnose's 150 ms and 100 ms apply.

**The verdict** is the finding that explains most of the stretch. Its length
counts three times when nothing worked (no internet, the gateway off, the
device kept off), twice for a problem and once for a warning. Ties go in
Diagnose's order. So 10 minutes without internet outweigh 15 slow ones, and
24 minutes of a full line outweigh 3 minutes of a slow DNS. When nothing was
wrong, the verdict is `OK`, with what was measured (`INTERNET_OK`, `DNS_OK`,
`WIFI_OK`). When there are no samples at all, it is `NO_DATA`.

**Changes AIGW made are named.** When a problem started within 5 minutes
after AIGW changed something on Wi-Fi (a channel, a band move, a device kept
on 2.4 GHz), its finding says so: "It started 2 min after a change at 20:00:
5 GHz (wlan1) moved from channel 36 to 44." Every change in the stretch is
listed as `AIGW_CHANGED`. An AI that changes a network has to show what it
did, so that its own changes can be blamed when they are the cause.

**The story** puts the findings and what happened in order of time, at most
16 lines: each finding's longest stretch, the events (for one device, only its
own joins, leaves and signal), and a device starting a big download (a
quarter of the line, at least 5 Mbit/s).

The rules are pure and unit-tested (`core/libag/why.c`,
`tests/test_why.c`).

## Asking

- **At home:** `agctl why [device] [--at 20:00 | --from 19:30 --to 21:00 |
  --last 2h] [--raw]`. `--at` is half an hour either side of a time (the
  last time it was 20:00, or "yesterday 20:00"). Without a time, the last
  hour. `--raw` adds the samples and the events, and `-j` prints JSON. Over
  ubus: `ubus call ag why '{"from":..,"to":..,"host":"laptop"}'` (wall
  seconds, or `minutes`).
- **The ISP, over USP:** `Device.X_AIGW_.WhySlow()`, a synchronous command
  (everything is in memory). Inputs: `Start`, `End` (dateTime; default the
  last hour), `Host` (a `Device.Hosts.Host.{i}` path, MAC, IP or name;
  optional). Outputs: `Code`, `Area`, `Severity`, `Summary`, `Advice`,
  `Findings` (every code, comma-separated), `Story` (one line per event,
  local time first), `Start`, `End`, `RecordedPercent`. The stretch is at
  most 24 hours.
- **The ISP, over TR-069:** the same command through `Device.X_AIGW_.Request.`
  (`Command` = `Device.X_AIGW_.WhySlow()`, `Start`, `End`, `Host`).
- **prplOS:** the same command through aigw-amx.
- **What is kept:** `Device.X_AIGW_.Timeline.` (`Enable`, `Hours`,
  `RecordedSince`, `Samples`, `Events`).

`WhySlow()` only reads: it changes nothing, so a management system may run it
without `mgmt.actions`.

## Settings (`/etc/config/aigw`, section `why`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no timeline |
| `hours` | `72` | how far back it goes (1 to 168); about 90 KB of memory an hour |
| `probe` | `1` | each sample sends 3 echo requests to the next router and the first two `diag.targets` (about 9 packets a minute); `0`: the line isn't sampled |
| `file` | `/tmp/aigw/agd-why.bin` (OpenWRT), `/var/lib/aigw/agd-why.bin` | where it is kept over a restart of agd; `''`: not kept |
| `interval` | `60` | seconds between samples (2 to 600); shorter is for tests |

## Tests

- `tests/test_why.c` (unit): an ordinary evening is `OK`; the TV filling the
  line is `LINE_FULL`, named, with the rest's delay, and not the ISP's fault;
  the ISP slow; the line slow; one slow minute a blip; the internet down
  outweighing a longer slow stretch, and down with the next router silent
  too; the gateway off (a power cut) against only agd restarted; DNS failing;
  a poor access point named with its cause; a laptop on 2.4 GHz with a weak
  signal that dropped off twice; a device AIGW kept off; a channel change
  just before a problem, told with it; the normal from the timeline; slow
  judged against the home's own normal; the line's speed unknown; no
  timeline, and a stretch half before it starts; packet loss.
- `tests/why-test.sh` (CI): agd keeps the timeline with a fake hostapd (a
  sample every 2 s). The line is sampled. A laptop leaves and comes back with
  a weak signal: asked about it, the verdict is its weak signal, with its
  story in order, while the home's story leaves it out. The timeline is kept
  over a restart of agd, with the restart in it. The internet stops
  answering: the stretch is the internet down. The same over the data model
  (`WhySlow()`, `Timeline.`).
