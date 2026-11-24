# Proactive care before the call

[Proactive care](proactive-care.md) (v0.28) checks the home by itself and
reports open problems. The [help-desk brief](help-desk.md) answers once a
customer calls. Proactive
care before the call (AIGW OS 0.40) acts **first**. The gateway already watches
the things a customer notices: the internet going down or slowing, Wi-Fi
crowded by the neighbours, a device that keeps dropping off Wi-Fi or switching
bands, calls and games breaking up on a full line. For each fault the customer
is likely to notice:

- **AIGW fixes it itself** when it can, as the act mode allows. In `enforce`
  mode it applies the fix and records how it went. In `advise` mode it only
  **proposes** the fix, and a person says yes (`agctl approve`, or
  `Decision.{i}.Approve()` over USP or TR-069). In `off` mode it fixes nothing.
- **Otherwise it raises a ticket to the ISP**: the brief, the evidence, the
  suggested next step, and a short note in plain words the ISP can send the
  customer.

When a fix AIGW made has done its job, the ISP hears "we noticed X and fixed it",
with a note ready for the customer.

```
$ agctl care proactive
Before the call: a look every 1m; worth acting on at 3 points; fixes by AIGW (act mode enforce); 1 of 3 new tickets to the ISP left today
  so far    4 worth acting on: 1 fixed by AIGW, 1 proposed, 2 told to the ISP, 1 kept at home, 1 repeats not told again; calls likely avoided 3 (called anyway 0)
  tickets
    #4   Raised     ISP_DOWN         LINE, open for 6m, 5 devices, 4 points (a problem +2, 5 devices +2)
         The ISP's line or network, now: The internet didn't answer for 6 min while the next router did: the problem was beyond the home, in the ISP's network (ISP_DOWN).
         next: No fix on the gateway: the ISP's side (...). Technician: No: the home is fine up to the ISP; this is for the network team.
         customer: We noticed your internet connection stopped working this evening. Our team is already on it, so there is no need to call us. We will let you know when it is back to normal.
    #3   Fixed      WIFI_HOPPING     HOME_WIFI 3c:22:fb:..., over for 2h, 1 device, 3 points (a warning +1, for 34 min +2)
         A TV keeps moving between 2.4 and 5 GHz Wi-Fi (seen for 34 min): it is at the edge of 5 GHz ...
         next: Nothing to do: AIGW fixed it (its decision is in effect). The note can go to the customer.
         customer: We noticed one of your devices kept losing its Wi-Fi connection this evening, so your gateway set it to use a steadier connection. There is nothing you need to do.
```

(An example evening. `tests/proactive-test.sh` produces tickets like these in a
few minutes.)

## What it watches

| Source | Faults |
|---|---|
| Proactive care's open problem (its quiet Diagnose, the nightly speed test) | the line, the ISP's network, DNS, Wi-Fi, a slow line |
| The [why timeline](ARCHITECTURE.md), its last few minutes | the same, as they happen, and a full line with rough calls or games on it (the last half hour) |
| Self-Healing Wi-Fi, on each access point | crowded, weak, radio down; one device suffering, or one hopping between bands |
| Wi-Fi events | a device that dropped off Wi-Fi 3 times or more in an hour; a device hopping between 2.4 and 5 GHz |
| The experience score | a home having a bad week (under 90%) calls sooner |

One fault seen by several sources is one ticket. A fault is keyed by its code
for the whole home (`ISP_DOWN`), and by its code and the device's MAC for one
device (`WIFI_DROPS:<mac>`).

## Worth acting on

Not every blip is worth a ticket. A fault scores points for what makes a
customer pick up the phone:

| | |
|---|---|
| **How bad** | a warning 1, a problem 2 (below a warning: never); +1 when it hits badly: a quarter or more of the traffic lost (the worst affected device), or replies taking a second or more. Calls break up and pages stall, so the customer notices at once, however short it is and however few devices it hits |
| **How long** | 10 minutes or more +1, 30 minutes or more +2 |
| **How many devices** | 2 or more +1, 4 or more +2 (the whole home: those online) |
| **Whether it repeats** | its 2nd episode in a day +1, its 4th +2 (an episode ends after `proactive.clear` seconds away) |
| **A bad week** | the experience score this week under 90%: +1 |

