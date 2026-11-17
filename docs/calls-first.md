# AIGW calls and games first: priority for real-time traffic, and the proof

A video call or an online game needs very little bandwidth: a few hundred
kbit/s. But it needs it on time. When the TV starts a big download, the line
fills up and the call's packets wait behind the download. Voices break up,
video freezes, and the game lags. Smart queueing ([smart-queueing.md](smart-queueing.md))
keeps that wait short. Calls and games first goes one step further: inside
the shaped queue, the call's packets go ahead of the download.

Then AIGW proves it. When the call ends, the home's [timeline](why-slow.md)
says how it went: the delay, jitter and loss while the call lasted, and
whether the line was full at the time. At home, with names:

```
$ agctl calls
Calls and games first: they go first inside smart queueing on cfw9748
  now       no call or game
  last      The call on laptop stayed smooth for 13 s while the line was full for 13 s (tv downloading, 9.7 Mbit/s):
            the internet answered in 1 ms, jitter 1 ms, no loss. It went first in smart queueing. (ended 17:36)
  so far    2 calls, 0 games; 2 met a full line, 1 of them smooth; 1 went first
  the ISP   sees the reports, without device or app names
```

And for the ISP, over USP or TR-069, without names:

```
Device.X_AIGW_.CallReport!  Kind=Call  Verdict=Smooth  Prioritized=true  FullLineSeconds=13  LineKbps=9700
  Summary="A call stayed smooth for 13 s while the line was full for 13 s (9.7 Mbit/s): the internet answered
           in 1 ms, jitter 1 ms, no loss. It went first in smart queueing."
```

(Both are from the integration test, on real veth links, with a real 10
Mbit/s stream on a line sold at 8 and real shaping.)

## How

| | |
|---|---|
| Noticed | Each time agd looks at the connection counters (`calls.poll`, every 10 s), it checks every UDP connection a device has started. A call or a game sends packets in both directions, at a steady rate, for at least two looks in a row. It needs at least 5 packets a second in each direction, an average packet of 1300 bytes or less, and 10 Mbit/s or less in each direction. The busier direction can carry at most 25 times more packets than the other, and the slowest look must reach at least 40% of the fastest. DNS, QUIC (port 443, the web) and VPN tunnels (WireGuard, OpenVPN, IPsec) are never counted, because a tunnel may carry a download. No packet contents are read. |
| Which one | A known port tells which app it is: STUN/TURN 3478–3481 (Teams, WhatsApp, Meet, FaceTime), Zoom 8801–8810, Google Meet 19302–19309, FaceTime 16384–16402, Xbox Live 3074, Steam 27000–27050, Riot Games 7000–7500, Minecraft 19132. So does the name the device looked up through agdns, for example `zoom.us`, `teams.microsoft.com`, `whatsapp.net`, `xboxlive.com`, `riotgames.com` or `epicgames.dev`. A port or a name on its own is not enough, because an idle app keeps its connection open without a call. Without a hint, big packets or more than 300 kbit/s mean a video call. Otherwise the kind is "call or game". All of a device's real-time connections count as one call. |
| First | Calls only go first while AIGW's own smart queueing shapes the line (a kept trial, or `agctl sqm --on`). Then `aigw-sqm prio` adds one tc flower filter for each far end of the call. Uploads match on the destination address and port, and downloads (on the ifb) on the source address and port, so NAT doesn't matter. With cake, the filter puts the packets in diffserv4's voice tin (the tins are on only while there is a call). With htb, it puts them in the priority class `1:5`: 30% of the line is guaranteed to it, and it can use all of the line when the line is free. The filters follow the calls and are removed when the calls end. When smart queueing is removed, the filters go with it. |
| Never without SQM | Calls and games first never shapes anything by itself, and never turns smart queueing on. Smart queueing has its own policy for that (`bufferbloat`, [smart-queueing.md](smart-queueing.md)). When smart queueing is off, calls are still noticed and reported, and the report says that smart queueing was off. |
| Proved | A call is over when, for 30 s, none of its connections still looks like a call. A muted call still counts while packets flow both ways. Calls shorter than `calls.min_s` (60 s) are not reported. The timeline's samples taken while the call lasted give the verdict. When the line was full in some samples, only those count. **Smooth** means the internet answered in 150 ms or less (the median), the jitter was 30 ms or less, and 1% or less of the echoes were lost. Otherwise the call was **rough**. The jitter is the average change in delay from one sample to the next. |

