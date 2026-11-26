# Wi-Fi sensing

*v0.53.0, `AIGW_FEATURE_SENSING`.* Someone home or not, movement per zone, and
"activity seen as usual today", a check-in for someone who lives alone. From
the Wi-Fi signals the gateway already gets. **Opt-in**: off, even when built,
until the home agrees. It stays on the gateway.

## What it is for

| Service an ISP can offer | What sensing gives |
|---|---|
| Smart home and security | "nobody home" to arm an alarm or turn the heating down, "someone is back" to disarm, movement in a zone while nobody should be home |
| A care check-in | for an elderly relative who lives alone: a quiet message to the family when there has been no movement at a time there usually is ("no movement since 07:40; usually about 40 minutes of it by 11:00") |
| Energy | away periods for the heating, without a sensor in every room |

No camera, no microphone, nothing to wear, no extra device: the gateway is
already there.

## How it works

People change the radio signal between a device and the access point: a body
in the path absorbs and reflects it. A device that stays put (a TV, a smart
plug, a speaker) has a steady signal while nobody moves near it, and a
spread-out one when someone does.

**What it reads.** At each Wi-Fi poll (5 s) agd asks hostapd about each
associated device, as it always does: the signal of its last frames and its
frame counter. No monitor mode, no other adapter, nothing sent to the devices.
Where the driver offers channel state information (CSI), a vendor handler can
add a motion score per device (below); AIGW never reads CSI itself.

**The model** (C, a few hundred lines, no torch), shaped by the RoomSense
research project (a 1D-CNN on the same kind of signals):

- **Links**: one per device and band. Mixing what is not the same link blurs it
  (RoomSense: 2.7 dB of jitter mixed, 0.1-0.5 dB apart).
- **Fresh readings only**: a look counts only when the device sent frames since
  the last one. Devices send in bursts with gaps of 3-7 s; an old reading says
  nothing about now.
- **Features**, per window of 6 looks (30 s): the median, the spread around it
  (the mean distance from the median, the largest one left out: one stray
  frame is not movement), and how far the median moved since the last window
  (a change point).
- **Movement on a link**: the spread 2 times its own quiet spread and 1 dB above
  it, or the median moved 4 dB. The quiet spread is learned from quiet windows
  only, over the first 20 and then slowly.
- **Zones**: a sticky two-state filter (an HMM: still or moving) over the links
  of each zone. One noisy window doesn't flip it; two in a row do.
- **Presence**: a sticky two-state filter (nobody or someone home) over:
  movement anywhere (strong evidence); people's own devices (phones, watches by
  their type, or `sensing.personal`) here or all gone; and stillness by day,
  weighed by how often there is movement at this hour on usual days. At night
  (23:00-07:00) stillness means asleep, not away.
- **The check-in** (`sensing.checkin`): each hour's usual minutes of movement,
  learned over days, weekdays and weekends apart. A stretch without movement in
  which there would usually be `checkin_minutes` (45) minutes of it is
  **unusual inactivity**, unless the household is away by its own devices (all
  gone). It ends after 2 minutes of movement in a row.

**Zones** come from, in order: `sensing.zone '<MAC>=<zone>'`; else the home
model's place names ([home-model.md](home-model.md), when built:
`agctl home name tv-lounge living`); else one zone, "home". Sensing doesn't
need the home model; it uses its names when they are there (see below why).

An optional heavier model can run outside agd on a platform that has the room
for it, fed by the local `ag.sense` events; none ships in this version.

## Privacy first

- **Opt-in, per home.** `agctl sense on` prints what is sensed and asks for
  `--i-agree`; it then sets `sensing.enabled` and `sensing.consent` (who agreed,
  and when). Without both, nothing runs. Everyone who lives in the home should
  agree; the person who turns it on is asked to make sure they do.
- **The ISP can't turn it on.** `Device.X_AIGW_.Sensing.` is read-only over USP
  and TR-069; only the home (agctl, as root on the gateway) can.
- **Off keeps nothing.** Off, agd reads nothing for sensing, keeps nothing in
  memory beyond an empty table, and writes nothing to its state file.
  `agctl sense off` withdraws the consent and forgets everything it learned,
  in memory and in the state file, at once.
- **Local by default.** Events go to ubus `ag.sense` on the gateway (`agctl
  watch`, and only plug-ins you trust: built in, approved or signed). The
  bridge never reports sensing to cortex.
- **The ISP gets coarse events only, and only with `sensing.share_events 1`**:
  `PresenceChanged!` (Home, Away) and `UnusualInactivity!` (Started, Ended), and
  the data model's `Presence` and `Activity`. Never zones, devices or minutes
  by room.
