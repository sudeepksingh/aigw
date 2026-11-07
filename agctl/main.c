/* SPDX-License-Identifier: Apache-2.0 */
/* agctl: command-line view of the AIGW brain over ubus. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"

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
		"  flows [N]           last N finished connections (default 30)\n"
		"  alerts [N]          what AIGW thinks you should know, and why\n"
		"  score <domain>      the domain-risk model's view of a name\n"
		"  watch [dns|flows|wifi|alerts|decisions|all]   follow activity live (Ctrl+C to stop)\n"
		"\n"
		"  decisions [N]       actions waiting for you, in effect, and past ones\n"
		"  approve <id> [time] carry out a proposed action (optionally for another time limit)\n"
		"  reject <id>         say no to a proposed action\n"
		"  undo <id>           end an action now\n"
		"  quarantine <device> [time]  cut a device off from everything but the gateway\n"
		"  prioritize <device> [time]  send a device's traffic first (Wi-Fi video queue)\n"
		"  block <domain> [time]       stop every device resolving a domain and its subdomains\n"
		"                      time: 90s, 30m, 2h, 1d (default from config, 1h)\n"
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
	printf("  devices         %llu (%llu on Wi-Fi now)\n", (unsigned long long)fnum(msg, "devices"),
	       (unsigned long long)fnum(msg, "wifi_stations"));
	printf("  dns lookups     %llu (last %s)\n", (unsigned long long)fnum(msg, "dns_queries"),
	       ago(fnum(msg, "dns_last_event"), t, sizeof(t)));
	printf("  connections     %llu, %llu without a DNS lookup\n", (unsigned long long)fnum(msg, "flows"),
	       (unsigned long long)fnum(msg, "flows_no_dns"));
	if (s)
		printf("  sensors         dns %s, flow %s, wifi %s\n",
		       fnum(s, "dns") ? "on" : "waiting", fnum(s, "flow") ? "on" : "OFF",
		       fnum(s, "wifi") ? "on" : "no stations");
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
		struct blob_attr *m = field(msg, "models"), *e;
		size_t rem;
		int k = 0;

		printf("  models         ");
		if (m)
			blobmsg_for_each_attr(e, m, rem) {
				printf(" %s (%u trees)", blobmsg_name(e), blobmsg_get_u32(e));
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
		if (*fstr(d, "vendor_class") || *fstr(d, "dhcp_options"))
			printf("  dhcp            %s  options %s\n", orb(fstr(d, "vendor_class"), "-"),
			       orb(fstr(d, "dhcp_options"), "-"));
		printf("  seen            first %s, last %s\n", ago(fnum(d, "first_seen"), t1, sizeof(t1)),
		       ago(fnum(d, "last_seen"), t2, sizeof(t2)));
		if (w && fnum(w, "connected"))
			printf("  wi-fi           %d dBm, %.1f Mbit/s, on %s since %s\n", (int32_t)fnum(w, "signal"),
			       fnum(w, "rate_kbps") / 1000.0, fstr(w, "iface"), ago(fnum(w, "since"), t1, sizeof(t1)));
		else
			printf("  wi-fi           not connected\n");
		printf("  traffic         %s down, %s up, %llu connections (%llu without DNS)\n",
		       human_bytes(fnum(d, "bytes_down"), b1, sizeof(b1)),
		       human_bytes(fnum(d, "bytes_up"), b2, sizeof(b2)),
		       (unsigned long long)fnum(d, "flows"), (unsigned long long)fnum(d, "flows_no_dns"));
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
		       (unsigned long long)fnum(e, "duration_s"), fnum(e, "no_dns") ? "no DNS lookup" : "");
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
	return !strcmp(action, "quarantine") ? "quarantine" : !strcmp(action, "priority") ? "prioritize" :
	       !strcmp(action, "block") ? "block" : action;
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
			char t1[32], t2[32];
			int block = !strcmp(action, "block");

			n++;
			printf("#%-4llu %-9s %-10s %s%s%s\n", (unsigned long long)fnum(e, "id"), st,
			       action_words(action), block ? fstr(e, "target") : fstr(e, "device"),
			       block ? "" : "  ", block ? "" : fstr(e, "target"));
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

/* ---------- calls ---------- */

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb)
{
	uint32_t id;
	int rc;

	if (ubus_lookup_id(ctx, path, &id)) {
		fprintf(stderr, "agctl: '%s' not found on ubus. Is agd running (and are you allowed to use it)?\n", path);
		return 1;
	}
	rc = ubus_invoke(ctx, id, method, args, cb, NULL, 3000);
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
		else
			printf("end   %-16s %-5s %-44s %s down, %s up, %llus\n", fstr(msg, "client"),
			       fstr(msg, "proto"), dst, human_bytes(fnum(msg, "bytes_down"), b1, sizeof(b1)),
			       human_bytes(fnum(msg, "bytes_up"), b2, sizeof(b2)),
			       (unsigned long long)fnum(msg, "duration_s"));
	} else if (!strcmp(type, AG_EV_ALERT)) {
		printf("ALERT [%s] %s\n", fstr(msg, "kind"), fstr(msg, "reason"));
	} else if (!strcmp(type, AG_EV_DECISION)) {
		const char *action = fstr(msg, "action");

		printf("DECISION #%llu %s: %s %s", (unsigned long long)fnum(msg, "id"), fstr(msg, "state"),
		       action_words(action), !strcmp(action, "block") ? fstr(msg, "target") : fstr(msg, "device"));
		if (!strcmp(fstr(msg, "state"), "proposed"))
			printf("  (agctl approve %llu)", (unsigned long long)fnum(msg, "id"));
		if (*fstr(msg, "note"))
			printf("  -- %s", fstr(msg, "note"));
		printf("\n");
	} else if (!strcmp(type, AG_EV_WIFI)) {
		printf("wifi  %-16s %s on %s", orb(fstr(msg, "hostname"), fstr(msg, "mac")),
		       fstr(msg, "event"), fstr(msg, "iface"));
		if (!strcmp(fstr(msg, "event"), "connected"))
			printf(" (%d dBm)", (int32_t)fnum(msg, "signal"));
		printf("\n");
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
	if ((all || !strcmp(what, "alerts")) && ubus_register_event_handler(ctx, &ev, AG_EV_ALERT))
		rc = 1;
	if ((all || !strcmp(what, "alerts") || !strcmp(what, "decisions")) &&
	    ubus_register_event_handler(ctx, &ev, AG_EV_DECISION))
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
	} else if ((!strcmp(cmd, "quarantine") || !strcmp(cmd, "prioritize") || !strcmp(cmd, "block")) && i < argc) {
		int ttl = i + 1 < argc ? parse_time(argv[i + 1]) : 0;

		if (ttl < 0) {
			fprintf(stderr, "agctl: unreadable time '%s' (try 90s, 30m, 2h, 1d)\n", argv[i + 1]);
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", !strcmp(cmd, "prioritize") ? "priority" : cmd);
			blobmsg_add_string(&args, "target", argv[i]);
			if (ttl)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
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
	} else {
		usage();
		rc = 1;
	}
	ubus_free(ctx);
	return rc;
}
