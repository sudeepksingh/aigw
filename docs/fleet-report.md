# The ISP's fleet report

Each gateway's [proactive care](proactive-care.md) already knows how its home
is doing: an experience score, the problems it found, fixed or reported, its
early warnings and nightly speeds. cortex, AIGW's optional service outside the
home, puts every home's together for the ISP. It shows what AIGW is worth on
the network: calls that never came, faults known before the customer called,
outages seen as outages, and the homes a help desk should look at first.

```
$ python3 -m aigw_cortex fleet --days 7
Fleet report, 2026-11-09 to 2026-11-15: 1204 of 1210 homes reporting, 6 silent
  experience   97.8% on average (median 99.3%), +0.6 from the 7 days before; 1081 good, 97 fair, 26 poor homes; 1213632 checks
  problems     412 found by the gateways: 167 fixed by AIGW itself (no call needed), 58 cleared by themselves, 221 reported to the ISP before anyone called; 391.5 h in all; 9 open now
               bad checks by cause: Internet 15232, WiFi 6120, DNS 1811
               codes: WIFI_CONGESTION 151, ISP_DOWN 96, LINE_LOSS 61, DNS_FAILING 38, ...
  warnings     58 early warnings; 21 problems seen coming, 31.4 h ahead on average; 12 open now
  speed        1188 homes tested, median 94.2 Mbit/s; 31 homes below their line's speed (64 tests)
  OUTAGE       38 of 212 homes in exchange-north from 2026-11-14 19:02 (within 6 min: ISP_DOWN 35, LINE_DOWN 3): the ISP's network, not the homes; over at 2026-11-14 19:47
  attention
    home-0412 (exchange-east): LINE_LOSS (Internet) for 5 h, reported: 22% of packets to the next router (10.0.0.1) are lost: ...
    home-0877 (exchange-north): early warning, NextHopLatency: a fault in about 4 days
    home-0090 (exchange-west): experience 61.2%, mostly WiFi
    home-1133: not heard from for 3 days
```

(The numbers are illustrative.) `--json` gives the same as data, `--group G`
one group only. The MCP server's `fleet_report` tool hands it to an AI
assistant, read-only.

## What it says

| Section | |
|---|---|
| experience | Each home's score over the period (the time without a problem, of the time its gateway watched; days from gateways before v0.29.1 have no times and count by checks). The fleet's average and median, and the change from the period before. Homes good (95% or more), fair (80 to 95) or poor. |
| problems | Found by the gateways; fixed by AIGW itself (Wi-Fi problems over before they had to be reported, after AIGW changed something for them: no call needed); cleared by themselves (over though AIGW changed nothing: not counted as its fix); reported to the ISP the moment they were found; the hours with a problem; bad checks by cause; the codes. |
| warnings | Early warnings, the problems they saw coming, and how long before on average. |
| speed | The nightly speed tests: the median, and the homes below their line's speed. |
| outages | v0.39: the [area outages](area-outages.md) open or begun in the period: many homes behind one uplink whose line went down, slow or lossy within 15 minutes of each other (at least 3 homes and 10% of the uplink's), kept as incidents with their id, uplink, kind, confidence and homes, closed when 80% recovered. The homes on the attention list that are part of one say so. |
| signals | v0.32: each home's open [signals for the ISP](isp-signals.md) (a faster plan, smart queueing, mesh Wi-Fi, a customer at risk, a line to repair), how many homes, by product and how many of high confidence, and the list in the order a team works through it: repairs first, then homes at risk, then sales by confidence. |
| rollouts | v0.34: each [staged firmware rollout](staged-rollout.md) still going, or changed in the period: its version, state, wave, how many homes kept it or got worse, and why it waits or stopped, with counts and no names. |
| help_desk | v0.37: the [help desk's briefs](help-desk.md) cortex answered in the period: how many, for how many homes, by fault (`LINE`, `HOME_WIFI`, `DEVICE`, `OUTSIDE`, `NONE`, `UNKNOWN`), how many advised a technician (and only if it persists), how many had a remote fix and how many were proposed; and the briefs the gateways answered themselves (USP, TR-069, agctl), from their own counts. |
| attention | First the open problems (oldest first), then the open early warnings (the nearest fault first), then the poor scores, then the gateways that went quiet for a day. |

## Groups

Give each gateway the area it is in, e.g. the exchange, OLT or town. Outages
are looked for within a group:

```
python3 -m aigw_cortex enroll home-0412 --group exchange-east
python3 -m aigw_cortex group home-0412 exchange-east
```

## What reaches cortex

The bridge (AIGW's plug-in that talks to cortex) adds proactive care to each
report. It sends the score, the problems, the early warning and the speed test,
and the last 8 days' rows. Nothing in it names a person or a device, except
that a Wi-Fi problem's summary can name the device that suffers. That summary
leaves the home only with the bridge's `devices = names` (the default); with
`anonymous` or `none` it stays home. Each day's row is replaced as the day
fills, so a missed report is covered by the next one.

## Tests

- `tests/test_cortex.py` (unit): 12 homes in two groups.
  - A worse week than the one before; homes good and poor.
  - What AIGW fixed and what was seen coming.
  - An outage: six homes of one group within 10 minutes, not the one home
    alone in the other group. It closes as homes come back.
  - The attention order; one group only; the MCP tool.
  - A gateway older than 0.29: "no care data yet".
- `tests/outside-test.sh` (CI): a gateway's care reaches cortex through the
  real bridge, and `aigw_cortex fleet` reports it.
