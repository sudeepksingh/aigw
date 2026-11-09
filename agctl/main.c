/* SPDX-License-Identifier: Apache-2.0 */
/* agctl: command-line view of the AIGW brain over ubus. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"
#include "ag/qr.h"
#include "ag/sign.h"

static struct ubus_context *ctx;
static int json_out;
static const char *device_filter;

static void usage(void)
{
	fprintf(stderr,
		"usage: agctl [-j] <command>\n"
		"\n"
		"  status              brain and sensor status\n"
		"  devices             devices: identity, Wi-Fi, lookups, traffic\n"
		"  device <ip|mac|name>  one device in detail\n"
		"  dns [N]             last N DNS lookups (default 30)\n"
		"  dns cache           the DNS cache: hits, answer times, prefetches, stale answers\n"
		"  wifi                access points (band, channel), their devices, band steering\n"
		"  flows [N]           last N finished connections (default 30)\n"
		"  alerts [N]          what AIGW thinks you should know, and why\n"
		"  score <domain>      the domain-risk model's view of a name\n"
		"  watch [dns|flows|wifi|home|alerts|decisions|plugins|models|all]   follow activity live (Ctrl+C to stop)\n"
		"\n"
		"  decisions [N]       actions waiting for you, in effect, and past ones\n"
		"  approve <id> [time] carry out a proposed action (optionally for another time limit)\n"
		"  reject <id>         say no to a proposed action\n"
		"  undo <id>           end an action now\n"
		"  prioritize <device> [time]    send a device's traffic first (Wi-Fi video queue)\n"
		"  deprioritize <device> [time]  send it last (Wi-Fi background queue)\n"
		"  throttle <device> [rate] [time]  limit its speed, e.g. 2mbit, 500kbit (default from config)\n"
		"  block <domain> [time] [device]   stop resolving a domain and its subdomains,\n"
		"                                   for every device or just one\n"
		"  quarantine <device> [time]    cut a device off from everything but the gateway\n"
		"  disconnect <device> [time]    kick it off Wi-Fi and refuse it until the time is up\n"
		"  channel <ap|2.4|5> <ch> [time]  move an access point to another channel (devices\n"
		"                                follow); back to the old one when the time is up\n"
		"  do <plugin.action> <device|target> [time]   an action a plug-in offers (agctl plugins)\n"
		"                      time: 90s, 30m, 2h, 1d (default from config, 1h)\n"
		"\n"
		"  plugins           plug-ins: sensors, actuators, skills, connectors; state and why\n"
		"  plugin <name>       one plug-in in detail\n"
		"  plugin add <folder>           install a plug-in folder (needs sudo); it starts by itself\n"
		"  plugin remove <name>          remove a plug-in you added\n"
		"  plugin allow|deny <name>      let a plug-in you added use its privileged capabilities\n"
		"  plugin enable|disable <name>  turn a plug-in on or off\n"
		"  plugin config <name> <key> <value|->  change a plug-in's setting (- reads it from stdin)\n"
		"\n"
		"  models              models in use: version, where from, who signed it\n"
		"  model install <file.agm> [file.sig] [--force]  load a model update now (needs a\n"
		"                      trusted signature; --force also allows an older version)\n"
		"  model rollback [name]         go back to the previous model and hold that version off\n"
		"  trust <key.pub>     accept models signed with this usign key (e.g. your cortex's)\n"
		"  untrust <fingerprint>         stop accepting a key\n"
		"  bridge [url]        link to cortex: show the bridge, or set cortex's address\n"
		"  bridge token        set this gateway's token (paste it; read from stdin)\n"
		"  fed                 federated learning: your answers, rounds joined, privacy spent\n"
		"  label <domain> safe|threat    tell AIGW what a name is (kept here; it trains the\n"
		"                      shared layer without the name leaving)\n"
		"  fed forget [domain] drop one answer, or all of them\n"
		"\n"
		"  remote              remote access: on or off, peers, who is connected\n"
		"  remote on <address> turn it on; <address>: how phones reach this home (public IP or\n"
		"                      DDNS name; forward UDP 51820 to the gateway if it is behind a router)\n"
		"  remote off          turn it off\n"
		"  remote add <name> [--conf]  a WireGuard QR code for a phone (--conf: the config as text)\n"
		"  remote revoke <name>        take a peer's access away\n"
		"\n"
		"  keys                Wi-Fi keys of individual devices\n"
		"  key add <name> [zone] [time]  new key and a QR code to scan; the first device to\n"
		"                                join with it owns it (zone e.g. iot; time: how long it waits)\n"
		"  key qr <name>       show a key's QR code again\n"
		"  key revoke <name>   take a device's key away and disconnect it\n"
		"  zone <device> <zone|none>     put a device in a zone; policies can be limited to one\n"
		"  qr <text> [--pbm]   any text as a QR code\n"
		"  selftest            checks on this device's CPU: signatures, model, QR, DNS via agdns\n"
		"                      (aigw-selftest runs it with the service checks)\n"
		"  version             agctl version\n"
		"\n"
		"  -j                  print raw JSON\n");
}

/* ---------- formatting helpers ---------- */

static const char *ago(uint64_t ts, char *out, size_t n)
{
	int64_t d = ag_wall_s() - (int64_t)ts;

	if (!ts)
		snprintf(out, n, "-");
	else if (d < 60)
		snprintf(out, n, "%llds ago", (long long)d);
	else if (d < 3600)
		snprintf(out, n, "%lldm ago", (long long)(d / 60));
	else if (d < 86400)
		snprintf(out, n, "%lldh ago", (long long)(d / 3600));
	else
		snprintf(out, n, "%lldd ago", (long long)(d / 86400));
	return out;
}

static const char *human_bytes(uint64_t v, char *out, size_t n)
{
	if (v < 1024)
		snprintf(out, n, "%lluB", (unsigned long long)v);
	else if (v < 1024 * 1024)
		snprintf(out, n, "%.1fK", v / 1024.0);
	else if (v < 1024ull * 1024 * 1024)
		snprintf(out, n, "%.1fM", v / (1024.0 * 1024));
	else
		snprintf(out, n, "%.2fG", v / (1024.0 * 1024 * 1024));
	return out;
}

static struct blob_attr *field(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, tbl, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static const char *fstr(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	return (a && blobmsg_type(a) == BLOBMSG_TYPE_STRING) ? blobmsg_get_string(a) : "";
}

static const char *orb(const char *s, const char *dflt)
{
	return (s && *s) ? s : dflt;
}

static uint64_t fnum(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8:  return blobmsg_get_u8(a);
	default: return 0;
	}
}

/* What to show when a lookup returned no IP address. */
static const char *no_address_text(uint64_t rcode, const char *qtype, char *tmp, size_t n)
{
	switch (rcode) {
	case 0:
		if (!strcmp(qtype, "A") || !strcmp(qtype, "AAAA"))
			return "(no address)";
		snprintf(tmp, n, "(%s record)", qtype);
		return tmp;
	case 2: return "SERVFAIL";
	case 3: return "NXDOMAIN";
	case 5: return "REFUSED";
	default:
		snprintf(tmp, n, "RCODE %llu", (unsigned long long)rcode);
		return tmp;
	}
}

static void print_json(struct blob_attr *msg)
{
	char *s = blobmsg_format_json_indent(msg, true, 0);

	if (s) {
		printf("%s\n", s);
		free(s);
	}
}

static void show_radios(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "radios"), *r, *s, *st = field(msg, "steering");
	size_t rem, rem2;

	if (json_out)
		return print_json(msg);
	if (!arr || !blobmsg_len(arr)) {
		printf("No access point found (hostapd control sockets).\n");
		return;
	}
	blobmsg_for_each_attr(r, arr, rem) {
		printf("%s  \"%s\"  %s GHz channel %llu (%llu MHz, %llu MHz wide)  %s\n", fstr(r, "iface"), fstr(r, "ssid"),
		       orb(fstr(r, "band"), "?"), (unsigned long long)fnum(r, "channel"), (unsigned long long)fnum(r, "freq"),
		       (unsigned long long)fnum(r, "width"), fstr(r, "bssid"));
		if (field(r, "busy_pct"))
			printf("  air             %llu%% used by others%s\n", (unsigned long long)fnum(r, "busy_pct"),
			       fnum(r, "busy_since") ? " (busy for a while)" : "");
		else if (*fstr(r, "air"))
			printf("  air             %s\n", fstr(r, "air"));
		if (*fstr(r, "neighbours"))
			printf("  neighbours      by channel, weighted by strength: %s%s", fstr(r, "neighbours"),
			       fnum(r, "suggest") ? "" : "\n");
		if (fnum(r, "suggest"))
			printf("; quieter: channel %llu\n", (unsigned long long)fnum(r, "suggest"));
		blobmsg_for_each_attr(s, field(r, "stations"), rem2) {
			printf("  %-17.17s %-15.15s %4d dBm %-4s %7.1f Mbit/s  %-13s %s",
			       orb(fstr(s, "hostname"), fstr(s, "mac")), orb(fstr(s, "ip"), "-"), (int32_t)fnum(s, "signal"),
			       fstr(s, "near"), fnum(s, "rate_kbps") / 1000.0,
			       fnum(s, "dual_band") ? "2.4 + 5 GHz" : "2.4 GHz only", fnum(s, "bss_transition") ? "802.11v" : "");
			if (fnum(s, "steered") || fnum(s, "steer_missed"))
				printf("  moved %llu, stayed %llu", (unsigned long long)fnum(s, "steered"),
				       (unsigned long long)fnum(s, "steer_missed"));
			printf("\n");
		}
	}
	if (st) {
		if (!fnum(st, "dual_band"))
			printf("band steering   needs a 2.4 GHz and a 5 GHz access point with one name\n");
		else if (!fnum(st, "enabled"))
			printf("band steering   off (wifi.steer 0)\n");
		else
			printf("band steering   on: to 5 GHz at %d dBm or better, to 2.4 GHz at %d dBm or worse; "
			       "%llu moves asked, %llu done\n", (int32_t)fnum(st, "up_dbm"), (int32_t)fnum(st, "down_dbm"),
			       (unsigned long long)fnum(st, "moves_asked"), (unsigned long long)fnum(st, "moves_done"));
	}
}

