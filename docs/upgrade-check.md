# AIGW firmware upgrades that check themselves

A firmware upgrade is where an ISP's fleet is most at risk. When an image breaks
something, it breaks it in every home it reaches, and the help desk hears about
it first. A firmware handler, on prplOS, RDK-B or a plain OpenWRT, knows one
thing about the new image: whether it booted. If it doesn't, the boot loader
goes back to the old one. If it boots but the home works worse on it, nothing
notices until the calls come in.

AIGW already measures the home all day (the devices, the line, DNS, Wi-Fi, the
experience score), so it compares the home before and after the upgrade:

- **kept** when the home is no worse, and the ISP hears that too;
- **rolled back by itself** when it is worse, on gateways whose firmware handler
  still has the previous image: A/B flash, as most ISP gateways have, or, since
  v0.36, a copy kept before the upgrade on OpenWRT routers with one firmware
  partition;
- **confirmed** once back on the previous image: are the devices that stayed
  away back? Does the internet answer again? Then it really was the firmware;
- **held off:** the version AIGW rolled back isn't installed again for a week,
  so an ACS that retries the upgrade doesn't put the home back on it;
- **told to the ISP** in every case (`UpgradeChecked!`), with the reasons in
  words, so the ISP can stop a rollout after the first homes.

## What is compared

| | |
|---|---|
| Before | When an image is activated (USP `FirmwareImage.N.Activate()` or `Download(AutoActivate)`, TR-069 `Download`, or agctl), before the handler runs: the versions and images, the devices connected then (on Wi-Fi, or active in the last 10 minutes), the share of the last 7 days without a problem, the mean nightly speed, and whether the internet answered. It is saved in `/etc/aigw/agd-upgrade.json`, which `sysupgrade` keeps, and written to flash before the reboot. |
| After | From agd's first start on the new version: which of those devices came back (seen on Wi-Fi, or sending traffic, DNS, DHCP), every proactive care check (every 2 minutes in the first hour), with the delays, loss and DNS time beside the home's learned normal for that hour, the speed tests, and how often agd started again. |
| Worse at once | **Devices:** at least 2, and a quarter, of the devices connected before don't come back within `upgrade.reconnect` (30 minutes). This catches a Wi-Fi driver that drops a chip, WPA3 settings that old devices can't join, or a LAN port that doesn't come up. One phone that left the house isn't enough. **No internet:** none at all for 15 minutes since the boot (every check, at least 3), while it worked before. **Restarts:** agd, or the gateway, started more than 3 times on the new image. |
| Worse at the end | After `upgrade.watch` (24 hours), on at least 6 checks: the next hop, the internet or DNS 1.5 times slower than this home's normal for those hours and at least 10, 20 or 50 ms more; 2 points more loss; the nightly speed under 80% of the week before; or the time without a problem 10 points under the week before (both with at least 4 hours watched). |
| Kept | Otherwise: "No worse after 24 h: 8 of 8 devices back, delays, loss and DNS as usual for these hours; without a problem 100% of the time (98% the week before)." |

Only what the firmware can change counts: the comparison is with the home's
own normal for the same hours, not with a fixed limit. Since v0.29 the gateway
learns that normal anyway, for [early warnings](proactive-care.md).

## Rolling back, and confirming it

With `upgrade.rollback 1` (the default), a worse upgrade boots the previous
image again. AIGW asks the firmware handler for an image that isn't running,
has the old version and is `Available`, activates it and reboots. Back on it,
AIGW checks what went wrong:

- the devices that stayed away: are they back within `upgrade.reconnect`?
  "Confirmed: back on 1.0, 3 of the 3 devices that stayed away on 2.0 came back."
- no internet: does it answer again? If it still doesn't, "Not confirmed:
  probably not the firmware (the line?)."

That confirmation tells the ISP whether to blame the image or look elsewhere.

