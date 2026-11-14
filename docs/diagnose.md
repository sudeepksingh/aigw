# AIGW Diagnose: "my internet is slow", answered by the gateway

The call a help desk gets most is "my internet is slow", and most of its time
goes into finding out *where*: the line, the ISP's network, DNS, the Wi-Fi, the
one device, or something the gateway itself did. AIGW OS answers that in about
3 seconds, from the gateway, in plain words and with a code a help desk can
count, for the person at home and the ISP alike.

## What it checks

| | How |
|---|---|
| The line | the default route; 10 echo requests (200 ms apart) to its next hop (the modem or the ISP's router) and to each internet address in `diag.targets` (the best one counts, as some don't answer echo requests) |
| Names | one query to the gateway's own DNS (agdns) for `diag.name`: answered or not, how fast, and whether it said it failed (SERVFAIL) |
| Wi-Fi | Self-Healing Wi-Fi's verdict on every access point, in its own words, and the channel trials it ran today |
| One device (optional) | connected or not, when last seen, its signal, what it feels (Self-Healing Wi-Fi's probes: loss and delay over the last 2 minutes), and every AIGW action on it right now (quarantine, disconnect, throttle, deprioritize, stay24, a blocked domain) |

## The verdict

Findings are put in order of what explains the problem best, and the first one
is the verdict:

| Code | Area | Means |
|---|---|---|
| `DEVICE_BLOCKED` | AIGW | AIGW itself keeps the device off (quarantine, disconnect): the quickest fix, named first |
| `NO_ROUTE` | Internet | no default route: the gateway's internet connection is down or not set up |
| `LINE_DOWN` | Internet | neither the next router nor the internet answers: modem, cable or fibre |
| `ISP_DOWN` | Internet | the next router answers, the internet doesn't: beyond the home |
| `DNS_FAILING` | DNS | the internet answers, names don't resolve |
| `DEVICE_UNKNOWN`, `DEVICE_OFFLINE` | Device | no such device; not connected for 5 minutes or more |
| `DEVICE_LIMITED` | AIGW | AIGW slows the device on purpose (throttle, deprioritize) |
| `LINE_LOSS`, `LINE_SLOW` | Internet | packets lost, or slow answers, already to the next router |
| `ISP_LOSS`, `ISP_SLOW` | Internet | lost or slow only beyond it |
| `WIFI_COVERAGE`, `WIFI_CONGESTION`, `WIFI_HOPPING`, `WIFI_DEVICE`, `WIFI_RADIO` | WiFi | the device's signal is weak, or Self-Healing Wi-Fi's cause on its access point (or any access point) |
| `DNS_SLOW` | DNS | names take 300 ms or more |
| `OK` | All | everything the gateway can measure is fine: likely the service used, or the device itself |

Fine things are findings too (`INTERNET_OK`, `DNS_OK`, `WIFI_OK`), and
information (`DEVICE_ON_24GHZ`): a help desk sees what was ruled out. Each
verdict comes with advice (what to do next). The rules are in
`core/libag/diag.c` (no I/O) and unit-tested case by case in
`tests/test_diag.c`.

## Asking

- **At home:** `agctl diagnose [device]` (a name, MAC or IP). For example, on
  the PC access point (v0.27.0, a real home line; its next hop, the Windows
  host, doesn't answer echo requests, which is not counted as a fault):

  ```
  $ agctl diagnose
  Everything the gateway can measure is fine: the problem is likely the service or website used, or one device.
    code      OK (All, ok)
    advice    Ask which app or website is slow, and try another device in the same place.
    measured  next hop 172.30.160.1 no answer, internet 2 ms (0% lost), DNS ok 10 ms
    findings
    ok       OK               Everything the gateway can measure is fine: the problem is likely the service or website used, or one device.
    ok       INTERNET_OK      The internet answers in 2 ms, 0% lost.
    ok       DNS_OK           Names resolve in 10 ms.

  $ agctl diagnose OnePlus-Nord-5
  OnePlus-Nord-5: OnePlus-Nord-5 loses 10% of its traffic on Wi-Fi while the signal is fine (-69 dBm): the device itself (its Wi-Fi, power saving).
    code      WIFI_DEVICE (WiFi, warning)
    advice    Restart the device's Wi-Fi; turn off its power saving; update it.
    measured  next hop 172.30.160.1 no answer, internet 2 ms (0% lost), DNS ok 2 ms
    findings
    warning  WIFI_DEVICE      OnePlus-Nord-5 loses 10% of its traffic on Wi-Fi while the signal is fine (-69 dBm): the device itself (its Wi-Fi, power saving).
    ok       INTERNET_OK      The internet answers in 2 ms, 0% lost.
    ok       DNS_OK           Names resolve in 2 ms.
  ```

  The line is fine and so is the access point; only the phone loses traffic,
  so the help desk's next question is about the phone, not the line.

- **The ISP, over USP:** `Device.X_AIGW_.Diagnose()`, an asynchronous command
  (input `Host`: a `Device.Hosts.Host.{i}` path, MAC, IP or name; optional).
  Outputs: `Code`, `Area`, `Severity`, `Summary`, `Advice`, `Findings` (every
  code, in order, comma-separated), `InternetLossPercent`, `InternetLatencyMs`,
  `NextHopLatencyMs`, `DNSLatencyMs`.
- **The ISP, over TR-069:** the same command through `Device.X_AIGW_.Request.`
  (`Command` = `Device.X_AIGW_.Diagnose()`, `Host`; the outputs as
  `Request.Code` etc. once `Status` is `Done`).
- **Afterwards, from anywhere:** `Device.X_AIGW_.LastDiagnosis.` (`Time`,
  `Host`, `Code`, `Severity`, `Summary`, `Advice`, `Findings`), and the event
  `Device.X_AIGW_.DiagnosisDone!` (`Host`, `Code`, `Severity`, `Summary`),
  whoever asked: a help desk sees what the home found.

The gateway also runs Diagnose by itself, and tells the ISP about problems
before anyone asks: see [proactive-care.md](proactive-care.md).

Diagnose only reads: it changes nothing, so a management system may run it
without `mgmt.actions`. One runs at a time; the same question asked meanwhile
gets the same answer.

## Settings (`/etc/config/aigw`, section `diag`)

| Option | Default | |
|---|---|---|
| `targets` | `1.1.1.1 8.8.8.8 9.9.9.9` | internet addresses for echo requests (up to 4); an ISP puts its own |
| `name` | `example.com` | the name asked of the gateway's DNS; `''` = DNS not checked |

## Tests

- `tests/test_diag.c` (unit): every code, the order (AIGW's own block before a
  line that is down; the line before Wi-Fi), DNS failing not reported when the
  line is down (a consequence), what is only information.
- `tests/hwsim-test.sh` (simulated radios, CI): with no DNS server, the verdict
  is `DNS_FAILING` with the "internet" answering; with the client disconnected
  by AIGW, `DEVICE_BLOCKED` naming the decision, the same over the data model
  (`Diagnose()` outputs, `LastDiagnosis.`, `DiagnosisDone!`).
