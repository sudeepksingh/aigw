# AIGW Self-Healing Wi-Fi

"My Wi-Fi is slow" is the most common, and most expensive, call an ISP's help
desk gets. AIGW OS answers it on the gateway itself: it measures what each
device actually feels, learns what is normal in this home, names the cause,
tells both the home and the ISP, and fixes what is safe to fix, through
AIGW's guardrails, checking afterwards that the fix really helped.

## Measure what devices feel, not what the radio says

Once a second, agd sends a small probe (ICMP) to each Wi-Fi device that is in
use, and keeps the last two minutes: how many got no answer (loss) and how
long the answers took (delay). Together with each device's signal and how
often it changes band, that is enough to tell most problems apart.

- **Only devices in use count.** A phone asleep in a pocket answers late, or
  not at all, on a perfect link. A device counts when hostapd saw it active in
  the last 300 ms, or it passes traffic beyond the probes themselves.
- **Any radio works.** The existing channel self-healing needs the radio to
  report how busy its channel is; many don't (the RTL8821CU in the PC
  access point doesn't). This works from what devices feel.
- **Each device's normal is learned:** its usual delay while it is well, so a
  printer that is always slow isn't flagged.

## Name the cause

Every 10 seconds, for each access point (`core/libag/heal.c`, unit-tested in
`tests/test_heal.c`):

| Cause | What it looks like | What AIGW does |
|---|---|---|
| `Congestion` | loss or delay while the signal is fine | a measured channel trial (below) |
| `WeakCoverage` | loss or delay with a weak signal (-75 dBm or weaker) | advice: alert `wifi-coverage` (move the access point or its antennas) |
| `BandHopping` | a device changing band by itself 4 times in 10 minutes with a marginal 5 GHz signal (-65 dBm or weaker); moves AIGW's band steering asked for don't count | keeps it on 2.4 GHz for a day (below) |
| `Device` | one device suffers while the others are fine | advice: alert `wifi-device` |
| `Radio` | no device answers at all | alert `wifi-radio` (warning) |

The status is `Good`, `Degraded` (5 % loss, or 100 ms and 3x its usual delay)
or `Poor` (20 %, or 300 ms). A new verdict must hold for two looks (20 s), and
`Good` again for three, so one bad moment doesn't flap.

**A steady verdict** (v0.26). On a real phone at the edge of coverage, the
signal wanders around -75 dBm, and the cause used to flip between
`Congestion` and `WeakCoverage` every minute or two, which also kept a channel
trial from ever starting. Now:

- A device's signal counts as weak at -75 dBm or below, and as fine again only
  above -72 (`heal_weak_margin`, 3 dB).
- While there is a problem, a different cause must hold for a minute (six
  looks) before the verdict changes. `Degraded` and `Poor` don't count as a
  change of cause.
- A channel trial waits for the *cause* to hold for `heal_hold`, whether the
  status reads `Degraded` or `Poor` meanwhile.

## Fix it, and check the fix

Every action goes through the same guardrails as the rest of AIGW: `act.mode`
(`advise` proposes and waits for a person, `enforce` acts on its own), the
policies, the hourly limit on automatic actions, a time limit on every action,
and undo (`agctl undo <id>`).

**A channel trial, for `Congestion`.** When `Congestion` has held for
`heal_hold` (5 minutes), agd raises alert `wifi-congestion` for another
channel the radio may use here (not one that failed in the last day). The
`wifi_congestion` policy makes it a `channel` decision. Once the switch is in
effect, agd measures the new channel for `heal_trial` (3 minutes), counting
only probes sent after the switch (and not the first 20 s, while devices
reconnect), and compares with what devices felt before:

- **Clearly better** (`Good`, or half the loss, or half the delay, and never
  more loss): the decision becomes `kept`. It no longer switches back at its
  time limit, and the channel is saved in the Wi-Fi config through the Wi-Fi
  handler without restarting anything (`AIGW_WIFI_SAVE_ONLY=1`).
- **Not better:** agd switches back, and the decision becomes `tried` (not
  `reverted`, so cortex doesn't learn "a person said no"). That channel is
  avoided for a day, and the next trial waits `heal_retry` (30 minutes).

**A device kept on 2.4 GHz, for `BandHopping`.** Alert `wifi-hopping` (once a
day per device). The `wifi_hopping` policy makes it a `stay24` decision for a
day: the 5 GHz access points refuse the device (it is disconnected there, and
joins 2.4 GHz), band steering leaves it alone, and the 2.4 GHz access points
still take it. A person can do the same: `agctl do stay24 <device> 1d`.

An ISP or OEM changes any of this with its own `policy` sections (another
action, a weaker mode, a zone) or turns acting off with `heal_act 0`.

## Report it

- **To the home:** `agctl wifi`, `agctl decisions` and agd's log, in plain
  words, e.g. *"2.4 GHz (channel 11): OnePlus-Nord-5 loses 6% of its traffic
  with a weak signal (-77 dBm): too far from the access point, or walls in the
  way."* (found on the PC access point, on real radios), or *"Channel 6 -> 1
  worked (devices lose 0%, before 15%): kept and saved."*
- **To the ISP**, over USP and TR-069 alike: `Device.X_AIGW_.WiFiHealth.{i}`,
  one per radio (`Radio` refers to `Device.WiFi.Radio.{i}`), with `Status`,
  `Cause`, `Explanation`, `LossPercent`, `LatencyMs`, `StationsMeasured`,
  `AffectedHosts` and `Since`, and the event `WiFiHealthChanged!`; the alerts
  and decisions (states `kept` and `tried` included) in
  `Device.X_AIGW_.Alert.{i}` and `Device.X_AIGW_.Decision.{i}`. A help desk
  sees the diagnosis, and what was done about it, before anyone calls.
- `ag.wifi` events for plug-ins: `health`, `trial-kept`, `trial-tried`.

## The healing report: what an ISP measures it by

Per access point and day, kept for 31 days in agd's state (it survives
restarts), summed over the last day, 7 days and 30 days:

