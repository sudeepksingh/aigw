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
opkg install aigw-cwmp           # pulls icwmp, libcurl, mxml (on 25.12: mxml's apk from AIGW's release)
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
| `mgmt.manufacturer`, `mgmt.model_name`, `mgmt.product_class` | `AIGW OS project`, `AIGW OS on <host>`, `AIGW OS gateway` | an OEM's or ISP's own names in `DeviceInfo.` and the Inform's DeviceId |
| `mgmt.vendor_prefix` | `X_AIGW_` | the prefix of AIGW's own names (see below) |
| `mgmt.wifi`, `mgmt.firmware`, `mgmt.reset` | `1`, `1`, `1` | may a management system change Wi-Fi settings, upgrade the firmware, reboot or factory-reset the gateway |
| `usp.client_cert` | — | this gateway's TLS client certificate to the broker: one PEM file, certificate and key |
| `cwmp.client_cert`, `cwmp.client_key` | — | its TLS client certificate and key (PEM) to the ACS |
| `mgmt.firmware_handler`, `wifi.handler` | the platform's | the programs that carry those out (see below) |
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

## Your own names: vendor prefix and identity

Everything AIGW adds to TR-181 is a vendor extension, and TR-106 wants those
under the prefix of whoever ships the device: `X_<OUI>_` or
`X_<domain name, upper case, '.' as '-'>_`. AIGW OS doesn't pick one for you.
It writes `X_AIGW_` by default, and an ISP or OEM sets its own:

```sh
uci set aigw.mgmt.vendor_prefix='X_EXAMPLE-COM_'     # or X_00D09E_
uci set aigw.mgmt.manufacturer='Example Networks'
uci set aigw.mgmt.model_name='EX-1000'
uci set aigw.mgmt.product_class='EX Gateway'
uci set aigw.mgmt.oui='00D09E'
uci commit aigw && /etc/init.d/aigw restart     # then aigw-usp / aigw-cwmp
```

Then `Device.X_AIGW_.Mode` is `Device.X_EXAMPLE-COM_.Mode`,
`Device.Hosts.Host.1.X_AIGW_Zone` is `...X_EXAMPLE-COM_Zone`, the events are
`Device.X_EXAMPLE-COM_.Alert!`, and so on, over USP and TR-069 alike. Only `agd`
knows the prefix (`agctl -j dm schema` reports it as `vendor_prefix`); both
adapters take every name from it. The data model file for your controller or
ACS is made the same way:
`agctl -j dm schema | scripts/dm-xml > my-dm.xml`.

- Choose the prefix before a controller or ACS starts using the names: they
  change with it.
- A prefix that isn't `X_`, upper-case letters, digits, `-` or `_`, and `_` is
  refused (agd says so in its log) and `X_AIGW_` is used.
- In this document, read `X_AIGW_` as your prefix.

## Wi-Fi settings

An ISP's help desk changes the Wi-Fi name and passphrase, the security mode,
the channel and the width through the standard TR-181 parameters, over USP or
TR-069 alike:

| Parameter | Values |
|---|---|
| `Device.WiFi.SSID.{i}.SSID` | 1 to 32 characters |
| `Device.WiFi.AccessPoint.{i}.Security.KeyPassphrase` | 8 to 63 printable characters, or 64 hex digits; always reads empty |
| `Device.WiFi.AccessPoint.{i}.Security.ModeEnabled` | `None`, `WPA2-Personal`, `WPA3-Personal`, `WPA2-WPA3-Personal` |
| `Device.WiFi.Radio.{i}.Channel` | a channel AIGW uses on that band |
| `Device.WiFi.Radio.{i}.OperatingChannelBandwidth` | `20MHz`, `40MHz`, `80MHz` (5 GHz), `Auto` |

- agd checks every value, and `mgmt.wifi 0` stops a management system from
  changing any of them (the default is `1`: managing Wi-Fi is what an ISP is
  for).
- A request is still all or nothing. Everything it changes on one access point
  goes to the **Wi-Fi handler** in one call:
  - On OpenWRT, `/usr/libexec/aigw/wifi-config` writes `/etc/config/wireless`
    and reloads that radio.
  - On a PC, it writes `/etc/aigw/pc-ap.conf` and restarts that access point
    (`aigw-ap reconf`). There, the name, passphrase and mode are the same on
    both bands.
