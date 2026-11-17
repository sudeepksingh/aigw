# AIGW staged firmware rollouts that stop themselves

An ISP rarely sends new firmware to every home at once. It starts with a few
homes, waits, and goes on if the help desk stays quiet. That wait is the weak
point: the help desk hears about a bad image only once people call, and
someone has to remember to stop the next wave.

Since v0.30 each AIGW home checks its own upgrade ([upgrade-check.md](upgrade-check.md)).
It compares the home before and after: the devices come back, the internet,
the delays, the speed and the experience score. When the home is worse, it
rolls back by itself. A staged rollout in cortex adds up those verdicts, wave by
wave:

- **waves:** 1% of the homes, then 10%, 50% and all of them (or lists of homes);
- **a wave passes** when enough of its homes have reported and few enough got
  worse. Only then do the next wave's homes get the firmware;
- **it stops by itself** when a wave's homes get worse: too many rollbacks, or
  the experience score falls. It says why in words, with counts and never the
  homes' names;
- **it never goes further without a pass.** Homes that don't report make it
  wait, and after a time limit, stop;
- **the ISP decides too:** pause, resume or abort at any time.

## The rules

For each wave that has started:

| | |
|---|---|
| Got worse | A home that rolled back, stayed worse (no image to go back to), didn't boot the new image, or couldn't install it (a refused or failed download). If more than `--max-bad` percent of the wave's homes got worse (default 5%), the rollout stops. This is counted as results come in, so a bad wave stops early, without waiting for the rest. With a canary of one home, one bad home is enough. |
| Score | The experience score is the share of the time without a problem, as each home measured it before and after the upgrade. If the homes that kept the firmware lost more than `--max-score-drop` points on average (default 2), the rollout stops. Homes whose score isn't known yet (care off, too little time watched) don't count. Each home already rolls back on a large drop of its own. This rule catches a small drop spread over many homes. |
| Enough reports | A wave passes only when at least `--min-reports` percent of its homes reported a verdict (default 80%). Until then it waits. If the homes don't report within `--timeout-h` hours (default 72), the rollout stops. |
| Pass | Otherwise the wave passes, and the next wave's homes are cleared. After the last wave, the rollout is done. |

A wave that passed can still stop the rollout: its last homes keep reporting,
and a late rollback counts like any other.

The reasons read like this:

```
Wave 1 of 4 passed: 2 of the 2 homes kept it; score 97.4% before, 97.6% after. Wave 2 starts.
Stopped in wave 2 of 4: 3 of the 18 homes got worse on it (2 rolled back, 1 did not boot it): more than the 5% allowed (0 of 18).
Stopped in wave 3 of 4: the experience score fell by 3.1 points in the 74 homes that kept it (97.9% of the time without a problem before, 94.8% after): more than the 2 allowed.
Stopped in wave 2 of 4: only 9 of the 18 homes reported in 72 h; 15 needed.
```

## Pause, resume, abort

| | |
|---|---|
| `pause` | No new home gets the firmware. Homes already upgrading still report, and their results can still stop the rollout. A wave that passes while paused doesn't open the next one until you resume. |
| `resume` | Goes on from where it was paused. |
| `abort` | No more homes get it, ever. |
| stopped | A rollout that stopped itself stays stopped. Fix the cause, then start a new rollout. |

An AI assistant (MCP) can pause or abort, never resume or start one: it can only
hold firmware back.

## How a home gets the firmware

Whoever pushes it, the image always goes through agd's own firmware Download
path: the same one that TR-069 `Download` and USP `Download()` with
`AutoActivate` use. So the upgrade is checked the same way, the hold on a
version rolled back still applies, and `mgmt.firmware 0` still forbids it.

