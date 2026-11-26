# Per-app experience

Customers don't call about packet loss. They call because "my video call froze" or "Netflix
keeps buffering". Per-app experience lets the gateway answer in those terms: it recognises the
sessions of each kind of app, estimates how each one went for the person using it, and, for
every bad moment, says why, from what the gateway already knows about the Wi-Fi, the line and
the ISP.

```text
$ agctl apps
Per-app experience: Active; server names help tell apps apart (families only, never a name kept); the ISP sees kinds, numbers and causes, never names
  rules     12 kinds, 30 families, built in
  scores    calls 98% (2 h 10 min), streaming 100% (5 h), gaming 91% (1 h 20 min) over 7 days
  now       video stream on tv (Netflix) since 20:05, 52 min, Good
  moments   The video call on laptop (Zoom) at 10:02 froze for about 8 s: the 2.4 GHz Wi-Fi channel was congested; then at 10:04 AIGW moved 2.4 GHz (wlan0) from channel 11 to 6, and the 3 calls since were fine. (confidence 80%)
  sessions  video call on laptop (Zoom), 10:31-11:02, 31 min, 0 bad
  cost      9 of 160 connections watched, 2 sessions, 96 KB of memory, 40 us a look
```

It is a compile-time feature, `AIGW_FEATURE_APPQOE` ([features.md](features.md)), that needs
the timeline (`WHY`). It is in the **full** and the **small** profile: the small profile is
"what an ISP needs most", and these are the calls an ISP's help desk gets most.

## How it works

```text
 conntrack counters (agd/flow.c, every apps.poll s)        the timeline (agd/why.c, every minute)
        │                                                    line, internet delay/loss/jitter, DNS,
        ▼                                                    Wi-Fi health, who downloaded, events
 kind of each connection ── rules file + families                   │
        │                                                          │
        ▼                                                          ▼
 sessions (a device's connections of one kind) ──► each minute judged in the app's terms
                                                          │ bad minutes in a row
                                                          ▼
                                                    a bad moment ──► its cause (the timeline's
                                                          │          verdict for that device then)
                                                          ▼
                     timeline, agctl apps, help-desk brief, Device.X_AIGW_.AppExperience., AppMoment!
                                                          │ later
                                                          ▼
                     what AIGW did next (a channel moved, a device steered, smart queueing on),
                     and how the same kind of session went since
```

### Recognising sessions

Every look at the connection-tracking table (`apps.poll`, 10 s) gives each TCP and UDP
connection's packets and bytes each way since the look before. Its last looks (up to 6) give
its traits:

| Trait | What it is |
|---|---|
| protocol | UDP, TCP, or QUIC (UDP to port 443) |
| `down`, `up` | the mean rate each way, kbit/s |
| `pps` | packets a second, both ways |
| `pkt` | the mean packet of the busier way, bytes |
| `ratio` | up bytes per 100 down bytes |
| `duty` | the share of looks with traffic |
| `steady` | how much its packet rate varies from look to look |
| `life` | how long the connection has been open |
| port | the far end's port (STUN, Zoom, Meet, FaceTime, Xbox Live, Steam, ...) |
| family | the server-name family (below), from the name the device looked up or its TLS/QUIC handshake |

A table of rules turns them into a kind, first match wins: video call, voice call, screen
share, video streaming, cloud gaming, online gaming, large download, smart-home device.
Browsing is a device opening new web connections (4 or more a minute, `browse` in the rules).

A family ("Zoom", "Netflix") makes a kind surer (+25% confidence) and decides between kinds
whose traits look alike (a 6 Mbit/s QUIC flow from YouTube is a stream, not a download). A
name alone never makes a real-time kind: Zoom's web connection is not a call. A name alone
does keep a stream, a download or a smart-home connection while it is idle (a player whose
buffer is full fetches nothing for a while).

A device's connections of one kind are one session. A real-time session ends 30 s after its
traffic stops; a stream, a download or browsing after 5 minutes idle; a smart-home device after
an hour away.

#### The rules file

