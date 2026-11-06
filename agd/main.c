/* SPDX-License-Identifier: Apache-2.0 */
/*
 * agd: the AIGW brain.
 *
 * v0.1 ("observe"): builds the Context Graph's first two tables from sensor
 * events and host data -- devices (DHCP leases, ARP, DNS activity) and the
 * DNS log -- and serves them over ubus:
 *
 *   ubus call ag status
 *   ubus call ag.device list
 *   ubus call ag.dns log '{"limit": 50}'
 *
 * Models, policies and actuators plug into this process in later phases.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"

#define DEV_MAX      256
#define DEV_DOM_MAX  32
#define DOM_LEN      128
#define DNSLOG_MAX   512
#define POLL_MS      5000

struct dom_count {
	char name[DOM_LEN];
	uint32_t n;
};

struct device {
	int used;
	char ip[INET6_ADDRSTRLEN];
	char mac[18];
	char hostname[64];
	int64_t first_seen, last_seen;
	uint32_t queries;
	struct dom_count doms[DEV_DOM_MAX];
};

struct dnslog {
	int64_t ts;
	char client[INET6_ADDRSTRLEN];
	char qname[DOM_LEN];
	char answer[INET6_ADDRSTRLEN];
	uint16_t qtype, rcode;
	uint32_t latency_us;
	uint8_t n_ans;
};

static struct ag_config cfg;
static struct device devs[DEV_MAX];
static struct dnslog dlog[DNSLOG_MAX];
static unsigned dlog_head, dlog_count;
static uint64_t dns_total;
static int64_t started, last_dns_event;

static struct ubus_auto_conn conn;
static struct ubus_event_handler dns_ev;
static struct uloop_timeout poll_timer;
static struct blob_buf b;

/* ---------- device table ---------- */

static struct device *dev_find(const char *ip, int create)
{
	struct device *oldest = NULL;
	int i;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && !strcmp(devs[i].ip, ip))
			return &devs[i];
	if (!create)
		return NULL;
	for (i = 0; i < DEV_MAX; i++) {
		if (!devs[i].used) {
			oldest = &devs[i];
			break;
		}
		if (!oldest || devs[i].last_seen < oldest->last_seen)
			oldest = &devs[i];
	}
	memset(oldest, 0, sizeof(*oldest));
	oldest->used = 1;
	snprintf(oldest->ip, sizeof(oldest->ip), "%s", ip);
	oldest->first_seen = oldest->last_seen = ag_wall_s();
	return oldest;
}

static int dev_count(void)
{
	int i, n = 0;

	for (i = 0; i < DEV_MAX; i++)
		n += devs[i].used;
	return n;
}

/* Space-saving top-k: keeps the most frequent domains per device in fixed memory. */
static void dev_count_domain(struct device *d, const char *name)
{
	struct dom_count *min = &d->doms[0];
	int i;

	for (i = 0; i < DEV_DOM_MAX; i++) {
		if (d->doms[i].n && !strncmp(d->doms[i].name, name, DOM_LEN - 1)) {
			d->doms[i].n++;
			return;
		}
		if (d->doms[i].n < min->n)
			min = &d->doms[i];
	}
	snprintf(min->name, sizeof(min->name), "%s", name);
	min->n++;
}

/* dnsmasq lease lines: "<expiry> <mac> <ip> <hostname|*> <client-id|*>" */
static void poll_leases(void)
{
	FILE *f = fopen(cfg.leases, "r");
	char line[512];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char mac[32], ip[64], host[128];
		long long exp;
		struct device *d;

		if (sscanf(line, "%lld %31s %63s %127s", &exp, mac, ip, host) != 4)
			continue;
		d = dev_find(ip, 1);
		snprintf(d->mac, sizeof(d->mac), "%.17s", mac);
		if (strcmp(host, "*"))
			snprintf(d->hostname, sizeof(d->hostname), "%.63s", host);
	}
	fclose(f);
}

/* /proc/net/arp fills in MACs for devices with static addresses */
static void poll_arp(void)
{
	FILE *f = fopen("/proc/net/arp", "r");
	char line[256];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char ip[64], hw[32], mac[32], mask[32], dev[32];
		unsigned flags;
		struct device *d;

		if (sscanf(line, "%63s %31s %x %31s %31s %31s", ip, hw, &flags, mac, mask, dev) != 6)
			continue;
		if (!(flags & 0x2))
			continue;
		d = dev_find(ip, 0);
		if (d && !d->mac[0])
			snprintf(d->mac, sizeof(d->mac), "%.17s", mac);
	}
	fclose(f);
}

