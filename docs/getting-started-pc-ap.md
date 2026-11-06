# Getting started: PC as AP

Turn a Linux PC into an AIGW Wi-Fi access point. Devices that join its Wi-Fi
go through AIGW, and the PC forwards their traffic to your existing router.
Your router does not need to change.

```
phones, laptops, IoT ──Wi-Fi──► PC (AIGW) ──Ethernet──► your router ──► internet
```

## What you need

| | |
|---|---|
| OS | Debian 12+ or Ubuntu 22.04+ (native or WSL2 on Windows 10/11) |
| Internet | Through a different interface than the Wi-Fi adapter (usually Ethernet) |
| Wi-Fi adapter | One that supports **AP mode**. Good choices: MediaTek mt76 USB adapters (MT7612U, MT7921AU); Realtek rtw88 (RTL8821CU, RTL8822BU) also work |

Check AP support on Linux: `iw list | grep -A8 "Supported interface modes"`
must list `* AP`.

## Install

```bash
git clone https://github.com/sudeepksingh/aigw.git
cd aigw
sudo ./install.sh
```

The installer:

1. installs packages (hostapd, dnsmasq-base, nftables, iw, build tools)
2. builds the OpenWRT libraries AIGW uses (libubox, ubus, uci) into `/usr/local`, at pinned versions
3. builds AIGW, runs the unit tests, installs `agd`, `agdns`, `agctl`, `aigw-nft`, `aigw-ap`
4. writes `/etc/config/aigw` and `/etc/aigw/pc-ap.conf` (random Wi-Fi password)
5. installs systemd units

Running it again updates AIGW and keeps your settings.

## Start

```bash
sudo aigw-ap up
```

Join the Wi-Fi name the installer printed (also in `/etc/aigw/pc-ap.conf`).
Then:

```bash
agctl devices        # devices, how many DNS queries, top domains
agctl dns 20         # last 20 queries
agctl watch          # live
sudo aigw-ap status  # access point, clients, services
```

Start automatically at boot: `sudo systemctl enable --now aigw`.
Stop: `sudo aigw-ap down`.

## Windows (WSL2)

WSL2 can drive a **USB** Wi-Fi adapter through
[usbipd-win](https://github.com/dorssel/usbipd-win). Built-in laptop Wi-Fi
cannot be passed to WSL.

1. Install usbipd-win in Windows: `winget install usbipd`
2. In WSL: clone the repository and run `sudo ./install.sh`
3. In Windows, from the repository folder:

   ```bat
   platform\pc-ap\wsl\aigw-wsl.cmd
   ```

   It finds the USB Wi-Fi adapter, shares it with WSL (asks for admin once),
   keeps WSL running, prepares the firmware and starts the access point.
   Stop with `aigw-wsl.cmd -Stop`, which also gives the adapter back to Windows.

Pass `-BusId 1-6` if it picks the wrong USB device (`usbipd list` shows ids)
and `-Distro Ubuntu-24.04` if AIGW is not in your default distribution.

WSL's kernel cannot read the distribution's firmware files directly.
`aigw-ap` copies the Wi-Fi firmware and the regulatory database to
`/mnt/wsl/firmware` and points the kernel there, so no manual step is needed.

## Settings

`/etc/aigw/pc-ap.conf`, apply with `sudo aigw-ap restart`:

| Setting | Default | Meaning |
|---------|---------|---------|
| `AP_IFACE` | `auto` | Wi-Fi interface for the access point. `auto` picks the first one with AP mode that isn't your internet connection |
| `SSID`, `PASSPHRASE` | generated | Network name and WPA2 password |
| `COUNTRY` | detected | Two-letter country code. Required for 5 GHz |
| `BAND`, `CHANNEL` | `2.4`, `6` | For 5 GHz use `BAND=5` and channel 36, 40, 44 or 48 |
| `SUBNET` | `10.42.0` | Clients get `.20`-`.250`; the PC is `.1` |
| `UPLINK` | `auto` | Internet interface; `auto` = default route |

`/etc/config/aigw` (UCI format, same file as on OpenWRT) holds the AIGW
settings: log level, DNS port and upstream resolver.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `no Wi-Fi adapter with AP mode found` | Check `iw dev`. On WSL, start with `aigw-wsl.cmd` so the adapter is attached |
| `hostapd could not start` | The message above it shows hostapd's reason. Try `BAND=2.4`, `CHANNEL=6`, or set `COUNTRY` |
| 5 GHz refused | Set `COUNTRY`. Some adapters can't run an AP on 5 GHz at all |
| Clients connect but have no internet | `sudo aigw-ap status`; check `UPLINK`; Docker or ufw may block forwarding (`sudo iptables -P FORWARD ACCEPT` or allow `10.42.0.0/24`) |
| `agctl: cannot connect to ubus` | `sudo systemctl status aigw-ubusd aigw-agd` |
| `agctl: ... failed: Not found` as a normal user | The installer gives read access to the user who ran `sudo ./install.sh`. For other users: `sudo AIGW_USERS="alice bob" ./install.sh`, or use `sudo agctl` |

Logs: `journalctl -u aigw-agd -u aigw-agdns -u aigw`.

## Uninstall

```bash
sudo ./platform/pc-ap/uninstall.sh          # keep settings
sudo ./platform/pc-ap/uninstall.sh --purge  # remove settings too
```
