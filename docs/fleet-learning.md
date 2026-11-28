# Fleet learning: every home teaches the next

A gateway on its own learns slowly. It sees one home's faults, tries a fix,
and needs weeks before it knows what works there. An ISP has thousands of
homes like it. Fleet learning lets each gateway use what happened in the
others:

- **Choosing a fix.** When proactive care finds a fault it could fix itself
  (a device hopping between the bands, a full line), the gateway looks at how
  that fix did in homes like this one, and how often the fault cleared by
  itself. When waiting clears it clearly more often, the gateway waits first
  and tries the fix only if the fault is still there after the grace time.
  As the home's own outcomes add up, they count for more than the fleet's.
- **Warnings from day one.** Early warning normally waits `care.learn_days`
  (3 days) before it knows the home's normal. With the fleet's normal for
  homes on the same kind of line, a new gateway warns from its first day.
  Those warnings say so: *"The normal is from the fleet (homes like this
  one): this gateway is still learning its own."*

The gateway decides and explains, on the gateway. The fleet's numbers never
make it act by themselves: a fix still goes through `act.mode` (advise by
default), the policies and the hourly limit, like any other.

It extends [federated learning](ARCHITECTURE.md) (`FED`): the same bridge, the
same cortex, the same signed-update path, and only coarse numbers leave the
home.

## How it works

```
  outcome ───── a fix (or waiting) for a fault, judged:
     │          act.c       a trial kept (helped), tried (no change, or worse),
     │                      undone by a person (undone)
     │          proactive   fixed by AIGW, cleared by itself (waiting helped),
     │                      still there after the grace time (no change)
     │          (proven self-repair's receipts, when built: the same interface)
     ▼
  cells ─────── counts per fault x action x context bucket, kept in agd's state
     │
  summary ───── only with fleet.share 1 (opt-in): each count capped and noised,
     │          and the learned normal's shape per hour band
     ▼
  bridge ────── POST /v1/fleet/summary (capability fleet.share)
     ▼
  cortex ────── the reference aggregator (cortex/aigw_cortex/fleetlearn.py) adds the
     │          homes up; a bucket fewer than k homes add up to is left out
     ▼
  priors ────── "fleet-priors", signed with cortex's key, through /v1/models
     │
  gateway ───── checks the signature and the file (models.c), then uses buckets of
                at least fleet.k homes
```

### The context buckets

An outcome is counted in a bucket of six coarse dimensions:

