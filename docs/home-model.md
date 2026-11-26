# The home Wi-Fi model

*v0.53.0, `AIGW_FEATURE_HOMEMODEL` (needs `COVERAGE`).* A small model of the
home's Wi-Fi, learned on the gateway with no floor plan, and "what if"
predictions from it: what an extender, moving the gateway or another band
would fix, before anyone buys anything.

## Why

Coverage advice ([coverage.md](coverage.md)) says which device is weak, when
and where. The next question from the customer, and from the ISP's sales desk,
is "will an extender fix it?". Today that is a guess, and a wrong guess costs
twice: an extender sold to a home where a free change would have done, or one
put in the wrong place and returned.

The gateway is the only thing that has seen every device in every corner of
the home for weeks. So it answers with numbers:

> An extender near the spot where work-laptop and pixel-phone are (its own
> link there about -60 dBm) would raise the signal in the bedroom by about
> 5 dB (give or take 3) and fix 13 of the 13 coverage problems of the last 30
> days. Confidence Medium.

and it says when buying is not needed:

> Keeping devices in the bedroom on 2.4 GHz would raise their signal by about
> 7 dB and fix 13 of the 13 coverage problems of the last 30 days.
> Try this before buying an extender: an extender would fix 13.

Middleware such as prplOS or RDK-B exposes the signal of each station; it does
not decide what to buy, prove it afterwards, or explain it to the customer.

## How it works

The model learns from coverage advice's looks (every `coverage.interval`, 60 s):
for each Wi-Fi device, the access point and band it is on and its signal there,
what the mesh's other access points hear of it (EasyMesh Data Elements, when
there is a mesh), the loss Self-Healing Wi-Fi's probes see, and the rate.

### Places

Each look has a **signature**: what each access point hears of the device, with
2.4 GHz brought to 5 GHz terms (7 dB weaker). Looks of all devices, weak or not,
are clustered into the home's **places** (at most 8; the two closest merge when
a ninth appears; a place a person named is kept). A place is named by the
devices seen there most ("the spot where tv-bedroom and pixel-phone are"), or by
the name a person gave it.

Devices that are at one place at one look and another place at the next are
walking between them: the two places are **next to** each other. That is how
AIGW learns which places lie on the way to which, without a plan.

### What is known of each place

| | |
|---|---|
| signal | what the gateway hears there (5 GHz terms) |
| path loss | per band: the gateway's transmit power (20 dBm on 2.4 GHz, 23 on 5 GHz) minus what arrives |
| distance-like | from the 5 GHz path loss, indoor log-distance (exponent 3, walls included); a figure to compare places, not metres on a plan |
| 2.4 GHz offset | how much stronger 2.4 GHz is heard there than 5 GHz: learned when both bands were seen at the place, else 7 dB |
| experience | per band and access point: looks, signal, loss, rate, problems; and the best band and access point there |
| next to | the places devices walk to from here |

### Coverage problems

A **coverage problem** is a device that suffered for some minutes in a row
(5 looks by default) at one place, on one band:

- weak (at or below `wifi.heal_weak`, -75 dBm) and losing traffic (10% or
  more; without a loss measure, resends of 30% or more; without either, weak
  alone counts), or
- losing 10% or more with a strong signal (-65 dBm or better): a crowded band,
  not coverage.

A device coverage advice judged a weak transmitter (its own radio is weak) is
not the home's problem and is left out. A problem that goes on all day counts
once a day. The last 30 days' problems are kept (64 at most).

## What if

| Option | Costs | The prediction |
|---|---|---|
| `Extender` | buy | An extender near a known place whose own link to the gateway would be good (-65 dBm or better, 5 GHz terms), on the way to the far place (closer to the gateway, ideally next to it). With no such place: about halfway. The gain at the far place is the log-distance difference: from the gateway's distance-like figure to the extender's. Places next to the extender gain too; places elsewhere are counted as gaining nothing. |
| `GatewayUp` | free | The gateway higher, in the open: about 4 dB everywhere. |
| `GatewayCentral` | free | The gateway moved toward the middle, when the home is lopsided (the far place 3 times or more as far as the near one). Far places gain, near ones lose: the places that are fine now and would turn weak are counted. |
| `Band24` | free | Devices at a place kept on 2.4 GHz (Self-Healing Wi-Fi's `stay24`): + the place's 2.4 GHz offset. Fixes weak 5 GHz problems. |
| `Band5` | free | Devices that can use 5 GHz moved to it: fixes strong-signal 2.4 GHz losses (a crowded band) where 5 GHz is heard at -67 dBm or better. |

A problem is **fixed** when its signal plus the predicted gain reaches 3 dB
above weak (-72 dBm). Each option says how many of the last 30 days' problems it
would fix, how many places it may make weak, and a confidence:

- more looks and more problems at the place raise it; a place whose looks
  spread a lot lowers it;
- a mesh (more than one access point) raises it for `Extender`; an extender
  next to the far place raises it; an extender whose own link would be weak
  lowers it;
- `Band24`/`Band5` are surer when the offset was learned at that place;
- `GatewayUp` and `GatewayCentral` stay Low: AIGW can't see where the gateway
  sits or in which direction the rooms are.

Confidence 70 and up is High, 45 and up Medium, else Low.

**Ranking**: most problems fixed first; then fewer places made weak; then, when
AIGW is fairly sure (Medium or better) of both, the free one before the one
that costs; then the surer one.

**Before you buy**: an extender is worth buying (`ExtenderHelps` Yes) only when
it fixes more problems than any free change AIGW is fairly sure of. When it is
No, the ISP's mesh Wi-Fi signal (`MESH_WIFI`, [isp-signals.md](isp-signals.md))
is held, with the reason in its evidence; when it is Yes, the evidence says how
many problems the extender would fix. While the model is learning, offers are
as before.

### Proof

When an `Extender` prediction stands and a new access point appears (an
extender joined the mesh), AIGW watches the far place again: after 30 looks it
keeps the measured gain beside the prediction:

> AIGW predicted an extender would add about +9 dB in the bedroom; measured
> since it appeared: +7 dB.

This is how the model is checked on real homes, one extender at a time.

## Where it shows

- `agctl home`: the status, the advice, whether an extender is worth buying,
  the proof, the options (best first, in words) and the places.
- `agctl home name <device|N> <zone>`: call the place a device is at now (or
  place N) "bedroom", "office"; `""` clears. Names stay home.
- Coverage advice: the home's advice (`agctl coverage`, Diagnose, the
  timeline, the help-desk brief, proactive tickets) adds "The home model: ...".
