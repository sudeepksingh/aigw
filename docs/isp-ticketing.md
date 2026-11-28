# AIGW in the ISP's ticketing: TMF621 and signed webhooks

Since v0.39 cortex finds **area outages** (many homes behind one uplink down,
slow or lossy at once: [area-outages.md](area-outages.md)), and since v0.40
each gateway raises **proactive tickets** with a brief, the evidence, the next
step and a note for the customer ([before-the-call.md](before-the-call.md)).
Until v0.42 they lived only in cortex, its dashboard and the data model.

From v0.42 cortex puts them where the ISP's NOC and help desk already work:

- **TM Forum TMF621 Trouble Ticket API** (v4 by default, v5 too): one
  TroubleTicket per outage incident and per proactive ticket, updated as it
  changes and resolved when it is over. The ISP's own events (it resolved,
  closed or cancelled a ticket) can come back to cortex through the TMF621 hub.
- **Signed generic webhooks**, for ISPs without TMF621: a JSON POST per event,
  signed with HMAC-SHA256 over the timestamp and the body.

Both are **off by default**, and both can run at once.

```
area outage (cortex)  ─┐                       ┌─► TMF621  POST/PATCH /troubleTicket ──► ISP ticketing
proactive ticket      ─┼─► scan ─► outbox ─► flush                                         │
(gateway → bridge)    ─┘   (cortex.db)        └─► webhooks  POST, X-AIGW-Signature     hub events
                                                                                           │
cortex API  POST /tmf621/listener  ◄───────────────────────────────────────────────────────┘
```

## Turning it on

Write `tickets.json` in the cortex data directory (or point `--tickets` /
`$AIGW_CORTEX_TICKETS` at it), then restart `aigw_cortex serve`:

```json
{
  "enabled": true,
  "dry_run": true,
  "events": ["outage", "proactive"],
  "proactive": "told_isp",
  "tmf621": {
    "url": "https://tt.isp.example/tmf-api/troubleTicket/v4",
    "version": 4,
    "auth": {"type": "oauth2", "token_url": "https://sso.isp.example/oauth2/token",
             "client_id": "aigw-cortex", "client_secret_file": "/etc/aigw-cortex/tt-client.secret",
             "scope": "troubleTicket"},
    "ticket_type": {"outage": "AreaOutage", "proactive": "ProactiveCare"},
    "related_party": [{"id": "aigw-cortex", "name": "AIGW OS cortex", "role": "originator",
                       "@referredType": "Organization"}],
    "gateway_id": "name",
    "listener": {"token_file": "/etc/aigw-cortex/tt-listener.token"},
    "hub_callback": "https://cortex.isp.example:8790/tmf621/listener"
  },
  "webhooks": [
    {"url": "https://hooks.isp.example/aigw", "secret_file": "/etc/aigw-cortex/hook.secret",
     "events": ["outage", "proactive.opened"]}
  ]
}
```

Start with `"dry_run": true`: cortex works out every ticket and keeps what it
would send (`python3 -m aigw_cortex tickets outbox`, and the bodies in
`ticket_outbox`), but sends nothing. When the bodies look right, set it to
`false`.

