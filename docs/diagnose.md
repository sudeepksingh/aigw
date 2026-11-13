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

- **At home:** `agctl diagnose [device]` (a name, MAC or IP). For example (the
  shape of the output; the numbers are illustrative):

  ```
  OnePlus-Nord-5: OnePlus-Nord-5's Wi-Fi signal is weak (-82 dBm on 2.4 GHz): too far from the access point, or walls in the way.
    code      WIFI_COVERAGE (WiFi, warning)
    advice    Move the device closer, or the access point higher and more central; a mesh node or extender would help.
    measured  next hop 172.20.0.1 1 ms (0% lost), internet 14 ms (0% lost), DNS ok 22 ms
    findings
      warning  WIFI_COVERAGE    OnePlus-Nord-5's Wi-Fi signal is weak (-82 dBm on 2.4 GHz): ...
      info     DEVICE_ON_24GHZ  OnePlus-Nord-5 is kept on 2.4 GHz for now (it kept switching bands at the edge of 5 GHz).
      ok       INTERNET_OK      The internet answers in 14 ms, 0% lost.
      ok       DNS_OK           Names resolve in 22 ms.
  ```

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