static void show_dnscache(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "cache")) {
		printf("DNS cache off (dns.cache 0)\n");
		return;
	}
	printf("DNS cache       %llu of %llu answers kept\n", (unsigned long long)fnum(msg, "entries"),
	       (unsigned long long)fnum(msg, "size"));
	printf("  lookups       %llu from the cache, %llu from upstream (%llu%% from the cache)\n",
	       (unsigned long long)fnum(msg, "hits"), (unsigned long long)fnum(msg, "misses"),
	       (unsigned long long)fnum(msg, "hit_pct"));
	printf("  answer time   %.2f ms from the cache, %.1f ms from upstream\n", fnum(msg, "hit_avg_us") / 1000.0,
	       fnum(msg, "upstream_avg_us") / 1000.0);
	printf("  prefetched    %llu names refreshed before they expired\n", (unsigned long long)fnum(msg, "prefetches"));
	printf("  stale         %llu expired answers served while upstream failed\n",
	       (unsigned long long)fnum(msg, "stale"));
	printf("  not kept      %llu answers marked \"don't cache\" (TTL 0), %llu others (errors, truncated)\n",
	       (unsigned long long)fnum(msg, "uncached_ttl0"), (unsigned long long)fnum(msg, "uncached_other"));
	printf("  blocked       %llu   timeouts %llu   dropped %llu\n", (unsigned long long)fnum(msg, "blocked"),
	       (unsigned long long)fnum(msg, "timeouts"), (unsigned long long)fnum(msg, "dropped"));
}

static const char *dev_name(struct blob_attr *d)
{
	return orb(fstr(d, "hostname"), orb(fstr(d, "mac"), "-"));
}

/* ---------- renderers ---------- */

static void show_status(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s = field(msg, "sensors");
	char t[32];

	if (json_out)
		return print_json(msg);
	printf("AIGW %s on %s  (phase: %s)\n", fstr(msg, "version"), fstr(msg, "host"), fstr(msg, "phase"));
	printf("  uptime          %llus\n", (unsigned long long)fnum(msg, "uptime_s"));
	printf("  devices         %llu (%llu home, %llu on Wi-Fi now)\n", (unsigned long long)fnum(msg, "devices"),
	       (unsigned long long)fnum(msg, "home"), (unsigned long long)fnum(msg, "wifi_stations"));
	if (fnum(msg, "radios") > 1)
		printf("  wi-fi           %llu access points, %llu band moves (agctl wifi)\n",
		       (unsigned long long)fnum(msg, "radios"), (unsigned long long)fnum(msg, "steered"));
	printf("  dns lookups     %llu (last %s)\n", (unsigned long long)fnum(msg, "dns_queries"),
	       ago(fnum(msg, "dns_last_event"), t, sizeof(t)));
	printf("  connections     %llu, %llu without a DNS lookup, %llu named from their handshake\n",
	       (unsigned long long)fnum(msg, "flows"), (unsigned long long)fnum(msg, "flows_no_dns"),
	       (unsigned long long)fnum(msg, "flows_sni"));
	if (s)
		printf("  sensors         dns %s, flow %s, names %s, wifi %s\n",
		       fnum(s, "dns") ? "on" : "waiting", fnum(s, "flow") ? "on" : "OFF",
		       fnum(s, "sni") ? "on" : "off", fnum(s, "wifi") ? "on" : "no stations");
	{
		struct blob_attr *m = field(msg, "meter"), *f = field(msg, "fastpath"), *e;
		size_t rem;

		if (m) {
			printf("  traffic meter   ");
			if (fnum(m, "active")) {
				printf("eBPF on");
				blobmsg_for_each_attr(e, field(m, "ifaces"), rem)
					printf(" %s", blobmsg_get_string(e));
				printf(" (live rates per device)\n");
			} else {
				printf("off: %s\n", orb(fstr(m, "why"), "?"));
			}
		}
		if (f && fnum(f, "enabled"))
			printf("  fast path       %llu connections offloaded (%llu by the switch chip), %llu sent back for actions\n",
			       (unsigned long long)fnum(f, "offloaded"), (unsigned long long)fnum(f, "hw_offloaded"),
			       (unsigned long long)fnum(f, "released"));
	}
	printf("  alerts          %llu\n", (unsigned long long)fnum(msg, "alerts"));
	{
		struct blob_attr *d = field(msg, "decisions");
		const char *mode = orb(fstr(msg, "mode"), "advise");

		printf("  acting          %s mode (%s)\n", mode,
		       !strcmp(mode, "enforce") ? "policies act on their own, within guardrails" :
		       !strcmp(mode, "off") ? "alerts only" : "actions wait for your approval");
		if (d)
			printf("  decisions       %llu waiting for you, %llu in effect, %llu policies\n",
			       (unsigned long long)fnum(d, "waiting"), (unsigned long long)fnum(d, "active"),
			       (unsigned long long)fnum(d, "policies"));
	}
	{
		struct blob_attr *p = field(msg, "plugins");

		if (p)
			printf("  plug-ins        %llu found, %llu working\n", (unsigned long long)fnum(p, "found"),
			       (unsigned long long)fnum(p, "working"));
		printf("  wi-fi keys      %llu per-device keys\n", (unsigned long long)fnum(msg, "wifi_keys"));
		if (field(msg, "remote")) {
			struct blob_attr *r = field(msg, "remote");

			if (fnum(r, "enabled"))
				printf("  remote access   on, %llu peers, %llu connected\n", (unsigned long long)fnum(r, "peers"),
				       (unsigned long long)fnum(r, "online"));
			else
				printf("  remote access   off\n");
		}
	}
	{
		struct blob_attr *m = field(msg, "models"), *v = field(msg, "model_versions"), *e;
		size_t rem;
		int k = 0;

		printf("  models         ");
		if (m)
			blobmsg_for_each_attr(e, m, rem) {
				uint64_t ver = fnum(v, blobmsg_name(e));

				if (ver)
					printf(" %s v%llu (%u trees)", blobmsg_name(e), (unsigned long long)ver, blobmsg_get_u32(e));
				else
					printf(" %s (packaged, %u trees)", blobmsg_name(e), blobmsg_get_u32(e));
				k++;
			}
		printf("%s\n", k ? "" : " none loaded");
	}
}

static void top_line(struct blob_attr *arr, const char *count, int bytes, int max)
{
	struct blob_attr *e;
	size_t rem;
	int k = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		char b[16];

		if (k++ == max)
			break;
		if (bytes)
			printf("%s%s(%s)", k > 1 ? ", " : "", fstr(e, "name"),
			       human_bytes(fnum(e, count), b, sizeof(b)));
		else
			printf("%s%s(%llu)", k > 1 ? ", " : "", fstr(e, "name"),
			       (unsigned long long)fnum(e, count));
	}
}

static void show_devices(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "devices"), *d;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-15s %-18s %-11s %-6s %6s %6s %5s %8s %8s  %s\n", "NAME", "IP", "TYPE", "SIGNAL",
	       "DNS", "CONNS", "NODNS", "DOWN", "UP", "TOP DESTINATIONS");
	if (!arr)
		return;
	blobmsg_for_each_attr(d, arr, rem) {
		struct blob_attr *w = field(d, "wifi");
		char sig[12] = "-", dn[16], up[16];

		if (w && fnum(w, "connected"))
			snprintf(sig, sizeof(sig), "%d", (int32_t)fnum(w, "signal"));
		printf("%-15.15s %-18.18s %-11.11s %-6s %6llu %6llu %5llu %8s %8s  ", dev_name(d),
		       orb(fstr(d, "ip"), "-"), orb(fstr(d, "type"), "-"), sig,
		       (unsigned long long)fnum(d, "queries"), (unsigned long long)fnum(d, "flows"),
		       (unsigned long long)fnum(d, "flows_no_dns"),
		       human_bytes(fnum(d, "bytes_down"), dn, sizeof(dn)),
		       human_bytes(fnum(d, "bytes_up"), up, sizeof(up)));
		if (field(d, "top_destinations") && fnum(d, "bytes_down") + fnum(d, "bytes_up"))
			top_line(field(d, "top_destinations"), "bytes", 1, 2);
		else
			top_line(field(d, "top_domains"), "lookups", 0, 2);
		printf("\n");
	}
}

