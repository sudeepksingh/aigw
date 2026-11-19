# Security: what AIGW OS exposes, to whom, and how it is protected

This is AIGW OS's threat model, written for v0.43.0 after a review of the whole
code base before the public try-it image brought new readers. It lists every
way into a gateway running AIGW OS and into cortex, who can use each one, what
stops everyone else, what the review found and fixed, and what it chose not to
fix (with the reason). To report a problem, see [SECURITY.md](../SECURITY.md).

## The parts and who talks to them

```
 the internet ──(WAN)──┐                              ┌── ISP: ACS (TR-069), USP controller (MQTT)
                       │                              │
 phones away from ── WireGuard ──┐   ┌── icwmp ── aigw-cwmp-dm ──┐
 home (remote access)            │   │   obuspa ── aigw-usp.so ───┤
                                 ▼   ▼                            ▼
 LAN devices ── DNS ──► agdns ──► ubus ◄── agd (the brain, root) ──► platform handlers
                                  ▲  ▲         │                     (firmware, wifi-config,
          LuCI (rpcd ACL) ────────┘  │         └── plug-ins (user nobody) ── bridge ──► cortex
          agctl (root, or ubus ACL) ─┘                                                (fleet API,
                                                                                       dashboard, MCP)
```

| Who | Reaches | Trusted for |
|-----|---------|-------------|
| root on the gateway | everything | everything (out of scope) |
| a LAN device | agdns (DNS), the gateway's web UI login page, DHCP | nothing: its traffic and names are data AIGW judges |
| the internet (WAN) | WireGuard's UDP port when remote access is on; TR-069 connection requests (port 7547) when an ACS is set; nothing else | nothing |
| a phone with remote access | the LAN, through its WireGuard peer | what a LAN device may do |
| a LuCI user | ubus methods in the AIGW rpcd ACL (read, or read and write) | as rpcd grants |
| a local non-root user on a PC as AP | ubus methods in `platform/pc-ap/ubus-acl.json.in` | reading, and answering AIGW's proposals |
| a plug-in | its own pipe to agd, as user `nobody`, its capabilities | what its manifest asks and you allowed |
| the ISP's ACS / USP controller | the data model (`ag.dm`) | what `aigw.mgmt.*` allows |
| cortex (via the bridge) | proposals, models, firmware offers, area notices | proposals wait for a person; models must be signed; firmware needs `firmware.install` and `mgmt.firmware` |
| an operator | cortex's dashboard, fleet API, MCP | the fleet |

## Each way in

### Cortex: the fleet API (`python3 -m aigw_cortex serve`)

- **Who**: gateways' bridges. Every request carries `Authorization: Bearer
  <token>` from `enroll`; cortex keeps only the token's SHA-256 and looks it up
  by hash, so a lookup's timing tells nothing useful about a token.
- **Binds** 127.0.0.1:8790 by default. **Fixed in 0.43**: bound to any other
  address without `--cert`, it now says that tokens and reports travel in the
  clear (`server.exposure_note`).
- Model files are served by name only (`FILE_RE`, no `/` or `..`).
- **Fixed in 0.43**: a request with a malformed `Content-Length` got a stack
  trace; now 400.
- **Fixed in 0.43**: the data directory was created with the default umask, so
  `cortex.db` (homes' reports, device names where shared, token hashes) was
  readable by every local user. Now the directory is 0700 and `cortex.db` 0600,
  and an existing directory is tightened with a note at start.
- The signing key (`keys/cortex.key`) was already 0600 in a 0700 directory.

### Cortex: the operator dashboard and its write token

- **Binds** 127.0.0.1:8090 by default. On loopback, reading needs no token, but
  only for a request whose `Host` is local (against DNS rebinding). On any other
  address, every request needs the token.
- **Writes** (pausing or aborting a rollout) always need the token, in the
  `Authorization` header. A page from another site can't set that header
  without a CORS preflight, which the dashboard never answers, so there is no
  CSRF. Resuming or starting a rollout is not possible from the dashboard.
- **XSS**: everything a gateway sent is put on the page with `textContent`
  (never HTML), no attribute takes a URL from data, the JSON escapes `<>&`, and
  the CSP allows only the page's own script and style (`default-src 'none'`),
  with `frame-ancestors 'none'` and `nosniff`.
- The token is made on first use (`dashboard.token`, 0600). The browser keeps it
  in `sessionStorage` for the tab; the start link carries it in the URL's
  `#fragment`, which never reaches a server, and the page removes it from the
  address bar.
