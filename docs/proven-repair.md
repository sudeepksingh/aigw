# Proven self-repair

AIGW fixes things in the home: it moves an access point to a quieter channel, keeps a device on
2.4 GHz, turns smart queueing on, moves a device to another access point of the mesh. Before an
ISP lets a gateway do that on its own, it wants evidence that the fixes work.

Proven self-repair gives every fix a **receipt**. Before the fix, AIGW says what it expects.
After it, AIGW measures what happened and judges it. A fix that didn't help is undone, and the
home is back exactly as it was. The receipts add up to a **fix rate** per fault and kind of fix,
which an ISP can read before it turns acting on, and can require before AIGW acts alone.

It is the `PROVEN` feature (`AIGW_FEATURE_PROVEN`, it needs `CARE`), built in the full and small
profiles.

## What a receipt says

```
$ agctl repair
Proven repair: every fix is predicted, measured for 600 s (or by its own trial) and judged; act mode enforce
  undo      a fix of AIGW's own that didn't help is undone, as it was before
  shadows   on: in advise mode, what AIGW would have done is measured without it
  gate      off: AIGW acts as act.mode says
  last day     3 fixes: 1 helped (fix rate 50%), 1 no change, 0 made worse; 1 undone; 1 not known (...)
  last 7 days  ...
  by fault and fix (30 days)
    wifi-hopping       stay24    2 fixes: 1 helped (fix rate 100%), ...
    wifi-congestion    channel   1 fix: 0 helped (fix rate 0%), 1 no change, 0 made worse; 1 undone
  receipts (newest first)
    #3   applied  channel   wifi-congestion  NoChange, decision 3, 25s ago
         predicted  Wi-Fi 5 GHz (wlan0): devices' loss 20% -> under 5% within 3 min
         measured   Wi-Fi 5 GHz (wlan0): devices' loss 20% -> 20% (expected under 5%); undone by its trial
    #2   applied  stay24    wifi-hopping     Helped (as predicted), decision 2, 34s ago
         predicted  OnePlus: band changes in 10 min 4 -> under 1 within 10 min
         measured   OnePlus: band changes in 10 min 4 -> 0 (expected under 1, met); it stays
```

Each receipt has:

- **what was done and why**: the decision (`agctl decisions`), the kind of fix, the fault it
  fixes (the alert or the ticket code), who asked for it;
- **the prediction**, made before the fix: the metric, the device or radio, its value now, the
  value expected and the deadline;
- **the result**: the value measured after it, the home's normal for that hour, and the verdict;
- **whether it was undone**, and by whom (proven repair, or the fix's own trial);
- **calls likely avoided**: 1 when it fixed a proactive ticket that closed without the customer
  calling.

## Before the fix: the prediction

The part of AIGW that proposes a fix knows best what to expect, and says so:

| Fix | Metric | Before | Expected |
|---|---|---|---|
| Self-Healing Wi-Fi's channel (congestion) | the devices' loss on that access point | Self-Healing Wi-Fi's measurement now | 5% (the healing threshold), or the home's normal for the hour if that is higher |
| A planned channel move (neighbour-aware channels) | the air used by others over the busy hours | the plan's history for the channel now | less by the plan's gain |
| A device kept on 2.4 GHz for hopping | the device's band changes in 10 minutes | Self-Healing Wi-Fi's count | at most 1 |
| A device kept on 2.4 GHz for another fault | the device's loss | its loss now | 5% |
| Smart queueing | the internet's delay while the line is full | the last hour of the timeline | the home's normal delay (proactive care's, for the hour) plus 15 ms |
| A mesh steer | the device's signal | where it is now | what the target access point hears of it |
| A mesh radio's channel | frames sent again | (its trial's) | half |

A prediction is never "worse than now". When the metric is already good, the receipt says it is
expected to stay so: a fix that changes nothing measurable is "no change".

## After the fix: the verdict

The metric is measured from `repair.settle` seconds after the fix took effect (devices reconnect)
until the deadline, every `repair.tick` seconds, and averaged. It is compared with the value
before **and with the home's normal for the hour**: if the hour after is normally busier (or
quieter) than the hour before, that difference is not counted for or against the fix. The normal
comes from proactive care's learned normal (the line's delay), or, for Wi-Fi loss, from the same
learning rule applied to the home's Wi-Fi loss while all is well.