- **What is kept** (in agd's state file on the gateway, while on): per device
  link its quiet spread and last median; per zone its state; per hour of the
  day the usual minutes of movement. No raw signal history.

### What is sensed (`agctl sense what`)

- each connected device's signal strength and frame count, as the access point
  already measures them;
- per zone: whether something moves, now and when it last did;
- someone home or not;
- with the check-in: minutes of movement for each hour of the day, today and
  on usual days;
- **not** who it is, what they do, sound or images.

## Settings

| Option | Default | What it does |
|---|---|---|
| `sensing.enabled` | `0` | On, with a consent (set by `agctl sense on --i-agree`). |
| `sensing.consent` | `''` | Who agreed, and when. Empty: it does not run. |
| `sensing.zone` | | `'<MAC>=<zone>'`: where a device stays (a list). |
| `sensing.personal` | | MACs of people's own devices, besides phones and watches by type (a list). |
| `sensing.share_events` | `0` | The ISP gets `PresenceChanged!` and `UnusualInactivity!`. |
| `sensing.checkin` | `0` | The "activity as usual today" check-in. |
| `sensing.checkin_minutes` | `45` | Minutes of usual movement missed before it says so. |
| `sensing.window` | `6` | Looks (5 s) in a window. |
| `sensing.csi_handler` | `''` | A vendor handler: `<handler> motion` prints `<MAC> <0-100>` lines each window; 50 and up is movement. |

## agctl

```sh
agctl sense                 # off: what it senses and how to turn it on; on: presence, zones, check-in
agctl sense what            # the "what is sensed" page
agctl sense on --i-agree    # on, with the home's consent
agctl sense off             # off, and forget everything
agctl watch                 # ag.sense events, among others
```

## Data model (1.24)

`Device.X_AIGW_.Sensing.`: `Enable` (read-only), `Status` (Off, Learning, On),
`ShareEvents`, and only with `share_events`: `Presence` (Home, Away),
`PresenceSince`, `Activity` (AsUsual, Less, None, Inactive, Learning; with the
check-in).

## Events

- ubus `ag.sense` (local): `{event: presence, presence: home|away, text}`,
  `{event: inactive|active, text}`.
- `Device.X_AIGW_.PresenceChanged!`: `Presence`, `Since`, `Reason` (Activity,
  NoActivity). Only with `share_events`.
- `Device.X_AIGW_.UnusualInactivity!`: `State` (Started, Ended), `Since`,
  `QuietMinutes`, `UsualMinutes`, `Summary`. Only with `share_events` and the
  check-in.

## How good it is (honestly)

**It is not validated on real, human-labelled activity.** The numbers below are
on a simulated home (`tests/test_sense.c`), and the simulator is an assumption:
one person; a TV, a speaker, a smart plug and a laptop that stay put, and the
person's phone; quiet links spread about 0.8 dB, a person moving near one adds
about 3 dB and sometimes shifts it; rare stray readings of 10 dB; bursty
traffic. Weekdays: out 8:30-17:30 with the phone; weekends at home; asleep
23:00-6:30; times move by up to 20 minutes a day. Movement comes in episodes of
a few minutes.

Ten days learn; **four whole days are held out** (the model learns nothing from
them) and scored per 30 s window. Over 30 simulated homes (seeds 1-30):

| | With a phone of the person's own | Living alone without a phone |
|---|---|---|
| Presence, right | 99.8-99.9% of windows | 84.5-90.3% |
| Movement per zone | precision 0.78-0.81, recall 0.83-0.87, F1 0.80-0.83 | precision 0.79-0.83, recall 0.75-0.82, F1 0.79-0.82 |
| Unusual inactivity on the 4 usual held-out days | no false alarm (120 days) | no false alarm (120 days) |
| A day the person stops moving at 10:00 | found after 76-229 min (about 1.5 h typically) | found after 81-199 min |

On 1 of the 60 "stopped moving" days, stray readings later in the day ended
the alarm (two minutes of what looked like movement). Same-day validation
would have looked better than this; RoomSense found it overstated accuracy
(92% against 63% on unseen days), so these figures are on unseen days only.

**What to expect on real homes**: worse until checked. Busy neighbours'
interference, devices that send rarely, a gateway on the far side of the home,
pets (they move signals too), and several people living together all make it
harder. Without a phone, an empty home by day takes about half an hour of
stillness to be called empty.

## Budget

On this PC: 0.1-0.7 us a look, 2.2 KB of state. `agd --sense-bench` measures
the same on any router, and `aigw-selftest` checks it (budget 50 us a look):
at agd's rate, 32 devices take well under 0.1% of one CPU. The QEMU CI jobs run
it on x86-64, aarch64 and mipsel.

## Tests

- `tests/test_sense.c` (ctest `wifi_sensing`): the simulated home above, both
  households, held-out days; stale readings not used; zones; learning; RAM
  (under 4 KB) and CPU per look. `AIGW_SENSE_SEED=<n>` runs another home.
- `tests/sense-test.sh`: off by default (nothing in the state file, the data
  model says Enable false and nothing else), `sense on` refused without
  `--i-agree`, consent written; movement in the living room and not the
  kitchen; the phone leaves: nobody home, locally only; `share_events`:
  `PresenceChanged!` Home, no names or zones; a CSI handler; `sense off`
  forgets everything; `agd --sense-bench`.

## Why it doesn't need the home model

Sensing is a service of its own (presence for a smart-home or security
package, a care check-in) that an ISP may want on a gateway without the
what-if model, and the home model needs coverage advice and the timeline (about
50 KB together). So sensing takes zones from its own setting, and uses the home
model's place names when that is built too.

## Size

Installed flash it adds to the full profile: 12 KB on x86-64, 16 KB on
aarch64, 12 KB on mipsel (4 KB blocks, as `aigw-selftest` counts them).
In the **full** profile only: it is an opt-in service, not
what an ISP needs most on 8 or 16 MB of flash.
