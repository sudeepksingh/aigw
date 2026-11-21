# Coverage placement advice

*v0.46.0.* Which device has weak Wi-Fi, when, where in the home, and what
would fix it. No floor plan, no app, no AI model on the router.

## Why

The old advice for a weak signal was always the same line: "move the device
closer, or the access point higher and more central; a mesh node or extender
would help". It is true, and a help desk can't act on it.

A real case from the PC access point: in its first days, all six proactive
tickets were one Android phone, weak at -83 to -101 dBm and losing 36 to 45%
of its traffic. The help desk needs to say:

- **which device**: the phone, not "a device";
- **when**: mostly 22:00 to 01:00, on 5 of the last 7 days;
- **where**: the spot where the phone and the TV are at night;
- **what fixes it**: move the phone, or (when devices share the spot) move the
  gateway toward it, or put an extender about halfway.

## How it works

Every minute agd looks at each Wi-Fi device. It uses what it already measures:

- the signal on the device's link, and which access point and band it is on;
- the rate to the device;
- the loss Self-Healing Wi-Fi's own probes see;
- with a mesh (EasyMesh, [mesh.md](mesh.md)): resends, and what the other
  access points hear of the device;
- drops and roams, from the Wi-Fi events.

It keeps a short history per device: its usual signal (the median, and the
10th and 90th percentile), how often it is weak, at which hours, on how many of
the last 7 days, and how much it loses when weak. The last week or two weigh
most. The history is kept in agd's state file, so a restart keeps it.

### Three kinds of weak device

| Kind | What it means | How AIGW tells |
|---|---|---|
| `Sometimes` | It moves into a far spot (the phone in the bedroom at night) | weak part of the time, fine at others |
| `Always` | It lives in a far spot (the TV) | weak 70% of the time or more |
| `WeakTransmitter` | Its own radio is weak (a cheap plug), not the coverage | weak and steady, yet: it loses little where other weak devices lose a lot, or the rate *to* it stays high where the others' falls, or it is a smart-home device in a home where the others are fine |

A device must be seen for `coverage.learn` looks (an hour by default) before it
is judged. Until then the old generic line is shown.

### Spots, without a floor plan

Each device's weak looks are grouped into **spots** by their signature: what
each access point hears there (2.4 GHz is brought to 5 GHz terms, about 7 dB
weaker), and the time of day. Spots of different devices with the same
signature are one spot of the home. A spot is named by the devices seen there:

> where tv-lounge and pixel-7 are, in the evening (about -86 dBm at the gateway)

This follows the idea behind the RoomSense project (signal signatures tell
places apart), kept lean: running means and counts in C, no model.

**With one access point**, a signature is one number per radio. AIGW can tell
how far a spot is, not in which direction. Two far rooms at the same distance
look alike. A mesh tells them apart.

### The verdict