**When there is nothing to go back to,** AIGW doesn't roll back. It reports
the upgrade as `Worse`, with "There is no copy of 1.0 on this gateway to go back
to (*why, in the handler's words*): it stays on 2.0. Better not to roll 2.0 out
further."

## Three ways back

Which one a gateway has is in `agctl upgrade` ("way back"), in `aigw-selftest`
(`INFO rollback`) and in `Device.X_AIGW_.Upgrade.RollbackMethod`, with the
details in words in `RollbackNote`:

| `RollbackMethod` | The gateway | Going back |
|---|---|---|
| `TwoBanks` | A/B flash: the new image is written to the bank that isn't running | boot the other bank again. An OEM's handler does this (`tests/fw/firmware` is an example) |
| `KeptCopy` | One firmware partition (most OpenWRT routers): `sysupgrade` overwrites the running image | write back a copy of the old image, kept before the upgrade, with the settings it had |
| `None` | One partition, and no place for a copy (or `mgmt.firmware_keep off`) | none: the upgrade is reported `Worse`, and stays |
| `Unknown` | A handler that doesn't say (no `firmware rollback`) | whatever its `list` offers |

## Routers with one firmware image

Many OpenWRT routers have a single firmware partition. `sysupgrade` writes the
new image over the running one, so there is no second bank to boot. AIGW's
OpenWRT handler (`/usr/libexec/aigw/firmware`) therefore keeps a copy of the
running image before it writes the new one, and lists it as a third image:

| Image | |
|---|---|
| 1 | the running image (`Active`) |
| 2 | the next one, in RAM, downloaded and checked before anything is written |
| 3 | the copy of the image it came from (`Available`, with its version), or `NoImage` |

In TR-181 terms, image 3 is an image the gateway holds and can activate:
`FirmwareImage.3.Activate()` writes it back. A download into it is refused.

**Where the copy goes.** First USB storage mounted under `/mnt` or `/media`
(the one with the most room), else the flash's overlay
(`/etc/aigw-rollback`). `mgmt.firmware_keep_dir` names a place. RAM (tmpfs)
alone never holds it, because RAM is empty after the reboot.

- **On USB storage,** the copy is simply there after the upgrade.
- **On the overlay,** which `sysupgrade` rewrites, the copy rides over the
  upgrade in `sysupgrade`'s archive of the settings, this one time (a
  `/lib/upgrade/keep.d` entry made just before, gone after). RAM holds that
  archive while the flash is written.

**Room first.** Before anything is written, the handler checks the free space:

- on USB storage, room for the copy;
- on the overlay, room for the copy after the new image is written (a bigger
  image leaves a smaller overlay), and room in RAM for the archive;
- on x86 and EFI targets, `sysupgrade` leaves the archive on the boot
  partition until the first boot. It is small (16 MB on x86, with the kernel
  on it), so there the copy needs USB storage, and the handler says so;
- always 512 KB to spare.

When there is no room, it says so in words, for example "no copy of 1.0 was
kept: no room on the flash: 3.1 MB free, 8.4 MB needed; USB storage would do".
`mgmt.firmware_keep` decides what happens then:

| `firmware_keep` | No room for a copy |
|---|---|
| `auto` (default) | upgrade without one, and say so (the handler's answer, `RollbackNote`) |
| `require` | refuse the upgrade: `Activate()` fails with the reason |
| `off` | copies are never kept; no image 3 |

**Where the copy comes from.** The running image isn't read back from flash
(what is on a partition is not always an image `sysupgrade` accepts). It is,
in order:

1. a copy of the running version already kept: on USB storage, the new image
   from the last upgrade, kept beside the old one when both fit, or the copy
   left there after a rollback (on the overlay only the old image is carried,
   so that the archive isn't twice as big);
2. the URL the running version was downloaded from (`http://` or `https://`),
   remembered in `/etc/aigw/firmware/` and fetched again, then checked: it must
   pass `sysupgrade -T`, and when its metadata names a version, that must be
   the running one (an image without metadata, such as x86's `.img.gz`, is
   taken for the version it was fetched as);
3. `firmware keep <url or file>`, run by hand or by an installer once.

A version installed by TR-069 comes through icwmp as a file in `/tmp`, so it
has no URL to fetch again. Its copy is the one kept beside the old image on USB
storage. Without one, there is no copy for the upgrade after it, and the
handler says so.

**Going back.** When an upgrade check finds the home worse, agd activates image
3. The handler copies the kept image into RAM (it checks there is room), then
runs `sysupgrade -f` with the settings saved when the copy was kept (`sysupgrade
-b`), plus two things from now: agd's upgrade record (`AIGW_FW_CARRY`), so that
after the boot agd knows it rolled back and confirms the cause, and icwmp's
session, so that the upgrade's `TransferComplete` isn't sent twice. Settings
changed on the new version are not kept: the old image comes back as it was.

```
$ /usr/libexec/aigw/firmware rollback
copy a copy of 23.05.5-r24106-10cc5fcd00 is kept on the flash (overlay) (image 3): a failed upgrade check writes it back with its settings
```

**The hold.** A version rolled back is refused for `upgrade.hold_days` (7):
`Download(AutoActivate)` and `Activate()` fail with the reason. `agctl upgrade
--forget`, or `Device.X_AIGW_.Upgrade.Forget()`, lifts it.

**Never twice.** Going back is never watched in turn, and AIGW never upgrades
by itself.

## The ISP's view

| | |
|---|---|
| `Device.X_AIGW_.UpgradeChecked!` | `Result` (`Kept`, `RolledBack`, `Worse`, `NotBooted`, then `RollbackConfirmed` or `RollbackNotConfirmed`), `FromVersion`, `ToVersion`, `Reasons` (`DevicesMissing`, `NoInternet`, `Restarts`, `NextHopLatency`, `InternetLatency`, `InternetLoss`, `DNSLatency`, `DownloadSpeed`, `ExperienceScore`), `Summary` in words. A verdict reached while agd starts (back on the old image after a rollback, or the new image never ran) is sent `upgrade.report_delay` (60 s) later, once the USP and TR-069 clients are up. It is kept in the record until then. |
| `Device.X_AIGW_.Upgrade.` | `Enable`, `AutoRollback`, `WatchSeconds`, `Status` (`None`, `Activated`, `Watching`, `Kept`, `Worse`, `RollingBack`, `RolledBack`, `NotBooted`), `FromVersion`, `ToVersion`, `Activated`, `Ended`, `DevicesBefore`, `DevicesBack`, `Reasons`, `Summary`, `ScoreBefore`, `ScoreAfter`, `RollbackConfirmed` (`Unknown`, `Yes`, `No`), `HeldVersion`, `HeldUntil`, and since data model 1.15 `RollbackMethod` (`TwoBanks`, `KeptCopy`, `None`, `Unknown`) and `RollbackNote` (in words); `Forget()`, which needs `mgmt.firmware`. |
| `Device.DeviceInfo.FirmwareImage.3.` | On a router with one firmware image: the kept copy (`Available`, its `Version`), or `NoImage`. |
| `NotBooted` | The gateway restarted after the activation, but on the old version: the boot loader went back, or the image wasn't written. |

```
$ agctl upgrade
Firmware upgrades are checked for 24 h after the boot, and rolled back when the home is worse on them.
  way back  two firmware banks: the image that ran before stays in the other one
  held off  2.0, rolled back: 3 of the 4 devices connected before the upgrade did not come back within 30 min (all on Wi-Fi).
  upgrade   1.0 -> 2.0, activated 41 min ago: worse: rolled back
  devices   1 of the 4 connected before came back on 2.0; stayed away: tv (Wi-Fi, back after the rollback), ...
  verdict   3 of the 4 devices connected before the upgrade did not come back within 30 min (all on Wi-Fi).
            Rolled back to 1.0 (image 1). Confirmed: back on 1.0, 3 of the 3 devices that stayed away on 2.0 came back.
  confirmed yes
```

`agctl watch upgrade` follows it live.

## Settings (`config upgrade 'upgrade'`)

| Option | Default | |
|---|---|---|
| `check` | 1 | compare the home before and after an upgrade |
| `rollback` | 1 | worse: boot the previous image again by itself |
| `watch` | 86400 | seconds the new image is watched |
| `reconnect` | 1800 | seconds devices have to come back |
| `hold_days` | 7 | days a version rolled back is refused |
| `report_delay` | 60 | seconds before a verdict found at start is sent |
| `file` | beside `main.state_file` | the record (kept over the upgrade) |

On OpenWRT routers with one firmware image, in `config mgmt 'mgmt'`:

| Option | Default | |
|---|---|---|
| `firmware_keep` | `auto` | keep a copy of the running image before an upgrade: `auto` when there is room, `require` (no upgrade without one), `off` |
| `firmware_keep_dir` | USB storage, else `/etc/aigw-rollback` | where |

The delays, loss, DNS and score need [proactive care](proactive-care.md) on
(`care.enabled`, the default). The devices and restarts are checked without it.

## How it is tested

- `tests/test_upgrade.c`: the verdict on simulated homes. A good upgrade is
  watched, then kept. Devices that don't come back are worse only after
  `reconnect`, not for one device, and not when nothing was connected. No
  internet counts only when it worked before, and not once it comes back.
  Restarts, slower delays (not small ones, and not on 4 checks), the speed, the
  score (not when the week before was hardly watched), several reasons at once.
- `tests/upgrade-test.sh` (CI): agd with the two-bank handler and a fake
  hostapd, stopping for each reboot as on a gateway:
  - 1.0 to 2.0 with 3 of the 4 devices staying away: worse after 15 s, rolled
    back by itself, the devices back on 1.0 (confirmed);
  - `UpgradeChecked!` `RolledBack` and `RollbackConfirmed` after the start delay;
  - 2.0 refused again (download and activate) until `--forget`;
  - 2.0 again with everyone back: watched over an agd restart, kept.
- `tests/single-image-test.sh` (CI): OpenWRT's handler on a fake flash. A
  directory stands in for the overlay, files for the partition, a list for the
  free space; `sysupgrade`, `df`, `fwtool`, `uclient-fetch`, `uci` and `reboot`
  are shims, so nothing real is flashed or rebooted:
  - a copy kept by hand (not a bad image, not another version); image 3 is no
    download slot;
  - a `sysupgrade` that fails: the keep undone, nothing left to carry;
  - 1.0 to 2.0: 1.0 kept on the overlay, carried over the flash once (and
    only 1.0), listed as image 3;
  - x86 and EFI: no room on the boot partition for the archive, said so;
  - image 3 written back: refused without room in RAM, then 1.0 with its own
    settings and agd's record;
  - no room on the flash: `require` refuses in words, `auto` upgrades and says
    so, `off` has no image 3;
  - USB storage chosen before the full overlay and never a tmpfs; a version
    fetched again from the URL it came from; back on it, its copy stays;
  - as root, agd on this handler: a bad upgrade, worse, rolled back through
    image 3 by itself, the record carried over, confirmed; `RollbackMethod`
    `KeptCopy`.
- The QEMU job on x86-64 (CI): after the real upgrade from the ACS, a copy of
  the running image (x86's `.img.gz`, without metadata) is kept on a second
  disk mounted as USB storage, survives a second real `sysupgrade` (image 3
  `Available`), and is written back by `FirmwareImage.3.Activate()`, with the
  settings it had. (A first try on the overlay found x86's small boot
  partition, where `sysupgrade` leaves the archive: the handler now checks it.)

Not tested yet: a real router with one firmware partition (flash sizes, USB
storage, the time a write takes), a real image on a router with A/B flash, and
an ACS's view of the events over TR-069.
