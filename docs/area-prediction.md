# Area fault prediction

A street cabinet, an OLT port or a feeder rarely fails at once. A joint corrodes, water gets
into a cable, a card overheats, and for days the lines it serves get a little worse: the noise
margin slides, CRC errors climb, the sync rate drops, retrains start. No customer calls yet.
Each gateway sees only its own line, and a middleware stack (TR-069, USP, prplOS or RDK-B)
only exposes the counters. Someone still has to notice that 14 lines on the same cabinet are
going the same way in the same week.

AIGW OS does that, in two parts:

- **On the gateway** (small, in C): each of the line's numbers, day by day, with a robust
  trend, a change point, the days until it is a fault at that rate, and a confidence.
  `agctl line`, `Device.X_AIGW_.LineHealth.`, `LineDegrading!`. A compact, privacy-safe line
  health vector goes to the ISP's side with the bridge's report.
- **On the ISP's side** (cortex, the reference implementation in Python): the vectors of the
  homes on one cabinet, OLT or area are compared. Many homes degrading on the same number, from
  the same days, far more than chance would explain, is the shared plant. One home on its own is
  its line. The answer: *"The plant serving 14 of 20 homes on olt-3/cab-12 is degrading: the
  noise margin worse in all of them since about 2026-11-20; first calls likely in about 3 days;
  confidence high. One ticket for the plant, not 14."* One TMF621 ticket goes to the ISP for the
  plant, and the homes' own line tickets it explains are not sent.

It is the `AREAPREDICT` feature (it needs `CARE`). It only watches, predicts and tells: it
never changes anything in the home, and agd's act mode is not involved.

## On the gateway

```
the WAN handler   every line.interval: the line's own numbers, as TR-181 has them
  │               (line.handler, else /usr/libexec/aigw/line-stats)
proactive care    each regular check's next-hop delay and loss; each speed test
  ▼
a value a day     per metric: a gauge's median, a counter's rate an hour, a count a day
  ▼
once a day        each metric's trend over the last 14 complete days
  ▼
LineHealth.  LineDegrading! / LineDegradingCleared!  agctl line  the bridge's vector
```

### The metrics

