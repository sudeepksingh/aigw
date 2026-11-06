/* SPDX-License-Identifier: Apache-2.0 */
/*
 * agd: the AIGW brain.
 *
 * Phase 1 ("observe"): joins what every sensor reports into one picture of
 * the home -- devices (DHCP, ARP, Wi-Fi), what they look up (agdns), what
 * they connect to and how much they transfer (conntrack) -- keeps it across
 * restarts, and serves it over ubus:
 *
 *   ubus call ag status
 *   ubus call ag.device list
 *   ubus call ag.dns log '{"limit": 50}'
 *   ubus call ag.flow recent '{"limit": 50}'
 *   ubus listen ag.dns ag.flow ag.wifi
 *
 * Models, policies and actuators plug into this process in later phases.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "agd.h"
#include "ag/dns.h"

#define DNSLOG_MAX 512
#define POLL_MS    5000

struct dnslog {
	int64_t ts;
	char client[IP_LEN];
	char qname[NAME_LEN];
	char answer[IP_LEN];
	uint16_t qtype, rcode;
	uint32_t latency_us;
	uint8_t n_ans;
};

struct ag_config cfg;
int64_t started;

static struct dnslog dlog[DNSLOG_MAX];
static unsigned dlog_head, dlog_count;
static uint64_t dns_total;
static int64_t last_dns_event;

static struct ubus_auto_conn conn;
static struct ubus_event_handler dns_ev, dhcp_ev;
static struct uloop_timeout poll_timer, save_timer;
static struct blob_buf b;

void agd_emit(const char *type, struct blob_buf *msg)
{
	if (conn.ctx.sock.fd >= 0)
		ubus_send_event(&conn.ctx, type, msg->head);
}

/* ---------- sensor events ---------- */

enum { EV_CLIENT, EV_QNAME, EV_QTYPE, EV_RCODE, EV_LAT, EV_ANSWERS, EV_TTL, __EV_MAX };

static const struct blobmsg_policy ev_policy[__EV_MAX] = {
	[EV_CLIENT]  = { .name = "client", .type = BLOBMSG_TYPE_STRING },
	[EV_QNAME]   = { .name = "qname", .type = BLOBMSG_TYPE_STRING },
	[EV_QTYPE]   = { .name = "qtype", .type = BLOBMSG_TYPE_INT32 },
	[EV_RCODE]   = { .name = "rcode", .type = BLOBMSG_TYPE_INT32 },
	[EV_LAT]     = { .name = "latency_us", .type = BLOBMSG_TYPE_INT32 },
	[EV_ANSWERS] = { .name = "answers", .type = BLOBMSG_TYPE_ARRAY },
	[EV_TTL]     = { .name = "ttl", .type = BLOBMSG_TYPE_INT32 },
};

static void on_dns_event(struct ubus_context *ctx, struct ubus_event_handler *ev,
			 const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__EV_MAX], *cur;
	struct dnslog *e;
	const char *client, *qname;
	uint32_t ttl;
	size_t rem;

	blobmsg_parse(ev_policy, __EV_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[EV_CLIENT] || !tb[EV_QNAME])
		return;
	client = blobmsg_get_string(tb[EV_CLIENT]);
	qname = blobmsg_get_string(tb[EV_QNAME]);
	ttl = tb[EV_TTL] ? blobmsg_get_u32(tb[EV_TTL]) : 0;

	dns_total++;
	last_dns_event = ag_wall_s();

	if (!is_local(client)) {
		struct device *d = dev_find_ip(client, 1);

		d->last_seen = last_dns_event;
		d->queries++;
		top_add(d->doms, DEV_DOM_MAX, qname, 1);
	}

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
			/* remember which name each address belongs to, for the flow join */
			ep_add(blobmsg_get_string(cur), qname, client, ttl);
		}
	}
	dlog_head = (dlog_head + 1) % DNSLOG_MAX;
	if (dlog_count < DNSLOG_MAX)
		dlog_count++;
}

/* from aigw-dhcp-event (dnsmasq hook): richer identity than the leases file */
enum { DH_ACTION, DH_MAC, DH_IP, DH_HOST, DH_VC, DH_OPTS, __DH_MAX };

static const struct blobmsg_policy dhcp_policy[__DH_MAX] = {
	[DH_ACTION] = { .name = "action", .type = BLOBMSG_TYPE_STRING },
	[DH_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[DH_IP] = { .name = "ip", .type = BLOBMSG_TYPE_STRING },
	[DH_HOST] = { .name = "hostname", .type = BLOBMSG_TYPE_STRING },
	[DH_VC] = { .name = "vendor_class", .type = BLOBMSG_TYPE_STRING },
	[DH_OPTS] = { .name = "options", .type = BLOBMSG_TYPE_STRING },
};

static void on_dhcp_event(struct ubus_context *ctx, struct ubus_event_handler *ev,
			  const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__DH_MAX];
	struct device *d;

	blobmsg_parse(dhcp_policy, __DH_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[DH_MAC] || !tb[DH_IP] || is_local(blobmsg_get_string(tb[DH_IP])))
		return;
	d = dev_learn(blobmsg_get_string(tb[DH_IP]), blobmsg_get_string(tb[DH_MAC]),
		      tb[DH_HOST] ? blobmsg_get_string(tb[DH_HOST]) : NULL);
	if (!d)
		return;
	if (tb[DH_VC] && *blobmsg_get_string(tb[DH_VC]))
		snprintf(d->vendor_class, sizeof(d->vendor_class), "%s", blobmsg_get_string(tb[DH_VC]));
	if (tb[DH_OPTS] && *blobmsg_get_string(tb[DH_OPTS]))
		snprintf(d->dhcp_options, sizeof(d->dhcp_options), "%s", blobmsg_get_string(tb[DH_OPTS]));
	d->last_seen = ag_wall_s();
	dev_guess_type(d);
}