static int dev_matches(struct blob_attr *d, const char *q)
{
	return !strcasecmp(fstr(d, "ip"), q) || !strcasecmp(fstr(d, "mac"), q) ||
	       strcasestr(fstr(d, "hostname"), q) != NULL;
}

static void show_device(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "devices"), *d, *w, *e;
	size_t rem, r2;
	char t1[32], t2[32], b1[16], b2[16];
	int found = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(d, arr, rem) {
		if (!dev_matches(d, device_filter))
			continue;
		found++;
		if (json_out) {
			print_json(d);
			continue;
		}
		w = field(d, "wifi");
		printf("%s\n", dev_name(d));
		printf("  ip / mac        %s / %s\n", orb(fstr(d, "ip"), "-"), orb(fstr(d, "mac"), "-"));
		printf("  type            %s\n", orb(fstr(d, "type"), "unknown"));
		if (*fstr(d, "zone") || *fstr(d, "wifi_key"))
			printf("  zone / key      %s / %s\n", orb(fstr(d, "zone"), "-"), orb(fstr(d, "wifi_key"), "shared password"));
		if (field(d, "plugins"))
			printf("  plug-ins        %llu reports, last %s %s\n",
			       (unsigned long long)fnum(field(d, "plugins"), "events"), fstr(field(d, "plugins"), "last"),
			       ago(fnum(field(d, "plugins"), "last_ts"), t1, sizeof(t1)));
		if (*fstr(d, "vendor_class") || *fstr(d, "dhcp_options"))
			printf("  dhcp            %s  options %s\n", orb(fstr(d, "vendor_class"), "-"),
			       orb(fstr(d, "dhcp_options"), "-"));
		printf("  seen            first %s, last %s\n", ago(fnum(d, "first_seen"), t1, sizeof(t1)),
		       ago(fnum(d, "last_seen"), t2, sizeof(t2)));
		if (w && fnum(w, "connected"))
			printf("  wi-fi           %d dBm (%s), %.1f Mbit/s, %s%s%son %s since %s\n", (int32_t)fnum(w, "signal"),
			       orb(fstr(w, "near"), "-"), fnum(w, "rate_kbps") / 1000.0, *fstr(w, "band") ? fstr(w, "band") : "",
			       *fstr(w, "band") ? " GHz " : "", "", fstr(w, "iface"), ago(fnum(w, "since"), t1, sizeof(t1)));
		else
			printf("  wi-fi           not connected\n");
		if (w && (field(w, "dual_band") || fnum(w, "steered") || fnum(w, "steer_missed")))
			printf("  bands           %s, %s; moved %llu times, stayed %llu\n",
			       fnum(w, "dual_band") ? "2.4 and 5 GHz" : "2.4 GHz only (as far as seen)",
			       fnum(w, "bss_transition") ? "takes 802.11v move requests" : "no 802.11v",
			       (unsigned long long)fnum(w, "steered"), (unsigned long long)fnum(w, "steer_missed"));
		if (field(d, "presence"))
			printf("  home            %s since %s\n", fnum(field(d, "presence"), "home") ? "yes" : "no",
			       ago(fnum(field(d, "presence"), "since"), t2, sizeof(t2)));
		printf("  traffic         %s down, %s up, %llu connections (%llu without DNS)\n",
		       human_bytes(fnum(d, "bytes_down"), b1, sizeof(b1)),
		       human_bytes(fnum(d, "bytes_up"), b2, sizeof(b2)),
		       (unsigned long long)fnum(d, "flows"), (unsigned long long)fnum(d, "flows_no_dns"));
		if (field(d, "meter")) {
			struct blob_attr *m = field(d, "meter");

			printf("  right now       %.2f Mbit/s down, %.2f Mbit/s up (kernel meter: %s down, %s up so far)\n",
			       fnum(m, "rate_down") / 1e6, fnum(m, "rate_up") / 1e6,
			       human_bytes(fnum(m, "bytes_down"), b1, sizeof(b1)), human_bytes(fnum(m, "bytes_up"), b2, sizeof(b2)));
		}
		{
			struct blob_attr *bl = field(d, "baseline"), *h;
			size_t r3;
			int k = 0;

			if (bl) {
				printf("  baseline        %s: %llu known sites, %.1f new/day%s, usual upload %s per hour\n",
				       fstr(bl, "state"), (unsigned long long)fnum(bl, "known_sites"),
				       field(bl, "new_sites_per_day") ? blobmsg_get_double(field(bl, "new_sites_per_day")) : 0,
				       fnum(bl, "stable") ? " (stable pattern)" : "",
				       human_bytes(field(bl, "upload_per_bucket") ?
						   (uint64_t)blobmsg_get_double(field(bl, "upload_per_bucket")) : 0, b1, sizeof(b1)));
				printf("  active hours   ");
				if (field(bl, "active_hours"))
					blobmsg_for_each_attr(h, field(bl, "active_hours"), r3) {
						printf("%s", blobmsg_get_u32(h) ? "#" : ".");
						k++;
					}
				printf("  (00-23h, # = seen active)\n");
			}
		}
		printf("  top by traffic\n");
		if (field(d, "top_destinations"))
			blobmsg_for_each_attr(e, field(d, "top_destinations"), r2)
				printf("    %-44s %s\n", fstr(e, "name"), human_bytes(fnum(e, "bytes"), b1, sizeof(b1)));
		printf("  top by lookups (%llu total)\n", (unsigned long long)fnum(d, "queries"));
		if (field(d, "top_domains"))
			blobmsg_for_each_attr(e, field(d, "top_domains"), r2)
				printf("    %-44s %llu\n", fstr(e, "name"), (unsigned long long)fnum(e, "lookups"));
	}
	if (!found)
		fprintf(stderr, "agctl: no device matches '%s'\n", device_filter);
}

static void show_dns(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "log"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-8s %-16s %-16s %-5s %-44s %s\n", "TIME", "CLIENT", "NAME", "TYPE", "QUERY", "ANSWER");
	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		time_t ts = (time_t)fnum(e, "ts");
		struct tm tm;
		char hhmmss[16], tmp[32];

		localtime_r(&ts, &tm);
		strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);
		printf("%-8s %-16s %-16.16s %-5s %-44.44s %s\n", hhmmss, fstr(e, "client"),
		       orb(fstr(e, "hostname"), "-"), fstr(e, "qtype"), fstr(e, "qname"),
		       *fstr(e, "answer") ? fstr(e, "answer") :
		       no_address_text(fnum(e, "rcode"), fstr(e, "qtype"), tmp, sizeof(tmp)));
	}
}

static void show_flows(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "flows"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-8s %-16s %-44s %-9s %8s %8s %6s  %s\n", "ENDED", "CLIENT", "DESTINATION", "PROTO",
	       "DOWN", "UP", "SECS", "NOTE");
	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		time_t ts = (time_t)fnum(e, "end");
		struct tm tm;
		char hhmmss[16], proto[16], dn[16], up[16];

		localtime_r(&ts, &tm);
		strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);
		snprintf(proto, sizeof(proto), "%s/%llu", fstr(e, "proto"), (unsigned long long)fnum(e, "dport"));
		printf("%-8s %-16.16s %-44.44s %-9s %8s %8s %6llu  %s\n", hhmmss,
		       orb(fstr(e, "hostname"), fstr(e, "client")), orb(fstr(e, "domain"), fstr(e, "dst")),
		       proto, human_bytes(fnum(e, "bytes_down"), dn, sizeof(dn)),
		       human_bytes(fnum(e, "bytes_up"), up, sizeof(up)),
		       (unsigned long long)fnum(e, "duration_s"), fnum(e, "no_dns") ? "no DNS lookup" :
		       !strcmp(fstr(e, "named_by"), "sni") ? "name from handshake" : "");
	}
}

static void show_alerts(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "alerts"), *e;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			char t1[32];

			n++;
			printf("#%-4llu %-8s %-10s %-16.16s %s\n", (unsigned long long)fnum(e, "id"),
			       fstr(e, "severity"), fstr(e, "kind"), fstr(e, "device"),
			       ago(fnum(e, "last"), t1, sizeof(t1)));
			if (fnum(e, "count") > 1)
				printf("      seen %llu times\n", (unsigned long long)fnum(e, "count"));
			printf("      %s\n\n", fstr(e, "reason"));
		}
	if (!n)
		printf("No alerts. AIGW is watching in advise mode and hasn't seen anything worth flagging.\n");
}

