# AIGW proactive care: fixed, or reported, before the customer calls

[Diagnose](diagnose.md) answers "my internet is slow" when someone asks.
Proactive care asks by itself. The gateway runs the same checks on its own,
deals with what it can fix, and tells the ISP about the rest, often before the
customer notices. Every check also counts towards a daily **experience
score** for the home, which an ISP can collect across its homes.

What an ISP can measure with it:

- **calls avoided:** problems the gateway fixed itself (`FixedByAIGW`: only
  where it changed something; those over without that are `ClearedByItself`), and
  problems the help desk already knew about when the customer called
  (`ProblemDetected!` before the call);
- **time to find a fault:** the event already says where the problem is
  (the line, the ISP's network, DNS or Wi-Fi), with a code and advice;
- **experience per home:** the share of the day when the line, DNS and Wi-Fi
  were all fine, with the main cause on the bad days;
- **faults seen coming** (v0.29): early warnings, the problems they saw
  coming and how long before (`ProblemsWarnedOf`, `MeanWarningLeadSeconds`);
- **speed delivered** (v0.29): a nightly speed test against the ISP's own
  server, against the line's speed (`LINE_SLOW_SPEED` when far below it).

Across many homes, cortex puts this together in the ISP's
[fleet report](fleet-report.md).

## How it works

| | |
|---|---|
| Checks | Diagnose without a device (the line, DNS and every access point), quietly: it doesn't replace `LastDiagnosis.` or send `DiagnosisDone!`, which stay for the diagnoses someone asked for. It runs every `care.interval` (10 minutes). When Self-Healing Wi-Fi turns a radio Poor, it checks within seconds. If someone asks Diagnose about a device during a check, their diagnosis runs right after it, about 3 seconds later. |
| A problem | A verdict of warning or worse on 2 checks in a row; one bad check is a blip. After a bad check, the next one comes after `care.recheck` (60 s), to confirm it quickly and then to see it clear. A problem is over after 2 fine checks in a row. |
| Who deals with it | **The line, the ISP's network, DNS:** the ISP's. Reported at once. **Wi-Fi:** AIGW's own, through Self-Healing Wi-Fi (channel trials, keeping a device on 2.4 GHz). Reported only if it is still there after `care.wifi_grace` (30 minutes). If it is over before that, it counts as fixed by AIGW only if AIGW changed something for it: a channel trial took effect, or a device was kept on 2.4 GHz, during the problem or up to 10 minutes before it. Otherwise it was `ClearedByItself` (v0.29.2). On the PC access point, in advise mode with nobody answering the proposals, 9 problems in one day were counted as fixed by AIGW, though AIGW had changed nothing. **A device, or an AIGW action on one:** nobody's to report. |
| Telling the ISP | `Device.X_AIGW_.ProblemDetected!` (Code, Area, Severity, Summary, Advice, Since), once per problem. It is sent again only if the cause changes, for example when the Wi-Fi problem turns out to be the line. When the problem is over, `ProblemCleared!` (Code, Area, Since, DurationSeconds). Both go over USP and TR-069 like the other events, and to prplOS through aigw-amx. |
| The score | Per day, for 31 days: checks, checks without a problem, bad checks by area, problems, reported, fixed by AIGW, the time watched (between two checks; a gap while agd was down isn't), and of it the time with a problem open, from the problem's first bad check. `ExperienceScore` = % of the time watched without a problem. It is of the time, not of the checks: checks come every 10 minutes when all is well and every minute while something is wrong, so counting checks would weigh a bad minute like ten good ones (a home fine for 78% of the day scored 34%, until v0.29.1). A single bad check is a blip, not a problem, and doesn't lower it. Days saved before v0.29.1 have no times and keep the score by checks. `MainCause` = the area with the most bad checks. |
| Restarts | The day counts, an open problem, the learned normal and an open early warning are saved with agd's state. After a restart, an open problem that was already reported isn't reported again. |

## Early warning: the home's normal, and a drift before it is a fault

Most faults don't start as faults. A line that begins to fail loses a packet
now and then, then more; an ISP's link filling up adds a few milliseconds in the
evenings, a little more each week. Every check measures what a fault is later
judged on: the next hop's delay, the internet's delay and loss, and the time
DNS takes. The gateway learns what is normal for this home and says when the
line drifts away from it, while it is still below what Diagnose calls a fault.
The ISP hears of it (`EarlyWarning!`) with what is worse, by how much, and, for a
steady drift, when it would be a fault at that rate.

| | |
|---|---|
| The normal | Per hour of the day: evenings are busier than nights. Each hour's average and typical deviation, over about two weeks; an hour with few samples leans on the whole day's. Learned from the regular checks (not the quick ones that confirm a problem). Nothing is learned while a problem is open, and very little while a warning is, so a fault never becomes the normal. |
| A shift | Each check counts how many typical deviations it is worse than normal, at most 3 (a blip stays a blip), less an allowance of 0.5; these add up (a CUSUM). At 12 it is a warning: about an hour of clearly worse, or about four hours of a bit worse. It must also be a real difference: 10 ms on the next hop, 15 ms on the internet, 2% loss, 40 ms on DNS, 20% of the speed. |
| A trend | The day's average, day by day: worse for 4 days or more along a straight line that fits (R² 0.6 or better). If at that rate it would be a fault within 14 days (100 ms to the next hop, 150 ms on the internet, 5% loss, 300 ms DNS, or below the speed the line must reach), that is a warning, with `PredictedFaultDays`. |
| Quiet first | No warning during the first `care.learn_days` (3), nor about something that is already a problem. |
| Told to the ISP | `Device.X_AIGW_.EarlyWarning!` (Metric, Area, Kind `Shift` or `Trend`, Summary, Advice, Normal, Current, PredictedFaultDays, Since); `EarlyWarningCleared!` (Metric, Area, Since, DurationSeconds, BecameProblem) when it is over. |
| Did it help? | When a problem starts in the area of an open warning (or of one that ended less than 48 hours before), it counts as seen coming: `ProblemsWarnedOf`, and how long before, `MeanWarningLeadSeconds`. |

The rules are pure and unit-tested on weeks of simulated checks
(`tests/test_warn.c`). The tests include a month of a normal line, busier in the
evenings, with no warning. A drift of 6 ms a day is warned of after about 2.6
days, at +16 ms, far below the 150 ms fault. Packet loss creeping up to about 3%
is warned of within a few hours, and a speed falling from 95 to 60 Mbit/s after
two nights.

## The nightly speed test

Diagnose measures delay and loss, not speed, so a line that delivers 20 of
its 100 Mbit/s goes unseen. With `care.speed_url` set (an HTTP download on the
ISP's own speed test server), the gateway runs a TR-143 download
([tr143.md](tr143.md)) every night, at a random minute of `care.speed_hour` (3 in
the morning), for `care.speed_seconds` (8) at most.

- Its speed is learned like the rest: a speed lower than usual is an early
  warning, even when nobody said what the line should reach.
- With `care.speed_mbps` (the speed sold), a test below `care.speed_min_pct`
  (50%) of it is checked again 15 minutes later; two in a row are a problem,
  `LINE_SLOW_SPEED` (Internet), reported at once. While it lasts, a test every
  hour; one fine test and it is over.
- A test that fails (the server can't be reached) is tried again twice that
  night; it isn't counted as a slow line.
- `agctl care --speed` runs a test now.

## Seeing it

At home:

```
$ agctl care
Proactive care: the gateway checks itself every 10m (last 4m ago)
  now       OK: no problem found
  warning   InternetLatency (Internet, trend) for 1d 6h
            The internet's delay has got worse for 5 days (now 46 ms, normal 18 ms; 6 ms a day): at this rate it is a fault (150 ms) in about 14 days.
  advice    Check the ISP's network beyond the home: congestion is growing at these hours.
  normal    at this hour: next hop 1.2 ms, internet 18.4 ms, loss 0.3%, DNS 9.8 ms, speed 94.1 Mbit/s (learned over 21 days)
  speed     93.8 Mbit/s, 9h ago; the line's speed 100, at least 50% required; next in 14h 51m
  last day     experience score 98% (1d 0h watched, 144 checks); 1 problem, mostly DNS: reported to the ISP 1, fixed by AIGW 0, 12m with a problem; 1 early warning; speed 93.8 Mbit/s on average (93.8 at least, 1 test)
  last 7 days  experience score 99% (7d 0h watched, 1008 checks); 3 problems, mostly WiFi: reported to the ISP 1, fixed by AIGW 2, 41m with a problem; ...
```

(The numbers are illustrative.) `agctl care --check` runs a check now,
`agctl care --speed` a speed test, and `agctl -j care` gives the same as JSON.
The `ag.care` ubus event (`found`, `changed`, `reported`, `cleared`, `warning`,
`warning-cleared`) carries every step, for `agctl watch` and plug-ins.

For the ISP, in the data model (version 1.7):

| Path | |
|---|---|
| `Device.X_AIGW_.Care.Enable`, `Status` (`OK` / `Problem`), `LastCheck` | |
| `Device.X_AIGW_.Care.Problem.` | `Code`, `Area`, `Severity`, `Summary`, `Advice`, `Since`, `ReportedToISP` (empty when there is no problem) |
| `Device.X_AIGW_.Care.Stats.Day.`, `Week.`, `Month.` | `ExperienceScore`, `Checks`, `Problems`, `ReportedToISP`, `FixedByAIGW`, `ProblemSeconds`, `MainCause`; v0.29.1: `WatchedSeconds`; v0.29.2: `ClearedByItself`; v0.29: `EarlyWarnings`, `ProblemsWarnedOf`, `MeanWarningLeadSeconds`, `SpeedTests`, `SpeedTestsLow`, `MeanSpeedMbps`, `MinSpeedMbps` |
| `Device.X_AIGW_.Care.EarlyWarning.` | `Status` (`None` / `Learning` / `Warning`), `Metric`, `Area`, `Kind`, `Summary`, `Advice`, `Since`, `Normal`, `Current`, `PredictedFaultDays` (-1: not known) |
| `Device.X_AIGW_.Care.Normal.` | at this hour: `NextHopLatency`, `InternetLatency` (ms), `InternetLoss` (%), `DNSLatency` (ms), `DownloadSpeed` (Mbit/s); `LearnedDays` |
| `Device.X_AIGW_.Care.Speed.` | `Enable`, `Status` (`None` / `OK` / `Low` / `Error`), `LastTest`, `LastMbps`, `ExpectedMbps` |
| Events | `Device.X_AIGW_.ProblemDetected!`, `ProblemCleared!`, `EarlyWarning!`, `EarlyWarningCleared!` |

The codes are Diagnose's ([diagnose.md](diagnose.md)), so a help desk counts
them the same way whether the customer or the gateway asked.

## Settings (`/etc/config/aigw`, section `care`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no checks, no events, no score |
| `interval` | `600` | seconds between checks while all is fine (10 to 86400) |
| `recheck` | `60` | seconds to the next check after one found something (1 to 3600) |
| `wifi_grace` | `1800` | seconds Self-Healing Wi-Fi has before a Wi-Fi problem is reported (0: at once) |
| `warn` | `1` | early warnings (`0`: the normal is still learned and shown, not told) |
| `learn_days` | `3` | days of learning before the first warning (0 to 60) |
| `speed_url` | `''` | the nightly speed test's HTTP download (the ISP's server); `''`: no test |
| `speed_hour` | `3` | local hour it runs in, at a random minute (spread over a fleet) |
| `speed_seconds` | `8` | how long it downloads at most (TR-143 TimeBasedTestDuration) |
| `speed_mbps` | `0` | the line's speed (what was sold); `0`: not known, no `LINE_SLOW_SPEED` |
| `speed_min_pct` | `50` | below this share of it, on two tests in a row: `LINE_SLOW_SPEED` |

What a check sends: 10 echo requests to the next hop and to each
`diag.targets` address, and one DNS query to the gateway's own DNS. At the
default interval that is about 6 packets a minute.

## Tests

- `tests/test_care.c` (unit): a blip is no problem; the line and DNS are
  reported at once, once; Wi-Fi is reported only after the grace time, and is
  counted as fixed by AIGW when it ends before that after AIGW changed
  something, else as cleared by itself; a Wi-Fi problem that turns
  out to be the line is reported at once; time while agd was off isn't problem
  time; the score, and the days.
- `tests/care-test.sh` (CI): with no DNS server, the gateway's own checks find
  `DNS_FAILING`, report it (`ProblemDetected!`, `Care.Status = Problem`) and
  remember it over a restart without reporting it again. When names resolve,
  it is over (`ProblemCleared!`). The checks don't replace `LastDiagnosis.`,
  and a Diagnose asked about a device during a check waits for it instead of
  being refused.
- `tests/hwsim-test.sh` (CI, simulated radios): the same with real access
  points and a client, and the day's score with `MainCause` DNS.
- `tests/test_warn.c` (unit, v0.29): weeks of a simulated line, a check every
  10 minutes and a speed test each night. Quiet when it is normal (a month,
  busy evenings, spikes). Warned of: a sub-fault shift, a slow drift (then a
  trend, with when it is a fault), creeping loss, slower DNS, a speed drop, a
  falling speed. A fault isn't learned.
- `tests/warn-test.sh` (CI, v0.29): speed tests against a local server teach
  the normal speed; a slower one is an early warning (`EarlyWarning!`), over
  when it is fast again; the normal survives a restart; with the line's speed
  set, two slow tests are `LINE_SLOW_SPEED`, reported, then over.