The rules are a text file, `/etc/aigw/apps.rules` (`apps.rules`). When it is missing, AIGW OS
uses its built-in rules, which are [config/apps.rules](../config/apps.rules) without its comments: copy it,
change it, put it on the gateway, and run `agctl apps --reload` (or `ubus call ag apps
'{"reload":true}'`). No new code is needed to recognise a new app or to tune a kind.

```text
kind VideoCall proto=udp port=3478-3481,5349,8801-8810,19302-19309,16384-16402,9000 down=200-20000 pps=30- conf=75
kind LargeDownload proto=tcp,quic down=6000- ratio=0-6 pkt=1000- duty=100- steady=35 looks=4 conf=50
family Netflix VideoStreaming nflxvideo.net netflix.com nflxso.net
family Xbox_Cloud CloudGaming gssv-play-prod.xboxlive.com xcloud.xboxlive.com
browse 4
```

A line AIGW can't read is skipped and the first one is shown (`agctl apps`, and the log); a file
without any rule falls back to the built-in rules. At most 32 rules, 64 families and 192 server
name suffixes. The longest matching suffix wins, on a label boundary (`notzoom.us` is not
Zoom).

### How each session went

Every `why.interval` (a minute) each session's minute is judged in its own terms, from its own
counters and the timeline's sample of the home: the internet's round trip and loss (3 echo
requests), its change from the sample before (jitter), the device's own Wi-Fi hop (Self-Healing
Wi-Fi's probes, when built), DNS, and whether the line was full.

| Kind | Estimate | Bad when |
|---|---|---|
| video call, voice call, screen share | a **mean opinion score** from the ITU-T G.107 E-model: one-way delay = half the round trip + a jitter buffer of twice the jitter + 20 ms of codec; loss counted twice for video; and the **seconds it froze**: its packets missing in looks that fell below half its usual rate between usual looks | froze 2 s or more, or MOS under 3.6 (choppy) |
| video streaming | **rebuffering** and **lower resolution**: a stream that fetches all the time below its usual bitrate plays more than comes in; the seconds played beyond what came in, past a 10 s buffer, are rebuffering | likely rebuffered, or fetched all minute at under 60% of its bitrate |
| cloud gaming | freezes, a worse picture (under 60% of its usual rate), input lag | froze 1 s, worse picture, round trip over 100 ms or 1% lost |
| online gaming | **lag spikes**: the round trip 60 ms above its normal (and at least 100 ms), 2% lost, jitter 40 ms, or server updates missing for half a second | any lag spike |
| large download | slowed to under 30% of the rate it had | only when the cause is the home, the line or the ISP (a slow server is not a moment) |
| browsing | pages likely slow: the round trip twice its normal (at least 150 ms), lookups failing (10%) or slow (300 ms) | as said |
| smart home | lost its connection: its connections gone, or off Wi-Fi, for 30 s or more | as said |

A stream's usual bitrate is the mean rate of its minutes when it was not fetching all the time
(over a minute, what a player fetches is what it plays). A voice call goes quiet by itself when
nobody talks, so its gaps count as freezes only when the network shows trouble too; video doesn't
stop by itself.

Each estimate has a **confidence** (%): higher with the network measured, the session's own
usual rate known, and a family naming the app.

### The bad moment and its cause

Bad minutes in a row are one bad moment. When it is over, its cause comes from the timeline's
verdict for that device over the moment (`why`, the same codes as Diagnose and the help desk):

