# Device management: USP (TR-369) and TR-069

An ISP can manage an AIGW OS gateway with the protocol it already runs. The
data, the instance numbers and the rules exist once, in `agd`. Each protocol
client only translates.

```
  USP controller (ISP)                     ACS (ISP, TR-069)
        │ MQTT 5 (TLS)                          │ SOAP over HTTPS          (v0.19)
┌───────▼───────────────────┐       ┌───────────▼──────────────┐
│ aigw-usp                  │       │ aigw-cwmp                │
│ OB-USP-Agent, unmodified, │       │ CWMP client + glue       │
│ + aigw-usp.so plug-in     │       │                          │
└───────┬───────────────────┘       └───────────┬──────────────┘
        └──────────── ubus: ag.dm ──────────────┘
                   ┌───────▼────────┐
                   │ agd: dm.c      │  schema, instance numbers, write rules,
                   │ (data layer)   │  error codes, events
                   └───────┬────────┘
                   devices · alerts · decisions (act_request + guardrails) · Wi-Fi
```

Status: the data layer and USP are in v0.18. TR-069 comes in v0.19 on the same
layer; choosing its client (icwmp or easycwmp, or a small one of ours) starts with
a trial.

## Turning it on

On OpenWRT:

```sh
opkg install aigw-usp            # pulls obuspa, libmosquitto-ssl, libsqlite3
uci set aigw.mgmt.enabled=1
uci set aigw.usp.broker=usp.example.net
uci set aigw.usp.controller_id='self::usp-controller'   # the ISP's controller endpoint ID
uci commit aigw
/etc/init.d/aigw-usp enable
/etc/init.d/aigw-usp start
```

On a PC (PC as AP): `sudo AIGW_USP=1 platform/pc-ap/install.sh` builds the
pinned OB-USP-Agent and the plug-in; then set the same options in
`/etc/config/aigw` and run `sudo systemctl enable --now aigw-usp`.

`aigw-usp-run --print` shows the agent settings it makes from the config. The
agent's own database (`/etc/aigw/usp/usp.db`: controllers' subscriptions) is
rebuilt whenever those settings change, so `/etc/config/aigw` stays the one
place to set things.

| Option | Default | |
|--------|---------|---|
| `mgmt.enabled` | `0` | start the protocol clients |
| `mgmt.actions` | `0` | may a management system quarantine, block, and change `act.mode` |
| `mgmt.alert_details` | `0` | may it see alert subjects and reasons (the names devices looked up) |
| `usp.broker`, `usp.port`, `usp.tls` | —, `8883`, `1` | the ISP's MQTT broker |
| `usp.username`, `usp.password`, `usp.ca_file` | | broker login and trust store |
| `usp.agent_id` | `proto::aigw-<machine id>` | this gateway's endpoint ID |
| `usp.controller_id`, `usp.controller_topic` | `self::usp-controller`, `/usp/controller` | the controller |
| `usp.agent_topic` | `/usp/agent/<agent_id>` | where the agent listens |

## What a management system may do

Whatever protocol it uses, and decided in `agd`, not in the protocol client:

- **Read** everything below, except alert subjects and reasons, and blocked domains, while `mgmt.alert_details` is `0`.
- **Approve, reject and undo** decisions AIGW proposed. The decision records who, e.g. `policy dga, approved by usp`.
- **Quarantine, block, set `Mode`**: only with `mgmt.actions 1`.
- **Never** beyond AIGW's guardrails: no protected device (`act.protect`), no device outside the LAN, a time limit on every action. These are the same checks as for `agctl`.

OB-USP-Agent itself gives the controller full access (its role `Full Access`), so
that the one place these rules live is the data layer.

## The data model (version 1.0)

The full definition, as ISPs load it into their controllers, is
[`datamodel/aigw-dm.xml`](datamodel/aigw-dm.xml). It is made from what `agd`
serves (`agctl -j dm schema | scripts/dm-xml`), and `tests/dm-test.sh` fails when
the two differ.

