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
agctl alerts         # what AIGW noticed, and why
agctl decisions      # actions AIGW proposes; approve with: agctl approve <id>
agctl watch all      # live
sudo aigw-ap status  # access point, clients, services
```

Act yourself (every action ends on its own; `agctl undo <id>` ends it now):

```bash
agctl prioritize <device> 2h          # its traffic first, in the Wi-Fi video queue
agctl deprioritize <device> 2h        # its traffic last (Wi-Fi background queue)
agctl throttle <device> 2mbit 1h      # a speed limit
agctl block <domain> 1d               # no device can resolve it or its subdomains
agctl block <domain> 1h <device>      # ... just this device
agctl quarantine <device> 30m         # cut a device off from everything but the gateway
agctl disconnect <device> 30m         # kick it off Wi-Fi and refuse it
```

On a PC as AP, Wi-Fi clients talk to each other through the radio itself,
not through the kernel, so quarantine cuts a device off from the internet
and from wired devices; use `disconnect` to keep it off the Wi-Fi entirely.

Start automatically at boot: `sudo systemctl enable --now aigw`.
Stop: `sudo aigw-ap down`.

### Give a device its own Wi-Fi key

```bash
sudo agctl key add "Living room plug" iot     # prints a QR code: scan it with the device or phone
agctl keys                                    # who joined with which key
sudo agctl key revoke "Living room plug"      # take it away; disconnects that device only
```

The first device to join with a key owns it: the key then works only for its
MAC, the device gets the key's name and zone. `aigw-ap` adds the key file
(`/etc/aigw/wifi-keys`) to hostapd; the main password keeps working.

### Plug-ins and Home Assistant

```bash
agctl plugins                                     # what is installed, running or waiting, and why
sudo agctl plugin add plugins/examples/door-sensor   # your own: starts by itself
sudo agctl plugin allow <name>                    # privileged capabilities of one you added
```

The MQTT connector (`mqtt`) comes with the install. It waits until an MQTT
broker answers on `127.0.0.1:1883` (`sudo apt install mosquitto`), or set
yours in `/etc/config/aigw`:

```
config plugin 'mqtt'
	option broker '192.168.1.10'
	option username 'aigw'
	option password 'secret'
```

With Home Assistant's MQTT integration on the same broker, every device shows
up with presence and a Quarantine switch. `sudo agctl plugin disable mqtt`
turns it off.

### Remote access

```bash
sudo agctl remote on home.example.net      # your public IP or a DDNS name
sudo agctl remote add "My phone"           # scan the QR code with the WireGuard app
agctl remote
```

Forward UDP 51820 from your internet router to this PC (on WSL2, see
[Windows (WSL2)](#windows-wsl2) for getting it from Windows into WSL). Away from home, the phone's traffic and lookups
go through AIGW.

### Cortex (optional): retraining, signed model updates, MCP

Cortex can run on this same PC. Retraining needs numpy and scikit-learn,
for example in a venv: `python3 -m venv ~/.aigw-train && ~/.aigw-train/bin/pip
install -r cortex/training/requirements.txt`.

```bash
cd cortex
python3 -m aigw_cortex enroll home                 # prints the token
python3 -m aigw_cortex serve &                     # 127.0.0.1:8790
sudo agctl trust ~/.aigw-cortex/keys/cortex.pub
sudo agctl bridge http://127.0.0.1:8790
sudo agctl bridge token                            # paste it
python3 -m aigw_cortex retrain --python ~/.aigw-train/bin/python
agctl models                                       # new version in use, no restart
```

The bridge plug-in comes with the install and waits until you set its address
and token. See [cortex/README.md](../cortex/README.md) for the MCP server
("ask your home" from an AI assistant).

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

Remote access on WSL2: WSL sits behind Windows' own NAT, and Windows' port
proxy only forwards TCP. To let a phone's WireGuard (UDP 51820) in, use WSL's
mirrored networking (`networkingMode=mirrored` under `[wsl2]` in
`%UserProfile%\.wslconfig`, then `wsl --shutdown`), allow UDP 51820 in the
Windows firewall, and forward it from your internet router to the Windows PC.

## Settings

`/etc/aigw/pc-ap.conf`, apply with `sudo aigw-ap restart`:

| Setting | Default | Meaning |
|---------|---------|---------|
| `AP_IFACE` | `auto` | Wi-Fi interface for the access point. `auto` picks the first one with AP mode that isn't your internet connection. With `BAND=dual`, the 5 GHz one |
| `SSID`, `PASSPHRASE` | generated | Network name and WPA2 password |
| `COUNTRY` | detected | Two-letter country code. Required for 5 GHz |
| `BAND`, `CHANNEL` | `2.4`, `6` | For 5 GHz use `BAND=5` and channel 36, 40, 44 or 48 (in IN and US also 149–165). `BAND=dual`: two adapters, see below |
| `WIDTH` | `auto` | 20, 40 or 80 MHz; `auto` = 80 on 5 GHz, 20 on 2.4 GHz. A width the adapter refuses falls back to the next narrower one |
| `CHANNEL_24`, `WIDTH_24`, `AP_IFACE_24` | `6`, `20`, `auto` | `BAND=dual`: the 2.4 GHz access point |
| `USB_BUSID` | empty | WSL: the adapters to attach from Windows (bus ids from `usbipd list`); empty = every Wi-Fi adapter |
| `SUBNET` | `10.42.0` | Clients get `.20`-`.250`; the PC is `.1` |
| `UPLINK` | `auto` | Internet interface; `auto` = default route |

### Two adapters: both bands and band steering

One USB adapter has one radio: it runs 2.4 GHz *or* 5 GHz. With a second
adapter and `BAND=dual`, `aigw-ap` runs a 5 GHz access point on the first and
a 2.4 GHz one on the second, with the same name and password, in one bridge
(`aigwbr0`), so a device keeps its address when it changes band. AIGW then
moves close devices to 5 GHz and far ones to 2.4 GHz: by an 802.11v request
where the device supports it, otherwise by refusing it on its band for 30 s
(`wifi` section of `/etc/config/aigw`).

```bash
sudo sed -i 's/^BAND=.*/BAND=dual/' /etc/aigw/pc-ap.conf
sudo aigw-ap restart
agctl wifi              # both access points and who is on which band
agctl watch wifi        # moves, joins and leaves as they happen
```

On WSL, run `platform\pc-ap\wsl\aigw-wsl.cmd` once after plugging in the new
adapter: it shares it with WSL (administrator approval). From then on
`sudo aigw-ap up` attaches both by itself.

`/etc/config/aigw` (UCI format, same file as on OpenWRT) holds the AIGW
settings: log level, DNS port and upstream resolver, alert thresholds,
baselines, and acting (`act` section: `mode` advise/enforce/off, time limits,
devices AIGW must never act on, and the `policy` sections), plug-ins
(`plugins` section and one `plugin` section per plug-in) and Wi-Fi keys
(`wifi` section). Apply with `sudo systemctl restart aigw-agd`.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `no Wi-Fi adapter with AP mode found` | Check `iw dev`. On WSL, `usbipd list` should show the adapter as Shared or Attached; if it says Not shared, run `aigw-wsl.cmd` once |
| `BAND=dual needs two Wi-Fi adapters` | Only one adapter came up: check `iw dev`, and on WSL that both show in `usbipd list` |
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
