# AIGW Self-Healing Wi-Fi

"My Wi-Fi is slow" is the most common, and most expensive, call an ISP's help
desk gets. AIGW OS answers it on the gateway itself: it measures what each
device actually feels, learns what is normal in this home, names the cause,
and tells both the home and the ISP. Part 2 (next release) fixes what is safe
to fix, through AIGW's guardrails.

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

| Cause | What it looks like | What would help |
|---|---|---|
| `Congestion` | loss or delay while the signal is fine | a quieter channel |
| `WeakCoverage` | loss or delay with a weak signal (-75 dBm or weaker) | the access point or its antennas elsewhere |
| `BandHopping` | a device changing band by itself 4 times in 10 minutes with a marginal 5 GHz signal (-65 dBm or weaker); moves AIGW's band steering asked for don't count | keeping it on 2.4 GHz |
| `Device` | one device suffers while the others are fine | the device itself |
| `Radio` | no device answers at all | the radio |

The status is `Good`, `Degraded` (5 % loss, or 100 ms and 3x its usual delay)
or `Poor` (20 %, or 300 ms). A new verdict must hold for two looks (20 s), and
`Good` again for three, so one bad moment doesn't flap.

## Report it

- **To the home:** `agctl wifi` and agd's log, in plain words, e.g.
  *"2.4 GHz (channel 11): OnePlus-Nord-5 loses 6% of its traffic with a weak
  signal (-77 dBm): too far from the access point, or walls in the way."*
  (found on the PC access point, on real radios).
- **To the ISP**, over USP and TR-069 alike: `Device.X_AIGW_.WiFiHealth.{i}`,
  one per radio (`Radio` refers to `Device.WiFi.Radio.{i}`), with `Status`,
  `Cause`, `Explanation`, `LossPercent`, `LatencyMs`, `StationsMeasured`,
  `AffectedHosts` and `Since`, and the event `WiFiHealthChanged!`. A help
  desk sees the diagnosis before anyone calls.
- An `ag.wifi` event `health` for plug-ins.

## Settings (`/etc/config/aigw`, section `wifi`)

| Option | Default | |
|---|---|---|
| `heal` | `1` | measure and diagnose |
| `heal_loss` | `5` | % loss that is a problem |
| `heal_delay` | `100` | ms of delay that is a problem (and 3x a device's usual) |
| `heal_weak` | `-75` | dBm at or below which a signal is weak |
| `heal_hops` | `4` | band changes in 10 minutes that count as hopping |

## Tests

- `tests/test_heal.c` (unit): each cause, including the real case found on the
  PC access point (channel 6: 15 % loss at about -70 dBm).
- `tests/hwsim-test.sh` (simulated radios, CI): a client in use is measured
  `Good`; 30 % loss injected on its access point (`tc netem`) reads as
  `Congestion` in `agctl` and the data model, with a `WiFiHealthChanged!`
  event; without the loss, `Good` again.