- **Not changed**: the start line prints the link with the token on stderr, for
  the operator who started it. Under systemd that lands in the journal, which
  on most systems only root and the `adm`/`systemd-journal` groups read. Start it
  with `--quiet` and read `dashboard.token` instead if your logs go further.

### Cortex: the ISP's ticketing (TMF621 and webhooks, v0.42)

Cortex sends area outages and proactive tickets to the ISP's own system
([isp-ticketing.md](isp-ticketing.md)); off unless `tickets.json` turns it on.

- **Secrets** (the OAuth2 client secret, a bearer token, the webhook secrets,
  the listener's token) come only from a file or an environment variable, read
  when used; inline ones are refused. They never appear in logs, errors, the
  outbox, `tickets config`, the dashboard or MCP (`test_tickets.py`
  `test_secrets_never_shown`); an OAuth2 access token is kept in memory only.
  **Fixed in 0.43**: secret files were used whatever their mode. Now one that
  other users can read, or that its group or others can write, is refused
  (0600, or 0640 for cortex's group), and the row stays `failed` with
  "chmod 600 ...", never retried with the secret.
- **Where secrets go** is the configuration's: the TMF621 URL, the OAuth2
  token URL, the webhook URLs, the hub callback. That is the SSRF and
  credential-leak path, so **fixed in 0.43**: a `tickets.json` that its group
  or others can write is refused (anyone who could edit it could send the
  bearer token or client secret to their own host). Nothing a gateway or the
  ISP sends chooses a URL: the ISP's ticket ids go into paths URL-encoded,
  its `href`s are kept but never fetched.
- **TLS**: outbound calls use Python's default context (certificates and host
  names checked; `ca_file` adds a private CA, nothing turns checking off);
  `http://` only to this machine unless `allow_http`; no credentials in URLs;
  **redirects are not followed**, so a token never goes to another host.
- **Webhooks** are signed: `HMAC-SHA256(secret, "<timestamp>." + body)`, with a
  unique delivery id in the body and a new timestamp per try. The signature does
  not cover the `X-AIGW-Delivery` and `X-AIGW-Event` headers. **Fixed in 0.43**:
  the documented receiver check verified the signature and the timestamp window
  but deduplicated on the unsigned header, so a replay within 5 minutes with a
  new `X-AIGW-Delivery` would pass. `tickets.verify_delivery` and the example in
  isp-ticketing.md now take the id and the event from the signed body, refuse
  headers that differ, and refuse an id already seen (constant-time compare).
- **The hub's callback** (`POST /tmf621/listener`, on the fleet API's port)
  needs its own bearer token (`tmf621.listener`), compared in constant time;
  gateways' tokens don't work there, and without a configured token the path
  doesn't exist (404). A token file readable by others refuses every call (401;
  0.43). It changes only a ticket's ISP status, and may end a *declared* outage
  the ISP resolved. Put it behind HTTPS (cortex warns without TLS, 0.43).
- **The outbox** is rows in `cortex.db` (bodies, never secrets), so it is
  0600 in a 0700 directory since 0.43 (`test_tickets.py` class `Security`
  checks the file's bytes for every secret and access token).
- What leaves: incident and ticket documents as cortex's own API shows them,
  which name the affected homes (gateway names) for the ISP's own system;
  device kinds, not names, for proactive tickets.

### MCP

- **Cortex's MCP server** (`python3 -m aigw_cortex mcp`) speaks over stdio to
  the MCP client that started it: its trust is that local user's. It has no
  network listener.
- **The gateway has no MCP endpoint.** An assistant reaches a gateway only
  through cortex: `propose_action` and `support_brief --propose` queue a
  proposal that waits on the gateway for a person's yes (`agctl approve`,
  LuCI); `rollout_hold` can only pause or abort a rollout.
- **Prompt injection**: tools return what gateways reported, which includes
  names chosen by LAN devices (DHCP host names, domains looked up). An
  assistant must treat them as data. The design keeps the damage bounded: no
  tool changes a network without a person's approval, and none can resume a
  rollout or send firmware.

### USP (obuspa + aigw-usp.so) and TR-069 (icwmp + aigw-cwmp-dm)

