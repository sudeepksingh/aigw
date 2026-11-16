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
  still has the previous image (A/B flash, as most ISP gateways have);
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

**When there is nothing to go back to.** OpenWRT's own handler keeps the next
image in RAM, and after `sysupgrade` there is only the running one. Then AIGW
doesn't roll back. It reports the upgrade as `Worse`, with "There is no copy of
1.0 on this gateway to go back to ... Better not to roll 2.0 out further." An
OEM's handler with A/B banks (`tests/fw/firmware` is an example) can roll back.

**The hold.** A version rolled back is refused for `upgrade.hold_days` (7):
`Download(AutoActivate)` and `Activate()` fail with the reason. `agctl upgrade
--forget`, or `Device.X_AIGW_.Upgrade.Forget()`, lifts it.

**Never twice.** Going back is never watched in turn, and AIGW never upgrades
by itself.

## The ISP's view

| | |
|---|---|
| `Device.X_AIGW_.UpgradeChecked!` | `Result` (`Kept`, `RolledBack`, `Worse`, `NotBooted`, then `RollbackConfirmed` or `RollbackNotConfirmed`), `FromVersion`, `ToVersion`, `Reasons` (`DevicesMissing`, `NoInternet`, `Restarts`, `NextHopLatency`, `InternetLatency`, `InternetLoss`, `DNSLatency`, `DownloadSpeed`, `ExperienceScore`), `Summary` in words. A verdict reached while agd starts (back on the old image after a rollback, or the new image never ran) is sent `upgrade.report_delay` (60 s) later, once the USP and TR-069 clients are up. It is kept in the record until then. |
| `Device.X_AIGW_.Upgrade.` | `Enable`, `AutoRollback`, `WatchSeconds`, `Status` (`None`, `Activated`, `Watching`, `Kept`, `Worse`, `RollingBack`, `RolledBack`, `NotBooted`), `FromVersion`, `ToVersion`, `Activated`, `Ended`, `DevicesBefore`, `DevicesBack`, `Reasons`, `Summary`, `ScoreBefore`, `ScoreAfter`, `RollbackConfirmed` (`Unknown`, `Yes`, `No`), `HeldVersion`, `HeldUntil`; `Forget()`, which needs `mgmt.firmware`. |
| `NotBooted` | The gateway restarted after the activation, but on the old version: the boot loader went back, or the image wasn't written. |

```
$ agctl upgrade
Firmware upgrades are checked for 24 h after the boot, and rolled back when the home is worse on them.
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

Not tested yet: a real image on a router with A/B flash, and an ACS's view of
the events over TR-069.
