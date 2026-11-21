# Neighbour-aware channels

AIGW OS learns how crowded each Wi-Fi channel is, hour by hour of the day, and
moves the access point only when another channel is **clearly** better over
the hours the home actually uses its Wi-Fi. Never from one moment.

Version 0.45. On by default (`wifi.chan_plan 1`). It acts through `act.mode`
like every other fix: in **advise** mode (the default) it proposes and waits for
a yes; in **enforce** mode it moves by itself, within the guardrails.

## Why

A real finding on a PC access point: the 2.4 GHz radio sat on channel 11 next
to neighbours' networks on 11 and 13 (a fixed-wireless ISP's). A phone lost a
third of its traffic and waited up to 2 s for replies, at a fine signal.

Self-Healing Wi-Fi ([self-healing-wifi.md](self-healing-wifi.md)) saw the
congestion and tried channel 1: 3% loss and 175 ms there, against 4% and
126 ms before. Not clearly better, so it went back. That was right, but the
choice was poor: channel 1 was simply the first on the list, picked in one
moment, with nothing learned about it.

Now each channel has a history, and the healing trial tries the quietest
channel known at this hour first.

## What it learns

For each channel AIGW may use (2.4 GHz: 1, 6 and 11; 5 GHz: the channels
without radar detection), for each hour of the day:

| What | From | How often |
|------|------|-----------|
| The air others use (% of the time) | the radio's survey (`iw dev <ap> survey dump`) | every `wifi.chan_poll` (30 s) for the channel in use; for others, after a scan visited them |
| The neighbouring networks | scan results: on the channel or overlapping it; a strong one weighs more, one that says its channel is busy (BSS Load) more | whenever there are fresh results |
| What devices felt | Self-Healing Wi-Fi's probes (loss, replies) and the link rates | every look, on the channel in use |
| The home's own use of the air | the survey's transmit time (a radio without a survey: its devices' traffic) | every look |

The home's **busy hours** are the hours its own use reaches at least half of
its busiest hour. Those hours weigh fully when channels are compared; the
others weigh a tenth.

Neighbours on 2.4 GHz overlap: a network on 13 hurts 11 (at 0.6 of its
weight), one on 9 hurts 11 and 6 a little. Channels 1, 6 and 11 don't overlap
each other. On 5 GHz, a network counts when it is in the same 20, 40 or 80 MHz
block.

## Scans, carefully

An access point that scans leaves its channel for a moment. So:

1. **Results already there first.** Every 10 minutes AIGW reads the scan
   results the radio already has (`iw dev <ap> scan dump`): anyone's scan,
   hostapd's own. A scan is learned once.
2. **Its own scans are rare.** At most one every `wifi.chan_learn_gap`
   (3 hours), and only when the home is quiet: no call or game on (calls and
   games first knows), and the home's own traffic under `wifi.chan_quiet`
   (5%) of the air (without a survey: its devices under 2 Mbit/s). Only in
   an hour of the day not learned yet.
3. **Never during a call or a game.** This also holds for the older scan of
   channel self-healing (`wifi.chan_busy`).

## When it moves

Every `wifi.chan_plan_every` (1 hour) it compares the channels over the busy
hours:

- only after `wifi.chan_plan_days` (3) days of history;
- on the air others use, where both channels were measured in those hours;
  else on the neighbours, after at least 3 scans;
- **clearly better** means `wifi.chan_plan_better` (30%) less load, and at
  least 10 points of air (or 2 of neighbour weight) less, and not much worse
  in any one busy hour;
- the same answer for `wifi.chan_plan_hold` (3) looks in a row;
- at most one planned move every `wifi.chan_plan_gap` (7 days) per access
  point;
- not to a channel that didn't help lately (avoided for `wifi.chan_avoid`,
  7 days; a healing trial that failed avoids it a day);
- 2.4 GHz: 1, 6 or 11 only; 5 GHz: no radar (DFS) channels, and only primary
  channels that fit the width in use.

The two comparisons are not symmetric in a lucky way: going from 11 to 6 needs
6 to be clearly better, and going back needs 11 to be clearly better than 6.
Between the two, nothing moves.

When all that holds, AIGW waits for a quiet moment, then raises alert
`wifi-plan`; policy `wifi_plan` makes it a `channel` decision. It respects:

- `act.mode`: advise proposes, enforce applies (at most `act.max_auto_per_hour`
  automatic actions an hour);
- `act.protect`: no planned move while a protected device is on that access
  point;
- area outages: no channel move while cortex says the area is down;
- calls and games: never while one is on.

A configuration from before 0.45 has no `wifi_plan` section: it is built in
all the same, unless `wifi.chan_plan` is 0.

## The trial