| Verdict | When |
|---|---|
| Helped | better by at least the metric's effect (3 points of loss, 15 ms, 4 dB, ...) and by at least 20% of the value before |
| NoChange | neither better nor worse by that much |
| Worse | worse by that much |
| Unknown | not a result of the fix: the device or radio left, an area outage while it was measured, a person undid it first, fewer than 3 measurements, nothing measured before |

Unknowns are kept on the receipt but left out of the fix rate. "Prediction met" says whether the
expected value was reached; a fix can help without meeting it.

### Fixes with a trial of their own

Self-Healing Wi-Fi's channel trial, the planned channel moves, smart queueing on bufferbloat (and,
with proven repair, proactive care's smart queueing) and every mesh move already run a careful
trial that keeps the fix or puts it back. Proven repair doesn't duplicate them: the trial gives
the receipt its own before and after numbers when it decides, and the trial's decision settles
"helped" against "no change" (it measured more closely, with its own statistics). If the numbers
say "worse" the receipt says so, whatever the trial decided.

### Undoing a fix that didn't help

A fix without a trial of its own that is judged "no change" or "worse" is undone at once, through
the decision's own undo (`act.c`), which puts back exactly what was there before: the channel it
was on (kept in the decision), the 5 GHz access points no longer refusing the device, smart
queueing removed, the device steered back. The decision's state becomes `tried` (not `reverted`:
no person said it was wrong), and `RepairUndone!` is sent.

AIGW undoes a fix only:

- in **enforce mode** (changing the home is acting; in advise mode the receipt says "not undone:
  agctl undo N"),
- when it is **AIGW's own fix** (a policy's, proactive care's, the mesh's, or one AIGW proposed
  and a person approved); a person's own request (`agctl channel ...`) or a plug-in's is theirs,
- when `repair.undo` is on (the default).

## Advise mode: shadows