static void poll_cb(struct uloop_timeout *t)
{
	poll_leases();
	poll_arp();
	uloop_timeout_set(t, POLL_MS);
}

/* ---------- sensor events ---------- */

enum { EV_CLIENT, EV_QNAME, EV_QTYPE, EV_RCODE, EV_LAT, EV_ANSWERS, __EV_MAX };

static const struct blobmsg_policy ev_policy[__EV_MAX] = {
	[EV_CLIENT]  = { .name = "client", .type = BLOBMSG_TYPE_STRING },
	[EV_QNAME]   = { .name = "qname", .type = BLOBMSG_TYPE_STRING },
	[EV_QTYPE]   = { .name = "qtype", .type = BLOBMSG_TYPE_INT32 },
	[EV_RCODE]   = { .name = "rcode", .type = BLOBMSG_TYPE_INT32 },
	[EV_LAT]     = { .name = "latency_us", .type = BLOBMSG_TYPE_INT32 },
	[EV_ANSWERS] = { .name = "answers", .type = BLOBMSG_TYPE_ARRAY },
};

static void on_dns_event(struct ubus_context *ctx, struct ubus_event_handler *ev,
			 const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__EV_MAX], *cur;
	struct dnslog *e;
	struct device *d;
	const char *client, *qname;
	size_t rem;

	blobmsg_parse(ev_policy, __EV_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[EV_CLIENT] || !tb[EV_QNAME])
		return;
	client = blobmsg_get_string(tb[EV_CLIENT]);
	qname = blobmsg_get_string(tb[EV_QNAME]);

	dns_total++;
	last_dns_event = ag_wall_s();

	d = dev_find(client, 1);
	d->last_seen = last_dns_event;
	d->queries++;
	dev_count_domain(d, qname);

	e = &dlog[dlog_head];
	memset(e, 0, sizeof(*e));
	e->ts = last_dns_event;
	snprintf(e->client, sizeof(e->client), "%s", client);
	snprintf(e->qname, sizeof(e->qname), "%s", qname);
	e->qtype = tb[EV_QTYPE] ? (uint16_t)blobmsg_get_u32(tb[EV_QTYPE]) : 0;
	e->rcode = tb[EV_RCODE] ? (uint16_t)blobmsg_get_u32(tb[EV_RCODE]) : 0;
	e->latency_us = tb[EV_LAT] ? blobmsg_get_u32(tb[EV_LAT]) : 0;
	if (tb[EV_ANSWERS]) {
		blobmsg_for_each_attr(cur, tb[EV_ANSWERS], rem) {
			if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
				continue;
			if (!e->n_ans)
				snprintf(e->answer, sizeof(e->answer), "%s", blobmsg_get_string(cur));
			e->n_ans++;
		}
	}
	dlog_head = (dlog_head + 1) % DNSLOG_MAX;
	if (dlog_count < DNSLOG_MAX)
		dlog_count++;
}

/* ---------- ubus API ---------- */

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", AG_VERSION);
	blobmsg_add_string(&b, "host", cfg.host);
	blobmsg_add_string(&b, "phase", "observe");
	blobmsg_add_u64(&b, "uptime_s", (uint64_t)(ag_wall_s() - started));
	blobmsg_add_u32(&b, "devices", (uint32_t)dev_count());
	blobmsg_add_u64(&b, "dns_queries", dns_total);
	blobmsg_add_u64(&b, "dns_last_event", (uint64_t)last_dns_event);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int dom_cmp(const void *a, const void *b2)
{
	const struct dom_count *x = a, *y = b2;

	return (y->n > x->n) - (y->n < x->n);
}

static int m_device_list(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	void *arr, *t, *top, *dt;
	int i, k;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "devices");
	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];
		struct dom_count sorted[DEV_DOM_MAX];

		if (!d->used)
			continue;
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "ip", d->ip);
		blobmsg_add_string(&b, "mac", d->mac);
		blobmsg_add_string(&b, "hostname", d->hostname);
		blobmsg_add_u64(&b, "first_seen", (uint64_t)d->first_seen);
		blobmsg_add_u64(&b, "last_seen", (uint64_t)d->last_seen);
		blobmsg_add_u32(&b, "queries", d->queries);
		memcpy(sorted, d->doms, sizeof(sorted));
		qsort(sorted, DEV_DOM_MAX, sizeof(sorted[0]), dom_cmp);
		top = blobmsg_open_array(&b, "top_domains");
		for (k = 0; k < 5 && sorted[k].n; k++) {
			dt = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "domain", sorted[k].name);
			blobmsg_add_u32(&b, "count", sorted[k].n);
			blobmsg_close_table(&b, dt);
		}
		blobmsg_close_array(&b, top);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