The delay, jitter and loss come from the gateway's own probes to the
internet (three echo requests in each timeline sample). They do not come
from the call's packets, because AIGW doesn't read them. The probes take
the same line and the same queues as the call. With priority, the call's
packets wait less than the probes do, so the probes give an upper bound for
the call. A gateway with hardware flow offload sends traffic past the
qdiscs, so priority has no effect there. The report stays honest either way.

## What the ISP sees

The report has no device names, app names or call contents. The ISP sees
only the kind (Call, Game or Realtime), the numbers and the words. Only
calls or games that met a full line, or that were rough, raise
`CallReport!`. The ISP doesn't need the others. With `calls.share 0` the
ISP sees nothing: the data model shows only `Enable` and `Shared`, and its
`Status` is `NotShared`. The calls are also left out of the story that
`WhySlow()` returns to the ISP. `agctl calls` and `agctl why` still show
everything at home.

- `Device.X_AIGW_.CallsFirst.` (data model 1.14):
  - `Enable`, `Shared`, `Priority`;
  - `Status`: `Disabled`, `Idle`, `Active` (a call, nothing shaped for it), `Prioritized` (a call that goes first) or `NotShared`; value changes are notified;
  - `ActiveCalls`;
  - the counts since the start: `Total`, `OnFullLine`, `SmoothOnFullLine`, `Prioritized`;
  - the last one: `Last.Kind`, `Last.Start`, `Last.DurationSeconds`, `Last.FullLineSeconds`, `Last.LineKbps`, `Last.LatencyMs`, `Last.JitterMs`, `Last.LossPercent`, `Last.Prioritized`, `Last.Verdict` (`Smooth`, `Rough`, `Unknown`), `Last.Summary`.
- The event `Device.X_AIGW_.CallReport!` has `Kind`, `Verdict`, `DurationSeconds`,
  `FullLineSeconds`, `LineKbps`, `LatencyMs`, `JitterMs`, `LossPercent`,
  `Prioritized` and `Summary`.

`SmoothOnFullLine` out of `OnFullLine` is the number a help desk wants:
how often the home's calls stayed smooth while the line was busy.

## At home

- `agctl calls`, `ubus call ag calls`: the calls now (device, app, far
  ends, and whether they go first), the last report with names, and the
  counts.
- The ubus event `ag.call` (`start`, `end`) carries the device and the
  words.
- The timeline keeps a short line for each call ("Call on laptop: smooth,
  13 s (line full 13 s): 1 ms, jitter 1 ms, 0.0% lost; went first"). So
  [`agctl why`](why-slow.md) shows the call when someone asks about that
  evening. The line names the device, as the timeline's other lines do,
  but never the app.

## Settings (`/etc/config/aigw`, section `calls`)

| Option | Default | |
|---|---|---|
| `enabled` | `1` | `0`: calls and games are not noticed or reported |
| `priority` | `1` | `0`: reports only, nothing goes first |
| `share` | `1` | `0`: the ISP sees nothing (data model, `CallReport!`) |
| `poll` | `10` | seconds between looks at the connection counters (agd looks at least this often while it is on) |
| `min_s` | `60` | calls or games shorter than this are not reported |

It needs the flow sensor (`flow.enabled`, on by default). Priority uses
`sqm.handler` (`aigw-sqm prio <iface> [udp,<ip>,<port> ...]`). On OpenWRT the
package also brings `kmod-sched-flower`. Its `act_skbedit` comes with
`kmod-sched-core`.

## Tests

- `tests/test_calls.c` (unit):
  - Zoom, STUN, Teams, WhatsApp, Xbox Live and Riot Games found by their ports or names, and calls with no hint found by their traits;
  - not a call: one look only, QUIC, a VPN, TCP, keep-alives only, a one-way stream, a transfer, big packets, bursts;
  - how it went: smooth on a quiet line; smooth on a full line, judged on the full samples (jitter 8 ms); rough with bufferbloat (11.1% lost) and smart queueing off; unknown when nothing answered;
  - the words: names at home, none for the ISP.
- `tests/calls-test.sh` (CI): three network namespaces (home, gateway, internet). The TV streams 10 Mbit/s on a line sold at 8 Mbit/s, and the laptop makes a "call" (50 packets a second each way to port 3478):
  - with smart queueing on: the call is noticed, its far end goes first, and its packets are counted in cake's voice tin or htb's class 1:5;
  - when the call ends: it is reported smooth, with the full line, on the timeline, in `agctl calls` (with names), and in the data model and `CallReport!` (without names); the filters are removed;
  - without smart queueing and with netem bufferbloat: the next call is noticed, nothing is shaped, and it is reported rough;
  - after a restart with `share 0`: the ISP sees nothing more.

The PC access point's WSL kernel has no cake, so the test uses htb there.
The GitHub runners have cake.