| | |
|---|---|
| Time | seconds `Good`, `Degraded`, `Poor`, and % of the measured time that was `Good` |
| Found | problems found (a new verdict that isn't `Good`), in total and per cause |
| Fixed | channel trials kept and switched back, devices kept on 2.4 GHz, fixes that stayed |
| Time to fix | on average, seconds from the problem's start to the fix |
| Difference made | for kept channels, average loss before and after (and delay, in `agctl`) |

- **The home:** `agctl wifi --report`, e.g.

  ```
  wlan1  2.4 GHz, channel 1
    last day     Good 93% of 23h 10m (Degraded 1h 20m, Poor 15m)
                 found 3 problems (Congestion 2, BandHopping 1)
                 fixed 2: channel trials kept 1, switched back 1; devices kept on 2.4 GHz 1; 9m from problem to fix on average
                 kept channels: loss 15% -> 1%, delay 180 -> 12 ms
  ```

- **The ISP**, over USP and TR-069: `Device.X_AIGW_.WiFiHealth.{i}.Stats.Day.`,
  `.Week.` and `.Month.` (`GoodSeconds`, `DegradedSeconds`, `PoorSeconds`,
  `GoodPercent`, `ProblemsFound`, `CongestionFound`, `WeakCoverageFound`,
  `BandHoppingFound`, `DeviceFound`, `RadioFound`, `ChannelTrialsKept`,
  `ChannelTrialsTried`, `BandRules`, `Fixes`, `MeanTimeToFix`,
  `LossBeforePercent`, `LossAfterPercent`). Collected across homes, it shows
  how many homes had Wi-Fi problems, how many AIGW fixed by itself, and by how
  much: the numbers a help desk is measured by.
- `ag.wifi radios`, per access point: `health.report.day|week|month`.

## Settings (`/etc/config/aigw`, section `wifi`)

| Option | Default | |
|---|---|---|
| `heal` | `1` | measure and diagnose |
| `heal_loss` | `5` | % loss that is a problem |
| `heal_delay` | `100` | ms of delay that is a problem (and 3x a device's usual) |
| `heal_weak` | `-75` | dBm at or below which a signal is weak |
| `heal_hops` | `4` | band changes in 10 minutes that count as hopping |
| `heal_weak_margin` | `3` | dB above `heal_weak` before a weak signal counts as fine again |
| `heal_act` | `1` | act on it (channel trials, `wifi-hopping`), through `act.mode` |
| `heal_hold` | `300` | seconds `Congestion` must hold before a channel trial |
| `heal_trial` | `180` | seconds a trial is measured |
| `heal_retry` | `1800` | seconds between trials on one access point |

## For whoever ships the gateway

The Wi-Fi handler (`wifi.handler`) gets `AIGW_WIFI_SAVE_ONLY=1` when a value
is already in effect and only needs saving (a kept channel). The PC handler
writes `pc-ap.conf`, the OpenWRT one `/etc/config/wireless` without
`wifi reload`; an OEM's own handler should do the same.

## Tests

- `tests/test_heal.c` (unit): each cause, including the real case found on the
  PC access point (channel 6: 15 % loss at about -70 dBm); a signal wandering
  between -76 and -71 dBm changes the cause only twice with the margin; the
  report's day, week and month sums, and old days leaving the 31-day ring.
- `tests/hwsim-test.sh` (simulated radios, CI): a client in use is measured
  `Good`; `stay24` moves it to 2.4 GHz and only the 5 GHz access point refuses
  it; 30 % loss injected on its access point (`tc netem`) reads as
  `Congestion` in `agctl` and the data model, with a `WiFiHealthChanged!`
  event; in enforce mode, a channel trial starts, the loss goes away, the
  trial is `kept` and the channel saved in `pc-ap.conf`; `Good` again; with
  the loss back, a second trial on another channel doesn't help and is
  switched back (`tried`); `agctl wifi --report` and `Stats.Day` count the
  trials kept and switched back, the device kept on 2.4 GHz, with the loss
  before and after and the time to fix, also after an agd restart; undoing
  `stay24` lifts the refusal.