The switch is a channel switch announcement: devices follow without dropping
(a PC whose adapter can't announce one restarts the access point).

The trial is judged over `wifi.chan_plan_trial` (1 hour) of busy hours on the
new channel, against what the old channel gave in the same hours (learned):

- what devices felt: loss, replies, link rates;
- the air others use.

**Kept** when nothing devices feel is clearly worse (more loss, slower
replies, lower rates) and something is clearly better (half the loss, replies
30% faster, rates 20% higher, or clearly less air used by others). The channel
is saved in the Wi-Fi settings.

**Moved back** otherwise, or when no device could be measured. That channel is
avoided for `wifi.chan_avoid` (7 days).

A person can undo it any time (`agctl undo <id>`); its time limit (a day)
also switches back.

The field case above, judged by these rules: 4% / 126 ms before, 3% / 175 ms
after is "replies are slower": moved back. With history, AIGW would have tried
channel 6 first, where the neighbours were.

## See it

```
agctl channels
```

```
2.4 GHz (wlx38a28c211c49), channel 11: Planned
  Learned for 5 days, 14 scans. The home's busy hours: 19:00-24:00.
  Channel 6 has clearly fewer and weaker neighbouring networks in your busy hours (19:00-24:00): they weigh 1.4 on it, 12.8 on channel 11 (14 scans, 5 days).
  The move waits: the home is busy now.
  Air used by others, % by hour of day (. not measured, * the home's busy hours):
   hour  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23  neighbours
                                                                  *  *  *  *  *
     1   .  .  8  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  3.5
     6   .  .  4  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  1.4
  > 11  18 15 14 12 12 14 20 25 28 30 31 30 33 35 36 38 40 44 50 55 58 57 54 40  12.8
  Planned moves: 0 proposed, 0 kept, 0 moved back.
```

- `agctl wifi` has a one-line `channels` verdict per access point.
- `agctl why` shows "a quieter channel learned", "planned channel kept" or
  "moved back" on the home's timeline, beside the channel change itself.
- `agctl brief`: for Wi-Fi congestion, what was learned of the channels, and
  a waiting move to approve.
- `agctl -j channels` gives the whole table (air by hour, neighbours by hour
  x10, the home's use by hour).

## For the ISP

Data model 1.20 ([device-management.md](device-management.md)):

| Parameter | Meaning |
|-----------|---------|
| `Device.X_AIGW_.ChannelPlan.Enable` | on (`wifi.chan_plan` and `wifi.chan_heal`) |
| `Device.X_AIGW_.ChannelPlan.Radio.{i}.Radio` | the `Device.WiFi.Radio.{i}` it is about |
| `...Channel`, `...BestChannel`, `...GainPercent` | now, the clearly better one (0 none), by how much |
| `...Status` | `Learning`, `Stay`, `Confirming`, `Planned`, `Trial` |
| `...Verdict`, `...BusyHours`, `...LearnedDays`, `...Scans` | the verdict in words, and what it rests on |
| `...Proposed`, `...Kept`, `...MovedBack`, `...LastResult` | planned moves so far, and the last trial's verdict |

Event `Device.X_AIGW_.ChannelPlan!` (`Radio`, `FromChannel`, `ToChannel`,
`State`: `Proposed`, `Applied`, `Kept`, `MovedBack`; `Reason`, `Before`,
`After`).

Through the bridge, cortex gets each radio's band, channel, verdict and
counts (no names, no table). `python3 -m aigw_cortex fleet` lists the homes
with a clearly better channel in their busy hours. Many of them in one area
point at a crowded area: there, 5 GHz (or 6 GHz) devices help more than any
2.4 GHz channel.

## Settings

All in the `wifi` section of `/etc/config/aigw`:

| Option | Default | Meaning |
|--------|---------|---------|
| `chan_plan` | 1 | learn and plan; 0 off (healing still works) |
| `chan_plan_days` | 3 | days of history before a move |
| `chan_plan_better` | 30 | % less load over the busy hours that is clearly better |
| `chan_plan_hold` | 3 | looks in a row that must agree |
| `chan_plan_every` | 3600 | seconds between looks |
| `chan_plan_gap` | 604800 | seconds between planned moves on one access point |
| `chan_plan_trial` | 3600 | busy-hour seconds measured before a trial is judged |
| `chan_avoid` | 604800 | seconds a channel that didn't help is avoided |
| `chan_learn_gap` | 10800 | seconds between its own learning scans |
| `chan_quiet` | 5 | % of the air the home itself uses below which it is quiet |

What is learned is kept in agd's state file (`main.state_file`) and survives
restarts and upgrades.

## Tested

- `tests/test_chanplan.c`: the averages, the neighbours' weights (overlap,
  BSS Load, 5 GHz blocks), the busy hours, the field case (6, not 1, and not
  from one scan), the air by busy hour, a channel worse in one busy hour, a
  small difference, the trial's verdicts (the field trial moves back).
- `tests/chanplan-test.sh`: agd against a fake hostapd and a stand-in for
  `iw`, five days of history seeded: no scan and no move while the home is
  busy; at a quiet moment one scan, then channel 6 proposed (advise);
  approved, switched, kept on better rates, saved; the data model, the
  events, the timeline, the state; then enforce: applied by itself, worse
  rates, moved back, 6 avoided, no new move within the gap.

## What only a real network shows

The fake radio answers what the test tells it. On a real access point:

- whether the radio reports a survey (`iw dev <ap> survey dump` with
  "channel active time"; `agctl wifi` then shows "air ... used by others"),
  and for other channels after a scan: it depends on the driver. Without a
  survey, the neighbours and what devices feel still count, and the home's
  use is its devices' traffic (the traffic meter);
- whether the driver lets an access point scan (`scan ap-force`) and how long
  it is off its channel; whether hostapd's own results show up in
  `scan dump`;
- how the neighbours' load moves by hour, and how long it takes to learn
  enough (3 days by default);
- that phones follow the channel switch (the PC's adapter can't announce one,
  so the access point restarts and devices rejoin).