enum { LOG_LIMIT, __LOG_MAX };
static const struct blobmsg_policy log_policy[__LOG_MAX] = {
	[LOG_LIMIT] = { .name = "limit", .type = BLOBMSG_TYPE_INT32 },
};

static int m_dns_log(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__LOG_MAX];
	unsigned limit = 50, i;
	void *arr, *t;

	blobmsg_parse(log_policy, __LOG_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[LOG_LIMIT])
		limit = blobmsg_get_u32(tb[LOG_LIMIT]);
	if (limit > dlog_count)
		limit = dlog_count;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "log");
	for (i = 0; i < limit; i++) {
		/* newest first */
		struct dnslog *e = &dlog[(dlog_head + DNSLOG_MAX - 1 - i) % DNSLOG_MAX];
		struct device *d = dev_find(e->client, 0);
		char tmp[16];

		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_u64(&b, "ts", (uint64_t)e->ts);
		blobmsg_add_string(&b, "client", e->client);
		blobmsg_add_string(&b, "hostname", d ? d->hostname : "");
		blobmsg_add_string(&b, "qname", e->qname);
		blobmsg_add_string(&b, "qtype", ag_dns_type_str(e->qtype, tmp, sizeof(tmp)));
		blobmsg_add_u32(&b, "rcode", e->rcode);
		blobmsg_add_u32(&b, "latency_us", e->latency_us);
		blobmsg_add_string(&b, "answer", e->answer);
		blobmsg_add_u32(&b, "answers", e->n_ans);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method ag_methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
};
static struct ubus_object_type ag_type = UBUS_OBJECT_TYPE("ag", ag_methods);
static struct ubus_object ag_obj = {
	.name = "ag", .type = &ag_type, .methods = ag_methods, .n_methods = ARRAY_SIZE(ag_methods),
};

static struct ubus_method dev_methods[] = {
	UBUS_METHOD_NOARG("list", m_device_list),
};
static struct ubus_object_type dev_type = UBUS_OBJECT_TYPE("ag.device", dev_methods);
static struct ubus_object dev_obj = {
	.name = "ag.device", .type = &dev_type, .methods = dev_methods, .n_methods = ARRAY_SIZE(dev_methods),
};

static struct ubus_method dns_methods[] = {
	UBUS_METHOD("log", m_dns_log, log_policy),
};
static struct ubus_object_type dns_type = UBUS_OBJECT_TYPE("ag.dns", dns_methods);
static struct ubus_object dns_obj = {
	.name = "ag.dns", .type = &dns_type, .methods = dns_methods, .n_methods = ARRAY_SIZE(dns_methods),
};

static void on_ubus_connect(struct ubus_context *ctx)
{
	if (ubus_add_object(ctx, &ag_obj) || ubus_add_object(ctx, &dev_obj) ||
	    ubus_add_object(ctx, &dns_obj))
		AG_ERR("failed to publish ubus objects");
	dns_ev.cb = on_dns_event;
	if (ubus_register_event_handler(ctx, &dns_ev, AG_EV_DNS))
		AG_ERR("failed to subscribe to %s", AG_EV_DNS);
	AG_INFO("connected to ubus, serving ag, ag.device, ag.dns");
}

int main(int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		printf("agd %s\n", AG_VERSION);
		return 0;
	}

	ag_config_load(&cfg);
	ag_log_init("agd", ag_log_level_parse(cfg.log_level));
	started = ag_wall_s();

	uloop_init();
	conn.path = ag_ubus_path();
	conn.cb = on_ubus_connect;
	ubus_auto_connect(&conn);

	poll_timer.cb = poll_cb;
	uloop_timeout_set(&poll_timer, 100);

	AG_INFO("agd %s started (host %s, leases %s)", AG_VERSION, cfg.host, cfg.leases);
	uloop_run();
	uloop_done();
	return 0;
}