static void show_score(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s = field(msg, "score"), *f = field(msg, "features"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%s\n", fstr(msg, "name"));
	if (!s) {
		printf("  %s\n", fstr(msg, "verdict"));
		return;
	}
	printf("  label     %s\n", fstr(msg, "label"));
	printf("  score     %.3f  (alert at %.2f)\n", blobmsg_get_double(s),
	       field(msg, "threshold") ? blobmsg_get_double(field(msg, "threshold")) : 0);
	if (field(msg, "trees"))
		printf("  trees     %.3f  (before the layer homes learned together)\n",
		       blobmsg_get_double(field(msg, "trees")));
	printf("  verdict   %s\n", fstr(msg, "verdict"));
	if (f) {
		printf("  features ");
		blobmsg_for_each_attr(e, f, rem)
			printf(" %s=%.3g", blobmsg_name(e), blobmsg_get_double(e));
		printf("\n");
	}
}

static const char *duration(int64_t s, char *out, size_t n)
{
	if (s < 0)
		s = 0;
	if (s < 120)
		snprintf(out, n, "%llds", (long long)s);
	else if (s < 7200)
		snprintf(out, n, "%lldm", (long long)(s / 60));
	else if (s < 172800)
		snprintf(out, n, "%lldh%02lldm", (long long)(s / 3600), (long long)(s % 3600 / 60));
	else
		snprintf(out, n, "%lldd", (long long)(s / 86400));
	return out;
}

static const char *action_words(const char *action)
{
	return !strcmp(action, "priority") ? "prioritize" : action;
}

/* "block youtube.com (kids-tablet)", "throttle tv  10.0.0.5  2.0 Mbit/s", ... */
static void decision_subject(struct blob_attr *e, char *out, size_t n)
{
	const char *action = fstr(e, "action");

	if (!strcmp(action, "block"))
		snprintf(out, n, "%s  (%s)", fstr(e, "target"), fstr(e, "device"));
	else if (!strcmp(action, "throttle"))
		snprintf(out, n, "%s  %s  to %.1f Mbit/s", fstr(e, "device"), fstr(e, "target"),
			 fnum(e, "rate") / 1000.0);
	else
		snprintf(out, n, "%s  %s", fstr(e, "device"), fstr(e, "target"));
}

static void show_decisions(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "decisions"), *e;
	size_t rem;
	int n = 0, waiting = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			const char *st = fstr(e, "state"), *action = fstr(e, "action");
			char t1[32], t2[32], subj[256];

			n++;
			decision_subject(e, subj, sizeof(subj));
			printf("#%-4llu %-9s %-12s %s\n", (unsigned long long)fnum(e, "id"), st,
			       action_words(action), subj);
			if (!strcmp(st, "proposed")) {
				waiting++;
				printf("      proposed by %s %s, for %s\n", fstr(e, "origin"),
				       ago(fnum(e, "created"), t1, sizeof(t1)), duration((int64_t)fnum(e, "ttl"), t2, sizeof(t2)));
			} else if (!strcmp(st, "active")) {
				printf("      by %s%s, %s left\n", fstr(e, "origin"), fnum(e, "automatic") ? " (automatic)" : "",
				       duration((int64_t)fnum(e, "expires") - ag_wall_s(), t1, sizeof(t1)));
			} else {
				printf("      by %s, %s %s\n", fstr(e, "origin"), st, ago(fnum(e, "decided"), t1, sizeof(t1)));
			}
			printf("      why: %s\n", fstr(e, "reason"));
			if (*fstr(e, "note"))
				printf("      note: %s\n", fstr(e, "note"));
			printf("\n");
		}
	if (!n)
		printf("No decisions yet. AIGW is in %s mode.\n", orb(fstr(msg, "mode"), "advise"));
	else if (waiting)
		printf("%d waiting for you: agctl approve <id>  or  agctl reject <id>\n", waiting);
}

static int change_failed;

static void show_change(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (fnum(msg, "ok"))
		printf("done (decision #%llu). See: agctl decisions\n", (unsigned long long)fnum(msg, "id"));
	else
		fprintf(stderr, "agctl: refused: %s\n", fstr(msg, "error"));
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb);

/* ---------- QR codes ---------- */

static int qr_pbm;

/* Two modules per character cell, dark on light whatever the terminal's colours. */
static void print_qr(const char *text)
{
	struct ag_qr q;
	int x, y, quiet = 2, tty = isatty(1);

	if (ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_M) < 0 &&
	    ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_L) < 0) {
		fprintf(stderr, "agctl: too long for a QR code\n");
		return;
	}
	if (qr_pbm) {
		int scale = 8, side;

		quiet = 4;
		side = (q.size + 2 * quiet) * scale;
		printf("P1\n%d %d\n", side, side);
		for (y = 0; y < side; y++) {
			for (x = 0; x < side; x++) {
				int mx = x / scale - quiet, my = y / scale - quiet;

				putchar(mx >= 0 && my >= 0 && mx < q.size && my < q.size && q.m[my][mx] ? '1' : '0');
			}
			putchar('\n');
		}
		return;
	}
	for (y = -quiet; y < q.size + quiet; y += 2) {
		printf("  %s", tty ? "\033[30;47m" : "");
		for (x = -quiet; x < q.size + quiet; x++) {
			int top = x >= 0 && y >= 0 && x < q.size && y < q.size && q.m[y][x];
			int bot = x >= 0 && y + 1 >= 0 && x < q.size && y + 1 < q.size && q.m[y + 1][x];

			fputs(top && bot ? "\u2588" : top ? "\u2580" : bot ? "\u2584" : " ", stdout);
		}
		printf("%s\n", tty ? "\033[0m" : "");
	}
}

/* ---------- plug-ins ---------- */

static void list_line(const char *label, struct blob_attr *arr)
{
	struct blob_attr *e;
	size_t rem;
	int k = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		if (!k++)
			printf("  %-15s ", label);
		else
			printf(", ");
		printf("%s", blobmsg_get_string(e));
	}
	if (k)
		printf("\n");
}

static void show_plugins(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "plugins"), *p;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(p, arr, rem) {
			if (!n++ && !device_filter)
				printf("%-16s %-10s %-11s %-8s %-8s %s\n", "NAME", "KIND", "STATE", "FROM", "VERSION", "NOTE");
			if (!device_filter) {
				printf("%-16.16s %-10s %-11s %-8s %-8.8s %s\n", fstr(p, "name"), fstr(p, "kind"), fstr(p, "state"),
				       fstr(p, "signed_by") && *fstr(p, "signed_by") ? "signed" : fstr(p, "source"),
				       fstr(p, "version"), orb(fstr(p, "why"), fstr(p, "description")));
				continue;
			}
			{
				struct blob_attr *pr = field(p, "process"), *bud = field(p, "budget"), *card = field(p, "card"), *e;
				size_t r2;
				char t1[32];

				printf("%s %s (%s)\n", fstr(p, "name"), fstr(p, "version"), fstr(p, "kind"));
				if (*fstr(p, "description"))
					printf("  %s\n", fstr(p, "description"));
				printf("  state           %s%s%s\n", fstr(p, "state"), *fstr(p, "why") ? ": " : "", fstr(p, "why"));
				printf("  from            %s (%s), %s\n", fstr(p, "source"), fstr(p, "dir"),
				       fnum(p, "trusted") ? "trusted" : "privileged capabilities need your OK");
				if (fstr(p, "signed_by") && *fstr(p, "signed_by"))
					printf("  signed by       key %s (trusted)\n", fstr(p, "signed_by"));
				else if (fstr(p, "signature") && *fstr(p, "signature"))
					printf("  signed          %s\n", fstr(p, "signature"));
				list_line("emits", field(p, "emits"));
				list_line("listens to", field(p, "subscribes"));
				list_line("actions", field(p, "actions"));
				list_line("alerts", field(p, "alerts"));
				list_line("needs", field(p, "needs"));
				list_line("allowed", field(p, "granted"));
				list_line("waiting for OK", field(p, "waiting_for_ok"));
				if (field(p, "policies"))
					blobmsg_for_each_attr(e, field(p, "policies"), r2)
						printf("  policy          on %s alert%s%s: %s (%s)\n", fstr(e, "alert"),
						       *fstr(e, "zone") ? " in zone " : "", fstr(e, "zone"), fstr(e, "action"),
						       fstr(e, "mode"));
				if (card)
					printf("  card            %s: %s\n", fstr(card, "title"), fstr(card, "text"));
				if (bud)
					printf("  budget          %llu KB memory, %llu%% CPU\n", (unsigned long long)fnum(bud, "ram_kb"),
					       (unsigned long long)fnum(bud, "cpu_pct"));
				if (pr)
					printf("  process         pid %llu since %s, %llu KB, %.1f%% CPU\n",
					       (unsigned long long)fnum(pr, "pid"), ago(fnum(pr, "started"), t1, sizeof(t1)),
					       (unsigned long long)fnum(pr, "rss_kb"),
					       field(pr, "cpu_pct") ? blobmsg_get_double(field(pr, "cpu_pct")) : 0.0);
				printf("  messages        %llu in, %llu out, %llu dropped, %llu restarts\n",
				       (unsigned long long)fnum(p, "messages_in"), (unsigned long long)fnum(p, "messages_out"),
				       (unsigned long long)fnum(p, "dropped"), (unsigned long long)fnum(p, "restarts"));
			}
		}
	if (!n)
		printf(device_filter ? "No plug-in called '%s'.\n" :
		       "No plug-ins yet. Packages add them to %s; agctl plugin add <folder> adds your own.\n",
		       device_filter ? device_filter : fstr(msg, "dir"));
}

