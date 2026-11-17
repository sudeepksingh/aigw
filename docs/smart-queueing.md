# AIGW self-tuning smart queueing: bufferbloat found, fixed and proved

When a home's line is full (a game update, a cloud backup, a 4K stream), the
router's queues fill up and everything else waits behind the download.
Video calls stutter, games lag and pages hang, even though nothing is
broken. The fix has been known for years: smart queueing (SQM). It shapes
the line just below its speed, so the queue stays in the gateway, where cake
or fq_codel keeps it short and fair. But someone has to know to turn it on,
and what speed to give it. Most homes never do, and an ISP's help desk
can't see the problem.

AIGW OS sees it, fixes it, and checks that the fix worked:

```
$ agctl sqm
Smart queueing: on
  line      sqw0, shaped at 7.2 Mbit/s down, not shaped up
  now       on htb 7200 0
  before    while the line was full (4 samples): the internet in 200 ms, downloads 10.4 Mbit/s
  after     6 of 6 full samples: the internet in 1 ms, downloads 11.7 Mbit/s (kept)
  verdict   kept: while the line was full the internet answered in 1 ms (before 200 ms), downloads 11.7 Mbit/s (before 10.4)
```

(From the integration test, on a real veth link, with real bufferbloat from
netem and the real shaping.)

## How

| | |
|---|---|
| Found | Every sample of the home's [timeline](why-slow.md). In the last hour (60 samples), the line was full and the internet answered slowly meanwhile on `sqm.detect` (10) samples or more, and on half the full samples. A line well used, but still fast, isn't bloat. "Full" is 80% of the line's sold speed (`care.speed_mbps`), else of the last nightly speed test, else of the most seen. "Slow" is against the home's normal for the hour. |
| Decided | The alert `bufferbloat` and the policy `bufferbloat` (built in, even for a configuration from before it) make an `sqm` decision, through the usual guardrails. In **advise** mode (the default) it is a proposal for you (`agctl decisions`, `agctl approve`). In **enforce** mode it is tried by itself, within the hourly limit. |
| Applied | `aigw-sqm` (`sqm.handler`) shapes the WAN, which is the default route's interface (`why.wan`). Downloads are shaped at 90% of the line's speed (85% of the most seen when the speed isn't known), and uploads at 85% of the most seen. It uses cake when the kernel has it, else htb with fq_codel. Downloads go through an ifb device. An SQM AIGW didn't set up (sqm-scripts, a person's own tc) is left alone. |
| Judged | Once the line has been full on `sqm.trial` (30) samples since it went on (the first 2 aren't counted), the trial compares two things while the line was full: the internet's delay and the download speed, each against the day before. **Kept** when the delay is clearly lower (60% of before or less, or within 30 ms of normal) and downloads kept 75% of their speed. A kept decision has no end: it is restored after agd restarts, and put back when the WAN comes back. **Tried** otherwise: it is undone, and nothing is proposed again for `sqm.hold` (a day). The trial ends when the decision's time limit is reached (a day). |
| By hand | `agctl sqm --on` asks for it at the line's speed, and `agctl sqm --off` ends it (or `agctl undo`). A person's own decision is never judged or undone by AIGW. |

The ISP signal `SMART_QUEUE` ([isp-signals.md](isp-signals.md)) isn't
raised while AIGW runs smart queueing itself: there is nothing left to sell.
A gateway with hardware flow offload sends traffic past the qdiscs. There,
the trial measures no change, and SQM is undone. The trial is honest either
way.

## Seeing it

- `agctl sqm`, `ubus call ag sqm`: the status, the rates, what `aigw-sqm`
  reports, and the trial's before and after.
- `Device.X_AIGW_.SQM.` (data model 1.13):
  - `Enable`;
  - `Status`: `Off`, `Proposed`, `Trial`, `On`, `Tried` or `Unavailable`; value change notified;
  - `Interface`, `DownloadKbps`, `UploadKbps`, `Since`;
  - `LatencyBeforeMs`, `LatencyAfterMs`, `Summary`.

  The decision itself is in `Device.X_AIGW_.Decision.{i}.` (and `DecisionChanged!`).
- The timeline counts an SQM decision as an AIGW change. When a problem
  starts soon after one, [`agctl why`](why-slow.md) says so.

## Settings (`/etc/config/aigw`, section `sqm`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: no proposals, no trials |
| `detect` | `10` | full-and-slow samples in the last hour before it is bloat |
| `trial` | `30` | full samples after it went on before it is judged |
| `hold` | `86400` | seconds after a trial (or a "no") before it is proposed again |
| `handler` | `aigw-sqm` | the program that shapes the line (an OEM's own: `apply <iface> <down> <up>`, `remove <iface>`, `status <iface>`, `check`) |

On OpenWRT the package brings `tc-tiny`, `kmod-sched-core`,
`kmod-sched-cake` and `kmod-ifb`. On a PC, the kernel's htb, fq_codel and
ifb (cake if it has it). The policy is `config policy 'bufferbloat'` (action
`sqm`).

## Tests

- `tests/test_sqm.c` (unit):
  - bloat found: full and slow for 20 minutes;
  - no bloat when the line is full but fast, slow only now and then, or slow two hours ago;
  - the rates: from the sold speed, from the most seen, or nothing to go by;
  - the trial: kept at 35 ms after 180 ms; waiting with too few samples;
  - undone when no better, or when downloads slowed too much;
  - with no before, kept only when plainly fine.
- `tests/sqm-test.sh` (CI): a network namespace behind a veth WAN, a sender
  filling the line, netem making bufferbloat, agd sampling every 2 s:
  - **advise:** bloat seen and proposed; once approved, the real `aigw-sqm` shapes the WAN;
  - the delay goes, so SQM is kept, with before and after; restored after a restart; `agctl sqm --off` takes it away;
  - **enforce:** bloat again with no improvement, so it is tried by itself and undone;
  - `Device.X_AIGW_.SQM.` checked at each step.