| Metric | Key | From | Worse when | A fault at |
|---|---|---|---|---|
| `SyncDown` | `sync` | handler `down_kbps` (`Device.DSL.Line.{i}.DownstreamCurrRate`) | lower | 70% of its base |
| `NoiseMargin` | `margin` | handler `margin_db` (`DownstreamNoiseMargin`; a cable modem's downstream SNR) | lower | 3 dB |
| `OpticalPower` | `optical` | handler `rx_dbm` (`Device.Optical.Interface.{i}.OpticalSignalLevel`) | lower | -27 dBm |
| `FECErrors` | `fec` | handler `fec` counter (`Stats.Total.FECErrors`; corrected codewords) | higher | 10 x base + 1000 an hour |
| `CRCErrors` | `crc` | handler `crc` counter (`Stats.Total.CRCErrors`; uncorrectable codewords) | higher | 5 x base + 60 an hour |
| `Drops` | `drops` | handler `resyncs`, else `uptime_s` going back, else the WAN's carrier changes | higher | base + 4 a day |
| `NextHopLatency` | `latency` | proactive care's check: the next hop's ping | higher | base + 30 ms |
| `NextHopLoss` | `loss` | proactive care's check: the next hop's ping | higher | base + 2% |
| `DownloadSpeed` | `speed` | the speed tests (the nightly one, the ISP's, a person's) | lower | care.speed_mbps x speed_min_pct, else 70% of its base |

A line that has always been past a fixed fault level (a long DSL line whose margin is 2.5 dB)
is judged by its own normal only: it has no fault level, and never "a fault now".

### The handler

A vendor-neutral program that prints one JSON line. Every key is optional; counters are totals
(agd takes the differences, and a counter that goes back is a reset):

```json
{"tech": "dsl", "down_kbps": 99800, "up_kbps": 39900, "margin_db": 9.4, "fec": 1234, "crc": 12, "uptime_s": 86400}
{"tech": "pon", "rx_dbm": -19.2}
{"tech": "docsis", "margin_db": 38.5, "fec": 120, "crc": 3, "resyncs": 2}
{"tech": "ethernet"}
```

The platform's `line-stats` (OpenWRT) reads OpenWRT's DSL daemon (`ubus call dsl metrics`) and
an SFP module or ONT stick on the WAN port (`ethtool -m`). Anything else (a cable modem, an ONT
with its own management) is the OEM's handler: `line.handler`. With no line numbers at all
(Ethernet to a modem that says nothing), the gateway still has the next hop, the speed tests and
the WAN's carrier drops.

### The rules (core/libag/line.c, ag/line.h)

Of the last 14 complete days of a metric (today is never judged: it isn't over):

| Step | How |
|---|---|
| noise | the median of the day-to-day differences (robust: one step is one difference), at least a quarter of the change worth saying |
| change point | the split where the median after is furthest above the median before, in noise units x sqrt(i(k-i)/k), each side 3 days or more, and 75% of each side's days on its side (two bad days at the end are not a step); 4 or more |
| trend | a Theil-Sen slope (the median of every pair's slope: one bad day can't tilt it) on the days after the change point, else on all of them; if that is too short to prove, starting a day or more earlier (a ramp's median split comes late) |
| proven | Kendall's tau 0.5 or more, one-sided p < 0.01 (Kendall's test), and the slope over the days at least the change worth saying |
| level, base | where the trend line is on the last day; the median before it began |
| onset | a drift: where its line leaves the base; a step: its day |
| days left | (fault - level) / slope, rounded up; 0 there already; -1 not heading there, or beyond 30 days |
| confidence | a drift: 100 x tau x min(1, days / 10); a step: 100 x min(1, strength / 8) x min(1, days / 10); a fault: 60 at least |
| state | Fault, else Drifting, else Stepped, else OK; Learning with fewer than 5 days |
| early sign | the last 7 days alone, proven or not: their tau and slope. Weak for one home; cortex pools them |

The line's status is its worst metric with medium confidence (40) or more, or a fault: by
state, then the fewest days left, then the confidence. `LineDegrading!` is sent when the status
becomes Stepped, Drifting or Fault, or its worst metric changes; `LineDegradingCleared!` when it
is OK again. The trends change once a day, so the status doesn't flap.

### What it says

```
$ agctl line
Line: Drifting (dsl)
  The noise margin fell from 9.8 dB to 6 dB, 0.41 dB a day: at this rate a fault (3 dB) in about 8 days.
  fault     in about 8 days at this rate (high confidence)
  cortex    The plant serving 14 of 20 homes on olt-3/cab-12 is degrading: ... ISP ticket: aigw:1f2e3d4c:plant:7

  metric          state            now     normal       a day  days  fault in
  SyncDown        OK             99800      99800           -    14  -  (kbit/s)
  NoiseMargin     Drifting         6.1        9.8       -0.41    14  8 days  (dB)
      -> The noise margin fell from 9.8 dB to 6 dB, 0.41 dB a day: at this rate a fault (3 dB) in about 8 days.
  CRCErrors       Drifting          41        2.1        +4.6    14  7 days  (/h)
  Drops           OK                 0          0           -    14  -  (/day)
  NextHopLatency  OK               4.2        4.1           -    14  -  (ms)
```

`agctl line --look` looks at the line now; `agctl -j line` is the same as JSON.

### Settings (`config line 'line'`)

| Option | Default | What |
|---|---|---|
| `enabled` | `1` | learn the line's trends |
| `interval` | `900` | s between looks at the line's own numbers |
| `share` | `1` | the bridge sends the line health vector to cortex |
| `handler` | (the platform's `line-stats`) | the program that prints the WAN's numbers |

The bridge's own setting `line` (default `1`) sends the vector and passes cortex's answer on;
the answer needs the bridge's capability `line.notice`.

### Data model 1.23

`Device.X_AIGW_.LineHealth.`

| Parameter | Type | What |
|---|---|---|
| `Enable`, `Share` | boolean | line.enabled, line.share |
| `Technology` | string | dsl, pon, docsis, ethernet, unknown (the handler's) |
| `Status` | string | Learning, OK, Stepped, Drifting, Fault (notified) |
| `WorstMetric` | string | e.g. NoiseMargin; "" when OK |
| `Summary` | string | the worst metric's trend in words |
| `DaysToFault` | int | at this rate; 0 now; -1 not known |
| `Confidence` | unsignedInt | 0-100 |
| `Since` | dateTime | the status began |
| `LastLook`, `Error` | dateTime, string | the handler's last look, and what went wrong |
| `SharedPlant` | string | cortex: the node whose plant degrades with this line ("" none; notified) |
| `SharedPlantHomes`, `SharedPlantDaysToCalls`, `SharedPlantConfidence`, `SharedPlantTicket`, `SharedPlantSummary` | | cortex's prediction: homes, first calls in days, confidence, the ISP ticket's externalId, in words |
| `MetricNumberOfEntries` | unsignedInt | |
| `Metric.{i}.Name`, `Unit`, `State`, `Now`, `Level`, `Base`, `SlopePerDay`, `FaultLevel`, `Onset`, `DaysToFault`, `Confidence`, `Days`, `Summary` | | each metric with data (instance = its number in the table above, from 1) |

Events: `Device.X_AIGW_.LineDegrading!` (Metric, State, Summary, DaysToFault, Confidence,
Technology, Since) and `Device.X_AIGW_.LineDegradingCleared!` (Metric, Since, DurationSeconds).

### The line health vector (what leaves the home)

With line.share 1, the bridge's report carries:

```json
"line": {"v": 1, "tech": "dsl", "day": 20412, "status": "Drifting", "worst": "margin", "days_left": 8, "conf": 82,
         "metrics": [{"metric": "margin", "state": "Drifting", "days": 14, "level": 6.1, "base": 9.8,
                      "slope": -0.41, "fault": 3, "onset": 20403, "days_left": 8, "conf": 82,
                      "tau7": 0.9, "slope7": -0.43}, ...]}
```

The line's physics only, rounded to three significant digits: no device, no address, no
traffic, no name. The day numbers are local days since 1970.

## On the ISP's side (cortex)

```
python3 -m aigw_cortex predict [--json]                 the plants degrading, ranked; lines on their own
python3 -m aigw_cortex predict settings [--min-homes N --share F --align D --alpha P]
python3 -m aigw_cortex predict simulate [--seeds 20 --homes 60 --cabinets 3] [--json]
python3 -m aigw_cortex predict backtest [--json]
MCP tool plant_predictions; the dashboard's "Faults coming"; GET /api/predictions
```

Cortex keeps each home's vector of each day (45 days). Homes are grouped by what serves them,
the area outages' key ([area-outages.md](area-outages.md)): the operator's group (`enroll
--group`, `group`), else the id the ACS wrote on the gateway (`Device.X_AIGW_.AreaOutage.UplinkID`),
else the WAN's next hop. A `/` in it is a hierarchy: `olt-3/cab-12` is cabinet 12 under OLT 3.

### The rules (areapredict.correlate(), pure)

Node by node, the most specific first:

1. A home **degrades** on a metric when its trend is Stepped, Drifting or Fault with medium
   confidence or more (a Fault always). It shows an **early sign** when its last 7 days slide
   worse steadily (tau 0.6 or more, by at least the change worth saying), proven or not.
2. **Together**: the node's homes that degrade or show an early sign on the same metric, at
   least one of them proven, beginning within `align` (3) days of their median; at least
   max(`min_homes` 3, `share` 15% of the node's homes).
3. **Not chance**: how often homes elsewhere in the fleet show that metric (a base rate, 0.5%
   at least; 5% with fewer than 10 homes elsewhere), and how likely it is that this many of the
   node's homes would, by chance, within a window that narrow (x 7/14 days): a binomial tail, times
   the metrics tried. At most `alpha` (0.001): the shared plant.
4. **Which plant**: a cluster found at a parent with 80% of its homes in one child is that
   child's. Two or more children with plants on the same metric from the same days: their parent
   (the OLT, the feeder) instead. A cabinet with a plant of its own explains its homes' other signs:
   they don't join a search higher up.
5. An open plant keeps its homes while they show it, and its onset: a newcomer must have begun
   within `align` days of the plant's own onset.
6. The rest degrade **on their own**: this home's line. While other homes of its node show the
   same signs, its ticket waits (it may be the plant: one ticket then), unless it is a fault.

What it says of a plant:

| | |
|---|---|
| first calls | when the second home of it would reach the fault level (one home is a call; two is the start of many); a home that knows no rate of its own uses the plant's homes' median rate; 0 when one is there already |
| confidence | high with a tail of 1e-6 or less, 5 homes or more, and the homes' own trends 60 sure on average; else medium |
| rank | homes x urgency (1 for calls within a day, falling to 0.1 at 14 days; 0.3 when not known) x confidence (1 high, 0.7 medium) |
| outcome | when it is no longer found: `recovered` (its lines are steady: fixed in time, calls likely avoided) or `failed` (a home reached the fault level, or had a line problem after the warning: the lead time is from the warning to that) |

### Tickets (TMF621, tickets.py)

| Kind | Events | TroubleTicket |
|---|---|---|
| `plant` | `plant.opened`, `plant.updated`, `plant.resolved` (in the default events) | ticketType `PredictedPlantFault`; severity `Major` (high confidence) or `Minor`; relatedEntity the plant (role `plant`) and each home's gateway; notes: the prediction, the evidence (homes, metric, since, the chance tail), the next step (inspect the shared plant, no visits to the homes), and how many of the homes' tickets it explains |
| `line` | `line.*` (add `"line"` to events) | ticketType `PredictedLineFault`; a home's line degrading on its own: a fault, a drift with 14 days or fewer left, or a sure step; resolved when it is steady again or a plant explains it |

While a plant is open, its homes' own line tickets are not sent: proactive care's tickets on the
line (fault `LINE`, or a line code: `LINE_*`, `ISP_*`) and cortex's `line` tickets. The plant's
ticket counts them (`explains`). The externalId is `aigw:<this cortex>:plant:<number>`, and the
gateway shows it (`LineHealth.SharedPlantTicket`).

### Elsewhere

- **Proactive care, calls likely avoided**: the fleet report and the dashboard count the plants
  fixed before they failed, and their homes as calls likely avoided; and how many days before the
  first fault the warnings came.
- **Offers**: no faster plan or smart queueing is sold on a degrading line. On the gateway, the
  line's words make `LINE_REPAIR` ("The line degrades day by day: ... It needs a repair before
  anything is sold on it."), which holds `PLAN_UPGRADE`; in cortex's fleet report, `PLAN_UPGRADE`
  and `SMART_QUEUE` of a home in a plant or with a line of its own degrading are held.
- **Area outages**: a plant that fails becomes an area outage ([area-outages.md](area-outages.md));
  its prediction's outcome is `failed`, with how many days ahead it warned.

## How good is it

`predict simulate` runs simulated fleets with known faults: 60 homes on 3 cabinets under one
OLT, a month each; noise (each line its own normal and wobble, a bad day in 20, one home in 10 on
Ethernet, one in 15 much noisier); a plant fault in cabinet 1 (70-100% of its homes' noise margin
sliding 0.2-0.45 dB a day from about the same day, CRC errors rising with it); a plant fault in
cabinet 3 half the runs (CRC errors climbing 4-9 an hour a day); and 3-6 homes with a fault of
their own anywhere. Every simulated day the gateways' vectors go through the same rules. On 20
runs (`predict simulate --seeds 20`, this version):

| | |
|---|---|
| plant faults found (recall) | 100% (32 of 32) |
| plants named that were real (precision) | 100%; false plants 0 a run |
| how early | a median 5 days before the first home's fault (the first call), 2 at worst; 100% before the first call |
| homes put in a plant that were in it (precision) | 98% |
| plant homes named (recall) | 100% |
| homes' own faults seen as their own line | 99% |
| homes' own faults wrongly put in a plant | 12%, nearly all homes on the faulty cabinet whose own fault began later with the same signs |
| look-alikes (a home's own fault on the faulty cabinet, same signs, within 5 days of it) | all put in the plant: they can't be told apart |
| ISP tickets | 117 with prediction, against 606 (one per degrading home) without |

The simulator is ours: its faults are as fast as real plant faults usually are (one to three
weeks from the first sign to a fault). A fault faster than about 5 days from its first sign to a
customer's call can't be warned of days ahead: a home needs 4 to 7 days of a slide to show it at
all. `predict backtest` replays this cortex's own kept vectors and says, for each plant it would
have found, how many days before its homes' first line problem: the real lead time, on your
network.

### What can't be told apart

- **A home's own fault on a faulty cabinet**, beginning within days of the plant's, with the same
  signs: it is put in the plant. The plant's repair finds it (or doesn't, and the home is a line
  of its own again).
- **A plant fault that touches one or two homes** (one pair in a bundle): it is their lines, never
  a plant: too few homes to tell from chance.
- **A small node** (fewer than 3 homes reporting): never a plant.
- **The same cause on many cabinets**: rain on aerial drops, crosstalk from new VDSL lines, a
  heat wave. It is shared, and found as the parent (or each cabinet), but it may not be a fault to
  repair.
- **Next-hop delay and loss alone** (Ethernet, a fibre ONT that says nothing): a busy ISP network
  and a degrading plant look alike; the line's own numbers tell them apart, so a handler that
  reads them matters.
- **A busy fleet**: when many homes elsewhere degrade on a metric, more homes of a node must do so
  together before it counts (the base rate rises): fewer, later plants.

## Tests

- `tests/test_line.c` (ctest `line_health`): steady, a drift (slope, days to the fault, onset), a
  step, one bad day, a fault, learning, getting better, a sync rate's relative fault, a long line's
  own normal, a noisy drift (less sure), the days left out, drops.
- `tests/test_warn.c` (`early_warning`): the speed collapsing to about 1 Mbit/s: the warning says 0
  or more, never "-1 Mbit/s" (fixed in this version, below).
- `tests/test_areapredict.py` (ctest `area_prediction`): the same cases through cortex's copy;
  300 random series through both the C rules (`tests/line_dump`) and the Python ones, the same
  state and days left; correlation (a cabinet is the plant; early signs pool; one home is its line;
  two homes are too few and wait; not aligned is not a plant; two cabinets are their OLT; a busy
  fleet needs more); the 60-home simulation (recall, precision, lead, tickets); cortex end to end
  (reports, the plant kept, the gateway's notice, one TMF621 ticket for the plant, the members'
  line tickets suppressed and counted, a home's own line ticket, the lead time, the back-test, the
  fleet report).
- `tests/line-test.sh` (CI): a gateway with a fake DSL line on a test clock (a day is 2 s):
  learning, drifting (agctl line, LineHealth., Metric.{i}, LineDegrading! once, the vector at
  cortex), five more homes on the cabinet and the plant predicted, cortex's word shown on the
  gateway, LINE_REPAIR, a restart (the days kept, no second event), line.share 0, and a care state
  saved by 0.52 with the "-1 Mbit/s" warning read back.

## The "-1 Mbit/s" early warning (fixed)

Before 0.53 a shift warning said what the value was now as the normal less how much worse it had
been lately (care_learn.c `candidate()`), but the normal had learned the bad value meanwhile: a
speed that fell to about 1 Mbit/s could read "The download speed is -1 Mbit/s". Now each metric
keeps the value itself lately (smoothed like before) and the warning says that. A state saved by
0.52 or older has no such value: it is taken as the normal and how much worse, never below 0, and
an open warning that said less than 0 says that instead. Older versions reading a newer state
ignore the extra number.

## Flash

The feature is in the **full** and **small** profiles (the gateway side is small, needs only
`CARE`, and is what an ISP needs most: a failing cabinet found before the calls), not in
**minimal**. See [features.md](features.md) for the sizes.

## Limits

- One value a day: a fault is seen after 4 to 7 days of it, never within hours (area outages
  and proactive care do that).
- Today is never judged, and today's samples are not kept over a restart (the completed days are).
- The handler's numbers are only as good as the modem's: a modem that resets its counters at every
  read, or reports a margin it doesn't measure, gives the trends nothing.
- Cortex correlates all fresh vectors when one changes (once a day a home); a fleet of many
  thousands of homes on one cortex wants that done per node (the rules are per node already).
- The fault levels are typical ones (3 dB of margin, -27 dBm of light); an ISP with other limits
  sets the speed's through care.speed_mbps, and the others in a later version.