static void show_ok(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (!fnum(msg, "ok"))
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	else
		printf("done\n");
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

static void show_models(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "models"), *keys = field(msg, "trusted_keys"), *e;
	size_t rem;
	char t[32];
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("Models (updates in %s; signature %s)\n", fstr(msg, "dir"),
	       fnum(msg, "require_signature") ? "required" : "NOT required");
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			const char *src = orb(fstr(e, "source"), "-");

			if (!fnum(e, "loaded")) {
				printf("  %-12s %s\n", fstr(e, "name"), strcmp(orb(fstr(e, "kind"), ""), "model") ?
				       "none yet (comes from cortex through the bridge)" : "not loaded");
			} else if (!strcmp(orb(fstr(e, "kind"), ""), "layer")) {
				printf("  %-12s version %llu, shared layer learned by homes together, from %s, signed by %s, "
				       "published %s\n", fstr(e, "name"), (unsigned long long)fnum(e, "version"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"));
			} else if (!strcmp(orb(fstr(e, "kind"), ""), "list")) {
				printf("  %-12s version %llu, %llu names, from %s, signed by %s, published %s\n", fstr(e, "name"),
				       (unsigned long long)fnum(e, "version"), (unsigned long long)fnum(e, "entries"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"));
			} else if (!strcmp(src, "package"))
				printf("  %-12s packaged model, %llu trees\n", fstr(e, "name"), (unsigned long long)fnum(e, "trees"));
			else
				printf("  %-12s version %llu, %llu trees, from %s, signed by %s, trained %s, since %s\n",
				       fstr(e, "name"), (unsigned long long)fnum(e, "version"), (unsigned long long)fnum(e, "trees"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"), ago(fnum(e, "installed"), t, sizeof(t)));
			if (fnum(e, "held_back"))
				printf("  %-12s held back: version %llu and older (you rolled back)\n", "",
				       (unsigned long long)fnum(e, "held_back"));
			if (fstr(e, "refused") && *fstr(e, "refused"))
				printf("  %-12s last refused %s: %s\n", "", ago(fnum(e, "refused_at"), t, sizeof(t)), fstr(e, "refused"));
		}
	if (field(msg, "backends")) {
		printf("Computed by (models.backend %s; libraries in %s)\n", fstr(msg, "backend_choice"),
		       fstr(msg, "backend_dir"));
		blobmsg_for_each_attr(e, field(msg, "backends"), rem) {
			struct blob_attr *ns = field(e, "ns_per_score");
			double v = ns && blobmsg_type(ns) == BLOBMSG_TYPE_DOUBLE ? blobmsg_get_double(ns) : 0;

			if (fnum(e, "ok"))
				printf("  %-12s %s%.2f µs per score, same verdicts as the reference, %s\n",
				       fstr(e, "name"), fnum(e, "in_use") ? "IN USE, " : "", v / 1000, fstr(e, "source"));
			else
				printf("  %-12s not usable: %s (%s)\n", fstr(e, "name"), orb(fstr(e, "why"), "no model"),
				       fstr(e, "source"));
		}
		if (*fstr(msg, "backend_notes"))
			printf("  note: %s\n", fstr(msg, "backend_notes"));
	}
	printf("Trusted keys (%s)\n", fstr(msg, "keys_dir"));
	if (keys)
		blobmsg_for_each_attr(e, keys, rem) {
			printf("  %s  %s\n", fstr(e, "fingerprint"), orb(fstr(e, "comment"), ""));
			k++;
		}
	if (!k)
		printf("  none: model updates are refused until you trust a key (agctl trust <key.pub>)\n");
}

static double fdbl(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_DOUBLE ? blobmsg_get_double(a) : (double)fnum(tbl, name);
}

static void show_fed(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *list = field(msg, "list"), *ad = field(msg, "adapter"), *e = field(msg, "epsilon"), *x;
	size_t rem;
	char t[32];
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("Federated learning %s (names never leave this home; each round sends 14 noisy numbers)\n",
	       fnum(msg, "enabled") ? "on" : "OFF (alerts.fed)");
	printf("  examples   %llu: %llu safe (false alarms), %llu threats; a round needs %llu\n",
	       (unsigned long long)fnum(msg, "examples"), (unsigned long long)fnum(msg, "safe"),
	       (unsigned long long)fnum(msg, "threat"), (unsigned long long)fnum(msg, "min_examples"));
	printf("  rounds     %llu joined%s%s\n", (unsigned long long)fnum(msg, "rounds"),
	       *fstr(msg, "last") ? "; last: " : "", fstr(msg, "last"));
	if (e && blobmsg_type(e) == BLOBMSG_TYPE_STRING)
		printf("  privacy    %s\n", blobmsg_get_string(e));
	else
		printf("  privacy    ε = %.2f at δ = %g over all rounds (noise at least %.2f)\n", fdbl(msg, "epsilon"),
		       fdbl(msg, "delta"), fdbl(msg, "min_noise"));
	if (ad && fnum(ad, "loaded"))
		printf("  layer      version %llu in use\n", (unsigned long long)fnum(ad, "version"));
	else
		printf("  layer      none yet: the trees' own score is used\n");
	if (list)
		blobmsg_for_each_attr(x, list, rem) {
			if (!k++)
				printf("Your answers (kept here only):\n");
			printf("  %-7s %-40s %s, %s\n", fstr(x, "label"), fstr(x, "name"), fstr(x, "source"),
			       ago(fnum(x, "at"), t, sizeof(t)));
		}
}

static int remote_conf;

static void show_remote(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "peers"), *e;
	size_t rem;
	char t[32], rx[16], tx[16];
	int k = 0;

	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Remote access is off. Turn it on: agctl remote on <public address or DDNS name>\n");
		return;
	}
	printf("Remote access %s: %s, UDP %llu, tunnel %s (%s)\n", fnum(msg, "running") ? "on" : "NOT running",
	       orb(fstr(msg, "endpoint"), "(no endpoint)"), (unsigned long long)fnum(msg, "port"), fstr(msg, "subnet"),
	       fnum(msg, "full_tunnel") ? "all traffic goes home" : "home networks only");
	if (fstr(msg, "error") && *fstr(msg, "error"))
		printf("  problem: %s\n", fstr(msg, "error"));
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			if (!k++)
				printf("  %-20s %-12s %-10s %-22s %s\n", "PEER", "ADDRESS", "STATE", "FROM", "TRAFFIC");
			printf("  %-20.20s %-12s %-10s %-22.22s %s in, %s out\n", fstr(e, "name"), fstr(e, "ip"),
			       fnum(e, "online") ? "online" : fnum(e, "handshake") ? "away" : "never yet",
			       *fstr(e, "endpoint") ? fstr(e, "endpoint") : "-", human_bytes(fnum(e, "rx"), rx, sizeof(rx)),
			       human_bytes(fnum(e, "tx"), tx, sizeof(tx)));
			if (fnum(e, "handshake") && !fnum(e, "online"))
				printf("  %-20s last seen %s\n", "", ago(fnum(e, "handshake"), t, sizeof(t)));
		}
	if (!k)
		printf("  no peers yet: agctl remote add <name>\n");
}

static void show_remote_added(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out) {
		print_json(msg);
	} else if (!fnum(msg, "ok")) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	} else {
		printf("Scan this with the WireGuard app (+ > scan from QR code) on %s's phone.\n", fstr(msg, "name"));
		print_qr(fstr(msg, "config"));
		if (remote_conf)
			printf("\n%s\n", fstr(msg, "config"));
		printf("Tunnel address %s. The private key is not kept: this is the only time it is shown.\n",
		       fstr(msg, "ip"));
	}
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

static void show_model_change(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (!fnum(msg, "ok"))
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	else if (fnum(msg, "version"))
		printf("version %llu is in use now (no restart needed)\n", (unsigned long long)fnum(msg, "version"));
	else
		printf("done\n");
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

/* copy a usign public key into the trusted keys directory, named by its fingerprint */
static int trust(const char *file)
{
	struct ag_config c;
	uint8_t fp[8], pk[32];
	char text[1100], fph[17], dst[400];
	size_t n;
	FILE *f = fopen(file, "r"), *o;

	if (!f) {
		fprintf(stderr, "agctl: %s: %s\n", file, strerror(errno));
		return 1;
	}
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	if (ag_usign_pubkey(text, fp, pk)) {
		fprintf(stderr, "agctl: %s is not a usign public key (untrusted comment line + base64 line)\n", file);
		return 1;
	}
	ag_config_load(&c);
	ag_fingerprint_hex(fp, fph);
	if (mkdir(c.trusted_keys, 0755) && errno != EEXIST) {
		fprintf(stderr, "agctl: cannot create %s: %s (sudo?)\n", c.trusted_keys, strerror(errno));
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s.pub", c.trusted_keys, fph);
	if (!(o = fopen(dst, "w")) || fputs(text, o) < 0 || fclose(o)) {
		fprintf(stderr, "agctl: cannot write %s: %s (sudo?)\n", dst, strerror(errno));
		return 1;
	}
	chmod(dst, 0644);
	printf("trusting key %s: models it signs are accepted (%s)\n", fph, dst);
	return 0;
}

static int untrust(const char *fph)
{
	struct ag_config c;
	char path[400];

	ag_config_load(&c);
	snprintf(path, sizeof(path), "%s/%s.pub", c.trusted_keys, fph);
	if (strchr(fph, '/') || unlink(path)) {
		fprintf(stderr, "agctl: no trusted key %s (agctl models lists them)\n", fph);
		return 1;
	}
	printf("key %s is no longer trusted\n", fph);
	return 0;
}

static int plugin_config(const char *name, const char *key, const char *value)
{
	struct blob_buf a = { 0 };
	char line[512];
	int rc;

	if (!strcmp(value, "-")) {
		if (isatty(0))
			fprintf(stderr, "%s %s: ", name, key);
		if (!fgets(line, sizeof(line), stdin)) {
			fprintf(stderr, "agctl: nothing read\n");
			return 1;
		}
		line[strcspn(line, "\r\n")] = '\0';
		value = line;
	}
	blob_buf_init(&a, 0);
	blobmsg_add_string(&a, "name", name);
	blobmsg_add_string(&a, "key", key);
	blobmsg_add_string(&a, "value", value);
	rc = call("ag.plugin", "config", a.head, show_ok) || change_failed;
	blob_buf_free(&a);
	return rc;
}

/* fork/exec without a shell; 0 on success */
static int run(char *const argv[])
{
	pid_t pid = fork();
	int status = -1;

	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int plugin_add(const char *folder)
{
	struct ag_config c;
	struct blob_buf m = { 0 };
	struct blob_attr *cur;
	char manifest[512], dst[400], name[64] = "";
	size_t rem;
	struct stat st;

	ag_config_load(&c);
	snprintf(manifest, sizeof(manifest), "%s/manifest.json", folder);
	blob_buf_init(&m, 0);
	if (!blobmsg_add_json_from_file(&m, manifest)) {
		fprintf(stderr, "agctl: %s is missing or not valid JSON\n", manifest);
		blob_buf_free(&m);
		return 1;
	}
	blob_for_each_attr(cur, m.head, rem)
		if (!strcmp(blobmsg_name(cur), "name") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			snprintf(name, sizeof(name), "%s", blobmsg_get_string(cur));
	blob_buf_free(&m);
	if (!ag_plugin_name_ok(name)) {
		fprintf(stderr, "agctl: manifest name '%s' must be 1-31 of a-z, 0-9 and -\n", name);
		return 1;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "agctl: adding a plug-in needs root: sudo agctl plugin add %s\n", folder);
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s", c.plugin_user_dir, name);
	{
		char *mk[] = { "mkdir", "-p", c.plugin_user_dir, NULL };
		char *rm[] = { "rm", "-rf", dst, NULL };
		char *cp[] = { "cp", "-R", (char *)folder, dst, NULL };
		char *own[] = { "chown", "-R", "root:root", dst, NULL };
		char *mode[] = { "chmod", "-R", "go-w", dst, NULL };

		if (run(mk) || (stat(dst, &st) == 0 && run(rm)) || run(cp) || run(own) || run(mode)) {
			fprintf(stderr, "agctl: could not copy %s to %s\n", folder, dst);
			return 1;
		}
	}
	printf("installed %s in %s\n", name, dst);
	device_filter = name;
	if (call("ag.plugin", "rescan", NULL, NULL))
		return 1;
	{
		struct blob_buf a = { 0 };
		int rc;

		blob_buf_init(&a, 0);
		blobmsg_add_string(&a, "name", name);
		rc = call("ag.plugin", "list", a.head, show_plugins);
		blob_buf_free(&a);
		return rc;
	}
}

static int plugin_remove(const char *name)
{
	struct ag_config c;
	char dst[400];
	struct stat st;

	ag_config_load(&c);
	if (!ag_plugin_name_ok(name)) {
		fprintf(stderr, "agctl: no plug-in '%s'\n", name);
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s", c.plugin_user_dir, name);
	if (stat(dst, &st) != 0) {
		snprintf(dst, sizeof(dst), "%s/%s", c.plugin_dir, name);
		fprintf(stderr, stat(dst, &st) == 0 ? "agctl: %s comes from a package; remove the package instead\n" :
			"agctl: no plug-in '%s' that you added\n", name);
		return 1;
	}
	{
		char *rm[] = { "rm", "-rf", dst, NULL };

		if (geteuid() != 0 || run(rm)) {
			fprintf(stderr, "agctl: could not remove %s (sudo?)\n", dst);
			return 1;
		}
	}
	printf("removed %s\n", name);
	return call("ag.plugin", "rescan", NULL, NULL);
}

/* ---------- Wi-Fi keys ---------- */

static void show_keys(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "keys"), *k;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(k, arr, rem) {
			char t1[32];

			if (device_filter) {     /* one key: its QR again */
				n++;
				if (!*fstr(k, "qr")) {
					fprintf(stderr, "agctl: no access point answers, so the Wi-Fi name is unknown\n");
					continue;
				}
				printf("Scan to join \"%s\" as \"%s\":\n\n", fstr(k, "ssid"), fstr(k, "name"));
				print_qr(fstr(k, "qr"));
				printf("\n  Wi-Fi     %s\n  password  %s\n  state     %s%s%s\n", fstr(k, "ssid"),
				       fstr(k, "passphrase"), fstr(k, "state"), *fstr(k, "mac") ? ", bound to " : "", fstr(k, "mac"));
				continue;
			}
			if (!n++)
				printf("%-22s %-8s %-21s %-18s %s\n", "NAME", "ZONE", "STATE", "DEVICE", "SINCE");
			printf("%-22.22s %-8s %-21s %-18.18s %s\n", fstr(k, "name"), orb(fstr(k, "zone"), "-"),
			       fstr(k, "state"), orb(fstr(k, "device"), orb(fstr(k, "mac"), "-")),
			       ago(fnum(k, "bound") ? fnum(k, "bound") : fnum(k, "created"), t1, sizeof(t1)));
		}
	if (!n)
		printf(device_filter ? "No key called '%s'.\n" :
		       "No per-device keys. Give a device its own: agctl key add \"Living room plug\" iot\n", device_filter);
}

static void show_key_added(struct ubus_request *req, int type, struct blob_attr *msg)
{
	char until[64];
	time_t t = (time_t)fnum(msg, "expires");
	struct tm tm;

	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "ok")) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
		change_failed = 1;
		return;
	}
	localtime_r(&t, &tm);
	strftime(until, sizeof(until), "%Y-%m-%d %H:%M", &tm);
	printf("Scan to join \"%s\" as \"%s\"%s%s%s:\n\n", fstr(msg, "ssid"), fstr(msg, "name"),
	       *fstr(msg, "zone") ? " (zone " : "", fstr(msg, "zone"), *fstr(msg, "zone") ? ")" : "");
	print_qr(fstr(msg, "qr"));
	printf("\n  Wi-Fi     %s\n  password  %s   (for one device only)\n  waits     until %s for a device to join\n",
	       fstr(msg, "ssid"), fstr(msg, "passphrase"), until);
	printf("\nThe first device to join owns this key. Take it away any time:\n  agctl key revoke \"%s\"\n",
	       fstr(msg, "name"));
	if (*fstr(msg, "error"))
		printf("\nnote: %s\n", fstr(msg, "error"));
}

/* "90", "90s", "30m", "2h", "1d" -> seconds; -1 when unreadable */
static int parse_time(const char *s)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || v <= 0)
		return -1;
	switch (*end) {
	case '\0': case 's': break;
	case 'm': v *= 60; break;
	case 'h': v *= 3600; break;
	case 'd': v *= 86400; break;
	default: return -1;
	}
	if (*end && end[1])
		return -1;
	return v > 365L * 86400 ? 365 * 86400 : (int)v;
}