| Path | Contents | Writable |
|---|---|---|
| `Device.DeviceInfo.` | `Manufacturer`, `ModelName`, `ProductClass`, `SoftwareVersion`, `UpTime`, `X_AIGW_DataModelVersion` | no |
| `Device.Hosts.Host.{i}.` | `PhysAddress`, `IPAddress`, `HostName`, `Active`; `X_AIGW_` type, zone, first and last seen, rates and bytes up and down, Wi-Fi band and signal | no |
| `Device.WiFi.SSID.{i}.` | `Name` (interface), `SSID`, `BSSID`; `X_AIGW_` band, channel, width, stations | no |
| `Device.X_AIGW_.` | `Mode` (off / advise / enforce), `ActionsAllowed`, `AlertDetails` | `Mode` |
| `Device.X_AIGW_.Alert.{i}.` | `Kind`, `Severity`, `Host` (a reference), `Subject`, `Reason`, `Score`, `FirstSeen`, `LastSeen`, `Count` | no |
| `Device.X_AIGW_.Decision.{i}.` | `Action`, `Target`, `Host`, `State`, `Origin`, `Reason`, `Note`, `Alert` (a reference), `Automatic`, `Created`, `Expires` | through commands |
| Commands | `Decision.{i}.Approve()`, `Reject()`, `Undo()` → `State`; `Quarantine(Host, Duration)`, `Block(Domain, Duration)` → `Decision` | |
| Events | `Device.X_AIGW_.Alert!` (Alert, Kind, Severity, Host, Subject, Reason); `Device.X_AIGW_.DecisionChanged!` (Decision, Action, State, Host) | |

Instance numbers:
- Devices are numbered from their MAC, and access points from their interface. The numbers are saved with the state and never reused.
- Alerts and decisions use their own IDs.
- A USP controller and, later, an ACS see the same `Device.Hosts.Host.3`, before and after a restart.

`agctl dm get Device.` shows all of it locally. `agctl dm set` and
`agctl dm do <command()>` act as you, not as a management system.

## The layer: `ubus ag.dm`

| Call | Does |
|---|---|
| `schema` | every parameter (path, type, writable, notify), object table, command (inputs, outputs) and event (arguments) |
| `instances {path}` | the object instances under a path, e.g. `Device.Hosts.Host.3.` |
| `get {paths, origin}` | values; a path ending in `.` means everything under it; unknown paths and gone instances come back under `errors` |
| `set {params, origin}` | all or nothing: every value is checked before any is applied |
| `operate {command, args, origin}` | runs a command; `out` holds its outputs |
| event `ag.dm.event {event, args}` | e.g. `Device.X_AIGW_.Alert!` |

`origin` is `local` unless an adapter says otherwise (`usp`, later `acs`). The
error codes are protocol-neutral (`not_found`, `read_only`, `invalid_value`,
`refused`, `invalid_arguments`, `internal`); each adapter maps them to its own.
For USP these are 7026 / 7016, 7013, 7012, 7006 / 7022, 7004 and 7003.

## The USP adapter: `aigw-usp`

- **OB-USP-Agent** is the Broadband Forum's agent (BSD-3-Clause), the one prplWare and RDK-B use. It's pinned to v11.0.7 and unmodified, built with MQTT and Unix domain sockets only (`scripts/build-obuspa.sh`, `platform/openwrt/obuspa`).
- **`aigw-usp.so`** is loaded with `obuspa -x`. At start it reads `ag.dm schema` and registers every path in one group of the agent's vendor hooks.
  - The agent's reads, writes and commands, and its instance refreshes (every 5 s at most), become `ag.dm` calls.
  - `ag.dm.event` becomes `USP_SIGNAL_DataModelEvent`, sent from the plug-in's own thread.
  - The agent's own `DeviceInfo` parameters keep their values; `SoftwareVersion` comes from `agd`.