- Until the access point comes back with the new values, reads show them.
- agd logs who asked (`wifi: wlan0 settings applied (asked by usp)`).

One access point is one radio, one SSID and one AccessPoint, with the same
instance number in all three.

## Firmware upgrades

The standard TR-181 firmware objects:

- `Device.DeviceInfo.FirmwareImage.{i}.` (`Name`, `Version`, `Status`,
  `Available`).
- `ActiveFirmwareImage` and `BootFirmwareImage`.
- `Download(URL, AutoActivate, Username, Password, FileSize, CheckSumAlgorithm,
  CheckSum)` and `Activate()`. Both are asynchronous: over USP they end with
  OperationComplete.

`SoftwareVersion` is the running firmware's version, and
`AdditionalSoftwareVersion` is AIGW OS's own.

| Over | What happens |
|---|---|
| USP | `Download()` fetches and checks the image into a free slot. With `AutoActivate` (or a later `Activate()`) it is written or set to boot, and agd reboots into it. |
| TR-069 | icwmp's own `Download` RPC (file type `1 Firmware Upgrade Image`): icwmp fetches the file and checks it with `sysupgrade -T`. It then calls `FirmwareImage.N.Download()` and the `fwbank` bank calls, which aigw-cwmp-dm answers from agd, and reboots. After the boot it sends `TransferComplete`. Before the image is activated, aigw-cwmp-dm saves icwmp's session in `/etc/icwmpd`, which is kept over the upgrade. |

Nothing is written to flash before the image has passed its checks. A
management system may do this only with `mgmt.firmware 1` (the default).

### The firmware handler: your flash, your rules

What "an image", "check" and "activate" mean differs from one device to the
next, so agd leaves it to a small program, `mgmt.firmware_handler`
(default `/usr/libexec/aigw/firmware`):

| Call | Does |
|---|---|
| `firmware version` | the running firmware's version, one line |
| `firmware list` | one line per image: `<id> <status> <active 0/1> <boot 0/1> <version or -> <name>`; status as TR-181 (`NoImage`, `Active`, `Available`, `DownloadFailed`, `ValidationFailed`, ...) |
| `firmware download <id> <url>` | fetch and check; exit 0 when it is in slot `<id>`, 2 when it could not be fetched, 3 when it failed its checks. Credentials and checksum come in the environment (`AIGW_FW_USER`, `AIGW_FW_PASSWORD`, `AIGW_FW_SIZE`, `AIGW_FW_CHECKSUM_ALGORITHM`, `AIGW_FW_CHECKSUM`) |
| `firmware activate <id>` | exit 0 when that image boots next (it may also write it and reboot itself) |
| `firmware reboot` | reboot |

- **OpenWRT** (the default handler): image 1 is the running one, and image 2 is
  a slot in RAM. A download is checked with `sysupgrade -T`, and its version
  is read from the image's metadata. Activating it runs `sysupgrade`, keeping
  the configuration.
- **A/B flash, signed images, an OEM's own update agent:** point
  `mgmt.firmware_handler` at a program with the interface above.
  `tests/fw/firmware` is a two-bank example.
- **A PC** has no firmware image. Without a handler, the firmware objects are
  empty and `Download()` is refused.

## On prplOS: an Ambiorix component (`aigw-amx`)

prplOS builds its TR-181 tree from Ambiorix components. Each publishes its
objects on the system bus, and prplOS's own USP agent, TR-069 client, CLI and
web UI read them there. `aigw-amx` makes AIGW OS one of those components, so on
prplOS no AIGW protocol client is needed:

- **It runs** as a plug-in in `amxrt`, prplOS's component runtime, from
  `/etc/amx/aigw-amx/aigw-amx.odl`.
- **It publishes only AIGW's own subtree:** `X_<prefix>_` with its `Alert` and
  `Decision` tables, the mode, the commands (`Quarantine`, `Block`, and
  `Approve` / `Reject` / `Undo` on decisions) and the events (`Alert!`,
  `DecisionChanged!`). What prplOS already has (`WiFi`, `Hosts`, `DeviceInfo`,
  firmware) stays prplOS's. On prplOS, AIGW's Wi-Fi and firmware handlers can
  point at prplOS's own objects.