/* "2mbit", "500kbit", "1.5mbit", "800" (kbit/s) -> kbit/s; -1 when it isn't a rate */
static int parse_rate(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v <= 0)
		return -1;
	if (!*end || !strcasecmp(end, "k") || !strcasecmp(end, "kbit") || !strcasecmp(end, "kbps"))
		return (int)v;
	if (!strcasecmp(end, "m") || !strcasecmp(end, "mbit") || !strcasecmp(end, "mbps"))
		return (int)(v * 1000);
	if (!strcasecmp(end, "g") || !strcasecmp(end, "gbit") || !strcasecmp(end, "gbps"))
		return (int)(v * 1000000);
	return -1;
}

/* ---------- calls ---------- */

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb)
{
	uint32_t id;
	int rc;

	if (ubus_lookup_id(ctx, path, &id)) {
		fprintf(stderr, "agctl: '%s' not found on ubus. Is agd running (and are you allowed to use it)?\n", path);
		return 1;
	}
	/* generous: some calls run tools (remote on reloads the firewall: seconds on a small router) */
	rc = ubus_invoke(ctx, id, method, args, cb, NULL, 30000);
	if (rc) {
		fprintf(stderr, "agctl: %s %s failed: %s\n", path, method, ubus_strerror(rc));
		return 1;
	}
	return 0;
}