| Verdict | When | Advice |
|---|---|---|
| `Extender` | Two or more suffering devices share a spot | The gateway is too far from it: move the gateway toward that spot (higher, in the open), or add an extender **about halfway** between the gateway and the spot (halfway is best for the extender's own link, as in [mesh.md](mesh.md)) |
| `UseExtender` | The same, but a mesh extender already hears the spot well | The devices there belong on that extender: mesh steering moves the ones it can; turning Wi-Fi off and on near the extender moves the others |
| `MoveDevice` | One device in a far spot | Move that device, or use it nearer the gateway there; an extender halfway only if it is used there a lot |
| `WeakDevice` | Only devices with a weak radio of their own | Nothing to fix in the Wi-Fi |
| `Fine`, `Learning` | | |

With a mesh, AIGW also says which access point hears the spot best, or that
none does (then: one more extender, about halfway).

## Where you see it

The advice replaces the generic line everywhere it was shown:

- **Proactive tickets** ([before-the-call.md](before-the-call.md)):
  `WIFI_COVERAGE`, and `WIFI_DROPS` of a device that drops when it is weak. The
  next step names the spot; the customer's note says where and whether an
  extender would help, in plain words, without names.
- **The help-desk brief** ([help-desk.md](help-desk.md)): a `coverage` line in
  the evidence (the device's history), the fix, and what to tell the customer.
- **The why timeline** ([why-slow.md](why-slow.md)) and **Diagnose**
  ([diagnose.md](diagnose.md)): the advice for `WIFI_COVERAGE`.
- **`agctl coverage`**: the verdict, the advice, the spots, and each device's
  history.
- **The data model**: `Device.X_AIGW_.Coverage.` and two parameters per host.
- **cortex**: the fleet report and the dashboard count the homes that need an
  extender ([fleet-report.md](fleet-report.md)).

```
$ agctl coverage
Coverage: Extender (4 devices judged, 3 weak, 2 to move, 1 with a weak radio of their own; weak at -75 dBm)
  advice    2 devices are weak in the same spot (where tv-lounge and pixel-phone are, in the evening
            (about -86 dBm at the gateway)): the gateway is too far from it. Move the gateway toward
            that spot (higher, in the open), or add a Wi-Fi extender about halfway between the gateway and it.
  customer  We noticed the Wi-Fi is weak in one part of your home, where several of your devices are in
            the evening. A Wi-Fi extender placed about halfway between your gateway and that spot would
            fix it; we will be in touch about it.

  spot 1    where tv-lounge and pixel-phone are, in the evening (about -86 dBm at the gateway): 2 devices, 2 suffer (shared: the gateway's place)

  devices
  pixel-phone is weak some of the time (it moves into a far spot); usually -74 dBm (-82 to -54), weak 45% of the time, mostly 22:00-23:00, on 1 of the last 7 days
  sara-laptop is fine; usually -50 dBm (-50 to -50), weak 0% of the time
  tv-lounge is weak all the time (it stays in a far spot); usually -86 dBm (-86 to -86), weak 100% of the time
  smart-plug is weak, but by its own radio (it loses little); usually -80 dBm, weak 100% of the time; mostly on 2.4 GHz
```

`agctl -j coverage` gives the same as JSON.

## Data model (1.21)

`Device.X_AIGW_.Coverage.`

| Parameter | Type | What |
|---|---|---|
| `Enable` | boolean | `coverage.enabled` |
| `Status` | string | the verdict: `Learning`, `Fine`, `WeakDevice`, `MoveDevice`, `Extender`, `UseExtender` |
| `Advice` | string | what to do, in words |
| `CustomerNote` | string | the customer's words |
| `Devices` | unsigned | devices judged |
| `WeakDevices` | unsigned | weak some or all of the time, or by their own radio |
| `DevicesToMove` | unsigned | weak devices that suffer (lose traffic) |
| `WeakTransmitters` | unsigned | devices whose own radio is weak |
| `SharedSpots` | unsigned | spots two or more suffering devices share |
| `SpotNumberOfEntries` | unsigned | |
| `Spot.{i}.Where` | string | the devices seen there, the time of day, the signal |
| `Spot.{i}.When` | string | "at night", "in the evening", ... |
| `Spot.{i}.Devices` | unsigned | devices seen there |
| `Spot.{i}.Suffering` | unsigned | ... that lose traffic there |
| `Spot.{i}.Shared` | boolean | two or more suffer there |
| `Spot.{i}.Signal` | int | dBm the gateway hears there |
| `Spot.{i}.HeardBestBy` | string | with a mesh: the access point that hears it best |

And per host: `Device.Hosts.Host.{i}.X_AIGW_Coverage` (the device's kind:
`Fine`, `Sometimes`, `Always`, `WeakTransmitter`, `Learning`) and
`X_AIGW_UsualSignal` (its median signal, dBm).

## Names and privacy

At home, `agctl` names devices. For the ISP (the data model, tickets, the
brief, cortex) device names become kinds ("a TV", "an Android phone") unless
`brief.names` is on. MAC and IP addresses never leave. The customer's note
never names anything.

cortex gets the verdict, the counts and the spots by kind (`coverage.share`,
on by default). A home that needs an extender is a sales lead for the ISP; a
home where one phone is weak in the bedroom at night is not, and the fleet
report keeps them apart.

## Settings

```
config coverage 'coverage'
	option enabled '1'
	option interval '60'     # s between looks at each device
	option learn '60'        # looks before a device is judged
	option share '1'         # cortex gets the verdict and counts
```

A signal is weak at `wifi.heal_weak` (-75 dBm), the same line as Self-Healing
Wi-Fi's. Coverage advice only advises: it never changes anything.

## Limits

- With one access point AIGW knows how far a spot is, not where. The advice
  says "toward that spot"; the customer knows where the TV is.
- 2.4 GHz is brought to 5 GHz terms with a fixed 7 dB. Walls change that a
  little; spots on one band are the most exact.
- Telling a weak transmitter apart needs a measure of how it fares: loss from
  Self-Healing Wi-Fi's probes, resends from the mesh, or the rate to it. Some
  drivers report no useful rate (the RTL8821CU of the PC access point reports
  6.5 Mbit/s for everything); then it rests on the loss.
- A device that is new, or rarely on Wi-Fi, is still being learned.

## Testing

- `tests/test_coverage.c` (ctest `coverage_placement`): the rules.
- `tests/coverage-test.sh`: a simulated home with a fake hostapd. A phone that
  moves to a far room at night, a TV that lives there, a weak smart plug, a
  laptop that is fine. One shared spot, an extender about halfway; kinds for
  the ISP; a proactive ticket that says where; kept over a restart.
- What only a real home shows: see [testing-on-routers.md](testing-on-routers.md)
  and the PR. Walk with a phone from the room by the gateway to the far room
  and back over an evening, and check `agctl coverage` the next day.