It is worth acting on at `proactive.threshold` points (3), once it has lasted
`proactive.min_duration` seconds (180): a minute-long blip never is. Only faults
on the line, in the home's Wi-Fi or on a device count. "A service beyond the
ISP", "nothing wrong" and "not known" never do. A full line counts only when
calls or games were rough on it.

So the internet down for a home of five devices is worth acting on after
3 minutes (2 + 2). Crowded Wi-Fi touching one device takes half an hour (1 + 2),
or less if it is its second time today.

## What AIGW does

| The fault | AIGW's own fix | in `enforce` | in `advise` |
|---|---|---|---|
| A device hopping between bands (`WIFI_HOPPING`) | keep it on 2.4 GHz for a day (`stay24`) | applied: `Fixing`, then `Fixed` | proposed: `Proposed`, the ISP is told a fix waits for a yes |
| Calls and games rough on a full line (`LINE_FULL`) | smart queueing (`sqm`) | applied | proposed |
| A fix already waiting for a yes | (the help-desk brief's) | not approved: that is a person's yes | told to the ISP as `Proposed` |
| Crowded Wi-Fi, Wi-Fi trouble Self-Healing Wi-Fi works on | Self-Healing Wi-Fi's own channel trials | `Watching` for `care.wifi_grace` (30 min); raised if it lasts | the same |
| Anything else (the line, DNS, weak coverage, a radio down, a device dropping off) | none | `Raised` at once | `Raised` at once |

AIGW's fixes go through the same guardrails as every decision: protected
devices, the hourly limit of automatic actions (`act.max_auto`; past it, the fix
waits for a yes even in enforce mode), and time limits. A fix that hasn't
cleared the fault within `care.wifi_grace` is raised to the ISP ("AIGW's fix
didn't clear it"). `proactive.fix 0` turns AIGW's own fixes off (tickets only).

When it is over (not seen for `proactive.clear` seconds), a ticket is `Fixed`
(AIGW changed something for it), `Resolved` (the ISP was told), or `Passed`
(over by itself before anyone had to be told).

## Not spamming the ISP

- **One ticket per fault**, however often it is seen. The ISP hears of it once.
- **A flap**: a fault that comes back within `proactive.cooldown` (6 h) of
  clearing reopens the same ticket. The ISP is not told again. It hears more
  only if it stays (twice `proactive.clear`): "it came back after it had
  cleared, and stays". Each flap doubles how long it must stay away before the
  ticket closes, so a flapping line keeps one ticket open instead of opening
  and closing it all evening.
- **The day's limit**: at most `proactive.max_per_day` (3) new tickets reach the
  ISP in 24 hours. Past it they are kept at home (`Suppressed`), still visible
  with `agctl care proactive` and in the data model. News about a ticket the ISP
  already has (it was fixed, it is over) is free.
- **A home in a known outage across the area** (v0.39, [area-outages.md](area-outages.md):
  `Device.X_AIGW_.AreaOutage.` is `Active`) gets **no tickets and no customer
  notes** while it lasts: the ISP knows of the outage and tells its customers
  itself. No ticket is opened or reopened and none is raised after a grace
  (AIGW also holds its own fixes then). Each fault that would have made one is
  counted once, in `HeldForAreaOutage`. If an open ticket the ISP already has
  ends meanwhile, it still hears so, but with no note for the customer. Once the
  notice lapses, a fault that is still there gets its ticket as usual.

## The customer's note

Each ticket carries a note the ISP can send as it is, by text or in its app.
The note:

- starts with "We noticed";
- says when in plain words ("this morning", "this afternoon", "this evening",
  "tonight");
- says either that it is fixed and there is nothing to do, or that the team is
  already on it and there is no need to call.

It never names a device ("one of your devices"). It has no codes, units or
acronyms: no DNS, SQM, MAC, dBm or ms, and no "packet loss", "latency",
"channel", "band" or "router". The unit test checks every note, for every code,
fixed or not, at every hour (`pro_plain()` in `core/libag/proactive.c`).

For weak Wi-Fi (`WIFI_COVERAGE`, and `WIFI_DROPS` of a device that drops when it
is weak), the note comes from [coverage advice](coverage.md) once a device's
coverage is learned (v0.46). It says where, in plain words ("in one part of
your home, where several of your devices are in the evening"), and what fixes
it: a device used a little closer, or a Wi-Fi extender about halfway between
the gateway and that spot. The ticket's next step names the spot by the
devices seen there (by kind for the ISP).

## Where to see it

| | |
|---|---|
| At home | `agctl care proactive [--run]` (`--run`: look now); `agctl -j ...` for JSON; ubus `ag proactive {run}` |
| TR-181 (USP, TR-069, aigw-amx) | `Device.X_AIGW_.Proactive.`: `Enable`, `Fix`, `OpenTickets`, `Tickets`, `ToldISP`, `FixedByAIGW`, `Proposed`, `Suppressed`, `Deduplicated`, `CalledAnyway`, `CallsLikelyAvoided`, `HeldForAreaOutage`, `MaxPerDay`, `Since`, and the table `Ticket.{i}.` (`Status`, `Open`, `Code`, `Fault`, `Severity`, `Points`, `Reason`, `Devices`, `Episodes`, `Flaps`, `Since`, `Opened`, `LastSeen`, `Closed`, `ToldISP`, `CustomerCalled`, `Decision`, `Summary`, `Evidence`, `NextStep`, `CustomerNote`). Data model 1.18 |
| The event | `Device.X_AIGW_.ProactiveTicket!` (`Ticket, Status, Code, Fault, Severity, Points, Reason, Devices, Since, Summary, Evidence, NextStep, CustomerNote, Decision`): when a ticket is raised or proposed, when AIGW fixed one, when one the ISP knows is over. Also `ag.ticket` on ubus for plug-ins |
| cortex | the bridge sends the tickets and counts with each report. `python3 -m aigw_cortex proactive [home] [--open] [--json]`; the fleet report's "before call" lines and its `proactive` section; the MCP tool `proactive_tickets`; the dashboard's "Before the call" section with **calls likely avoided**, and `/api/proactive?open=1` |

**Calls likely avoided** counts tickets that are over and were either fixed
by AIGW or told to the customer first, with nobody asking the help desk about
the home while they were open. A help-desk brief asked while a ticket is open
(`agctl brief`, `SupportBrief()`) marks it "called anyway". cortex's own briefs
from the last report are not counted. It is a likely count, not a proof: a
customer may have been about to call, or may not have noticed at all.

## Names

What the ISP gets follows `brief.names`. By default it uses device kinds ("a
laptop", "a TV"), with the home's device names, MACs, addresses and sites taken
out, as in the help-desk brief. A device's MAC (a ticket's `target`) never
leaves the home. The bridge sends tickets with names only to a cortex that
shares device names (bridge `devices = names`).

## Settings (`/etc/config/aigw`, section `proactive`)

| Option | Default | |
|---|---|---|
| `enabled` | 1 | find faults a customer would notice |
| `fix` | 1 | AIGW may fix them itself, as `act.mode` allows |
| `interval` | 60 | seconds between looks |
| `threshold` | 3 | points worth acting on |
| `min_duration` | 180 | seconds a fault must have lasted |
| `clear` | 600 | seconds gone before it is over (each flap doubles it) |
| `cooldown` | 21600 | back within this: the same ticket, no new word to the ISP |
| `max_per_day` | 3 | new tickets to the ISP in 24 hours |

## Code and tests

- `core/libag/proactive.c`, `include/ag/proactive.h`: the rules, pure (points,
  worth acting on, tickets, episodes, flaps, the day's limit, no tickets in a known
  area outage, the customer's note and its plain-words check). Unit test:
  `tests/test_proactive.c`.
- `agd/proactive.c`: gathers the signals, builds each ticket from the
  help-desk brief, fixes (through `act_auto()`: enforce applies, advise
  proposes), watches, raises, and keeps the tickets in agd's state.
- `tests/proactive-test.sh` (CI) runs it end to end with agd in network
  namespaces and a fake hostapd:
  - the internet down: one ticket, one event, a plain note, no names;
  - a flap: not told twice;
  - advise mode proposes and enforce mode fixes;
  - past the day's limit, a ticket is kept at home.
- `tests/test_cortex.py` (class `Proactive`): the fleet, the MCP tool, the
  dashboard.
