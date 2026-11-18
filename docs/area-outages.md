# Area outages: when the fault is the ISP's network, not the home

An evening at an ISP: a line card in an OLT starts dropping packets. Within
ten minutes forty customers behind it call. Each support agent looks at one
home and sees "the internet loses packets". Each home's gateway, trying to
help, starts a smart-queueing trial or moves a device to another band. Some
calls end in a technician visit. None of it helps: nothing is wrong in any of
those homes.

AIGW OS sees it differently. Cortex already gets each home's line faults
(proactive care, the help-desk brief's timeline verdict) and knows when a
gateway stops reporting. When **many homes behind the same uplink go slow,
lose packets or drop in the same few minutes**, cortex opens **one network
incident**. Then:

- the help-desk brief of every home in it says **"known outage in your area"**:
  no technician, nothing to fix in the home, what to tell the customer;
- **fixes in those homes are held**: cortex keeps fix proposals and staged
  firmware back, and each gateway holds its own (smart-queueing trials, band
  steering, channel changes, keeping a device on 2.4 GHz). AIGW doesn't "fix"
  what isn't broken;
- the operator sees the incident on the dashboard, in the fleet report, the
  MCP tool `area_outages` and `aigw_cortex outages`;
- each gateway raises `Device.X_AIGW_.AreaOutage!` for the ISP's ACS, and
  `AreaOutageCleared!` when it is over, with what it held.

```
$ python3 -m aigw_cortex outages
Area outage 12: 5 of 8 homes on bng:198.51.100.7 losing packets from 2026-11-18 20:41 (within 3 min; high confidence): the ISP's network, not the homes; 5 still affected
    home-031: ISP_LOSS from 2026-11-18 20:41, still affected
    home-044: ISP_DOWN from 2026-11-18 20:42, still affected
    ...
```

## Which homes share an uplink

The first found wins:

| | |
|---|---|
| **The operator's id in cortex** | `aigw_cortex enroll <home> --group OLT-3/2`, or `aigw_cortex group <home> OLT-3/2`: an exchange, an OLT or DSLAM port, a BNG, a street cabinet, a town |
| **The id on the gateway** | `Device.X_AIGW_.AreaOutage.UplinkID`, written by the ISP's ACS over USP or TR-069 (agd's `area.uplink_id`), sent with each report |
| **The WAN's next hop** | `wan:<address>` from the gateway's default route |
| **The BNG** | with no next hop (PPPoE, a point-to-point WAN): its peer, `bng:<address>` |

Addresses a home network uses (RFC 1918, IPv6 ULA, link-local) group nothing:
behind a modem every home has 192.168.1.1. Carrier-grade NAT (100.64.0.0/10)
is the ISP's own, so it counts.

## The rules

All in `cortex/aigw_cortex/outage.py` (`correlate()`, pure, unit-tested in
`tests/test_cortex.py`):

- A home's **line fault**: proactive care's problem on the line or DNS
  (`LINE_DOWN`, `ISP_LOSS`, `DNS_FAILING`, ...), the brief's verdict `LINE` while
  it is current, or **silence**: no report for 15 minutes (at least three
  check-ins). Silence counts only while cortex hears from other gateways, so
  cortex's own network going down is never an outage. A gap that is over is
  kept as a fault from the missed report to the next one.
- Faults of one uplink that start within **15 minutes** of the first one
  belong together.
- At least **3 homes, and 10% of the uplink's homes**: one or two homes are
  never an outage, however small the uplink.
- Homes whose line goes later, while the incident is still on, **join** it.
- **Over** when 80% of its homes have recovered; its end is when that happened.
  A home still faulty then is its own problem again.
- **Confidence** `high` with twice the homes needed or half the uplink, else
  `medium`. **Kind** `DOWN`, `LOSS`, `SLOW` or `DNS`: the one most homes had.

Change them for the whole cortex (the API, dashboard and MCP server agree):

```
python3 -m aigw_cortex outages settings --min-homes 5 --share 0.2 --window 600 --recovered 0.9
```

The NOC often knows first. A declared outage covers every home of the uplink
(and those that join it) until it is ended:

```
python3 -m aigw_cortex outages declare OLT-3/2 --kind DOWN --note "card swap"
python3 -m aigw_cortex outages end 13
```

## The help desk

For a home in an open incident, `aigw_cortex brief`, the MCP tool
`support_brief` and the gateway's own `SupportBrief()` / `agctl brief` answer:

```
home-031: LINE (ISP_LOSS, high confidence), KNOWN AREA OUTAGE
  verdict   Known outage in the area: 5 of 8 homes on bng:198.51.100.7 losing packets since 20:41 (area outage 12). The ISP's network, not this home.
  tell      There is a known outage in your area since 20:41; our network team is working on it. Your equipment at home is fine: there is no need to restart or reset anything.
  fix       Nothing to fix in the home. AIGW holds its own fixes there ...
  visit     No: a known outage in the area (area outage 12, 5 homes); a visit to this home would not help.
```

A neighbour on the same uplink that is fine gets a note instead: "an outage
affects 5 of 8 homes on ... ; this home hasn't shown it". On the gateway the
code is `AREA_OUTAGE` unless the home's own verdict already names the line.

## On the gateway

The answer to each bridge report carries `"area": {incident, kind, since,
homes, of, confidence, uplink, hold_s}` (incident 0: none). The bridge passes
it to agd (`{"area": ...}`, capability `area.notice`; bridge setting `area 0`
turns it off), and agd (`agd/area.c`):

- holds its own fixes: no smart-queueing trial started or judged, no band
  steering, no channel change, no device kept on 2.4 GHz. Security actions and
  a person's own requests are never held;
- forgets the notice by itself `hold_s` (30 min) after the last one, so a cortex
  that went away never holds fixes for long. A hard drop can't reach the
  gateway, but then its line is down anyway; the notice arrives as the line
  comes back and holds fixes while the area recovers.

| `Device.X_AIGW_.AreaOutage.` | |
|---|---|
| `Enable` | `area.enabled` |
| `UplinkID` | writable: the operator's id of this home's uplink |
| `Status` | `None` or `Active` |
| `Incident`, `Kind`, `Since`, `Homes`, `HomesInArea`, `Confidence`, `Uplink` | the incident cortex told of |
| `HeldFixes` | what was held so far: `SmartQueueing`, `SmartQueueingTrial`, `BandSteering`, `ChannelChange`, `Stay24` |
| `Until` | when the notice lapses unless renewed |
| `LastIncident`, `LastEnded` | the last one, after it is over |

Events: `AreaOutage!` (Incident, Kind, Since, Homes, HomesInArea, Confidence,
Uplink) once when it starts, `AreaOutageCleared!` (Incident, Kind, Since,
DurationSeconds, HeldFixes, Reason) when it is over for this home. Data model
1.17.

## Tests

- `tests/test_cortex.py`: the rules (thresholds, a lone home, two areas,
  recovery, later homes, uplinks), incidents kept in cortex (the brief, held
  proposals, silence, the operator's uplink), and end to end: 14 simulated
  homes on two uplinks over cortex's HTTP API, the dashboard and the MCP tools.
- `tests/test_brief.c`: the gateway's brief during an outage.
- `tests/area-test.sh`: three real gateways (agd and the bridge each) and cortex:
  the uplink from the config and the data model, a declared outage told within
  seconds, the events, a held fix, the end, and a notice that lapses.