static void watch_cb(struct ubus_context *c, struct ubus_event_handler *ev,
		     const char *type, struct blob_attr *msg)
{
	char tmp[16], tmp2[32], b1[16], b2[16];
	struct blob_attr *ans, *a;
	size_t rem;
	int k = 0;

	if (json_out) {
		printf("%s ", type);
		return print_json(msg);
	}
	if (!strcmp(type, AG_EV_DNS)) {
		const char *qtype = ag_dns_type_str((uint16_t)fnum(msg, "qtype"), tmp, sizeof(tmp));

		printf("dns   %-16s %-5s %-44s ->", fstr(msg, "client"), qtype, fstr(msg, "qname"));
		ans = field(msg, "answers");
		if (ans)
			blobmsg_for_each_attr(a, ans, rem) {
				if (k++ == 2)
					break;
				printf(" %s", blobmsg_get_string(a));
			}
		if (!k)
			printf(" %s", no_address_text(fnum(msg, "rcode"), qtype, tmp2, sizeof(tmp2)));
		printf("  (%.1f ms)\n", fnum(msg, "latency_us") / 1000.0);
	} else if (!strcmp(type, AG_EV_FLOW)) {
		const char *what = fstr(msg, "event");
		const char *dst = orb(fstr(msg, "domain"), fstr(msg, "dst"));

		if (!strcmp(what, "new"))
			printf("conn  %-16s %-5s %-44s port %llu%s\n", fstr(msg, "client"), fstr(msg, "proto"), dst,
			       (unsigned long long)fnum(msg, "dport"),
			       fnum(msg, "no_dns") ? "   ** no DNS lookup **" : "");
		else if (!strcmp(what, "named"))
			printf("name  %-16s %-5s %-44s port %llu (from its handshake)\n", fstr(msg, "client"),
			       fstr(msg, "proto"), dst, (unsigned long long)fnum(msg, "dport"));
		else
			printf("end   %-16s %-5s %-44s %s down, %s up, %llus\n", fstr(msg, "client"),
			       fstr(msg, "proto"), dst, human_bytes(fnum(msg, "bytes_down"), b1, sizeof(b1)),
			       human_bytes(fnum(msg, "bytes_up"), b2, sizeof(b2)),
			       (unsigned long long)fnum(msg, "duration_s"));
	} else if (!strcmp(type, AG_EV_ALERT)) {
		printf("ALERT [%s] %s\n", fstr(msg, "kind"), fstr(msg, "reason"));
	} else if (!strcmp(type, AG_EV_DECISION)) {
		char subj[256];

		decision_subject(msg, subj, sizeof(subj));
		printf("DECISION #%llu %s: %s %s", (unsigned long long)fnum(msg, "id"), fstr(msg, "state"),
		       action_words(fstr(msg, "action")), subj);
		if (!strcmp(fstr(msg, "state"), "proposed"))
			printf("  (agctl approve %llu)", (unsigned long long)fnum(msg, "id"));
		if (*fstr(msg, "note"))
			printf("  -- %s", fstr(msg, "note"));
		printf("\n");
	} else if (!strcmp(type, AG_EV_WIFI)) {
		const char *ev = fstr(msg, "event"), *who = orb(fstr(msg, "hostname"), fstr(msg, "mac"));

		if (!strcmp(ev, "joined-with-key")) {
			printf("wifi  %-16s joined with its own key \"%s\"%s%s\n", fstr(msg, "mac"), fstr(msg, "key"),
			       *fstr(msg, "zone") ? ", zone " : "", fstr(msg, "zone"));
		} else if (!strcmp(ev, "steer")) {
			printf("wifi  %-16s asked to move %s -> %s GHz (%s): %s\n", who, fstr(msg, "from"), fstr(msg, "to"),
			       !strcmp(fstr(msg, "method"), "btm") ? "802.11v" : "refused on its band", fstr(msg, "reason"));
		} else if (!strcmp(ev, "steered") || !strcmp(ev, "steer-missed")) {
			printf("wifi  %-16s %s\n", who, !strcmp(ev, "steered") ? "moved as asked" : "stayed where it was");
		} else {
			printf("wifi  %-16s %s on %s", who, ev, fstr(msg, "iface"));
			if (*fstr(msg, "band"))
				printf(" (%s GHz", fstr(msg, "band"));
			if (!strcmp(ev, "connected") || !strcmp(ev, "roamed"))
				printf("%s%d dBm", *fstr(msg, "band") ? ", " : " (", (int32_t)fnum(msg, "signal"));
			if (*fstr(msg, "band") || !strcmp(ev, "connected") || !strcmp(ev, "roamed"))
				printf(")");
			printf("\n");
		}
	} else if (!strcmp(type, AG_EV_PRESENCE)) {
		uint64_t s = fnum(msg, !strcmp(fstr(msg, "event"), "arrived") ? "away_s" : "home_s");

		printf("home  %-16s %s", orb(fstr(msg, "hostname"), fstr(msg, "mac")), fstr(msg, "event"));
		if (s)
			printf(" (%s %lluh%02llum)", !strcmp(fstr(msg, "event"), "arrived") ? "away" : "home",
			       (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60));
		printf("\n");
	} else if (!strcmp(type, AG_EV_PLUGIN)) {
		printf("PLUG-IN %s (%s): %s%s%s\n", fstr(msg, "plugin"), fstr(msg, "kind"), fstr(msg, "state"),
		       *fstr(msg, "why") ? " -- " : "", fstr(msg, "why"));
	} else {
		char *body = blobmsg_format_json(msg, true);

		printf("%-5.5s %s  %s\n", type, type, body ? body : "");
		free(body);
	}
	fflush(stdout);
}

static int watch(const char *what)
{
	static struct ubus_event_handler ev;
	int all = !strcmp(what, "all"), rc = 0;

	uloop_init();
	ubus_add_uloop(ctx);
	ev.cb = watch_cb;
	if ((all || !strcmp(what, "dns")) && ubus_register_event_handler(ctx, &ev, AG_EV_DNS))
		rc = 1;
	if ((all || !strcmp(what, "flows")) && ubus_register_event_handler(ctx, &ev, AG_EV_FLOW))
		rc = 1;
	if ((all || !strcmp(what, "wifi")) && ubus_register_event_handler(ctx, &ev, AG_EV_WIFI))
		rc = 1;
	if ((all || !strcmp(what, "wifi") || !strcmp(what, "home")) &&
	    ubus_register_event_handler(ctx, &ev, AG_EV_PRESENCE))
		rc = 1;
	if ((all || !strcmp(what, "alerts")) && ubus_register_event_handler(ctx, &ev, AG_EV_ALERT))
		rc = 1;
	if ((all || !strcmp(what, "alerts") || !strcmp(what, "decisions")) &&
	    ubus_register_event_handler(ctx, &ev, AG_EV_DECISION))
		rc = 1;
	if ((all || !strcmp(what, "plugins")) &&
	    (ubus_register_event_handler(ctx, &ev, AG_EV_PLUGIN) || ubus_register_event_handler(ctx, &ev, "iot.*")))
		rc = 1;
	if ((all || !strcmp(what, "models")) && ubus_register_event_handler(ctx, &ev, AG_EV_MODEL))
		rc = 1;
	if (rc) {
		fprintf(stderr, "agctl: cannot subscribe to events (permission?)\n");
	} else {
		fprintf(stderr, "watching %s (Ctrl+C to stop)\n", what);
		uloop_run();
	}
	uloop_done();
	return rc;
}