- **It is built from agd's schema at start**, like the USP and TR-069 adapters,
  so the vendor prefix and new parameters follow.
- **Reads and writes:**
  - reads are agd's values, typed (booleans, numbers, dates);
  - a write is checked by agd when amxd validates it, then applied;
  - errors are amxd's: invalid value (10), permission denied (23).
- **Commands and events:**
  - commands return their outputs (e.g. `Block()` returns `Decision`);
  - instances follow agd within a second of an alert or a decision, and every 5 seconds;
  - agd's events become Ambiorix signals.
- **Who asks:** origin `prplos` (`AIGW_AMX_ORIGIN`). The home's rules hold as for
  any management system (`mgmt.actions`, `mgmt.alert_details`).

Building it needs Ambiorix. It is built whenever CMake finds the headers, as in
prplOS's SDK. On a PC, `scripts/build-ambiorix.sh` builds the pinned libraries
and `amxrt`:

```sh
scripts/build-ambiorix.sh                 # ~/.cache/aigw-amx/root
cmake -S . -B build -DAIGW_AMX_ROOT=$HOME/.cache/aigw-amx/root && cmake --build build
amxrt -B .../mod-amxb-ubus.so -u ubus:/var/run/ubus/ubus.sock build/amx/aigw-amx-build.odl
```

Not done yet:
- a prplOS feed package and init script;
- mounting `X_<prefix>_` under prplOS's `Device.` (the way the image joins its components' objects);
- a run on a real prplOS image.

The plug-in, Ambiorix and amxb_ubus are tested together in CI (`tests/amx-test.sh`).

## Reboot and factory reset

`Device.Reboot()` and `Device.FactoryReset()` over USP, and the TR-069
`Reboot` and `FactoryReset` RPCs:

- **USP:** OB-USP-Agent owns these commands. Without a plug-in hook it would
  only restart itself. AIGW's plug-in hands them to agd (`ubus ag.dm system`).
- **TR-069:** icwmp's `FactoryReset` runs `defaultreset`, a command iopsys's
  firmware has and OpenWRT doesn't. AIGW ships one (`/usr/libexec/aigw/defaultreset`)
  that asks agd too. icwmp's `Reboot` uses the system's own reboot.
- **agd** decides whether a management system may (`mgmt.reset`, default 1),
  logs who asked, and two seconds later runs the firmware handler's `reboot` or
  `factory-reset`. On OpenWRT that is `firstboot -y` and a reboot.

## Client certificates

Carrier networks often know their gateways by a TLS client certificate:

- **USP:** `usp.client_cert` is one PEM file with the certificate and its key.
  OB-USP-Agent presents it to the MQTT broker (`-a`).
- **TR-069:** `cwmp.client_cert` and `cwmp.client_key` are given to icwmp
  (`cpe.client_cert_path`, `client_key_path`), which presents them to the ACS.
  `cwmp.ca_file` may be a file or a directory.

Keep the key readable by root only (`chmod 600`).

The Wi-Fi handler (`wifi.handler`) is replaceable the same way:
`wifi-config set <iface>`, with the new values in `AIGW_WIFI_SSID`,
`AIGW_WIFI_KEY`, `AIGW_WIFI_MODE`, `AIGW_WIFI_CHANNEL` and `AIGW_WIFI_WIDTH`.

## The data model (version 1.5)

The full definition, as ISPs load it into their controllers, is
[`datamodel/aigw-dm.xml`](datamodel/aigw-dm.xml). It is made from what `agd`
serves (`agctl -j dm schema | scripts/dm-xml`), and `tests/dm-test.sh` fails when
the two differ.