| `--via` | |
|---|---|
| `acs` (default) | The ISP's ACS or USP controller pushes the image, as it does today. It asks cortex which homes are cleared now: `rollout homes <name> --json` (cortex's gateway names). Nothing is cleared while the rollout is paused, stopped or aborted. The verdicts reach cortex through the homes' bridges. For homes without a bridge, the ACS passes on the `UpgradeChecked!` it receives: `rollout result <name> <home> Kept` (or `RolledBack`, `Worse`, `NotBooted`, `Failed`). |
| `bridge` | The home's bridge asks cortex for an offer at each check-in (`GET /v1/firmware`), and gets one only when its wave is cleared and the rollout is running. It hands the offer to agd, which downloads the image into the bank that isn't running, checks the SHA-256, activates it and reboots. The bridge tells cortex how it went (`POST /v1/firmware/<id>`). This is off unless the home sets `agctl plugin config bridge firmware 1`. The bridge needs the capability `firmware.install`, which a plug-in installed by the user must be allowed (`agctl plugin allow`). |

A home already on the version answers "already running", which counts as
fine. A home holding the version off (it rolled it back in the last week)
refuses it, which counts as worse.

**Which version is which.** A home's verdict counts for a rollout when it is
for the rollout's version and was activated after the home's wave started. An
image that calls itself something else once booted (OpenWRT) still counts when
the bridge delivered it.

## Using it

```
$ python3 -m aigw_cortex rollout new spring --version 24.10.2-aigw1 \
      --url https://fw.example.net/24.10.2-aigw1.img --sha256 9f2c... --via bridge
Rollout spring: 24.10.2-aigw1 to 200 homes, through the bridge: running, wave 1 of 4
  homes    198 waiting for their wave, 2 cleared, not started
  wave 1   2 homes, opened 2026-11-17 10:02: wait: 0 of the 2 homes reported, 2 needed; 2 on their way.
  wave 2   18 homes
  wave 3   80 homes
  wave 4   100 homes

$ python3 -m aigw_cortex rollout status spring          (--homes: each home; --json)
$ python3 -m aigw_cortex rollout pause spring the help desk is busy today
$ python3 -m aigw_cortex rollout resume spring
$ python3 -m aigw_cortex rollout abort spring found a bug in the release notes
$ python3 -m aigw_cortex rollout list
```

`rollout new` takes `--waves 1,10,50,100` (rising percentages), or `--wave
home,home --wave home,...` for lists. It also takes `--group G` (only that
group's homes), `--max-bad`, `--max-score-drop`, `--min-reports`, `--timeout-h`
and `--paused` (planned, started with `resume`). With percentages, the homes
are taken in a fixed but spread order, so the first wave isn't always the same
homes.

The [fleet report](fleet-report.md) has a `rollouts` section: each rollout
going on, or changed in the period, with its wave, the homes' states and the
reason. Over MCP, `rollout_status` reads the same, and `rollout_hold` pauses or
aborts.

## What leaves the home

The bridge reports the last upgrade check without the devices: the state, the
versions, when it was activated, the reasons, the summary in words (counts), the
number of devices before and back, and the scores. The rollout's reasons hold
only counts. Home names appear only where the ISP asks for them: `rollout
homes` (for the ACS) and `rollout status --homes`.

## How it is tested

- `tests/test_rollout.py` (ctest `staged_rollout`):
  - the waves;
  - a pass;
  - a stop on rollbacks, early and with a canary of one;
  - a stop on a score drop, with unknown scores left out;
  - too few reports means wait, then a stop after the time limit;
  - a whole rollout in the store, fed by bridge reports, from wave 1 to done;
  - an older upgrade or another version doesn't count;
  - pause, resume and abort;
  - a stopped rollout can't resume;
  - the bridge offer is sent once, and a failed install counts as worse;
  - ACS results;
  - the fleet report, and MCP (`rollout_hold` can't resume).
- `tests/rollout-test.sh` (CI): four homes, each with its own agd, bridge and
  two-bank firmware handler, and a private cortex. A rollout through the
  bridges in three waves, planned paused (nothing happens), then resumed:
  - wave 1's home downloads, reboots, watches and keeps the image, and wave 1
    passes;
  - in wave 2, one home keeps it, while the other keeps restarting on it and
    rolls back by itself;
  - the rollout stops with the reason in words and no names;
  - wave 3's home never gets the image, and the stopped rollout won't resume.

Not tested yet: an ACS pushing through TR-069 against `rollout homes`, and a
real image on a router with A/B flash.
