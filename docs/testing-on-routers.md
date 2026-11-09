# Testing AIGW OS on a real router

Phase 6 ends when the same checks that pass in CI pass on real routers,
within the memory and flash budget. CI already installs every build on
OpenWRT in QEMU and runs `aigw-selftest --full` there
(`platform/openwrt/qemu-test.sh`). It does this on x86-64, on emulated MIPS
with the MT7621 packages, and on emulated ARM64 with the Filogic packages.
This page covers the part only real hardware can show: real radios, real
flash, real load, and the actual chips.

Reference boards: an **OpenWRT One** (Filogic, aarch64), any **MT7621**
router (mipsel), and a **Wi-Fi 7** Filogic board when one is at hand.

## 1. Install

Pick the packages for your router's architecture from the release
(`aarch64_cortex-a53`: OpenWRT One and other Filogic routers;
`mipsel_24kc`: MT7621). OpenWRT 25.12 uses `.apk` files.

```bash
scp aigw_*.ipk aigw-remote_*_all.ipk luci-app-aigw_*_all.ipk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
opkg update
opkg install /tmp/aigw_*.ipk /tmp/aigw-remote_*.ipk /tmp/luci-app-aigw_*.ipk
```

`aigw-bridge` (needs `curl`) and `aigw-mqtt` are optional.

## 2. Self-test

```bash
aigw-selftest            # changes nothing
aigw-selftest --full     # also: kills agd once (procd must restart it), reloads the
                         # firewall, turns remote access on and off once
```

Send the whole output. Every line is PASS, FAIL or INFO. The things only a
router can show:

| Check | What it proves on this hardware |
|-------|---------------------------------|
| `model` | the `.agm` runtime gives the same scores on this CPU (endianness, float) |
| `ed25519`, `sha512` | signature checks work on this CPU, so signed updates do too |
| `dns` | agdns answers through the router's real upstream |
| `ram-*`, `flash` | AIGW stays within budget: agd ≤ 10 MB, agdns ≤ 4 MB, all together ≤ 16 MB RAM, ≤ 1 MB flash |
| `procd`, `respawn`, `jail` | procd supervises both daemons; agdns runs in a ujail when the router has one |
| `fw4`, `remote-*` | the firewall include and the remote-access zone and port rule work with this router's fw4 |

## 3. Use it for a day

1. Join a phone to the router's Wi-Fi and open LuCI → **AIGW**. The phone
   should appear under Devices within a minute, with its lookups.
2. `agctl watch` for a few minutes of normal use. No alerts on everyday apps
   is the goal; send any you get.
3. Make a per-device key (`AIGW → Wi-Fi keys and remote access`, or
   `agctl key add test`). This needs `wpa_psk_file`; see
   [getting-started-openwrt.md](getting-started-openwrt.md#wi-fi-keys-per-device).
   Scan the QR code with a phone and check that it joins with that key.
4. Remote access: `agctl remote on <your public address>` and add a phone.
   From mobile data, open a website. `agctl remote` should show it
   connected, and its lookups should appear in AIGW.
5. After a day: `aigw-selftest` again, and `logread -e agd | tail -50`.
   Memory must not have crept up.

## 4. Remove

```bash
opkg remove luci-app-aigw aigw-remote aigw-bridge aigw-mqtt aigw
```

The firewall include and the remote-access zone go away with them.
`/etc/aigw` stays (keys, history); delete it by hand if you want.