int main(int argc, char **argv)
{
	struct blob_buf args = { 0 };
	const char *cmd;
	int rc = 0, i = 1;

	if (i < argc && !strcmp(argv[i], "-j")) {
		json_out = 1;
		i++;
	}
	if (i >= argc) {
		usage();
		return 1;
	}
	cmd = argv[i++];
	if (!strcmp(cmd, "version")) {
		printf("agctl %s\n", AG_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}
	if (!strcmp(cmd, "qr") && i < argc) {      /* no brain needed */
		qr_pbm = i + 1 < argc && !strcmp(argv[i + 1], "--pbm");
		print_qr(argv[i]);
		return 0;
	}
	if (!strcmp(cmd, "selftest")) {             /* on this CPU: crypto, model, QR, DNS through agdns */
		int agctl_selftest(void);

		return agctl_selftest() ? 1 : 0;
	}
	if (!strcmp(cmd, "trust") && i < argc)     /* files only: agd reads the keys at each offer */
		return trust(argv[i]);
	if (!strcmp(cmd, "untrust") && i < argc)
		return untrust(argv[i]);

	ctx = ubus_connect(ag_ubus_path());
	if (!ctx) {
		fprintf(stderr, "agctl: cannot connect to ubus. Is ubusd running?\n");
		return 1;
	}

	if (!strcmp(cmd, "status")) {
		rc = call("ag", "status", NULL, show_status);
	} else if (!strcmp(cmd, "devices")) {
		rc = call("ag.device", "list", NULL, show_devices);
	} else if (!strcmp(cmd, "device") && i < argc) {
		device_filter = argv[i];
		rc = call("ag.device", "list", NULL, show_device);
	} else if (!strcmp(cmd, "score") && i < argc) {
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "name", argv[i]);
		rc = call("ag.domain", "score", args.head, show_score);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "alerts")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 20);
		rc = call("ag.alert", "list", args.head, show_alerts);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "wifi")) {
		rc = call("ag.wifi", "radios", NULL, show_radios);
	} else if (!strcmp(cmd, "dns") && i < argc && !strcmp(argv[i], "cache")) {
		rc = call("ag.dnscache", "stats", NULL, show_dnscache);
	} else if (!strcmp(cmd, "dns") || !strcmp(cmd, "flows")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 30);
		if (!strcmp(cmd, "dns"))
			rc = call("ag.dns", "log", args.head, show_dns);
		else
			rc = call("ag.flow", "recent", args.head, show_flows);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "watch")) {
		rc = watch(i < argc ? argv[i] : "dns");
	} else if (!strcmp(cmd, "decisions")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 20);
		rc = call("ag.decision", "list", args.head, show_decisions);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "channel") && i + 1 < argc) {
		char target[64];
		int ttl = i + 2 < argc ? parse_time(argv[i + 2]) : 0;

		snprintf(target, sizeof(target), "%s:%s", argv[i], argv[i + 1]);
		if (i + 2 < argc && ttl <= 0) {
			fprintf(stderr, "agctl: usage: agctl channel <access point|2.4|5> <channel> [time]\n");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", "channel");
			blobmsg_add_string(&args, "target", target);
			if (ttl > 0)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			rc = call("ag.decision", "request", args.head, show_change) || change_failed;
			blob_buf_free(&args);
		}
	} else if ((!strcmp(cmd, "quarantine") || !strcmp(cmd, "prioritize") || !strcmp(cmd, "block") ||
		    !strcmp(cmd, "deprioritize") || !strcmp(cmd, "throttle") || !strcmp(cmd, "disconnect") ||
		    (!strcmp(cmd, "do") && i + 1 < argc)) && i < argc) {
		const char *action = !strcmp(cmd, "do") ? argv[i++] : !strcmp(cmd, "prioritize") ? "priority" : cmd;
		const char *target = argv[i++], *device = NULL, *bad = NULL;
		int ttl = 0, rate = 0;

		/* optional, in this order: [rate] (throttle) [time] [device] (block) */
		if (!strcmp(cmd, "throttle") && i < argc && parse_rate(argv[i]) > 0 && parse_time(argv[i]) < 0)
			rate = parse_rate(argv[i++]);
		if (i < argc && parse_time(argv[i]) > 0)
			ttl = parse_time(argv[i++]);
		if (!strcmp(cmd, "block") && i < argc)
			device = argv[i++];
		if (i < argc)
			bad = argv[i];
		if (bad) {
			fprintf(stderr, "agctl: unexpected '%s' (time: 90s, 30m, 2h, 1d%s)\n", bad,
				!strcmp(cmd, "throttle") ? "; rate: 2mbit, 500kbit" : "");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", action);
			blobmsg_add_string(&args, "target", target);
			if (ttl)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			if (rate)
				blobmsg_add_u32(&args, "rate", (uint32_t)rate);
			if (device)
				blobmsg_add_string(&args, "device", device);
			rc = call("ag.decision", "request", args.head, show_change) || change_failed;
			blob_buf_free(&args);
		}
	} else if ((!strcmp(cmd, "approve") || !strcmp(cmd, "reject") || !strcmp(cmd, "undo")) && i < argc) {
		int ttl = (!strcmp(cmd, "approve") && i + 1 < argc) ? parse_time(argv[i + 1]) : 0;

		if (ttl < 0 || atoi(argv[i]) <= 0) {
			fprintf(stderr, "agctl: usage: agctl %s <id>%s\n", cmd, !strcmp(cmd, "approve") ? " [time]" : "");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_u32(&args, "id", (uint32_t)atoi(argv[i]));
			if (ttl)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			rc = call("ag.decision", !strcmp(cmd, "undo") ? "revert" : cmd, args.head, show_change) ||
			     change_failed;
			blob_buf_free(&args);
		}
	} else if (!strcmp(cmd, "plugins")) {
		rc = call("ag.plugin", "list", NULL, show_plugins);
	} else if (!strcmp(cmd, "plugin") && i < argc) {
		const char *sub = argv[i++];

		if (!strcmp(sub, "add") && i < argc) {
			rc = plugin_add(argv[i]);
		} else if (!strcmp(sub, "remove") && i < argc) {
			rc = plugin_remove(argv[i]);
		} else if ((!strcmp(sub, "allow") || !strcmp(sub, "deny") || !strcmp(sub, "enable") ||
			    !strcmp(sub, "disable")) && i < argc) {
			int allow = !strcmp(sub, "allow") || !strcmp(sub, "deny");

			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", argv[i]);
			blobmsg_add_u8(&args, allow ? "allow" : "enabled", !strcmp(sub, "allow") || !strcmp(sub, "enable"));
			rc = call("ag.plugin", "set", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		} else if (!strcmp(sub, "config") && i + 2 < argc) {
			rc = plugin_config(argv[i], argv[i + 1], argv[i + 2]);
		} else if (i == argc || !strcmp(sub, "show")) {
			device_filter = i < argc ? argv[i] : sub;
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", device_filter);
			rc = call("ag.plugin", "list", args.head, show_plugins);
			blob_buf_free(&args);
		} else {
			usage();
			rc = 1;
		}
	} else if (!strcmp(cmd, "models")) {
		rc = call("ag.model", "list", NULL, show_models);
	} else if (!strcmp(cmd, "model") && i < argc) {
		const char *sub = argv[i++];

		blob_buf_init(&args, 0);
		if (!strcmp(sub, "install") && i < argc) {
			char file[PATH_MAX], sig[PATH_MAX + 8] = "";
			int force = 0;

			if (!realpath(argv[i++], file)) {
				fprintf(stderr, "agctl: %s: %s\n", argv[i - 1], strerror(errno));
				rc = 1;
			}
			for (; i < argc && !rc; i++) {
				if (!strcmp(argv[i], "--force"))
					force = 1;
				else if (!realpath(argv[i], sig)) {
					fprintf(stderr, "agctl: %s: %s\n", argv[i], strerror(errno));
					rc = 1;
				}
			}
			if (!sig[0]) {
				snprintf(sig, sizeof(sig), "%s.sig", file);
				if (access(sig, F_OK))
					sig[0] = '\0';
			}
			if (!rc) {
				blobmsg_add_string(&args, "file", file);
				if (sig[0])
					blobmsg_add_string(&args, "sig", sig);
				blobmsg_add_u8(&args, "force", (uint8_t)force);
				rc = call("ag.model", "install", args.head, show_model_change) || change_failed;
			}
		} else if (!strcmp(sub, "rollback")) {
			if (i < argc)
				blobmsg_add_string(&args, "name", argv[i]);
			rc = call("ag.model", "rollback", args.head, show_model_change) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "bridge")) {
		if (i == argc) {
			device_filter = "bridge";
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", "bridge");
			rc = call("ag.plugin", "list", args.head, show_plugins);
			blob_buf_free(&args);
		} else if (!strcmp(argv[i], "token")) {
			rc = plugin_config("bridge", "token", "-");
		} else {
			rc = plugin_config("bridge", "url", argv[i]);
		}
	} else if (!strcmp(cmd, "remote")) {
		const char *sub = i < argc ? argv[i++] : "";

		blob_buf_init(&args, 0);
		if (!*sub) {
			rc = call("ag.remote", "list", NULL, show_remote);
		} else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
			if (i < argc)
				blobmsg_add_string(&args, "endpoint", argv[i]);
			rc = call("ag.remote", sub, args.head, show_ok) || change_failed;
		} else if ((!strcmp(sub, "add") || !strcmp(sub, "revoke")) && i < argc) {
			blobmsg_add_string(&args, "name", argv[i++]);
			remote_conf = i < argc && !strcmp(argv[i], "--conf");
			rc = call("ag.remote", sub, args.head, !strcmp(sub, "add") ? show_remote_added : show_ok) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "fed") && (i == argc || !strcmp(argv[i], "forget"))) {
		if (i == argc) {
			rc = call("ag.fed", "status", NULL, show_fed);
		} else {
			blob_buf_init(&args, 0);
			if (i + 1 < argc)
				blobmsg_add_string(&args, "name", argv[i + 1]);
			rc = call("ag.fed", "forget", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		}
	} else if (!strcmp(cmd, "label") && i + 1 < argc) {
		if (strcmp(argv[i + 1], "safe") && strcmp(argv[i + 1], "threat")) {
			fprintf(stderr, "agctl: usage: agctl label <domain> safe|threat\n");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", argv[i]);
			blobmsg_add_string(&args, "label", argv[i + 1]);
			rc = call("ag.fed", "label", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		}
	} else if (!strcmp(cmd, "keys")) {
		rc = call("ag.wifi", "keys", NULL, show_keys);
	} else if (!strcmp(cmd, "key") && i + 1 < argc) {
		const char *sub = argv[i++], *name = argv[i++];

		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "name", name);
		if (!strcmp(sub, "add")) {
			const char *zone = NULL;
			int ttl = 0;

			if (i < argc && parse_time(argv[i]) < 0)
				zone = argv[i++];
			if (i < argc && (ttl = parse_time(argv[i])) > 0)
				i++;
			if (i < argc || ttl < 0) {
				fprintf(stderr, "agctl: usage: agctl key add <name> [zone] [time]\n");
				rc = 1;
			} else {
				if (zone && strcmp(zone, "none"))
					blobmsg_add_string(&args, "zone", zone);
				if (ttl > 0)
					blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
				rc = call("ag.wifi", "key_add", args.head, show_key_added) || change_failed;
			}
		} else if (!strcmp(sub, "qr")) {
			device_filter = name;
			rc = call("ag.wifi", "keys", args.head, show_keys);
		} else if (!strcmp(sub, "revoke")) {
			rc = call("ag.wifi", "key_revoke", args.head, show_ok) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "zone") && i + 1 < argc) {
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "device", argv[i]);
		blobmsg_add_string(&args, "zone", strcmp(argv[i + 1], "none") ? argv[i + 1] : "");
		rc = call("ag.device", "zone", args.head, show_ok) || change_failed;
		blob_buf_free(&args);
	} else {
		usage();
		rc = 1;
	}
	ubus_free(ctx);
	return rc;
}