- Both are off until `aigw.mgmt.enabled=1` and an ACS URL or MQTT broker is
  set. They reach agd's data model over ubus as `origin usp` / `origin acs`;
  agd decides what a management system may do (`aigw.mgmt.*`):
  - `X_AIGW_.Decision.{i}.Approve()/Reject()/Undo()`, quarantine and block:
    only with `mgmt.actions`; AIGW's guardrails (`act.protect`, `max_ttl`,
    `max_auto_per_hour`) always apply.
  - rollback, firmware Download()/Activate(): only with `mgmt.firmware`; every
    upgrade is checked afterwards and rolled back when the home got worse.
  - Reboot()/FactoryReset(): only with `mgmt.reset`.
  - SQM, policies and the act mode (`X_AIGW_.Mode`): writable parameters,
    checked against their allowed values (`dm.c`), never passed to a shell.
  - Wi-Fi: values are checked in `manage.c` (SSID 1–32 printable, passphrase
    8–63 printable ASCII or 64 hex, known modes and channels) and handed to the
    Wi-Fi handler in the environment, never on its command line.
  - `KeyPassphrase` always reads empty (TR-181).
- These callers are trusted to manage the gateway: a system that may flash
  firmware can do anything. That is why each power is a separate setting, and
  off by default.
- **TR-069 connection requests**: icwmp listens on `cwmp.port` (7547) for the
  ACS to wake it. **Fixed in 0.43**: without `cwmp.cr_username`/`cr_password`
  anyone who reaches that port could make the gateway call its ACS; now
  `aigw-cwmp-run` warns at every start, and also when the ACS URL is plain
  HTTP. (A connection request only makes the gateway open a session to the ACS
  it already has; it can't redirect it.)
- Passwords from `/etc/config/aigw` go to icwmp's own config (`umask 077`) and
  obuspa's settings (`umask 077`); `--print` hides them.

### Remote access and QR pairing

- `agctl remote on <endpoint>` brings up WireGuard (`aigw-remote`), opens its
  UDP port in fw4 and adds a zone for the tunnel. Off removes the port, the zone
  and the interface.
- A phone's private key is made, shown once in the QR code (and the reply to
  `ag.remote add`), and forgotten: only its public key is kept (`peers`). The
  gateway's key is `/etc/aigw/remote/server.key`, 0600 in a 0700 directory.
- Names and the endpoint are checked character by character; nothing reaches a
  shell.
- `ag.remote add/on/off/revoke` are write permissions in the LuCI ACL and not
  in the PC's non-root ACL. The read method `list` shows peers' names, tunnel
  addresses and the gateway's public key, never a private key.

### LuCI's rpcd ACL (`luci-app-aigw.json`)

- **Fixed in 0.43**: `ag.wifi keys` was in the *read* ACL, and given a key's
  name it returned that per-device Wi-Fi key's passphrase and QR code: a
  read-only LuCI user (and, on a PC as AP, the non-root user agctl works for)
  could read every device's Wi-Fi password. Now `keys` never returns a
  passphrase; the new `ag.wifi key_show` does, and is a write permission.
  `agctl key qr` uses it. `tests/plug-test.sh` checks both, and that no read ACL
  lists `key_show`.
- Everything else the pages change (decisions, zones, plug-ins, models,
  remote access, Wi-Fi keys) is in `write`; a call outside the ACL is refused
  (checked in QEMU by `qemu_console.py`'s `luci-acl-deny`).
- The pages put every name from the network into text nodes, never HTML
  (fixed in 0.10.1; `qemu-test.sh KEEP=1` shows a hostile host name).

### agctl and the local sockets

- **ubus** is the boundary: ubusd lets root call anything and other users only
  what an ACL file grants. On OpenWRT only root runs agctl. On a PC as AP the
  installer grants the user who ran it `platform/pc-ap/ubus-acl.json.in`:
  status, devices, DNS log, live events, and answering AIGW's proposals
  (approve, reject, undo, ask for one). Not Wi-Fi passwords, remote access,
  plug-ins, models or the data model. (0.43 corrected the installer's comment,
  which called this read-only.)
- The **data model** (`ag.dm`) takes the caller's `origin`. Only root and the
  protocol daemons reach `ag.dm` (no ACL grants it), so the origin is as
  trustworthy as the caller.
- **Fixed in 0.43**: agd's datagram socket for hostapd's answers was
  `/tmp/agd-wifi-<pid>`, made with the default umask. On a PC, where `/tmp` is
  shared with other users, a user could take that name first and turn off
  agd's Wi-Fi control. It is now in `/var/run/aigw` (root's, checked before
  use), mode 0600.
- Plug-ins run as `nobody`, each with its own state directory (0700), talking
  to agd over a pipe; what they may do is their capabilities. A plug-in's
  lines are length-limited and parsed as JSON by libubox; a line too long is
  skipped.