- Offers: `MESH_WIFI` held or backed, as above.
- The data model, events, and cortex (below).

## Settings

| Option | Default | What it does |
|---|---|---|
| `home.enabled` | `1` | Learn and predict. It only predicts: nothing is changed. |
| `home.learn` | `120` | Looks (all devices) before the model says anything. |
| `home.share` | `1` | Cortex gets the status, counts and the best option's kind (never places or names). |

It learns from coverage advice's looks, so `coverage.enabled` must be 1 too.

## Data model (1.25)

`Device.X_AIGW_.HomeModel.`: `Enable`, `Status` (Learning, Ready, Disabled),
`Places`, `Problems`, `AccessPoints`, `BestOption`, `ExtenderHelps` (Unknown,
No, Yes), `Advice`, `Proof`, `PlaceNumberOfEntries`, `OptionNumberOfEntries`.

`Place.{i}.`: `Name` (empty unless `brief.names`), `Where` (devices by kind),
`Samples`, `Signal`, `PathLoss24`, `PathLoss5`, `DistanceLike`, `Band24Offset`,
`Problems`, `BestBand`, `NextTo` (place numbers).

`Option.{i}.` (best first): `Kind`, `Cost` (Free, Buy), `Place`, `Near`,
`GainDb`, `PlusMinusDb`, `Fixed`, `Of`, `MayTurnWeak`, `Confidence`, `Summary`.

For the ISP, devices are named by kind ("a TV") and places by number, unless
`brief.names` is 1.

## Events

- ubus `ag.home` (local): `{event: <kind>|Measured, fixed, of, gain_db, confidence, text}`.
- `Device.X_AIGW_.HomeAdvice!` (USP, TR-069): `Kind`, `Fixed`, `Of`, `GainDb`,
  `Confidence`, `Summary` (no names): a new best option, or `Kind` Measured
  when an extender's prediction was checked.

## Tests

- `tests/test_home.c` (ctest `home_model`): places, path loss, neighbours;
  a free change first and no extender sale; an extender that wins (2.4 GHz
  already); a crowded 2.4 GHz moved to 5 GHz; the proof after an extender
  appears; naming, merging past 8 places, 30-day expiry, learning; the
  model's size (under 8 KB).
- `tests/test_offer.c`: `MESH_WIFI` held when a free change does as much, backed
  when an extender helps.
- `tests/home-test.sh`: agd with a fake hostapd: the walk living room ->
  hallway -> bedroom, the TV weak in the bedroom; Learning, Ready, 2.4 GHz
  first, extender not worth buying; zones; the data model without names;
  coverage advice's line; `ag.home` and `HomeAdvice!`; kept over a restart;
  off.

## Limits

- **No floor plan, no directions with one access point.** AIGW knows how far a
  place is (in path-loss terms), not where. Two far rooms at the same distance
  on opposite sides look alike; an extender "on the way" may be on the way to
  only one of them. Places next to each other (devices walking between them)
  and a mesh help.
- **The path-loss model is a standard indoor log-distance one (exponent 3)**,
  not a measurement of the walls. Gains are estimates; the "give or take" is
  the place's own spread plus 3 dB. Check one prediction with the proof.
- **Devices differ.** A phone and a laptop at the same spot can be heard a few
  dB apart; places are clustered over all devices.
- **Not validated on real homes yet.** The unit and shell tests are synthetic;
  real-router testing, and proofs from real extenders, come next.
- **Moving the gateway** is predicted with Low confidence on purpose: AIGW
  can't see where it sits now.
- It only predicts: band changes (`stay24`, steering) are Self-Healing Wi-Fi's
  and mesh steering's decisions, in act mode, bounded by their policies.

## Size

Installed flash it adds to the full profile: 28 KB on x86-64, 32 KB on
aarch64, 24 KB on mipsel (4 KB blocks, as `aigw-selftest` counts them).
6 KB of RAM for the model, and about 12 KB in the state file.
In the **full** profile only: it needs coverage advice, which the small profile
leaves out.