| Setting | Default | What it does |
|---------|---------|--------------|
| `enabled` | `false` | Nothing happens without it. |
| `dry_run` | `false` | Work out and keep, send nothing. |
| `events` | `["outage", "proactive", "plant"]` | Which events go to TMF621 (and to webhooks without their own list): `outage`, `proactive`, `plant`, `line`, or single events `outage.opened`, `outage.updated`, `outage.resolved`, `proactive.opened`, `proactive.updated`, `proactive.resolved`, `plant.*`, `line.*`. v0.54: `plant` is a shared plant predicted to fail (one ticket for its homes; their own line tickets are then not sent), `line` a home's line degrading on its own ([area-prediction.md](area-prediction.md)). |
| `proactive` | `told_isp` | Which proactive tickets: those the gateway told the ISP (`Raised`, `Proposed`), or `all` (also those AIGW fixed itself, `Fixed`, and those kept at home, as a record). |
| `interval_s` | 30 | How often the server looks; it also looks right after a gateway reports. |
| `retry` | `{"base_s": 10, "max_s": 3600, "max_attempts": 30}` | Exponential backoff (±20%), and when a row gives up (about a day). |
| `allow_http` | `false` | `http://` is refused except to this machine. |
| `tmf621.url` | | The API's root, up to the version: `.../troubleTicket/v4` or `.../troubleTicketManagement/v5`. |
| `tmf621.version` | 4 | 4 or 5 (the bodies differ: see the mapping). |
| `tmf621.auth.type` | `none` | `oauth2` (client credentials: `token_url`, `client_id`, `client_secret_file` or `client_secret_env`, `scope`), `bearer` (`token_file` or `token_env`), or `none` (mutual TLS at a proxy). |
| `tmf621.ca_file` | system | The CA to trust for the ISP's servers. |
| `tmf621.timeout` | 10 | Seconds per request. |
| `tmf621.ticket_type` | `AreaOutage`, `ProactiveCare` | `ticketType` per kind: use the names in your catalogue. |
| `tmf621.severity` | below | Override `severity` per key, e.g. `{"outage.SLOW": "Major"}`. |
| `tmf621.related_party` | cortex as `originator` | The `relatedParty` list, as your API wants it (v4 shape; cortex converts for v5). |
| `tmf621.gateway_id` | `name` | The id of a gateway in `relatedEntity`: its cortex `name` (set at `enroll`, often the CPE serial or the account), its TR-069 `serial` (from its last report), or cortex's `id`. |
| `tmf621.listener` | | `token_file` or `token_env`: the bearer token the ISP's hub sends. Without it the listener is off (404). |
| `tmf621.hub_callback`, `hub_query` | | For `tickets hub register`. |
| `webhooks[]` | | `url`, `secret_file` or `secret_env` (required: every webhook is signed), `events`, `timeout`, `ca_file`. |

