# Device management: USP (TR-369) and TR-069

An ISP can manage an AIGW OS gateway with the protocol it already runs. The
data, the instance numbers and the rules exist once, in `agd`. Each protocol
client only translates.

```
  USP controller (ISP)                     ACS (ISP, TR-069)
        │ MQTT 5 (TLS)                          │ SOAP over HTTP(S)
┌───────▼───────────────────┐       ┌───────────▼──────────────┐
│ aigw-usp                  │       │ aigw-cwmp                │
│ OB-USP-Agent, unmodified, │       │ icwmp, unmodified,       │
│ + aigw-usp.so plug-in     │       │ + aigw-cwmp-dm (ubus     │
│                           │       │   bbfdm, bbf.config)     │
└───────┬───────────────────┘       └───────────┬──────────────┘
        └──────────── ubus: ag.dm ──────────────┘
                   ┌───────▼────────┐
                   │ agd: dm.c      │  schema, instance numbers, write rules,
                   │ (data layer)   │  error codes, events, identity
                   └───────┬────────┘
                   devices · alerts · decisions (act_request + guardrails) · Wi-Fi
```

Status: the data layer and USP came in v0.18, TR-069 in v0.19. Both run at the
same time if an ISP wants: they see the same devices with the same instance
numbers, and the same rules apply.

## Turning it on

On OpenWRT, USP:

```sh
opkg install aigw-usp            # pulls obuspa, libmosquitto-ssl, libsqlite3
uci set aigw.mgmt.enabled=1
uci set aigw.usp.broker=usp.example.net
uci set aigw.usp.controller_id='self::usp-controller'   # the ISP's controller endpoint ID
uci commit aigw
/etc/init.d/aigw-usp enable
/etc/init.d/aigw-usp start
```

TR-069:

```sh
opkg install aigw-cwmp           # pulls icwmp, libcurl, libmxml
uci set aigw.mgmt.enabled=1
uci set aigw.cwmp.url=https://acs.example.net:7547/
uci set aigw.cwmp.cr_username=cr; uci set aigw.cwmp.cr_password='<secret>'   # connection requests
uci commit aigw
/etc/init.d/aigw-cwmp enable
/etc/init.d/aigw-cwmp start      # also allows TCP 7547 from the WAN zone, for connection requests
```

On a PC (PC as AP): `sudo AIGW_USP=1 AIGW_CWMP=1 platform/pc-ap/install.sh` builds
the pinned clients (either or both) and AIGW's adapters; then set the same
options in `/etc/config/aigw` and run `sudo systemctl enable --now aigw-usp`
or `aigw-cwmp`. For TR-069 on a PC, also set `aigw.cwmp.wan_device` to the
interface towards the ACS: there is no netifd to ask.

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
| `mgmt.serial`, `mgmt.oui` | machine id or LAN MAC, LAN MAC's OUI | `DeviceInfo.SerialNumber` and `ManufacturerOUI`, for both protocols |
| `cwmp.url`, `cwmp.username`, `cwmp.password`, `cwmp.ca_file` | — | the ACS and its login |
| `cwmp.periodic_enable`, `cwmp.periodic_interval` | `1`, `86400` | periodic Informs |
| `cwmp.port`, `cwmp.cr_username`, `cwmp.cr_password` | `7547` | connection requests from the ACS |
| `cwmp.wan`, `cwmp.wan_device` | `wan`, — | the WAN interface; on a PC its device too |
| `cwmp.provisioning_code`, `cwmp.notify_interval`, `cwmp.log_level` | —, `5`, `INFO` | |

`aigw-cwmp-run --print` shows what it gives icwmp. icwmp keeps its own config
(`/etc/config/cwmp`); the options above are written into it at every start.
When the ACS changes `Device.ManagementServer.URL`, `Username`, `Password`,
`PeriodicInformEnable`, `PeriodicInformInterval`, the connection request
login or `DeviceInfo.ProvisioningCode`, the change is saved in both places.

## What a management system may do

Whatever protocol it uses, and decided in `agd`, not in the protocol client:

