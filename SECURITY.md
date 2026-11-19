# Security policy

AIGW OS sits between every device in a home and the internet, so security
problems matter more here than in most projects. What AIGW OS exposes, to whom,
and how each way in is protected is in [docs/security.md](docs/security.md).

## Reporting a vulnerability

**Please do not open a public issue, pull request or discussion.** Report it
privately through GitHub's private vulnerability reporting: on this repository,
**Security → Report a vulnerability**
(<https://github.com/sudeepksingh/aigw/security/advisories/new>). Only the
maintainers see the report, and we can work on a fix with you there.

Include:

- what is affected: the component (`agd`, `agdns`, cortex, a plug-in, LuCI, an
  installer, ...), the version (`agctl version`), OpenWRT or a PC as AP;
- how to reproduce it, as small as you can make it;
- the impact you expect: who can do what they shouldn't.

You will get an acknowledgement within 7 days. We will agree a disclosure date
with you, publish a GitHub security advisory with the fixed release, and credit
you in it unless you prefer otherwise.

## Supported versions

| Version | Supported |
|---------|-----------|
| the latest release and `main` | yes |
| older releases | no: please upgrade (upgrades keep your settings and history) |

## Scope

In scope: everything in this repository: `agd`, `agdns`, `agctl`, the data
model and its USP, TR-069 and prplOS daemons, the platform handlers
(firmware, Wi-Fi, `aigw-sqm`, `aigw-nft`, `aigw-remote`), the plug-ins, cortex
(fleet API, dashboard, MCP server), LuCI's pages and ACL, the installers, the
OpenWRT packages and the try-it demo image. For example: parsing bugs in
network input, missing permission checks, secrets that leak, rules that expose
LAN devices or the gateway to the WAN, or anything that lets a device, a
plug-in or a remote system bypass or turn off AIGW's protections.

Out of scope: vulnerabilities in OpenWRT itself, hostapd, dnsmasq, icwmp,
OB-USP-Agent or the Linux kernel (please report those upstream; tell us too if
AIGW OS makes them worse), issues that need root on the gateway already, and
the demo image's simulated data.