**Secrets** (the OAuth2 client secret, a bearer token, the webhook secrets, the
listener's token) come only from a file or an environment variable, read when
used (so a rotated secret is picked up without a restart). A secret written in
the configuration itself is refused. Secrets never appear in logs, errors, the
outbox, `tickets config`, the dashboard or MCP; an OAuth2 error says only its
HTTP status. Keep the files `0600`, owned by the user cortex runs as.

## Commands

    python3 -m aigw_cortex tickets               the sync state, in words (--json)
    python3 -m aigw_cortex tickets list [--open] each ticket: its externalId, the ISP's id and status, sync
    python3 -m aigw_cortex tickets outbox        what was sent, is waiting, or failed (the last 50)
    python3 -m aigw_cortex tickets sync          look and send once (when serve isn't running)
    python3 -m aigw_cortex tickets retry [key]   send the failed rows again, once their cause is fixed
    python3 -m aigw_cortex tickets config        the configuration as cortex reads it (no secrets: they aren't in it)
    python3 -m aigw_cortex tickets hub register | unregister

`aigw_cortex serve` runs the same looks and sends in the background while
ticketing is on, and prints where it sends at start.

## When a ticket is made, changed and resolved

| Event | Area outage | Proactive ticket |
|-------|-------------|------------------|
| opened | cortex found it (or the operator declared it) | the gateway raised it to the ISP (or any ticket, with `proactive: all`) |
| updated | homes joined or recovered, its kind or confidence changed | the gateway's ticket changed: severity, devices, points, the texts, its status |
| resolved | 80% of its homes recovered, or the operator ended it | the gateway closed it: `Fixed` by AIGW, `Resolved`, `Passed`; or it is gone from the gateway's last 16 |
| reopened (sent as updated, TMF `status: inProgress`) | it came back within the window | a flap the gateway reopened |

Incidents and tickets that were already over when ticketing was turned on are
not sent: the ISP gets news, not history.

## The field mapping (TMF621)

`POST /troubleTicket` creates; `PATCH /troubleTicket/{id}` with
`Content-Type: application/merge-patch+json` changes (`name`, `description`,
`severity`, `relatedEntity`, `note`: TMF's merge patch replaces arrays, so each
PATCH carries the whole note list) and resolves. cortex never sets `status` on
a create: the ISP's system does (`acknowledged`).

| TMF621 field | Area outage | Proactive ticket |
|--------------|-------------|------------------|
| `externalId` (v4) / `externalIdentifier[0].id` (v5, `owner` "AIGW OS cortex", `externalIdentifierType` "aigw") | `aigw:<cortex>:outage:<incident>` | `aigw:<cortex>:proactive:<gateway id>:<ticket #>:<opened>` |
| `name` | "Area outage 12 on exch-1: 6 of 15 homes down" | "WIFI_COVERAGE at flat-12: Two devices keep losing the Wi-Fi..." (code, home, summary) |
| `description` | the incident in words (`words`) | the ticket's summary |
| `severity` | by kind: `DOWN` Critical, `LOSS` Major, `DNS` Major, `SLOW` Minor | by the gateway's severity: `problem` Major, `warning` Minor, `info` Warning |
| `ticketType` | `AreaOutage` | `ProactiveCare` |
| `relatedEntity` | the uplink (`role` "uplink", `@referredType` "Resource", `id` the uplink id: the operator's group, the ACS's `UplinkID`, `wan:` or `bng:` address), then each affected gateway (`role` "affectedGateway", up to 50) | the gateway (`role` "affectedGateway"), and its uplink when known |
| `relatedParty` | `tmf621.related_party` (cortex as originator) | the same. The customer is not named: cortex doesn't know who it is, and device names stay home |
| `note[]` (`author` "AIGW OS cortex", `date`) | `aigw-brief` the words; `aigw-evidence` homes, codes, how close together, still affected, confidence (and the operator's reason when declared); `aigw-next-step` "the access network, not the homes: no visits" | `aigw-brief` the summary; `aigw-evidence`; `aigw-next-step`; `aigw-customer-note` the note in plain words for the customer (blank during an area outage, as on the gateway); `aigw-status` the gateway's status, fault, points, devices |
| `status` on resolve | `resolved` | `resolved` |
| `statusChangeReason` | "Over: N homes behind U, recovered by T" or "Ended by the operator" | "Fixed by AIGW on the gateway", "Over on the gateway (the ISP had been told)", "Over by itself on the gateway", "No longer reported by the gateway" |
| `resolutionDate` | when cortex resolves it | the same |
| `note` on resolve | the notes, and `aigw-resolution` | the same |
| `@type` | `TroubleTicket` | `TroubleTicket` |

v5 differences, handled by `tmf621.version: 5`: `externalIdentifier[]`
instead of `externalId`; `relatedEntity[]` items are `RelatedEntity` with
`role` and `entity` (an `EntityRef`); `relatedParty[]` items are
`RelatedPartyRefOrPartyRoleRef` with `role` and `partyOrPartyRole` (a
`PartyRef`); notes carry `@type: Note`. The lookup before a create filters on
`externalIdentifier.id` instead of `externalId`.

Devices are named by kind only, as everywhere the ISP sees AIGW; a MAC address
never leaves the home.

## Never twice, never lost

- **A durable outbox.** Each change is a row in `cortex.db` (`ticket_outbox`)
  before anything is sent, so it survives a restart of cortex. The rows of one
  ticket go in order, to each target on its own. Several changes waiting for
  the same TMF621 ticket go as one PATCH with the ticket as it is then.
- **No duplicates.** The externalId is fixed for the ticket's life. Before a
  create that may have reached the ISP already (an earlier try timed out or got
  a 5xx, or cortex stopped between sending and keeping the ISP's id: a mark is
  kept before each POST), cortex first looks for it with
  `GET /troubleTicket?externalId=...` and takes the one it finds.
- **Retries.** Network errors, 5xx, 408 and 429 are tried again with
  exponential backoff from `retry.base_s` to `retry.max_s` (±20%), or after
  the server's `Retry-After`. A 401 with OAuth2 gets one new token and one more
  try. Other 4xx are the request's fault: the row stops as `failed` with the
  server's reason, shown on the dashboard, until `tickets retry`. A 404 on a
  PATCH means the ISP deleted the ticket: cortex stops updating it.
- **Redirects are not followed**, so a bearer token never goes to another host.

## The ISP's events (TMF621 hub)

`tickets hub register` posts `{"callback": tmf621.hub_callback}` (and
`query` if set) to `<url>/hub` and keeps the hub's id; `unregister` deletes it.
The ISP's system then posts `TroubleTicket*Event`s to cortex's API (the port of
`aigw_cortex serve`) at `/tmf621/listener` or any path below it (some hubs add
`/listener/troubleTicketStatusChangeEvent`), with
`Authorization: Bearer <tmf621.listener token>`; gateways' tokens don't work
there. Expose that path through your reverse proxy with HTTPS.

What an event does, found by the ticket's `id` or its externalId:

- the ISP's `status` is kept and shown (`remote_status`);
- `resolved`, `closed`, `cancelled`, `rejected` (or a delete event): cortex
  sends no more updates to that ticket (its webhooks still go);
- a **declared** area outage whose ticket the ISP resolved or closed is ended
  in cortex (the NOC fixed it): its homes' fixes are no longer held. A found
  outage still ends only when its homes recover;
- an event about a ticket that isn't cortex's is answered 202 and ignored.

## Signed webhooks

One `POST` per event to each webhook that wants it, `Content-Type:
application/json`:

```
X-AIGW-Event:     outage.opened
X-AIGW-Delivery:  5f0c1d2e3a4b5c6d7e8f9a0b        unique per delivery: deduplicate on it
X-AIGW-Timestamp: 1795469410                      unix seconds, when this try was sent
X-AIGW-Signature: v1=<hex HMAC-SHA256(secret, "<X-AIGW-Timestamp>." + body)>

{"id": "5f0c1d2e3a4b5c6d7e8f9a0b", "type": "outage.opened", "time": "2026-11-24T10:30:10Z",
 "source": "aigw-cortex/0.42.0", "externalId": "aigw:3fa1b2c4:outage:12", "kind": "outage", "ref": "12",
 "data": {"id": 12, "uplink": "exch-1", "kind": "DOWN", "state": "open", "homes": 6, "homes_in_area": 15, ...}}
```

`data` is the incident (with its affected homes) or the proactive ticket, as
cortex's own API shows them. A delivery that fails is retried with the same
body and `X-AIGW-Delivery`, and a new timestamp and signature. To check one
(Python; `aigw_cortex.tickets.verify` does the same):

```python
import hashlib, hmac, json, time

def verified(secret: bytes, headers, body: bytes, seen: set, tolerance=300) -> bool:
    ts = headers["X-AIGW-Timestamp"]
    if abs(time.time() - int(ts)) > tolerance:          # a replay of an old delivery
        return False
    want = "v1=" + hmac.new(secret, ts.encode() + b"." + body, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(want, headers["X-AIGW-Signature"]):
        return False
    j = json.loads(body)
    # the signature covers the timestamp and the body, not the other headers: take the
    # delivery id and the event from the body, and refuse headers that say otherwise
    if headers["X-AIGW-Delivery"] != j["id"] or headers["X-AIGW-Event"] != j["type"]:
        return False
    if j["id"] in seen:                                 # a replay within the window, or a retry
        return False                                    # that arrived after all: once only
    seen.add(j["id"])                                   # keep ids at least `tolerance` seconds
    return True
```

Sign over the raw body as received, before any JSON parsing.
`aigw_cortex.tickets.verify_delivery(secret, headers, body, seen)` is this
check (v0.43); `verify` checks only the signature and the timestamp.

**Files** (v0.43): cortex refuses a secret file that other users can read, or
that its group or others can write (0600, or 0640 for a group that runs cortex),
and a `tickets.json` that its group or others can write: the configuration says
where the secrets are sent, so one anyone could edit could send them elsewhere.
A refused secret leaves the row `failed` with the reason ("chmod 600 ..."); fix
the mode, then `tickets retry`.

## Where to see it

- **Dashboard** ("ISP tickets", [dashboard.md](dashboard.md)): on or off, dry
  run, open, synced, to send, failed, the last error and the tickets with
  errors; each area outage and each open proactive ticket shows its ISP ticket
  id and sync state. `GET /api/tickets?open=1`.
- **Fleet report** and MCP `fleet_report`: `isp_tickets`, and `isp_ticket` on
  each outage and open proactive ticket; a line "ISP tickets" in the text.
- **MCP**: `isp_tickets` (read only), and `isp_ticket` in `area_outages` and
  `proactive_tickets`.

Sync states: `pending` (waiting to go), `synced`, `retrying` (the last try
failed, another is coming), `failed` (stopped: see the error, then
`tickets retry`), `dry-run`, and `none` (no TMF621 target: webhooks only).

## Tested

`tests/test_tickets.py` against `tests/tmf621_mock.py`, a TMF621 v4/v5 server
with an OAuth2 token endpoint, a hub and a webhook receiver on 127.0.0.1 (no
call leaves the machine): an outage created with the mapping above, PATCHed
when a fourth home joins, resolved when the homes recover; a proactive ticket
created, updated and resolved, AIGW's own fixes only with `all`; the server
down (connection refused, then 503, then 429) and the ticket sent once it is
back, with the backoff; a reply lost after the ISP kept the ticket, then a
restart: found by externalId, no duplicate, and no new rows after a second
restart; a crash between POST and keeping the id; a 400 kept as failed until
`retry`; a revoked OAuth2 token renewed once; v5 bodies and lookup; dry run
sends nothing and the next real change creates the ticket; no secret in any
output; the hub registered, the listener's token checked, the ISP's status
kept, a declared outage ended when the ISP resolves it, no updates after; the
listener off without a token; webhooks signed and verified (and refused with
another secret), per-webhook event lists, retried when down; the dashboard,
fleet report and MCP views. Not yet against an ISP's production TMF621.