- **Read** everything below, except alert subjects and reasons, and blocked domains, while `mgmt.alert_details` is `0`.
- **Approve, reject and undo** decisions AIGW proposed. The decision records who, e.g. `policy dga, approved by usp`.
- **Quarantine, block, set `Mode`**: only with `mgmt.actions 1`.
- **Never** beyond AIGW's guardrails: no protected device (`act.protect`), no device outside the LAN, a time limit on every action. These are the same checks as for `agctl`.

OB-USP-Agent itself gives the controller full access (its role `Full Access`), so
that the one place these rules live is the data layer. The same holds for icwmp:
aigw-cwmp-dm asks with origin `acs`, and agd decides.

## The data model (version 1.1)

The full definition, as ISPs load it into their controllers, is
[`datamodel/aigw-dm.xml`](datamodel/aigw-dm.xml). It is made from what `agd`
serves (`agctl -j dm schema | scripts/dm-xml`), and `tests/dm-test.sh` fails when
the two differ.

| Path | Contents | Writable |
|---|---|---|
| `Device.DeviceInfo.` | `Manufacturer`, `ModelName`, `ProductClass`, `ManufacturerOUI`, `SerialNumber`, `SoftwareVersion`, `UpTime`, `X_AIGW_DataModelVersion` | no |
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
| `set {params, origin, check}` | all or nothing: every value is checked before any is applied; with `check` only checked |
| `operate {command, args, origin}` | runs a command; `out` holds its outputs |
| event `ag.dm.event {event, args}` | e.g. `Device.X_AIGW_.Alert!` |

`origin` is `local` unless an adapter says otherwise (`usp`, `acs`). The
error codes are protocol-neutral (`not_found`, `read_only`, `invalid_value`,
`refused`, `invalid_arguments`, `internal`); each adapter maps them to its own.
For USP these are 7026 / 7016, 7013, 7012, 7006 / 7022, 7004 and 7003; for
TR-069 9005, 9008, 9007, 9001, 9003 and 9002.

## The USP adapter: `aigw-usp`

- **OB-USP-Agent** is the Broadband Forum's agent (BSD-3-Clause), the one prplWare and RDK-B use. It's pinned to v11.0.7 and unmodified, built with MQTT and Unix domain sockets only (`scripts/build-obuspa.sh`, `platform/openwrt/obuspa`).
- **`aigw-usp.so`** is loaded with `obuspa -x`. At start it reads `ag.dm schema` and registers every path in one group of the agent's vendor hooks.
  - The agent's reads, writes and commands, and its instance refreshes (every 5 s at most), become `ag.dm` calls.
  - `ag.dm.event` becomes `USP_SIGNAL_DataModelEvent`, sent from the plug-in's own thread.
  - The agent's own `DeviceInfo` parameters keep their values; `SoftwareVersion` comes from `agd`.
- **Value-change subscriptions** are handled by the agent: it polls the values it watches.

## The TR-069 adapter: `aigw-cwmp`

- **icwmp** is iopsys's TR-069 client (BSD-3-Clause), the one in iopsys's
  firmware. It is maintained and uses only standard OpenWRT libraries. It's
  pinned to a commit and unmodified (`scripts/build-icwmp.sh`,
  `platform/openwrt/icwmp`).
  - It reaches its data model only through the ubus object `bbfdm` (`get`,
    `schema`, `set`, `add`, `del`, `operate`). After a SetParameterValues it
    commits or reverts through `bbf.config`.
  - Normally bbfdmd serves both. Here `aigw-cwmp-dm` serves them in front of
    `ag.dm`.
  - easycwmp was the other candidate. It is GPLv2, has not been updated since
    2019, and its data model is shell scripts.