- **Value-change subscriptions** are handled by the agent: it polls the values it watches.

## Adding to the data model

Everything lives in `agd/dm.c`, and neither adapter changes.

**A parameter:** one line in `params[]` and a getter:

```c
{ "Device.Hosts.Host.{i}.X_AIGW_Quarantined", T_BOOL, N, TB_HOST, g_h_quarantined, NULL },

GET(g_h_quarantined) { snprintf(buf, n, "%s", is_quarantined(c->dev) ? "true" : "false"); return 0; }
```

- Getters receive the resolved instance (`c->dev`, `c->ap`, `c->inst`).
- Flags: `W` means writable (it then needs a setter), and `N` means changes are worth notifying.

**A writable parameter:** add a setter. It is called once to check (`apply` 0) and
once to apply (`apply` 1), so a `set` of several values is all or nothing. It
returns `E_OK`, `E_INVALID_VALUE` or `E_REFUSED`. Look at `req_remote` and the
`mgmt.*` settings when a management system asks.

**An object table:** add an entry to `tables[]`, with a `list()` function that
returns its instance numbers, and a case in `resolve()`. Number instances from a
stable key with an `imap` (as Hosts do), or use an ID that is never reused.

**A command:** add an entry to `cmds[]`, with its inputs, its outputs and a handler.
A handler that changes something calls `act_request` (or `act_approve` /
`act_reject` / `act_revert`), so the guardrails apply. Check `cfg.mgmt_actions`
when `req_remote` is set.

**An event:** add an entry to `events[]`, and an `emit()` call where it happens in
`agd`.

Then:

```sh
ctest --test-dir build
agctl -j dm schema | scripts/dm-xml > docs/datamodel/aigw-dm.xml   # tests/dm-test.sh checks it
```

Rules for adding:

- **Standard names first.** Where TR-181 already has a parameter, use its exact name and type. Anything else gets the `X_AIGW_` prefix.
- **Never rename or remove a released parameter.** ISPs' controllers rely on paths staying the same. Deprecate it instead, keep serving it for at least one release, and say so in the release notes.
- **Raise `DM_VERSION` with every change.** It is reported as `Device.DeviceInfo.X_AIGW_DataModelVersion`.
- **At most one `{i}` per path** in this version.

## Tests

- **`tests/dm-test.sh`** tests the layer alone:
  - the schema, and the XML matching it;
  - reading subtrees, single instances and errors;
  - all-or-nothing writes;
  - what a management system sees and may do;
  - the `Alert!` event without the name;
  - instance numbers kept over a restart and never reused.
- **`tests/usp-test.sh`** runs a real USP controller (`tests/usp/uspc.py`, MQTT 5, the Broadband Forum's message definitions) against OB-USP-Agent with the plug-in, in network namespaces:
  - it reads the version, the devices, and the supported commands and events;
  - it gets the `Alert!` event, without the name;
  - it approves a proposed quarantine, and the quarantine really lands in nftables; then it undoes it;
  - it is refused quarantine and mode changes with `mgmt.actions 0`;
  - after a restart with `mgmt.actions 1`, it blocks a domain and sets the mode (which is saved), but it is refused a protected device;
  - instance numbers stay the same.

## Not done yet

- **TR-069 (v0.19):** a trial of the CWMP clients, then the adapter, tested against an ACS.
- **Writable Wi-Fi settings**, firmware upgrade over USP, and the WebSocket transport.
- **The vendor prefix** `X_AIGW_` is provisional. TR-106 expects `X_<OUI>_` or `X_<domain>_`, so the final prefix has to be chosen before ISPs depend on it. `Device.DeviceInfo.ManufacturerOUI` is OB-USP-Agent's placeholder until then.
- **The OpenWRT QEMU test** installs the packages but doesn't yet run the USP test there.
