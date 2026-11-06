/* SPDX-License-Identifier: Apache-2.0 */
/* agctl: command-line view of the AIGW brain over ubus. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"

static struct ubus_context *ctx;
static int json_out;

static void usage(void)
{
	fprintf(stderr,
		"usage: agctl [-j] <command>\n"
		"\n"
		"  status            brain status\n"
		"  devices           devices seen on the network\n"
		"  dns [N]           last N DNS queries (default 30)\n"
		"  watch             follow DNS activity live (Ctrl+C to stop)\n"
		"  version           agctl version\n"
		"\n"
		"  -j                print raw JSON\n");
}

static const char *ago(uint64_t ts, char *out, size_t n)
{
	int64_t d = ag_wall_s() - (int64_t)ts;

	if (!ts)
		snprintf(out, n, "-");
	else if (d < 60)
		snprintf(out, n, "%llds ago", (long long)d);
	else if (d < 3600)
		snprintf(out, n, "%lldm ago", (long long)(d / 60));
	else
		snprintf(out, n, "%lldh ago", (long long)(d / 3600));
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

static uint64_t fnum(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
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

/* ---------- renderers ---------- */

static void show_status(struct ubus_request *req, int type, struct blob_attr *msg)
{
	char t[32];

	if (json_out)
		return print_json(msg);
	printf("AIGW %s on %s  (phase: %s)\n", fstr(msg, "version"), fstr(msg, "host"), fstr(msg, "phase"));
	printf("  uptime        %llus\n", (unsigned long long)fnum(msg, "uptime_s"));
	printf("  devices       %llu\n", (unsigned long long)fnum(msg, "devices"));
	printf("  dns queries   %llu (last %s)\n", (unsigned long long)fnum(msg, "dns_queries"),
	       ago(fnum(msg, "dns_last_event"), t, sizeof(t)));
}

static void show_devices(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "devices"), *d;
	size_t rem;
	char t[32];

	if (json_out)
		return print_json(msg);
	printf("%-16s %-18s %-20s %7s  %-9s  %s\n", "IP", "MAC", "NAME", "QUERIES", "SEEN", "TOP DOMAINS");
	if (!arr)
		return;
	blobmsg_for_each_attr(d, arr, rem) {
		struct blob_attr *top = field(d, "top_domains"), *e;
		size_t r2;
		int k = 0;

		printf("%-16s %-18s %-20.20s %7llu  %-9s  ", fstr(d, "ip"),
		       *fstr(d, "mac") ? fstr(d, "mac") : "-",
		       *fstr(d, "hostname") ? fstr(d, "hostname") : "-",
		       (unsigned long long)fnum(d, "queries"), ago(fnum(d, "last_seen"), t, sizeof(t)));
		if (top)
			blobmsg_for_each_attr(e, top, r2) {
				if (k++ == 3)
					break;
				printf("%s%s(%llu)", k > 1 ? ", " : "", fstr(e, "domain"),
				       (unsigned long long)fnum(e, "count"));
			}
		printf("\n");
	}
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
		       *fstr(e, "hostname") ? fstr(e, "hostname") : "-", fstr(e, "qtype"),
		       fstr(e, "qname"), *fstr(e, "answer") ? fstr(e, "answer") :
		       no_address_text(fnum(e, "rcode"), fstr(e, "qtype"), tmp, sizeof(tmp)));
	}
}

/* ---------- calls ---------- */

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb)
{
	uint32_t id;
	int rc;

	if (ubus_lookup_id(ctx, path, &id)) {
		fprintf(stderr, "agctl: '%s' not found on ubus. Is agd running?\n", path);
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
	struct blob_attr *ans = field(msg, "answers"), *a;
	char tmp[16], tmp2[32];
	const char *qtype;
	size_t rem;
	int k = 0;

	if (json_out)
		return print_json(msg);
	qtype = ag_dns_type_str((uint16_t)fnum(msg, "qtype"), tmp, sizeof(tmp));
	printf("%-16s %-5s %-44s ->", fstr(msg, "client"), qtype, fstr(msg, "qname"));
	if (ans)
		blobmsg_for_each_attr(a, ans, rem) {
			if (k++ == 2)
				break;
			printf(" %s", blobmsg_get_string(a));
		}
	if (!k)
		printf(" %s", no_address_text(fnum(msg, "rcode"), qtype, tmp2, sizeof(tmp2)));
	printf("  (%.1f ms)\n", fnum(msg, "latency_us") / 1000.0);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	static struct ubus_event_handler ev;
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
	} else if (!strcmp(cmd, "dns")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 30);
		rc = call("ag.dns", "log", args.head, show_dns);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "watch")) {
		uloop_init();
		ubus_add_uloop(ctx);
		ev.cb = watch_cb;
		if (ubus_register_event_handler(ctx, &ev, AG_EV_DNS)) {
			fprintf(stderr, "agctl: cannot subscribe to %s\n", AG_EV_DNS);
			rc = 1;
		} else {
			fprintf(stderr, "watching DNS activity (Ctrl+C to stop)\n");
			uloop_run();
		}
		uloop_done();
	} else {
		usage();
		rc = 1;
	}
	ubus_free(ctx);
	return rc;
}
