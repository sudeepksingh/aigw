# AIGW signals for the ISP: what a home would buy, or why it may leave

An ISP sells by guessing. It offers mesh Wi-Fi to whoever calls about Wi-Fi,
a faster plan to everyone, and it hears that a customer was unhappy when
they cancel. The gateway knows better. It knows which homes fill their line
every evening, whose devices spend hours on a weak signal, whose calls
stutter while someone downloads, and which homes are having a bad month. It
also knows when the line itself delivers less than it should, and then the
answer is a repair, not a sale.

AIGW OS turns what it sees over the last week into signals for the ISP,
each with its evidence and a confidence. Like the rest of AIGW, it works it
out at home. What leaves the home is counts and words, never a device's name.

```
$ agctl offers
Signals for the ISP, from the last 7 days (7 watched): 2 found
  faster plan          PLAN_UPGRADE (upsell, high confidence, 5 of 7 days)
                       The line was used to its speed on 5 of the last 7 days, 1 h 40 min a day on average, 90% of it in the evening; up to 47 of 50 Mbit/s.
  mesh Wi-Fi           MESH_WIFI (upsell, high confidence, 6 of 7 days)
                       On 6 of the last 7 days, 2 devices spent a long time on a weak Wi-Fi signal (6 h of device time a day); 2 short drops off Wi-Fi a day: the access point doesn't reach every room.
                       devices: laptop 4 h a day, tv 2 h a day (stays home)
  not found            smart queueing, a call from the ISP, a line repair
  the ISP              sees them as counts and words, without names (offers.share)
```

(From the integration test's week.)

## The signals

| Code | Kind | Product | When |
|---|---|---|---|
| `PLAN_UPGRADE` | upsell | a faster plan | the line used to its speed for 30 min or more a day, on 3 of the last 7 days. Not while `LINE_REPAIR` holds |
| `SMART_QUEUE` | upsell | smart queueing | while the line was full, the internet answered slowly (the rest of the home suffered) for 15 min or more a day, and for half the full time, on 3 of 7 days. Calls and games stay responsive with smart queueing (SQM), without a faster plan |
| `MESH_WIFI` | upsell | mesh Wi-Fi | 2 or more devices on a weak signal (`wifi.heal_weak`) for 30 min or more a day, or one and 10 short drops off Wi-Fi (back within 2 min), on 3 of 7 days |
| `RETENTION_RISK` | retention | a call from the ISP | a bad week: the experience score (proactive care) below 90%, 5 points or more below the week before, or 4 problems or more |
| `LINE_REPAIR` | repair | a line repair | 2 or more nightly speed tests far below the line's speed (`care.speed_mbps`) in the week: the ISP owes a repair, and nothing is sold on that line meanwhile |

**Confidence:**
- `PLAN_UPGRADE`:
  - **high** with the line's sold speed known (`care.speed_mbps`) and 4 days or more;
  - **medium** with only a tested speed;
  - **low** with neither, when "full" is the most the timeline has seen.
- `MESH_WIFI`: **high** with 5 days or more and 2 devices or more.
- `SMART_QUEUE`: **high** with 5 days or more.
- `RETENTION_RISK`: **high** when two of its reasons hold.
- `LINE_REPAIR`: **high** with 3 slow tests.
- Otherwise **medium**.

Nothing is said before 3 days are watched (6 hours or more of samples each).
`LINE_REPAIR` and `RETENTION_RISK` come from proactive care's own days, so
they don't wait for that.

## Where it comes from

Each sample of the home's [timeline](why-slow.md) (a minute) adds to its
day (UTC days, like proactive care's):

- the time sampled;
- how long the line was used to its speed: 80% of the sold speed, else of the
  last speed test, else of the most seen on it. Of that time, how much was in
  the evening (18:00 to midnight, local time), and how much had the internet
  answering slowly, against the home's normal for the hour;
- the busiest sample's download;
- each device's time on a weak signal (the day's 16 weakest);
- short drops: a device that left Wi-Fi and was back within 2 minutes.

31 days are kept with agd's state. The signals are judged every hour, and
when someone asks.

## Telling the ISP

- **The data model** (version 1.12):
  - `Device.X_AIGW_.Offers.`: `Enable`, `Shared`, `Signals` (the active codes, comma-separated; value change notified);
  - `PlanUpgrade.`, `SmartQueue.`, `MeshWiFi.`, `Retention.` and `LineRepair.`, each with `Active`, `Confidence`, `Days`, `Since` and `Evidence`.
  - It reaches USP, TR-069 and prplOS (aigw-amx) like the rest.
- **An event:** `Device.X_AIGW_.OfferFound!` (Code, Kind, Product, Confidence,
  Evidence), once when a signal starts. It isn't sent again while the signal
  holds, and not after a restart either. It is sent again if the signal ends
  and comes back later.
- **cortex:** the bridge sends the active signals inside proactive care's
  report. The [fleet report](fleet-report.md) lists the homes in the order a
  team works through them: repairs first, then homes at risk, then sales by
  confidence. It counts them by product:

  ```
    signals      41 homes: faster plan 18 (12 high), mesh Wi-Fi 14 (9 high), smart queueing 9, a call from the ISP 6, a line repair 3
      home-0412 (exchange-east): LINE_REPAIR, a line repair (high, 3 of 7 days): 3 of the last 7 nightly speed tests were far below the line's speed: ...
  ```

## Privacy

What leaves the home is what the table says: how often, how long, how much,
and how many devices. A device's name (`MESH_WIFI`'s "laptop 4 h a day")
stays in `agctl offers` and `ubus call ag offers`. Nothing about sites or
apps is in a signal.

Whether the ISP may use this for sales is the ISP's decision, and in many
places the customer's, so it is a setting. `offers.share 0` keeps every
signal home: the data model shows none, there is no event, and the bridge
sends none. `offers.enabled 0` turns them off entirely.

## Settings (`/etc/config/aigw`, section `offers`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no signals |
| `share` | `1` | the ISP may see them (data model, event, cortex); `0`: they stay home |

The line's speed comes from `care.speed_mbps` and the nightly speed test
([proactive care](proactive-care.md)). The weak signal comes from
`wifi.heal_weak`.

## Tests

- `tests/test_offer.c` (unit):
  - a quiet week gives nothing;
  - the line full 5 evenings gives `PLAN_UPGRADE`, its confidence depending on whether the speed was sold, tested or neither;
  - two busy evenings are no pattern;
  - bursts that slow the rest give `SMART_QUEUE`, not a faster plan;
  - two weak devices give `MESH_WIFI`, by name only at home;
  - one weak device that keeps dropping off gives `MESH_WIFI` too;
  - the day's table of weak devices keeps the weakest;
  - a bad week after a good one gives `RETENTION_RISK`, and so do many problems with a fine score;
  - an underdelivering line gives `LINE_REPAIR` and no upsell;
  - too few days are learning;
  - last week's busy evenings don't count.
- `tests/offers-test.sh` (CI): agd starts with a week of days in its state:
  - `PLAN_UPGRADE` (high) and `MESH_WIFI`, named at home only;
  - `OfferFound!` once each, and not again after a restart;
  - `Device.X_AIGW_.Offers.` without names;
  - today counted from the timeline's samples;
  - `offers.share 0`: the signals stay home.
- `tests/test_cortex.py`: the fleet report's signals section (the order, the
  counts), a signal closed when it is gone, a report without signals changing
  nothing, signals arriving inside the bridge's report.
