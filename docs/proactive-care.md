# AIGW proactive care: fixed, or reported, before the customer calls

[Diagnose](diagnose.md) answers "my internet is slow" when someone asks.
Proactive care asks by itself. The gateway runs the same checks on its own,
deals with what it can fix, and tells the ISP about the rest, often before the
customer notices. Every check also counts towards a daily **experience
score** for the home, which an ISP can collect across its homes.

What an ISP can measure with it:

- **calls avoided:** problems the gateway fixed itself (`FixedByAIGW`), and
  problems the help desk already knew about when the customer called
  (`ProblemDetected!` before the call);
- **time to find a fault:** the event already says where the problem is
  (the line, the ISP's network, DNS or Wi-Fi), with a code and advice;
- **experience per home:** the share of the day when the line, DNS and Wi-Fi
  were all fine, with the main cause on the bad days.

## How it works

| | |
|---|---|
| Checks | Diagnose without a device (the line, DNS and every access point), quietly: it doesn't replace `LastDiagnosis.` or send `DiagnosisDone!`, which stay for the diagnoses someone asked for. It runs every `care.interval` (10 minutes). When Self-Healing Wi-Fi turns a radio Poor, it checks within seconds. If someone asks Diagnose about a device during a check, their diagnosis runs right after it, about 3 seconds later. |
| A problem | A verdict of warning or worse on 2 checks in a row; one bad check is a blip. After a bad check, the next one comes after `care.recheck` (60 s), to confirm it quickly and then to see it clear. A problem is over after 2 fine checks in a row. |
| Who deals with it | **The line, the ISP's network, DNS:** the ISP's. Reported at once. **Wi-Fi:** AIGW's own, through Self-Healing Wi-Fi (channel trials, keeping a device on 2.4 GHz). Reported only if it is still there after `care.wifi_grace` (30 minutes); otherwise it counts as fixed by AIGW. **A device, or an AIGW action on one:** nobody's to report. |
| Telling the ISP | `Device.X_AIGW_.ProblemDetected!` (Code, Area, Severity, Summary, Advice, Since), once per problem. It is sent again only if the cause changes, for example when the Wi-Fi problem turns out to be the line. When the problem is over, `ProblemCleared!` (Code, Area, Since, DurationSeconds). Both go over USP and TR-069 like the other events, and to prplOS through aigw-amx. |
| The score | Per day, for 31 days: checks, checks without a problem, bad checks by area, problems, reported, fixed by AIGW, and seconds with a problem open. `ExperienceScore` = % of checks without a problem. `MainCause` = the area with the most bad checks. |
| Restarts | The day counts and an open problem are saved with agd's state. After a restart, an open problem that was already reported isn't reported again. |

## Seeing it

At home:

```
$ agctl care
Proactive care: the gateway checks itself every 10m (last 4m ago)
  now       OK: no problem found
  last day     experience score 98% (144 checks); 1 problem, mostly DNS: reported to the ISP 1, fixed by AIGW 0, 12m with a problem
  last 7 days  experience score 99% (1008 checks); 3 problems, mostly WiFi: reported to the ISP 1, fixed by AIGW 2, 41m with a problem
  last 30 days experience score 99% (4320 checks); ...
```

(The numbers are illustrative.) `agctl care --check` runs a check now, and
`agctl -j care` gives the same as JSON. The `ag.care` ubus event (`found`,
`changed`, `reported`, `cleared`) carries every step, for `agctl watch` and
plug-ins.

For the ISP, in the data model (version 1.6):

| Path | |
|---|---|
| `Device.X_AIGW_.Care.Enable`, `Status` (`OK` / `Problem`), `LastCheck` | |
| `Device.X_AIGW_.Care.Problem.` | `Code`, `Area`, `Severity`, `Summary`, `Advice`, `Since`, `ReportedToISP` (empty when there is no problem) |
| `Device.X_AIGW_.Care.Stats.Day.`, `Week.`, `Month.` | `ExperienceScore`, `Checks`, `Problems`, `ReportedToISP`, `FixedByAIGW`, `ProblemSeconds`, `MainCause` |
| Events | `Device.X_AIGW_.ProblemDetected!`, `Device.X_AIGW_.ProblemCleared!` |

The codes are Diagnose's ([diagnose.md](diagnose.md)), so a help desk counts
them the same way whether the customer or the gateway asked.

## Settings (`/etc/config/aigw`, section `care`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no checks, no events, no score |
| `interval` | `600` | seconds between checks while all is fine (10 to 86400) |
| `recheck` | `60` | seconds to the next check after one found something (1 to 3600) |
| `wifi_grace` | `1800` | seconds Self-Healing Wi-Fi has before a Wi-Fi problem is reported (0: at once) |

What a check sends: 10 echo requests to the next hop and to each
`diag.targets` address, and one DNS query to the gateway's own DNS. At the
default interval that is about 6 packets a minute.

## Tests

- `tests/test_care.c` (unit): a blip is no problem; the line and DNS are
  reported at once, once; Wi-Fi is reported only after the grace time, and is
  counted as fixed by AIGW when it ends before that; a Wi-Fi problem that turns
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