/* ---------- ubus API ---------- */

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	void *s;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", AG_VERSION);
	blobmsg_add_string(&b, "host", cfg.host);
	blobmsg_add_string(&b, "phase", "observe");
	blobmsg_add_u64(&b, "uptime_s", (uint64_t)(ag_wall_s() - started));
	blobmsg_add_u32(&b, "devices", (uint32_t)dev_count());
	blobmsg_add_u32(&b, "wifi_stations", (uint32_t)wifi_station_count());
	blobmsg_add_u64(&b, "dns_queries", dns_total);
	blobmsg_add_u64(&b, "dns_last_event", (uint64_t)last_dns_event);
	blobmsg_add_u64(&b, "flows", flows_total);
	blobmsg_add_u64(&b, "flows_no_dns", flows_no_dns_total);
	blobmsg_add_u32(&b, "endpoints", (uint32_t)ep_count());
	s = blobmsg_open_table(&b, "sensors");
	blobmsg_add_u8(&b, "dns", last_dns_event ? 1 : 0);
	blobmsg_add_u8(&b, "flow", flow_active() ? 1 : 0);
	blobmsg_add_u8(&b, "wifi", wifi_station_count() > 0 ? 1 : 0);
	blobmsg_close_table(&b, s);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int m_device_list(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	void *arr, *t;
	int i;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "devices");
	for (i = 0; i < DEV_MAX; i++) {
		if (!devs[i].used)
			continue;
		t = blobmsg_open_table(&b, NULL);
		dev_blob(&b, &devs[i]);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

enum { LIM_LIMIT, __LIM_MAX };
static const struct blobmsg_policy limit_policy[__LIM_MAX] = {
	[LIM_LIMIT] = { .name = "limit", .type = BLOBMSG_TYPE_INT32 },
};

static unsigned get_limit(struct blob_attr *msg, unsigned dflt)
{
	struct blob_attr *tb[__LIM_MAX];

	blobmsg_parse(limit_policy, __LIM_MAX, tb, blob_data(msg), blob_len(msg));
	return tb[LIM_LIMIT] ? blobmsg_get_u32(tb[LIM_LIMIT]) : dflt;
}

static int m_dns_log(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	unsigned limit = get_limit(msg, 50), i;
	void *arr, *t;

	if (limit > dlog_count)
		limit = dlog_count;
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "log");
	for (i = 0; i < limit; i++) {
		struct dnslog *e = &dlog[(dlog_head + DNSLOG_MAX - 1 - i) % DNSLOG_MAX];
		struct device *d = dev_find_ip(e->client, 0);
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

static int m_flow_recent(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	flow_blob_recent(&b, get_limit(msg, 50));
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
	UBUS_METHOD("log", m_dns_log, limit_policy),
};
static struct ubus_object_type dns_type = UBUS_OBJECT_TYPE("ag.dns", dns_methods);
static struct ubus_object dns_obj = {
	.name = "ag.dns", .type = &dns_type, .methods = dns_methods, .n_methods = ARRAY_SIZE(dns_methods),
};

static struct ubus_method flow_methods[] = {
	UBUS_METHOD("recent", m_flow_recent, limit_policy),
};
static struct ubus_object_type flow_type = UBUS_OBJECT_TYPE("ag.flow", flow_methods);
static struct ubus_object flow_obj = {
	.name = "ag.flow", .type = &flow_type, .methods = flow_methods, .n_methods = ARRAY_SIZE(flow_methods),
};

static void on_ubus_connect(struct ubus_context *ctx)
{
	if (ubus_add_object(ctx, &ag_obj) || ubus_add_object(ctx, &dev_obj) ||
	    ubus_add_object(ctx, &dns_obj) || ubus_add_object(ctx, &flow_obj))
		AG_ERR("failed to publish ubus objects");
	dns_ev.cb = on_dns_event;
	dhcp_ev.cb = on_dhcp_event;
	if (ubus_register_event_handler(ctx, &dns_ev, AG_EV_DNS) ||
	    ubus_register_event_handler(ctx, &dhcp_ev, AG_EV_DHCP))
		AG_ERR("failed to subscribe to sensor events");
	AG_INFO("connected to ubus, serving ag, ag.device, ag.dns, ag.flow");
}

/* ---------- timers ---------- */

static void poll_cb(struct uloop_timeout *t)
{
	int i;

	local_addrs_refresh();
	for (i = 0; i < DEV_MAX; i++)       /* the gateway is not a device */
		if (devs[i].used && devs[i].ip[0] && is_local(devs[i].ip))
			devs[i].used = 0;
	dev_poll_leases();
	dev_poll_arp();
	wifi_poll();
	uloop_timeout_set(t, POLL_MS);
}

static void save_cb(struct uloop_timeout *t)
{
	dev_state_save();
	uloop_timeout_set(t, cfg.state_interval * 1000);
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
	local_addrs_refresh();
	dev_state_load();
	flow_init();
	wifi_init();

	conn.path = ag_ubus_path();
	conn.cb = on_ubus_connect;
	conn.ctx.sock.fd = -1;
	ubus_auto_connect(&conn);

	poll_timer.cb = poll_cb;
	uloop_timeout_set(&poll_timer, 100);
	save_timer.cb = save_cb;
	uloop_timeout_set(&save_timer, cfg.state_interval * 1000);

	AG_INFO("agd %s started (host %s)", AG_VERSION, cfg.host);
	uloop_run();     /* returns on SIGTERM / SIGINT */
	dev_state_save();
	AG_INFO("stopped, state saved");
	uloop_done();
	return 0;
}