| Path | Contents | Writable |
|---|---|---|
| `Device.DeviceInfo.` | `Manufacturer`, `ModelName`, `ProductClass`, `ManufacturerOUI`, `SerialNumber`, `SoftwareVersion`, `AdditionalSoftwareVersion`, `UpTime`, `X_AIGW_DataModelVersion`, `ActiveFirmwareImage`, `BootFirmwareImage` | no |
| `Device.DeviceInfo.FirmwareImage.{i}.` | `Name`, `Version`, `Available`, `Status`; `Download()`, `Activate()` | through commands |
| `Device.Hosts.Host.{i}.` | `PhysAddress`, `IPAddress`, `HostName`, `Active`; `X_AIGW_` type, zone, first and last seen, rates and bytes up and down, Wi-Fi band and signal | no |
| `Device.WiFi.Radio.{i}.` | `Enable`, `Status`, `Name`, `SupportedFrequencyBands`, `OperatingFrequencyBand`, `Channel`, `SupportedOperatingChannelBandwidths`, `OperatingChannelBandwidth` | `Channel`, `OperatingChannelBandwidth` |
| `Device.WiFi.SSID.{i}.` | `Name` (interface), `LowerLayers`, `SSID`, `BSSID`; `X_AIGW_` band, channel, width, stations | `SSID` |
| `Device.X_AIGW_.WiFiHealth.{i}.` | Self-Healing Wi-Fi, one per radio: `Radio`, `Status`, `Cause`, `Explanation`, `LossPercent`, `LatencyMs`, `StationsMeasured`, `AffectedHosts`, `Since`; event `WiFiHealthChanged!`; the healing report `Stats.Day.`, `Stats.Week.`, `Stats.Month.` (`GoodSeconds`, `DegradedSeconds`, `PoorSeconds`, `GoodPercent`, `ProblemsFound` and one count per cause, `ChannelTrialsKept`, `ChannelTrialsTried`, `BandRules`, `Fixes`, `MeanTimeToFix`, `LossBeforePercent`, `LossAfterPercent`) ([self-healing-wifi.md](self-healing-wifi.md)) | no |
| `Device.WiFi.AccessPoint.{i}.` | `SSIDReference`, `AssociatedDeviceNumberOfEntries`, `Security.ModesSupported`, `Security.ModeEnabled`, `Security.KeyPassphrase` | `ModeEnabled`, `KeyPassphrase` |
| `Device.X_AIGW_.` | `Mode` (off / advise / enforce), `ActionsAllowed`, `AlertDetails` | `Mode` |
| `Device.X_AIGW_.Alert.{i}.` | `Kind`, `Severity`, `Host` (a reference), `Subject`, `Reason`, `Score`, `FirstSeen`, `LastSeen`, `Count` | no |
| `Device.X_AIGW_.Decision.{i}.` | `Action`, `Target`, `Host`, `State`, `Origin`, `Reason`, `Note`, `Alert` (a reference), `Automatic`, `Created`, `Expires` | through commands |
| `Device.X_AIGW_.LastDiagnosis.` | the last Diagnose: `Time`, `Host`, `Code`, `Severity`, `Summary`, `Advice`, `Findings`; event `DiagnosisDone!` ([diagnose.md](diagnose.md)) | no |
| Commands | `Decision.{i}.Approve()`, `Reject()`, `Undo()` → `State`; `Quarantine(Host, Duration)`, `Block(Domain, Duration)` → `Decision`; `Diagnose(Host)` (async, read-only: no `mgmt.actions` needed) → `Code`, `Area`, `Severity`, `Summary`, `Advice`, `Findings`, `InternetLossPercent`, `InternetLatencyMs`, `NextHopLatencyMs`, `DNSLatencyMs` | |
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
  - instance numbers kept over a restart and never reused;
  - an OEM's names and vendor prefix in every path, command and event; a bad prefix refused.