- `agctl plugin add/remove`, `trust`, `untrust` refuse without root and never
  pass a name to a shell (`ag_plugin_name_ok`, fingerprints without `/`).

### DNS (agdns)

- agdns listens on `dns.listen` (0.0.0.0) port 5300; the LAN's port 53 is
  redirected to it.
- **Fixed in 0.43**: on OpenWRT, fw4 drops the WAN side's packets to 5300, but
  a PC as AP has no fw4: agdns answered anyone who could reach the PC, an open
  resolver on the PC's own network (or the internet, with a public address).
  `aigw-nft` now adds `dns_guard`: agdns's port is reachable from loopback, the
  LAN interfaces and redirected DNS (the LAN's port 53, remote access's tunnel)
  only; everything else is dropped and counted. `dns.guard 0` turns it off.
  `tests/security-test.sh` checks it with UDP and TCP from a LAN, a WAN and the
  gateway itself.
- The DNS, TLS ClientHello and QUIC Initial parsers (`core/libag/dns.c`,
  `sni.c`) were read for unchecked sizes: every read is bounds-checked, name
  compression has a hop limit, QUIC lengths are checked against what arrived,
  and a host name is copied only if it fits. No change needed; they are
  covered by `test_dns`, `test_sni` (with captured and cut-short handshakes).

### Shell helpers

- `scripts/aigw-sqm`: interface names, rates and far ends are checked
  (`ok_iface`, `ok_rate`, `ok_ep`) before any `tc` call, and every argument is
  quoted. No change needed.
- The **firmware handler** (`platform/openwrt/aigw/files/firmware`): URLs must
  be `http(s)://` or `file://` (agd checks too), so none can pass for an
  option, and are always quoted. **Fixed in 0.43**: when a
  download failed, the message (logged by agd, and sent to the ACS as the
  fault) quoted the URL with any `user:password@` in it; now it never shows
  them. The record of where an image came from (`source.url`, which may hold
  them) is 0600.
- `wifi-config` (OpenWRT): values arrive in the environment, already checked by
  agd, and are passed to `uci set` as one argument each.