- **`aigw-cwmp-dm`** translates. TR-069 has things USP doesn't, and those live
  here, not in agd:
  - **Paths and values** are agd's. `GetParameterNames` lists objects and
    instances from `ag.dm schema` and `instances`.
  - **A SetParameterValues is all or nothing.** Each value is checked when
    icwmp sets it (`ag.dm set` with `check`). All of them are applied in one
    `ag.dm set` when icwmp commits, and dropped when it reverts.
  - **`Device.ManagementServer.`** comes from icwmp's own config: `URL`,
    `Username`, `Password`, `PeriodicInformEnable`, `PeriodicInformInterval`,
    `ParameterKey`, `ConnectionRequestURL`, `ConnectionRequestUsername`,
    `ConnectionRequestPassword`. Also `Device.RootDataModelVersion` and
    `DeviceInfo.ProvisioningCode`. Passwords read as empty.
  - **Commands, `Device.X_AIGW_.Request.`:** CWMP has no commands. So set
    `Command` to a command's name (`Quarantine`, `Block`) or its path
    (`Device.X_AIGW_.Decision.3.Approve()`), together with its inputs (`Host`,
    `Domain`, `Duration`), in one SetParameterValues.
    - It runs when icwmp commits.
    - `Status` (`Done`, `Refused`, `Error`) and `Message` tell the result, and
      the outputs (`Decision`, `State`) are read in the same session.
    - Inputs are used once.
  - **Events, `Device.X_AIGW_.LastEvent.`:** `Name`, `Time`, `Count` and the
    event's arguments.
    - `Count` has active notification from the start, so every agd event (an
      alert, a decision that changed) reaches the ACS as `4 VALUE CHANGE`
      within `cwmp.notify_interval` seconds.
    - Alerts stay in `Device.X_AIGW_.Alert.` too, if two come between polls.
  - **On a PC** it also answers `network.interface.<wan> status`, which icwmp
    asks for its WAN device.

| CWMP | What happens |
|---|---|
| GetParameterValues | `ag.dm get`, plus the adapter's own parameters |
| GetParameterNames | the schema, with instances; `NextLevel` honoured |
| SetParameterValues | checked per value, applied in one set at commit; faults per parameter |
| AddObject, DeleteObject | refused (9005): AIGW's tables are not made by an ACS |
| Inform | DeviceId (`Manufacturer`, `ManufacturerOUI`, `ProductClass`, `SerialNumber`) and the forced parameters, all from agd or the adapter |

## Adding to the data model

Everything lives in `agd/dm.c`, and neither adapter changes. A new command
shows up in TR-069 as a value of `Request.Command`, its inputs and outputs as
`Request.` parameters; a new event's arguments as `LastEvent.` parameters.

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
- **`tests/cwmp-test.sh`** does the same for TR-069, in network namespaces.
  - **Part 1** asks `aigw-cwmp-dm` the way icwmp asks it (ubus `bbfdm`):
    - values and types, the identity, and `ConnectionRequestURL`;
    - names with `NextLevel`, and instances;
    - faults 9001 / 9005 / 9007 / 9008;
    - a write applied only at commit and dropped at revert;
    - `LastEvent.` without the name;
    - a refused quarantine, and an approved decision that really lands in nft and is then undone.
  - **Part 2** runs icwmp against an ACS (`tests/cwmp/acs.py`, SOAP over HTTP):
    - the boot Inform's DeviceId and parameters;
    - connection requests;
    - GetParameterValues and GetParameterNames;
    - a SetParameterValues fault 9001 per parameter;
    - a refused command;
    - an alert as `4 VALUE CHANGE`;
    - an approved quarantine;
    - a new `ParameterKey` and Inform interval, saved in `/etc/config/aigw`.
  - icwmp's own paths are private to the test (overlays in a mount namespace).

## Not done yet

- **Writable Wi-Fi settings**, firmware upgrade (USP and TR-069 Download), and the WebSocket transport.
- **TR-069 over HTTPS with client certificates** is icwmp's (`cwmp.ca_file`; client certificates not yet in `/etc/config/aigw`), and is not tested here.
- **The vendor prefix** `X_AIGW_` is provisional. TR-106 expects `X_<OUI>_` or `X_<domain>_`, so the final prefix has to be chosen before ISPs depend on it. `Device.DeviceInfo.ManufacturerOUI` is OB-USP-Agent's placeholder until then.
- **The OpenWRT QEMU test** installs the packages but doesn't yet run the USP or TR-069 tests there.