- **`tests/usp-test.sh`** runs a real USP controller (`tests/usp/uspc.py`, MQTT 5, the Broadband Forum's message definitions) against OB-USP-Agent with the plug-in, in network namespaces:
  - it reads the version, the devices, and the supported commands and events;
  - it gets the `Alert!` event, without the name;
  - it approves a proposed quarantine, and the quarantine really lands in nftables; then it undoes it;
  - it is refused quarantine and mode changes with `mgmt.actions 0`;
  - after a restart with `mgmt.actions 1`, it blocks a domain and sets the mode (which is saved), but it is refused a protected device;
  - instance numbers stay the same.
- **`tests/cwmp-test.sh`** does the same for TR-069, in network namespaces, under an ISP's own prefix (`X_EXAMPLE-COM_`).
  - **Part 1** asks `aigw-cwmp-dm` the way icwmp asks it (ubus `bbfdm`):
    - values and types, the identity, and `ConnectionRequestURL`;
    - names with `NextLevel`, and instances;
    - faults 9001 / 9005 / 9007 / 9008;
    - a write applied only at commit and dropped at revert;
    - `LastEvent.` without the name;
    - a refused quarantine, and an approved decision that really lands in nft and is then undone;
    - `fwbank dump` and `set_bootbank`, a bad image as fault 9010, and no reboot by agd for the ACS.
  - **Part 2** runs icwmp against an ACS (`tests/cwmp/acs.py`, SOAP over HTTP):
    - the boot Inform's DeviceId and parameters;
    - connection requests;
    - GetParameterValues and GetParameterNames;
    - a SetParameterValues fault 9001 per parameter;
    - a refused command;
    - an alert as `4 VALUE CHANGE`;
    - an approved quarantine;
    - a new `ParameterKey` and Inform interval, saved in `/etc/config/aigw`;
    - a firmware `Download`: icwmp fetches it, agd puts it in bank 2, icwmp reboots, and after the boot comes `1 BOOT`, `7 TRANSFER COMPLETE`, the new `SoftwareVersion` and a `TransferComplete` without a fault.
  - icwmp's own paths are private to the test (overlays in a mount namespace). There, `reboot` is a script that switches the test's banks and stops icwmp, so the test can never reboot the machine.
- **Firmware in dm-test and usp-test** uses `tests/fw/firmware`, a two-bank handler:
  - a bad image and a wrong checksum are refused;
  - a download into the running bank is refused;
  - `mgmt.firmware 0` holds;
  - over USP, `Download(AutoActivate)` ends with OperationComplete and a reboot into the new version.
- **`tests/amx-test.sh`** (prplOS) runs agd and `amxrt` with aigw-amx on a private ubus, as prplOS's agents reach components:
  - the subtree is published from the schema, with typed reads;
  - an alert becomes an instance and the `Alert!` signal, without the name;
  - an invalid mode is refused (10), and so are a mode change and `Block()` with `mgmt.actions 0` (23);
  - with `mgmt.actions 1`, the mode is set and saved, and a `Block()` and its `Undo()` are recorded as by prplos.
- **Reboot and factory reset:**
  - dm-test: `mgmt.reset 0` refuses a management system; a factory reset reaches the handler;
  - usp-test: `Device.Reboot()` reaches the handler through the plug-in's hook;
  - cwmp-test: an ACS's `FactoryReset` reaches it through `defaultreset`.
- **Client certificates:**
  - usp-test: the agent reaches mosquitto on a TLS listener that requires a client certificate;
  - cwmp-test part 2: the ACS speaks HTTPS and lets in only icwmp with its certificate (the test checks which certificate the Inform came with).
- **On OpenWRT itself** (`platform/openwrt/qemu-test.sh`, CI, OpenWRT 24.10 in QEMU, after the self-test):
  - OB-USP-Agent with the plug-in answers on the device on all three architectures;
  - icwmp sends its boot Inform to an ACS on the host and answers it;
  - on x86-64, the ACS upgrades the firmware for real: icwmp downloads the image, `sysupgrade -T` and agd's OpenWRT handler check it, `sysupgrade` writes it, and the VM reboots. The configuration is kept, the packages are reinstalled (an image with AIGW built in has them), and the ACS gets `1 BOOT`, `7 TRANSFER COMPLETE` and a `TransferComplete` without a fault.
- **`tests/hwsim-test.sh`** (simulated radios, CI) sets a new name, passphrase and channel through the data model. It checks that hostapd comes back with them, that they are saved in `pc-ap.conf`, and that the client joins again with the new passphrase.

## Not done yet

- **More Wi-Fi settings** (turning an access point off, guest networks, `RegulatoryDomain`), firmware `TimeWindow`s and scheduled downloads, and the WebSocket transport.
- **USP to a real broker in QEMU:** the agent is asked through its CLI there, not by a controller over MQTT; the firmware upgrade on OpenWRT is tested over TR-069.
- **Firmware upgrades on MIPS and ARM in QEMU:** those images boot from an initramfs, so there is nothing to `sysupgrade`.
