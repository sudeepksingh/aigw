# Security policy

AIGW OS sits between every device in a home and the internet, so security
problems matter more here than in most projects.

## Reporting a vulnerability

**Please do not open a public issue.** Report privately through GitHub:
[Security → Report a vulnerability](https://github.com/sudeepksingh/aigw/security/advisories/new).

Include what is affected (component, version, OpenWRT or PC as AP), how to
reproduce it, and the impact you expect. You will get an acknowledgement
within 7 days. We will agree a disclosure date with you and credit you in the
advisory unless you prefer otherwise.

## Supported versions

| Version | Supported |
|---------|-----------|
| latest release and `main` | yes |
| older releases | no, please upgrade |

## Scope

In scope: `agd`, `agdns`, `agctl`, `aigw-nft`, `aigw-ap`, the installers and
the OpenWRT package, for example parsing bugs in network input, privilege
problems, rules that expose LAN devices, or anything that lets a client
bypass or disable AIGW's protections.

Out of scope: vulnerabilities in OpenWRT itself, hostapd, dnsmasq or the
Linux kernel (report those upstream), and issues that need root on the
gateway already.