| Dimension | Values |
|---|---|
| band | `2.4`, `5`, `6` |
| channel width | `20`, `40`, `80+` |
| congestion | `low`, `mid`, `high` (the radio's loss, from Self-Healing Wi-Fi) |
| device class | `phone`, `laptop`, `tv`, `iot`, `other` (from the device type) |
| hours | `night` (0-6), `morning` (6-12), `afternoon` (12-18), `evening` (18-24) |
| line | `fibre`, `dsl`, `cable`, `fwa`, `satellite`, `other` (`fleet.line`) |

A dimension that isn't known is `-`. A bucket is written
`band/width/congestion/device/hours/line`, for example
`5/80+/high/phone/evening/fibre`.

The aggregator adds every bucket up at five levels, from every dimension to
none, so that a rare context still has a coarser prior:

| Level | Keeps | Example |
|---|---|---|
| 0 | everything | `5/80+/high/phone/evening/fibre` |
| 1 | all but the hours | `5/80+/high/phone/*/fibre` |
| 2 | band, congestion, line | `5/*/high/*/*/fibre` |
| 3 | band | `5/*/*/*/*/*` |
| 4 | nothing (the fault and the action) | `*/*/*/*/*/*` |

The gateway uses the most specific bucket that at least `fleet.k` homes add
up to.

### What leaves the home, exactly

Nothing, unless `fleet.share` is 1. Then, once every `fleet.interval`, the
bridge sends this and only this:

```json
{"v": 1, "line": "fibre",
 "outcomes": [["WIFI_HOPPING", "stay24", "5/80+/high/phone/evening/fibre", 20, 4, 2, 0]],
 "normals":  [[1, 3, 23, 4]]}
```

- **outcomes**: fault code, action, bucket, then tried, helped, worse, undone.
  Each count is capped at `fleet.cap` (50) and has Gaussian noise of
  `fleet.noise` (1.0) added, then rounded. The noise comes from a secret only
  the gateway knows and the true count, so the same count always gets the same
  noise: sending the totals again every day tells the aggregator nothing new,
  and averaging many summaries doesn't remove the noise.
- **normals**: the metric (0 the next router's delay, 1 the internet's delay,
  2 loss, 3 DNS time, 4 the download speed), the hour band, the normal and its
  typical deviation. Only once the home's own normal is learned. Rounded: whole
  milliseconds, 0.1% of loss, two significant figures of speed.

It never sends MACs, IP addresses, device or network names, SNIs, DNS names,
timelines or the times of anything. The device is only its class. Cortex
knows which gateway sent a summary (it needs that to count distinct homes, the
same as for any report); the priors it publishes carry no gateway.

The aggregator refuses a summary with anything else: an unknown field, a
bucket field outside the fixed words above (so a MAC, an address or a name
can't travel in one), or counts that don't add up.

### k-anonymity

A bucket or a normal that fewer than k homes add up to is not published
(`--fleet-k`, 5 by default). The gateway checks this again: it uses only
buckets of at least its own `fleet.k` homes, and it refuses a priors file with
a bucket of fewer homes than the file's own k. One home also counts at most
`--fleet-cap` tries per bucket, so it can't outweigh many.

### The priors file

```
# aigw-priors fleet-priors
# version 7
# published 2026-11-25T10:00:00Z
# k 5
# homes 60
o WIFI_HOPPING stay24 5/*/high/*/*/fibre 30 300 60 30 10
o WIFI_HOPPING wait */*/*/*/*/* 50 500 350 0 0
n fibre 1 3 20 23.5 4
```

`o <fault> <action> <bucket> <homes> <tried> <helped> <worse> <undone>` and
`n <line> <metric> <hour band> <homes> <normal> <deviation>` (line `*`: every
line together). It is signed with usign by cortex's key and installed like a
model update: the signature must be by a key the gateway trusts
(`agctl trust`), the version must be newer than the one in use and than any
rolled back, and it must parse: helped, worse and undone at most tried, k at
least 2, every bucket at least k homes. A file that fails any of these is
refused and the one in use stays. `agctl model rollback fleet-priors` goes
back one.

### The merge rules (for an ISP's own aggregator)

1. Keep each home's latest summary; it replaces the earlier one (counts are
   the home's totals). Drop summaries older than 30 days, and a home's when it
   is removed (`aigw_cortex remove`).
2. Project each outcome to the five levels and add up per bucket, per home.
3. Cap each home's tries in a bucket at `cap`, scaling its other counts with it.
4. Add the homes up. Leave out every bucket fewer than k homes add up to.
5. Normals per line type (and `*`), metric and hour band: the median of the
   homes' normals; the deviation is the median of theirs widened by how much
   the homes differ (1.4826 x the median absolute deviation). At least k homes.
6. Publish only when something changed, at most every `gap` seconds.

### The Bayesian blend

For each candidate (the rules' default fix, and waiting), the chance that it
helps and the chance that it makes things worse (or is undone) come from a
Dirichlet over helped, no change, worse-or-undone:

- the prior is uniform (1, 1, 1) plus the fleet's counts scaled to at most
  `fleet.strength` (20) tries;
- the home's own outcomes are added as they are.

So with nothing here, the fleet decides; after 20 of the home's own tries the
two weigh the same; after 400, the fleet is under 5%. The score is
*helps - worse*. The default changes only when another candidate scores better
by `fleet.margin` (0.15) and both have at least 5 tries behind them (the
fleet's and the home's). Otherwise the rules' own fix stays.

### Cold start

While early warning is still learning (`care.learn_days`), it judges each
check against the fleet's normal for the home's line type and hour band,
blended into the home's own as its learning goes on (all the home's own once
the learning days have passed). Only shifts (hours of "worse than usual"), not
trends, which need the home's own days, and only with twice the usual
difference: another home's normal is a looser guide. A value far from the
fleet's normal doesn't pull the home's own learning towards it.

## Settings

`/etc/config/aigw`, section `fleet`:

| Option | Default | What it does |
|---|---|---|
| `enabled` | `1` | Keep outcomes, use the fleet's priors (choices, warnings) |
| `share` | `0` | Send this home's summary through the bridge. Opt-in |
| `k` | `5` | A fleet bucket is used here only when at least this many homes add up to it |
| `interval` | `86400` | Seconds between summaries |
| `line` | `''` | The line: `fibre`, `dsl`, `cable`, `fwa`, `satellite`, `other` |
| `noise` | `1.0` | Noise on each count shared (standard deviation) |
| `cap` | `50` | A count shared is at most this |
| `strength` | `20` | The fleet's counts weigh at most this many of the home's own tries |
| `choose` | `1` | Let the fleet's evidence change which allowed fix is used first |
| `margin` | `0.15` | ... only when another candidate is better by this much |

The bridge passes summaries on with its `fleet` setting (1) and capability
`fleet.share`. Cortex: `serve --fleet-k 5 --fleet-cap 50 --fleet-gap 3600`, or
`--no-fleet`.

**Why sharing is opt-in.** Using the fleet's priors sends nothing, so it is
on. Sharing sends how this home's faults and fixes went. That is coarse,
capped and noised, and k-anonymity keeps one home from standing out in what
is published, but it is still about the home, and unlike federated learning's
rounds it has no formal differential-privacy bound. So it needs a yes: an ISP
turns it on (`fleet.share 1`, or `FleetLearning.Share` over USP or TR-069)
where its customers agreed to it. Ten sharing homes are enough to start.

## Surfaces

```
$ agctl fleet
Fleet learning on (every home teaches the next: how fixes went in homes like this one)
  sharing    on, every 86400 s; last summary 2h ago: 6 buckets of outcomes, 20 normals (each count at most 50, noise 1.00)
  here       49 outcomes in 3 buckets (kept here)
  received   priors version 4 from 57 homes (k 5; used here at 5 or more): 210 buckets, 40 normals, 3h ago
  choices    2 informed by the fleet, 1 changed (only among the fixes the rules allow)
    1h ago  WIFI_HOPPING: stay24 -> wait. Waited first for WIFI_HOPPING: keeping the device on 2.4 GHz helps 21%, ...

$ agctl fleet rank WIFI_HOPPING pixel-7
WIFI_HOPPING in 5/80+/high/phone/evening/fibre (band/width/congestion/device/hours/line), best first ...:
  wait      score +0.55: helps 61%, worse 6% (fleet: 42 of 60 tries in 3 homes; here 14 of 20); the fleet's share 50%
  stay24    score +0.07: helps 23%, worse 16% (fleet: 12 of 60 tries in 3 homes; here 4 of 20); the fleet's share 50%
```

ubus `ag.fleet status`, `ag.fleet rank {code, device}`; `agctl -j` for JSON.
`agctl care` shows `fleet_normal` while warnings use the fleet's normal, and
each warning `from_fleet`.

### Data model (1.23)

`Device.X_AIGW_.FleetLearning.`:

| Parameter | Type | |
|---|---|---|
| `Enable` | boolean, writable | `fleet.enabled` |
| `Share` | boolean, writable | `fleet.share` |
| `K` | unsignedInt, writable | `fleet.k` (2 or more) |
| `UploadInterval` | unsignedInt, writable | `fleet.interval` |
| `LineType` | string, writable | `fleet.line` |
| `OutcomesRecorded` | unsignedInt | outcomes kept here |
| `LastShared` | dateTime | the last summary handed to the bridge |
| `PriorsVersion`, `PriorsHomes`, `PriorsBuckets` | unsignedInt | the priors in use |
| `PriorsReceived` | dateTime | when they were loaded |
| `ColdStart` | boolean | warnings are judged against the fleet's normal now |
| `ChoicesMade`, `ChoicesChanged` | unsignedInt | choices the fleet informed, and changed |
| `LastChoice` | string | the last one, in words |

Events: `Device.X_AIGW_.FleetPriorsReceived!` (Version, Buckets, Normals,
Homes) and `Device.X_AIGW_.FleetChoice!` (Code, Default, Chosen, Reason).
Early warnings from the fleet's normal say so in `EarlyWarning!`'s Summary.
agd's own ubus event is `ag.fleet` (`priors`, `choice`).

### The outcome interface

`outcome_report(code, action, result, key, mac, iface)` in `agd/agd.h`, an
inline no-op when fleet learning isn't built. act.c and proactive.c feed it
today; proven self-repair (`PROVEN`) feeds its receipts through the same call.
One outcome per key (a decision id, or a ticket's): a later report with the
same key replaces the earlier one, so a receipt corrects what act.c guessed.

## The build option

`AIGW_FEATURE_FLEETLEARN` (CMake), "Fleet learning" in OpenWRT's
Network > aigw > AIGW OS features. It needs `FED` (federated learning: the
same channel and signed-update path); it reaches cortex through the bridge
(`PLUGINS`), and uses proactive care (`PROACTIVE`) and early warning (`CARE`)
when they are built. Since v0.51.1 it is in the **max** profile (every feature,
2048 KB of flash; [features.md](features.md)), not full: it needs an ISP-side
collector (cortex) anyway, and full keeps its 1024 KB with headroom. A full
build with `-DAIGW_FEATURE_FLEETLEARN=ON` (or Custom in menuconfig) is max.
Without it, `agctl fleet` says
`not built in (AIGW_FEATURE_FLEETLEARN)`, the data model has no
`FleetLearning.` object, and a `fleet` config section is ignored.

### Flash

As measured in v0.50 (OpenWRT 24.10.8 SDK, as `aigw-selftest` counts it): the
full profile, with fleet learning then, was x86-64 980 KB, aarch64 972 KB,
mipsel 812 KB of the 1024 KB budget. Fleet learning adds 28, 32 and 24 KB
(v0.48: 952, 940, 788). Today's figures per profile are in
[features.md](features.md). At the time, x86-64
and aarch64 have 44 and 52 KB left, so later features need room made at build
level before anything is dropped: for example one multi-call binary for agd
and agctl (they share libag, which each links in now), or `-Oz` where the
toolchain has it.

## Tests

- `tests/test_fleet.c` (ctest `fleet_learning`): buckets and levels; the
  priors file and what makes it refused; k-anonymity on the gateway; the blend
  moving from the fleet to the home as its outcomes grow; the choice (margin,
  evidence); cold start (a warning on day one from the fleet's normal, none
  for a home that is simply slower than the fleet, the home's own after the
  learning days).
- `tests/test_cortex.py` (`FleetLearning`): the aggregator's merge, the cap,
  k-anonymity, refused summaries (a MAC, an address, unknown fields), the
  signed file, publishing only on change, a removed home's share going.
- `tests/fleet-test.sh`: three homes and a fourth that doesn't share, bridges
  and a cortex with k 3: summaries reach cortex, signed priors come back to
  every home, the rank shows waiting first, the one-home bucket is left out,
  nothing that names a device reaches cortex, a new home uses the fleet's
  normal while learning, a changed file and one below its own k are refused.
- `tests/proactive-test.sh`: with priors saying waiting clears hopping far more
  often, a hopping phone gets no stay24: proactive care waits first, says why,
  `agctl fleet` shows the changed choice, and `FleetChoice!` is sent.
- `tests/dm-test.sh`: the data model file has `FleetLearning.`.

## Limits

- Outcomes are as good as their source. A trial that didn't help counts as no
  change unless its note says it made things worse; proven self-repair's
  receipts are more exact, and replace these when it is built.
- Only proactive care's own fixes (keeping a device on 2.4 GHz, smart queueing)
  are chosen with the fleet's help today, against waiting. Self-Healing Wi-Fi's
  channel trials and the mesh's moves feed outcomes but choose as before.
- Warnings from the fleet's normal can be wrong for a home far from the fleet
  (another line type, a long line): set `fleet.line` so the right homes are
  compared, and they need twice the usual difference.
- The noise protects counts, not that a home shares: cortex knows which
  gateways send summaries. k-anonymity protects what is published.
- Real-router testing is still to be done; the tests run on Linux.