In advise mode nothing is applied without a yes, so nothing would be measured. Proven repair
measures anyway: a fix that is proposed gets a **shadow receipt** with the same prediction, the
predicted gain and a **confidence** (the fix rate measured here for that kind of fix, once at
least 3 have been judged; until then it says it isn't measured yet). At the deadline, without the
fix, it says whether the problem stayed (**Needed**) or went by itself (**NotNeeded**). Shadows are
marked as not applied everywhere: `mode shadow`, `Mode Shadow` in the data model, "A shadow: not
applied" in the words. They are counted apart from real fixes and never in the fix rate.

When a proposal is approved, its shadow becomes the fix's receipt, measured from then.

## The confidence gate

`repair.gate 1` lets an ISP turn on enforce mode safely: AIGW acts on its own only with kinds of
fix whose measured fix rate here is at least `repair.gate_fix_rate` % over at least
`repair.gate_trials` judged trials in the last 30 days. Any other automatic fix becomes a proposal
that waits for a person, with a note saying why ("enforce mode, but not proven here yet (fix rate
60% over 5 trials, below the 80% the confidence gate needs): waiting for you"), and gets a shadow.
Fixes a person approves are trials too, so the evidence grows in advise mode. The gate is off by
default: acting is as `act.mode` says.

## Only proven fixes count

- **Proactive care, before the call** counts a ticket as "fixed by AIGW" only when the fix's
  receipt says it helped. A ticket that clears while its fix is still measured waits (at most two
  hours) for the verdict; a fix that didn't help, or couldn't be judged, closes the ticket as over
  by itself. A fixed ticket closed before the customer called marks the receipt's call likely
  avoided.
- **Signals for the ISP** (`agctl offers`): the counts of the week's proven fixes. RETENTION_RISK's
  evidence says how many of the week's problems AIGW fixed itself, proven. MESH_WIFI says so, with
  high confidence, when AIGW's own Wi-Fi fixes helped less than half the time: what is left needs
  hardware, not settings.
- **cortex** gets the evidence through the bridge (`repair`: settings, the day, week and month,
  and each fault and kind of fix), never a name.

## Settings (`/etc/config/aigw`, section `repair`)

| Option | Default | What it does |
|---|---|---|
| `enabled` | `1` | receipts for every fix, shadows, the evidence |
| `window` | `600` | seconds a fix without a trial of its own is measured (2 .. 86400) |
| `settle` | `60` | seconds after a fix before measuring starts |
| `undo` | `1` | undo a fix of AIGW's own that didn't help (enforce mode only) |
| `shadow` | `1` | in advise mode, measure what AIGW would have done |
| `gate` | `0` | the confidence gate (off) |
| `gate_fix_rate` | `80` | the fix rate the gate needs (%) |
| `gate_trials` | `10` | judged trials the gate needs, in 30 days |
| `tick` | `10` | seconds between measurements |

Nothing here changes `act.mode`: advise stays the default.

## Commands

- `agctl repair` the settings, the last day, 7 and 30 days, the evidence by fault and kind of fix
  (with the gate's word on each), the receipts newest first
- `agctl repair --stats` the evidence only; `agctl repair --shadow` the shadows only
- `agctl care` a line: the week's fixes, how many helped, were undone, are being measured
- `agctl why` the fixes made or judged in the stretch asked about, and their verdicts on the
  timeline ("AIGW's fix judged: ...")
- `agctl watch repair` the `ag.repair` events as they come
- ubus `ag repair {stats, shadow, limit}`

## Data model (1.24)

`Device.X_AIGW_.Repair.`

| Parameter | Type | |
|---|---|---|
| `Enable`, `Undo`, `Shadow`, `Gate` | boolean | the settings |
| `GateFixRate`, `GateTrials`, `WindowSeconds` | unsignedInt | |
| `Pending` | unsignedInt | receipts being measured |
| `<Day\|Week\|Month><Fixes\|Helped\|NoChange\|Worse\|Unknown\|Undone\|CallsLikelyAvoided\|Shadows\|ShadowsNeeded>` | unsignedInt | the whole home over the last day, 7 or 30 days |
| `<Day\|Week\|Month><FixRate\|UndoRate>` | int | %, -1 when nothing was judged |
| `<Day\|Week\|Month>MeanImprovement` | decimal | % better on average, of those judged |
| `ReceiptNumberOfEntries`, `StatsNumberOfEntries` | unsignedInt | |
| `Receipt.{i}.Decision` | unsignedInt | `Device.X_AIGW_.Decision.{i}` |
| `Receipt.{i}.Mode` | string | `Applied` or `Shadow` (not applied) |
| `Receipt.{i}.Action`, `Fault`, `Metric` | string | `stay24`, `wifi-hopping`, `DeviceLoss` (`RadioLoss`, `AirBusy`, `FullLineDelay`, `Signal`, `Retries`, `BandChanges`) |
| `Receipt.{i}.Before`, `Expected`, `After`, `Normal` | string | decimals in the metric's unit; empty when not measured |
| `Receipt.{i}.WithinSeconds` | unsignedInt | the deadline (0: when its own trial decides) |
| `Receipt.{i}.Result` | string | `Pending`, `Helped`, `NoChange`, `Worse`, `Unknown`, `Needed`, `NotNeeded` |
| `Receipt.{i}.PredictionMet`, `Undone` | boolean | |
| `Receipt.{i}.UndoneBy` | string | `proven repair`, `its trial`, or empty |
| `Receipt.{i}.ImprovementPercent` | decimal | |
| `Receipt.{i}.Confidence` | int | a shadow's: the measured fix rate, -1 not measured yet |
| `Receipt.{i}.CallsLikelyAvoided` | unsignedInt | |
| `Receipt.{i}.Created`, `Deadline`, `Judged` | dateTime | |
| `Receipt.{i}.Prediction`, `Summary` | string | in words; the home's names taken out for the ISP unless `brief.names` |
| `Stats.{i}.Fault`, `Action` | string | one fault and kind of fix |
| `Stats.{i}.Gate`, `GateWhy` | string | `Open` or `Held`, and why |
| `Stats.{i}.<Day\|Week\|Month><Fixes\|Helped\|FixRate\|MeanImprovement\|UndoRate>` | | as above, for that fault and fix |

Events:

- `Device.X_AIGW_.Repair!` (`Receipt`, `Decision`, `Mode`, `Action`, `Fault`, `Result`, `Metric`,
  `Before`, `Expected`, `After`, `PredictionMet`, `Undone`, `Prediction`, `Summary`): a fix, or a
  shadow, judged
- `Device.X_AIGW_.RepairUndone!` (`Receipt`, `Decision`, `Action`, `Fault`, `Result`, `Summary`):
  undone by proven repair because it didn't help

agd's own event `ag.repair` (`predicted`, `shadow`, `judged`, `shadow-judged`, `undone`) feeds
the timeline and plug-ins.

## Design

- `core/libag/repair.c`, `include/ag/repair.h` (pure): the metrics, the prediction, the verdict
  and the shadow's, the evidence by day (31 days per fault and kind of fix), the gate, the
  confidence.
- `agd/repair.c`: the receipts (at most 64; the oldest judged go first), the measurements (from
  Self-Healing Wi-Fi, the neighbour-aware channels' survey, the timeline for the line, the mesh's
  view of each device), the home's Wi-Fi normal (`care_norm_add`, care's learning rule), the
  hooks below, the ubus method, the data model's values, the state.
- Hooks, reusing the existing action paths: `act.c` calls `repair_on_decision()` on every change
  of a decision's state and `repair_gate()` before acting alone; the parts that propose a fix call
  `repair_expect()` (heal.c, chan.c, sqm.c, mesh.c, proactive.c); the trials call `repair_trial()`
  just before they keep or undo; proactive.c asks `repair_proven()` before counting a fix.
- The receipts and the evidence are saved with agd's state (`repair` in the state file, as
  named fields, so an upgrade keeps them) and survive restarts; a receipt being measured carries on
  after a restart.