| Cause (`Cause`) | From | In words |
|---|---|---|
| `WiFiCongestion` | `WIFI_CONGESTION` | the 2.4 GHz Wi-Fi channel was congested |
| `WeakSignal` | `WIFI_COVERAGE`, or the device's signal at -75 dBm or less | its Wi-Fi signal was weak (-79 dBm) |
| `WiFiDrops` | `WIFI_HOPPING`, `WIFI_DROPS` | it kept dropping off Wi-Fi or switching bands |
| `WiFi` | other Wi-Fi codes, `MESH_BACKHAUL`, or its own Wi-Fi hop losing 10% | its Wi-Fi was losing packets |
| `LineFull` | `LINE_FULL`, or the line full meanwhile | the line was full (TV downloading, 48.2 Mbit/s) |
| `Line` | `LINE_LOSS`, `LINE_SLOW`, `LINE_DOWN`, `NO_ROUTE`, `LINE_SLOW_SPEED` | the line to the ISP was losing packets or slow |
| `ISP` | `ISP_*`, `AREA_OUTAGE`, or the internet twice its normal / 2% lost | the ISP's network was slow or losing packets |
| `DNS` | `DNS_*` | names were slow to look up, or failed |
| `AIGWLimit` | `DEVICE_LIMITED`, `DEVICE_BLOCKED` | AIGW was limiting the device |
| `Device`, `GatewayOff` | `DEVICE_OFFLINE`, `GATEWAY_OFF` | |
| `FarEnd` | nothing wrong in the home or on the line, recorded | likely the far end (the app's servers, or the other side) |
| `Unknown` | not recorded | |

`FarEnd` matters: it is the gateway saying, with evidence, that the home and the line were fine.

For a cause the home can do something about, AIGW then looks for what happened next, within
30 minutes: a channel change (and whether AIGW decided it), a device steered, kept on 2.4 GHz
or moved to a better access point, smart queueing turned on. And it counts the device's
sessions of the same kind since: "the 3 calls since were fine", or "1 of the 2 calls since had
trouble too".

### Scores

Per kind of app (calls, streaming, gaming, downloads, browsing, smart home): the share of its
judged minutes that were fine over 7 days, said once it has 10 minutes. It sits beside proactive
care's experience score (`agctl care`), which is about the line; this one is about what people
did on it.

## Privacy

Per-app experience keeps only:

- a session's **kind** and, when `apps.names` is on, its **family name** from the rules file
  ("Netflix"), never the server name itself (it is hashed in memory only to see it change), no
  address of a server, no URL (the gateway never sees URLs) and no content;
- the device's MAC, times, numbers (minutes, seconds, MOS, the internet's round trip and loss,
  the device's signal), the cause and the timeline's code.

Kept in agd's state (saved with it, `state_file`): the last 32 bad moments, the last 48
sessions that ended and 8 days of per-kind minutes. On the timeline (`why.hours`, 72 h) each
moment is a line with the device's name and the kind, without the app.

The ISP (data model, `AppMoment!`, the brief and cortex) sees kinds, numbers and causes, never a
device's name and never the app's family, unless `brief.names` allows names. With `apps.share
0` it sees nothing: no moments, no scores, none in `WhySlow()`'s story. With `apps.names 0`
server names are not used at all.

## Settings (`/etc/config/aigw`, section `apps`)

| Option | Default | What it does |
|---|---|---|
| `enabled` | `1` | recognise sessions, judge them, explain bad moments |
| `names` | `1` | server-name families help tell apps apart (only the family is kept); `0`: traits only |
| `share` | `1` | the ISP may see it (kinds, numbers, causes; names only with `brief.names`) |
| `poll` | `10` | seconds between looks at the connections' counters |
| `min_s` | `30` | a session is scored once it has lasted this long |
| `rules` | `/etc/aigw/apps.rules` | the rules file; missing: the built-in rules |

The minute is `why.interval`. Nothing here changes the home: per-app experience only observes and
explains. The fixes it mentions are made by Self-Healing Wi-Fi, mesh, smart queueing and the rest,
as their own policies and `act.mode` allow (the default stays `advise`).

## Surfaces

- `agctl apps [device] [--app calls|streaming|gaming|downloads|browsing|smart-home] [--limit N]
  [--reload]`, `-j` for JSON; `ubus call ag apps '{"device":"laptop","app":"calls"}'`.
- The timeline: `agctl why` shows each moment ("Video call on laptop froze for about 6 s
  (WiFiCongestion)"), event kind `app`.
- The help desk: `agctl brief --app calls` ("my calls drop"), and `SupportBrief(App=Calls)` for
  the ISP, list the period's sessions of that app and their bad moments with causes. With no
  other finding, a moment with a cause in the home or on the line becomes the brief's verdict;
  when most moments were the far end's, the fault is `OUTSIDE_SERVICE`. Without `--app` the
  brief lists any app's moments.
- Events: `ag.app` on ubus (with names, at home).

### Data model (1.24)

`Device.X_AIGW_.AppExperience.` (only in builds with the feature):

| Parameter | Type | |
|---|---|---|
| `Enable`, `ServerNames`, `Shared` | boolean | `apps.enabled`, `apps.names`, `apps.share` |
| `Status` | string | `Disabled`, `NotShared`, `Learning`, `Active` |
| `ActiveSessions`, `Sessions`, `BadMoments` | unsignedInt | now; ended so far; so far |
| `Score` | int | all kinds, 7 days, %; -1 not known yet |
| `LastMoment` | dateTime | |
| `Kind.{i}.` | | `Name` (Calls, Streaming, Gaming, Downloads, Browsing, SmartHome), `Score` (int, -1), `Minutes`, `BadMinutes`, `Sessions` |
| `Moment.{i}.` | | `Time`, `Kind` (VideoCall, ...), `What` (Freeze, Stutter, Rebuffer, LowerResolution, Lag, Slow, Offline), `Seconds`, `Confidence`, `Cause`, `CauseCode`, `Fixed`, `FixTime`, `Summary` (no names) |

Event `Device.X_AIGW_.AppMoment!`: `Moment, Kind, What, Seconds, Confidence, Cause, CauseCode,
Time, Summary`. `SupportBrief()` takes `App` too.

## Cost

Measured by `tests/test_appqoe.c` and shown by `agctl apps` (`cost`):

- **CPU**: classifying a connection takes about 0.25 µs on an x86-64 PC (all 12 rules tried for
  the worst case); a busy home's 160 connections are about 40 µs a look. A MIPS 24Kc at 580 MHz is
  roughly 20 times slower: about 1 ms every 10 s. A server name's family is looked up once per
  connection. The minute's judgement is a handful of sums; asking the timeline for a moment's
  cause (once per moment) is the heaviest step, about 1 ms here.
- **Memory**: about 100 KB, fixed (160 connections with 6 looks each, 32 sessions, 32 moments,
  48 sessions ended, 8 days, the rules, and the buffers for a moment's cause).
- **Flash**: the feature adds 48 KB on x86-64 and aarch64 and 40 KB on mipsel (installed, as
  `aigw-selftest` counts it; OpenWRT 24.10.8 SDK, against v0.48 built the same way). The full
  profile is 1000 / 988 / 828 KB (x86-64 / aarch64 / mipsel) of the 1024 KB budget; the small
  profile 704 / 712 / 604 KB. See [features.md](features.md).

## Tests

- `tests/test_appqoe.c` (ctest `app_experience`): the rules file (the shipped one is the
  built-in rules, the same once parsed; bad lines skipped and said), families (longest suffix, label
  boundaries, nearby names), each kind from synthetic flows (voice, video, screen share, cloud
  and online gaming, streaming, downloads, smart home; a call app's web connection is not a
  call; a Netflix name nearby doesn't make a call Netflix), the E-model against known delay and
  loss, every estimate against known jitter, loss, gaps and bitrates, causes and their words, a
  moment in words for the home and for the ISP (no names), the score, and the cost budget.
- `tests/apps-test.sh` (CI): three namespaces, a real video call and a real stream whose server
  the TV looked up as a Netflix name. The call stops for 6 s: "froze for about 6 s", far end.
  The internet loses 40%: choppy, the line's fault. `agctl apps`, the timeline, `agctl brief
  --app calls`, the data model and `AppMoment!` without names; the server's name nowhere in what
  is kept; kept over a restart; `apps.share 0` and `apps.names 0`.

## Limits

- Counters every 10 s, not packets: a freeze shorter than a couple of seconds, or jitter inside
  one connection, is not seen directly. Jitter and loss come from the timeline's echo requests
  (3 a minute), so a short burst of loss can be missed; the confidence says how sure it is.
- A voice call and an online game on an unknown port look alike; without a port or a family the
  rules take it for a voice call.
- Traffic in a VPN is one connection: its kind is whatever its traits look like.
- A stream's bitrate is learned during the stream; its first minutes are not judged.
- Encrypted Client Hello hides the server name; DNS over HTTPS on the device hides its lookups.
  Then only traits are used (still a kind, without a family).
- The far end is a conclusion from the home and the line being fine, not a measurement of the
  far end.