- `aigw-nft`, `aigw-remote`: values come from `/etc/config/aigw` (root's).
- **Fixed in 0.43**: on a PC as AP, `install.sh` installed `/etc/config/aigw`
  0644, readable by every user, though it may hold the bridge's token and the
  ACS and MQTT passwords. It is now 0600, also when an existing install is
  upgraded (OpenWRT's package always installed it 0600). agctl reads agd over
  ubus, so non-root agctl is unaffected.

### Firmware download and verification

- A download goes to RAM (`/tmp/aigw-firmware`), is checked against the size
  and checksum the ACS gave (SHA-1/256/512), then by `sysupgrade -T` (the
  image's own metadata and the board), before anything is written to flash.
- After an upgrade agd compares the home before and after; a worse home rolls
  back (two banks, or the kept copy on one-image routers).
- **Not a signature check**: an image is accepted if it is one this board can
  run and matches the checksum the management system sent. Who may send one is
  `mgmt.firmware` (ACS, USP controller) or the `firmware.install` capability
  (the bridge, for cortex's staged rollouts). An OEM with signed images points
  `mgmt.firmware_handler` at its own handler, which checks signatures.
- `file://` URLs are accepted from those same callers (icwmp hands agd the file
  it downloaded). A caller that may flash firmware gains nothing from reading a
  local file through a checksum, so this stays.

### The try-it demo mode

- The demo (simulated home and fleet) is the package `aigw-demo`, built only
  into the demo image and never released as a package.
- Its init script turns on only when `aigw-demo check` finds a QEMU virtual
  machine (DMI vendor `QEMU`), and refuses when `/etc/aigw/agd-state.json`
  exists and isn't the demo's own: a real router (no DMI, or another vendor) or
  a real home's state never gets the demo.
- `aigw-demo seed` refuses outside QEMU too. **Fixed in 0.43**: its `--test`
  flag (for the test suite) skipped that check, and would have written over a
  real router's state given the system's paths. `--test` now refuses any file
  under `/etc`, `/var/lib`, `/var/run`, `/run`, `/usr`, `/overlay` or `/rom`,
  also through a link (`tests/demo-test.sh`).
- The demo's dashboard listens on the VM's port 8090 with a token, printed on
  the VM's console for `try-aigw.sh`; its fleet API listens on 127.0.0.1.

### Secrets on disk and in logs

| Secret | Where | Mode |
|--------|-------|------|
| per-device Wi-Fi keys | `/etc/aigw/wifi-keys` (hostapd's `wpa_psk_file`) | 0600 |
| WireGuard key, peers | `/etc/aigw/remote/` | dir 0700, key 0600 |
| ACS, MQTT passwords, bridge token | `/etc/config/aigw` | 0600 (PC: since 0.43) |
| icwmp's and obuspa's copies | `/etc/config/cwmp`, `/etc/aigw/usp/` | written with umask 077 |
| the bridge's token for curl | the plug-in's state directory | umask 077 |
| cortex: token hashes, homes' data | `cortex.db` | 0600 (since 0.43) |
| cortex's signing key, dashboard token | `keys/cortex.key`, `dashboard.token` | 0600 |
| ticketing secrets (OAuth2, bearer, webhooks, listener) | files you name in `tickets.json`, or the environment | refused unless 0600/0640 (since 0.43) |
| ticketing configuration (where secrets go) | `tickets.json` | refused if group or others can write it (since 0.43) |
| firmware source URL | `/etc/aigw/firmware/source.url` | 0600 (since 0.43) |

Logs: agd logs names of keys and peers, never passphrases or private keys;
`--print` of the TR-069 and USP launchers hides passwords; firmware messages
hide a URL's credentials (0.43). Anything agd puts in a model's `refused` text
reaches cortex, so it never names a home's domains.

## What 0.43 did not fix, and why

| What | Why not |
|------|---------|
| The firmware handler gives `uclient-fetch` the download password with `--password`, and `wifi-config` gives `uci set` the Wi-Fi passphrase as an argument: for a moment they are in `/proc/<pid>/cmdline`, which plug-ins (user `nobody`) can read. | Neither tool reads a secret from the environment or stdin. Plug-ins are code you installed and allowed; a pid namespace for plug-ins (so they see only their own processes) is the real fix, planned with the plug-in sandbox work. |
| No signature check on firmware images. | Who may install one is a setting per caller; OEMs with signed images use their own handler. A generic signed-image format is a separate feature. |
| The dashboard prints its token link at start. | It is the operator's own terminal or journal; `--quiet` and the 0600 file are there for wider logs. |
| `file://` firmware URLs from management systems and the bridge. | icwmp needs it; callers allowed to flash firmware already have more power than reading a file's checksum. |
| `ag.dm` trusts the `origin` a caller gives. | Only root and the protocol daemons can call `ag.dm`. |
| TR-069 connection requests without credentials are allowed (with a warning). | Refusing them would break ACSs that set credentials later through `ConnectionRequestUsername/Password`; the ACS should set them, and the warning says so. |

## Checks

- `tests/security-test.sh`: the agdns guard (UDP and TCP; LAN, loopback,
  WAN, redirected), firmware messages without credentials, the TR-069
  warnings.
- `tests/plug-test.sh`: `keys` without passphrases, `key_show`, the ACLs,
  agd's hostapd socket (0600, not in `/tmp`).
- `tests/test_cortex.py` (class `Security`): cortex's file modes, the
  exposure warning, malformed requests, the dashboard's token for changes.
- `tests/test_tickets.py` (class `Security`): secret files and the
  configuration refused when others can read or change them (and the secret
  never sent), the listener with a token file others can read, a receiver's
  check of replays and unsigned headers, no secret or access token in
  `cortex.db`.
- `tests/demo-test.sh`: `--test` never writes a system's files.
- The upgrade tests (below) check that `/etc/config/aigw` is 0600 after a PC
  as AP upgrade.

## Upgrades: nothing lost or reset

`platform/openwrt/upgrade-test.sh` (CI job `upgrade`, from v0.29.2, v0.33.1 and
the latest release) installs the older release's packages on OpenWRT 24.10
x86-64 in QEMU, sets up a home on it (settings that aren't the defaults, a
policy of its own and a default one removed, devices with names and a zone, a
block in effect, a care check, remote access with a phone, the bridge's token),
restarts agd so only what it keeps counts, then installs this build's packages
with `opkg` and reboots. Compared before and after: the settings word for word,
fw4's AIGW rules, the phone and the gateway's WireGuard key, devices, the
block, the care history, every data model path and the values set, every file
in `/etc/aigw`, and agd running the new version, enabled at boot.
`tests/pcap-upgrade-test.sh` (CI job `pcap-upgrade`) does the same for a PC as
AP with the latest release's `install.sh`, then this one's.