## Tests

- `tests/test_repair.c` (ctest `proven_repair`): predictions (back to good, the home's normal,
  the proposer's own figure, higher-is-better, never worse than now, nothing measured), verdicts
  (helped, no change, worse, unknown for a device that left, an area outage, a person's undo,
  too little measured, nothing before), the hour's normal, a trial deciding, shadows, the evidence
  over 1, 7 and 30 days, the 31-day ring and its counters' top, the gate, the confidence, the
  normal's rule.
- `tests/repair-test.sh` (CI): agd with a fake hostapd and a test plug-in raising alerts. In
  advise mode a shadow that is never applied; in enforce mode a fix that helped and stays, a
  channel 36 -> 44 that changed nothing and is undone back to 36 (the decision `tried`,
  `RepairUndone!`), a device that left (unknown, not undone); the evidence in `agctl repair`,
  `care` and `why`, `Device.X_AIGW_.Repair.` without the home's names, `Repair!`; a restart that
  keeps everything; the gate holding both kinds; the bound on receipts.
- `tests/proactive-test.sh`: the TV's fix counts as "fixed by AIGW" once its receipt says it
  helped.

## Limits

- A fix is judged on one metric. A fix that helps something else (a channel that cuts the delay
  but not the loss) is "no change" unless its own trial says otherwise.
- With no measurement at all (Self-Healing Wi-Fi off, a device that never answers), a receipt is
  "unknown" and the fix stays: AIGW doesn't undo what it can't judge.
- The normal for Wi-Fi loss is the whole home's, learned while all is well; a device's own normal
  isn't known.
- A trial that keeps a fix the numbers call "worse" leaves it in place (its trial owns it); the
  receipt says "made it worse", and `agctl undo` puts it back.
- Undo happens only in enforce mode. In advise mode, a fix a person approved that didn't help is
  reported, not undone.
- Shadows can't measure a fix whose metric needs the fix itself (a mesh radio's frames sent
  again on a channel it isn't on): such a shadow is "unknown".
