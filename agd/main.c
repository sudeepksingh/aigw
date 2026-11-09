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
 * Phase 2 ("understand") adds the on-device model, baselines and alerts
 * (ag.alert, ag.domain); phase 3 ("act") turns alerts into decisions with
 * guardrails, time limits and undo (ag.decision, see act.c); phase 4 ("plug")
 * hosts plug-ins and per-device Wi-Fi keys (ag.plugin, ag.wifi); phase 5
 * ("outside") takes signed model updates and hot-loads them (ag.model, see
 * models.c), which the bridge plug-in fetches from cortex.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "agd.h"
#include "ag/dns.h"
#include "ag/domfeat.h"
#include "ag/qr.h"

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

void agd_emit_from(const char *type, struct blob_buf *msg, const void *from)
{
	if (conn.ctx.sock.fd >= 0)
		ubus_send_event(&conn.ctx, type, msg->head);
	plugin_dispatch(type, msg->head, from);
}

void agd_emit(const char *type, struct blob_buf *msg)
{
	agd_emit_from(type, msg, NULL);
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
		baseline_domain(d, qname);
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

	/* DGA malware mostly gets NXDOMAIN, so every answer is judged */
	alerts_on_dns(client, qname);
	plugin_dispatch(AG_EV_DNS, msg, NULL);
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
	plugin_dispatch(AG_EV_DHCP, msg, NULL);
}

/* a QR code as rows of '1' (dark) and '0', for screens that draw it (LuCI) */
void agd_qr_rows(struct blob_buf *bb, const char *field, const char *text)
{
	struct ag_qr q;
	char row[AG_QR_MAX_SIZE + 1];
	void *arr;
	int x, y;

	if (ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_M) < 0 &&
	    ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_L) < 0)
		return;
	arr = blobmsg_open_array(bb, field);
	for (y = 0; y < q.size; y++) {
		for (x = 0; x < q.size; x++)
			row[x] = q.m[y][x] ? '1' : '0';
		row[q.size] = '\0';
		blobmsg_add_string(bb, NULL, row);
	}
	blobmsg_close_array(bb, arr);
}

/* ---------- ubus API ---------- */

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	void *s;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", AG_VERSION);
	blobmsg_add_string(&b, "host", cfg.host);
	blobmsg_add_string(&b, "phase", "advanced");
	blobmsg_add_u64(&b, "uptime_s", (uint64_t)(ag_wall_s() - started));
	blobmsg_add_u32(&b, "devices", (uint32_t)dev_count());
	blobmsg_add_u32(&b, "wifi_stations", (uint32_t)wifi_station_count());
	blobmsg_add_u32(&b, "home", (uint32_t)presence_count());
	blobmsg_add_u32(&b, "radios", (uint32_t)wifi_n_aps);
	blobmsg_add_u64(&b, "steered", steer_ok_total);
	blobmsg_add_u64(&b, "dns_queries", dns_total);
	blobmsg_add_u64(&b, "dns_last_event", (uint64_t)last_dns_event);
	blobmsg_add_u64(&b, "flows", flows_total);
	blobmsg_add_u64(&b, "flows_no_dns", flows_no_dns_total);
	blobmsg_add_u64(&b, "flows_sni", flows_sni_total);
	blobmsg_add_u32(&b, "endpoints", (uint32_t)ep_count());
	blobmsg_add_u64(&b, "alerts", alerts_total());
	blobmsg_add_string(&b, "mode", ag_mode_str(cfg.act_mode));
	s = blobmsg_open_table(&b, "decisions");
	blobmsg_add_u32(&b, "waiting", (uint32_t)act_count(0));
	blobmsg_add_u32(&b, "active", (uint32_t)act_count(1));
	blobmsg_add_u64(&b, "applied", act_applied_total());
	blobmsg_add_u32(&b, "policies", (uint32_t)cfg.n_policies);
	blobmsg_close_table(&b, s);
	s = blobmsg_open_table(&b, "plugins");
	blobmsg_add_u32(&b, "found", (uint32_t)plugin_count(0));
	blobmsg_add_u32(&b, "working", (uint32_t)plugin_count(1));
	blobmsg_close_table(&b, s);
	blobmsg_add_u32(&b, "wifi_keys", (uint32_t)keys_count());
	s = blobmsg_open_table(&b, "remote");
	blobmsg_add_u8(&b, "enabled", (uint8_t)cfg.remote_enabled);
	blobmsg_add_u32(&b, "peers", (uint32_t)remote_count(0));
	blobmsg_add_u32(&b, "online", (uint32_t)remote_count(1));
	blobmsg_close_table(&b, s);
	s = blobmsg_open_table(&b, "models");
	if (alerts_model_trees())
		blobmsg_add_u32(&b, alerts_model_name(), (uint32_t)alerts_model_trees());
	blobmsg_close_table(&b, s);
	s = blobmsg_open_table(&b, "model_versions");
	if (alerts_model_trees())
		blobmsg_add_u32(&b, alerts_model_name(), (uint32_t)models_version(alerts_model_name()));
	blobmsg_close_table(&b, s);
	s = blobmsg_open_table(&b, "sensors");
	blobmsg_add_u8(&b, "dns", last_dns_event ? 1 : 0);
	blobmsg_add_u8(&b, "flow", flow_active() ? 1 : 0);
	blobmsg_add_u8(&b, "sni", sni_active() ? 1 : 0);
	blobmsg_add_u8(&b, "wifi", wifi_station_count() > 0 ? 1 : 0);
	blobmsg_close_table(&b, s);
	sni_blob(&b);
	fastpath_blob(&b);
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
		dev_blob(&b, &devs[i], 0);
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

static int m_alert_list(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	alerts_blob(&b, get_limit(msg, 50), "alerts");
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

enum { SC_NAME, __SC_MAX };
static const struct blobmsg_policy score_policy[__SC_MAX] = {
	[SC_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
};

/* Explain the domain-risk model's view of one name. */
static int m_domain_score(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	static const char *const names[AG_DOMFEAT_N] = {
		"length", "entropy", "digit_ratio", "vowel_ratio", "max_consonant_run", "hyphens",
		"labels", "bigram_mean", "bigram_min", "digit_letter_switches", "unique_ratio", "hex_like",
	};
	struct blob_attr *tb[__SC_MAX];
	float x[AG_DOMFEAT_N];
	char label[256] = "";
	const char *name;
	double s;
	void *t;
	int i;

	blobmsg_parse(score_policy, __SC_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[SC_NAME])
		return UBUS_STATUS_INVALID_ARGUMENT;
	name = blobmsg_get_string(tb[SC_NAME]);
	s = alerts_domain_score(name, label, sizeof(label), x);
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "name", name);
	blobmsg_add_string(&b, "label", label);
	if (s < 0) {
		blobmsg_add_string(&b, "verdict", alerts_model_trees() ? "not judged" : "model not loaded");
	} else {
		float tx[AG_DOMFEAT_N];
		char tl[256];
		double ts = alerts_tree_score(name, tl, sizeof(tl), tx);

		blobmsg_add_double(&b, "score", s);
		if (ts != s)                     /* the shared layer changed it */
			blobmsg_add_double(&b, "trees", ts);
		blobmsg_add_double(&b, "threshold", cfg.dga_threshold);
		blobmsg_add_string(&b, "verdict",
				   s >= cfg.dga_threshold && (int)strlen(label) >= cfg.dga_min_label ?
				   "looks machine-generated" : "looks normal");
		t = blobmsg_open_table(&b, "features");
		for (i = 0; i < AG_DOMFEAT_N; i++)
			blobmsg_add_double(&b, names[i], x[i]);
		blobmsg_close_table(&b, t);
	}
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

/* ---------- decisions ---------- */

static int m_decision_list(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "mode", ag_mode_str(cfg.act_mode));
	act_blob(&b, get_limit(msg, 50), "decisions");
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

enum { DR_ID, DR_ACTION, DR_TARGET, DR_TTL, DR_REASON, DR_RATE, DR_DEVICE, __DR_MAX };
static const struct blobmsg_policy dec_policy[__DR_MAX] = {
	[DR_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[DR_ACTION] = { .name = "action", .type = BLOBMSG_TYPE_STRING },
	[DR_TARGET] = { .name = "target", .type = BLOBMSG_TYPE_STRING },
	[DR_TTL] = { .name = "ttl", .type = BLOBMSG_TYPE_INT32 },
	[DR_REASON] = { .name = "reason", .type = BLOBMSG_TYPE_STRING },
	[DR_RATE] = { .name = "rate", .type = BLOBMSG_TYPE_INT32 },        /* throttle, kbit/s */
	[DR_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },   /* block for one device */
};

/* request / approve / reject / revert: reply {ok, id, error} so callers can
 * show the guardrail's reason instead of a bare ubus error code. */
static int m_decision_change(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__DR_MAX];
	char err[256] = "";
	uint32_t id = 0;
	int ttl, rc;

	blobmsg_parse(dec_policy, __DR_MAX, tb, blob_data(msg), blob_len(msg));
	ttl = tb[DR_TTL] ? (int)blobmsg_get_u32(tb[DR_TTL]) : 0;
	if (tb[DR_ID])
		id = blobmsg_get_u32(tb[DR_ID]);
	if (!strcmp(method, "request")) {
		struct act_opts o = { 0 };

		if (!tb[DR_ACTION] || !tb[DR_TARGET])
			return UBUS_STATUS_INVALID_ARGUMENT;
		o.rate_kbps = tb[DR_RATE] ? (int)blobmsg_get_u32(tb[DR_RATE]) : 0;
		o.device = tb[DR_DEVICE] ? blobmsg_get_string(tb[DR_DEVICE]) : NULL;
		rc = act_request(blobmsg_get_string(tb[DR_ACTION]), blobmsg_get_string(tb[DR_TARGET]), ttl, &o,
				 tb[DR_REASON] ? blobmsg_get_string(tb[DR_REASON]) : NULL, &id, err, sizeof(err));
	} else {
		if (!tb[DR_ID])
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (!strcmp(method, "approve"))
			rc = act_approve(id, ttl, err, sizeof(err));
		else if (!strcmp(method, "reject"))
			rc = act_reject(id, err, sizeof(err));
		else
			rc = act_revert(id, err, sizeof(err));
	}
	if (rc == 0)
		dev_state_save();   /* decisions are worth keeping right away */
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_u32(&b, "id", id);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method decision_methods[] = {
	UBUS_METHOD("list", m_decision_list, limit_policy),
	UBUS_METHOD("request", m_decision_change, dec_policy),
	UBUS_METHOD("approve", m_decision_change, dec_policy),
	UBUS_METHOD("reject", m_decision_change, dec_policy),
	UBUS_METHOD("revert", m_decision_change, dec_policy),
};
static struct ubus_object_type decision_type = UBUS_OBJECT_TYPE("ag.decision", decision_methods);
static struct ubus_object decision_obj = {
	.name = "ag.decision", .type = &decision_type, .methods = decision_methods,
	.n_methods = ARRAY_SIZE(decision_methods),
};

/* ---------- plug-ins ---------- */

enum { PL_NAME, PL_ALLOW, PL_ENABLED, PL_KEY, PL_VALUE, __PL_MAX };
static const struct blobmsg_policy plugin_policy[__PL_MAX] = {
	[PL_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[PL_ALLOW] = { .name = "allow", .type = BLOBMSG_TYPE_BOOL },
	[PL_ENABLED] = { .name = "enabled", .type = BLOBMSG_TYPE_BOOL },
	[PL_KEY] = { .name = "key", .type = BLOBMSG_TYPE_STRING },
	[PL_VALUE] = { .name = "value", .type = BLOBMSG_TYPE_STRING },
};

static int m_plugin_list(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__PL_MAX];

	blobmsg_parse(plugin_policy, __PL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!strcmp(method, "rescan"))
		plugin_rescan();
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "dir", cfg.plugin_dir);
	blobmsg_add_string(&b, "user_dir", cfg.plugin_user_dir);
	blobmsg_add_u8(&b, "enabled", (uint8_t)cfg.plugins_enabled);
	plugin_blob(&b, "plugins", tb[PL_NAME] ? blobmsg_get_string(tb[PL_NAME]) : NULL);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int m_plugin_set(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__PL_MAX];
	char err[256] = "";
	int rc = 0;

	blobmsg_parse(plugin_policy, __PL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[PL_NAME] || (!tb[PL_ALLOW] && !tb[PL_ENABLED]))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[PL_ALLOW])
		rc = plugin_set(blobmsg_get_string(tb[PL_NAME]), "allow", blobmsg_get_bool(tb[PL_ALLOW]), err, sizeof(err));
	if (!rc && tb[PL_ENABLED])
		rc = plugin_set(blobmsg_get_string(tb[PL_NAME]), "enabled", blobmsg_get_bool(tb[PL_ENABLED]), err, sizeof(err));
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

/* a plug-in's setting (its "config plugin" section); the plug-in restarts with it */
static int m_plugin_config(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__PL_MAX];
	char err[256] = "";
	int rc;

	blobmsg_parse(plugin_policy, __PL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[PL_NAME] || !tb[PL_KEY] || !tb[PL_VALUE])
		return UBUS_STATUS_INVALID_ARGUMENT;
	rc = plugin_config(blobmsg_get_string(tb[PL_NAME]), blobmsg_get_string(tb[PL_KEY]),
			   blobmsg_get_string(tb[PL_VALUE]), err, sizeof(err));
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method plugin_methods[] = {
	UBUS_METHOD("list", m_plugin_list, plugin_policy),
	UBUS_METHOD("rescan", m_plugin_list, plugin_policy),
	UBUS_METHOD("set", m_plugin_set, plugin_policy),
	UBUS_METHOD("config", m_plugin_config, plugin_policy),
};
static struct ubus_object_type plugin_type = UBUS_OBJECT_TYPE("ag.plugin", plugin_methods);
static struct ubus_object plugin_obj = {
	.name = "ag.plugin", .type = &plugin_type, .methods = plugin_methods, .n_methods = ARRAY_SIZE(plugin_methods),
};

/* ---------- models: signed updates, rollback ---------- */

enum { MD_NAME, MD_FILE, MD_SIG, MD_FORCE, __MD_MAX };
static const struct blobmsg_policy model_policy[__MD_MAX] = {
	[MD_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[MD_FILE] = { .name = "file", .type = BLOBMSG_TYPE_STRING },
	[MD_SIG] = { .name = "sig", .type = BLOBMSG_TYPE_STRING },
	[MD_FORCE] = { .name = "force", .type = BLOBMSG_TYPE_BOOL },
};

static int m_model_list(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "dir", cfg.models_dir);
	blobmsg_add_string(&b, "package_dir", cfg.model_dir);
	blobmsg_add_string(&b, "keys_dir", cfg.trusted_keys);
	blobmsg_add_u8(&b, "require_signature", (uint8_t)cfg.models_require_sig);
	models_blob(&b, "models");
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int m_model_change(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__MD_MAX];
	char err[256] = "";
	int rc, version = 0;

	blobmsg_parse(model_policy, __MD_MAX, tb, blob_data(msg), blob_len(msg));
	if (!strcmp(method, "install")) {
		if (!tb[MD_FILE])
			return UBUS_STATUS_INVALID_ARGUMENT;
		rc = models_offer(blobmsg_get_string(tb[MD_FILE]), tb[MD_SIG] ? blobmsg_get_string(tb[MD_SIG]) : NULL,
				  "you", tb[MD_FORCE] && blobmsg_get_bool(tb[MD_FORCE]), &version, err, sizeof(err));
	} else {
		rc = models_rollback(tb[MD_NAME] ? blobmsg_get_string(tb[MD_NAME]) : NULL, err, sizeof(err));
	}
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", rc == 0);
	if (version)
		blobmsg_add_u32(&b, "version", (uint32_t)version);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method model_methods[] = {
	UBUS_METHOD_NOARG("list", m_model_list),
	UBUS_METHOD("install", m_model_change, model_policy),
	UBUS_METHOD("rollback", m_model_change, model_policy),
};
static struct ubus_object_type model_type = UBUS_OBJECT_TYPE("ag.model", model_methods);
static struct ubus_object model_obj = {
	.name = "ag.model", .type = &model_type, .methods = model_methods, .n_methods = ARRAY_SIZE(model_methods),
};

/* ---------- federated learning: your examples, rounds, privacy spent ---------- */

enum { FD_NAME, FD_LABEL, __FD_MAX };
static const struct blobmsg_policy fed_policy[__FD_MAX] = {
	[FD_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[FD_LABEL] = { .name = "label", .type = BLOBMSG_TYPE_STRING },     /* safe | threat */
};

static int m_fed(struct ubus_context *ctx, struct ubus_object *obj,
		 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__FD_MAX];
	char err[200] = "";
	int rc = 0;

	blobmsg_parse(fed_policy, __FD_MAX, tb, blob_data(msg), blob_len(msg));
	blob_buf_init(&b, 0);
	if (!strcmp(method, "status")) {
		fed_blob(&b, 1);
		ubus_send_reply(ctx, req, b.head);
		return 0;
	}
	if (!strcmp(method, "label")) {
		const char *l = tb[FD_LABEL] ? blobmsg_get_string(tb[FD_LABEL]) : "";

		if (!tb[FD_NAME] || (strcmp(l, "safe") && strcmp(l, "threat")))
			return UBUS_STATUS_INVALID_ARGUMENT;
		rc = fed_label(blobmsg_get_string(tb[FD_NAME]), !strcmp(l, "threat"), "you", err, sizeof(err));
	} else {
		blobmsg_add_u32(&b, "forgotten", (uint32_t)fed_forget(tb[FD_NAME] ? blobmsg_get_string(tb[FD_NAME]) : NULL));
	}
	if (rc == 0)
		dev_state_save();
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method fed_methods[] = {
	UBUS_METHOD_NOARG("status", m_fed),
	UBUS_METHOD("label", m_fed, fed_policy),
	UBUS_METHOD("forget", m_fed, fed_policy),
};
static struct ubus_object_type fed_type = UBUS_OBJECT_TYPE("ag.fed", fed_methods);
static struct ubus_object fed_obj = {
	.name = "ag.fed", .type = &fed_type, .methods = fed_methods, .n_methods = ARRAY_SIZE(fed_methods),
};

/* ---------- remote access ---------- */

enum { RM_NAME, RM_ENDPOINT, __RM_MAX };
static const struct blobmsg_policy remote_policy[__RM_MAX] = {
	[RM_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[RM_ENDPOINT] = { .name = "endpoint", .type = BLOBMSG_TYPE_STRING },
};

static int m_remote_list(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	remote_blob(&b);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int m_remote_change(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__RM_MAX];
	char err[256] = "";
	int rc;

	blobmsg_parse(remote_policy, __RM_MAX, tb, blob_data(msg), blob_len(msg));
	blob_buf_init(&b, 0);
	if (!strcmp(method, "on") || !strcmp(method, "off")) {
		rc = remote_enable(method[1] == 'n', tb[RM_ENDPOINT] ? blobmsg_get_string(tb[RM_ENDPOINT]) : NULL,
				   err, sizeof(err));
	} else {
		if (!tb[RM_NAME])
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (!strcmp(method, "add"))
			rc = remote_add(blobmsg_get_string(tb[RM_NAME]), &b, err, sizeof(err));
		else
			rc = remote_revoke(blobmsg_get_string(tb[RM_NAME]), err, sizeof(err));
	}
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method remote_methods[] = {
	UBUS_METHOD_NOARG("list", m_remote_list),
	UBUS_METHOD("on", m_remote_change, remote_policy),
	UBUS_METHOD("off", m_remote_change, remote_policy),
	UBUS_METHOD("add", m_remote_change, remote_policy),
	UBUS_METHOD("revoke", m_remote_change, remote_policy),
};
static struct ubus_object_type remote_type = UBUS_OBJECT_TYPE("ag.remote", remote_methods);
static struct ubus_object remote_obj = {
	.name = "ag.remote", .type = &remote_type, .methods = remote_methods, .n_methods = ARRAY_SIZE(remote_methods),
};

/* ---------- per-device Wi-Fi keys ---------- */

enum { WK_NAME, WK_ZONE, WK_TTL, __WK_MAX };
static const struct blobmsg_policy key_policy[__WK_MAX] = {
	[WK_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[WK_ZONE] = { .name = "zone", .type = BLOBMSG_TYPE_STRING },
	[WK_TTL] = { .name = "ttl", .type = BLOBMSG_TYPE_INT32 },
};

static int m_wifi_keys(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__WK_MAX];

	blobmsg_parse(key_policy, __WK_MAX, tb, blob_data(msg), blob_len(msg));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "file", cfg.wifi_keys);
	keys_blob(&b, tb[WK_NAME] ? blobmsg_get_string(tb[WK_NAME]) : NULL);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static int m_wifi_key_change(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__WK_MAX];
	char err[256] = "";
	int rc;

	blobmsg_parse(key_policy, __WK_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[WK_NAME])
		return UBUS_STATUS_INVALID_ARGUMENT;
	blob_buf_init(&b, 0);
	if (!strcmp(method, "key_add"))
		rc = keys_add(blobmsg_get_string(tb[WK_NAME]), tb[WK_ZONE] ? blobmsg_get_string(tb[WK_ZONE]) : NULL,
			      tb[WK_TTL] ? (int)blobmsg_get_u32(tb[WK_TTL]) : 0, &b, err, sizeof(err));
	else
		rc = keys_revoke(blobmsg_get_string(tb[WK_NAME]), err, sizeof(err));
	blobmsg_add_u8(&b, "ok", rc == 0);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

/* ubus ag.wifi radios: access points, their stations, band steering */
static int m_wifi_radios(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	void *arr, *t, *sa, *st;
	int i, j;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "radios");
	for (i = 0; i < wifi_n_aps; i++) {
		const struct wifi_ap *a = &wifi_aps[i];

		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "iface", a->iface);
		blobmsg_add_string(&b, "ssid", a->ssid);
		blobmsg_add_string(&b, "bssid", a->bssid);
		blobmsg_add_string(&b, "band", a->band == 5 ? "5" : a->band == 2 ? "2.4" : "");
		blobmsg_add_u32(&b, "channel", (uint32_t)a->channel);
		blobmsg_add_u32(&b, "freq", (uint32_t)a->freq);
		blobmsg_add_u32(&b, "width", (uint32_t)a->width);
		chan_blob(&b, a->iface);
		sa = blobmsg_open_array(&b, "stations");
		for (j = 0; j < DEV_MAX; j++) {
			const struct device *d = &devs[j];

			if (!d->used || !d->wifi_connected || strcmp(d->wifi_iface, a->iface))
				continue;
			st = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "mac", d->mac);
			blobmsg_add_string(&b, "ip", d->ip);
			blobmsg_add_string(&b, "hostname", d->hostname);
			blobmsg_add_u32(&b, "signal", (uint32_t)d->wifi_signal);
			blobmsg_add_u32(&b, "signal_avg", (uint32_t)(int)d->wifi_sig_avg);
			blobmsg_add_u32(&b, "rate_kbps", d->wifi_rate_kbps);
			blobmsg_add_string(&b, "near", presence_near(d));
			blobmsg_add_u8(&b, "dual_band", d->wifi_dual);
			blobmsg_add_u8(&b, "bss_transition", d->wifi_btm);
			blobmsg_add_u32(&b, "steered", d->steer_moves);
			blobmsg_add_u32(&b, "steer_missed", d->steer_misses);
			blobmsg_close_table(&b, st);
		}
		blobmsg_close_array(&b, sa);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	steer_blob(&b);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method wifi_methods[] = {
	UBUS_METHOD_NOARG("radios", m_wifi_radios),
	UBUS_METHOD("keys", m_wifi_keys, key_policy),
	UBUS_METHOD("key_add", m_wifi_key_change, key_policy),
	UBUS_METHOD("key_revoke", m_wifi_key_change, key_policy),
};
static struct ubus_object_type wifi_type = UBUS_OBJECT_TYPE("ag.wifi", wifi_methods);
static struct ubus_object wifi_obj = {
	.name = "ag.wifi", .type = &wifi_type, .methods = wifi_methods, .n_methods = ARRAY_SIZE(wifi_methods),
};

/* ---------- device zone ---------- */

enum { DZ_DEVICE, DZ_ZONE, __DZ_MAX };
static const struct blobmsg_policy zone_policy[__DZ_MAX] = {
	[DZ_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },
	[DZ_ZONE] = { .name = "zone", .type = BLOBMSG_TYPE_STRING },
};

static int m_device_zone(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__DZ_MAX];
	struct device *d;
	const char *z;
	char err[128] = "";
	size_t i;

	blobmsg_parse(zone_policy, __DZ_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[DZ_DEVICE] || !tb[DZ_ZONE])
		return UBUS_STATUS_INVALID_ARGUMENT;
	z = blobmsg_get_string(tb[DZ_ZONE]);
	d = act_find_device(blobmsg_get_string(tb[DZ_DEVICE]));
	for (i = 0; z[i] && !err[0]; i++)
		if (!((z[i] >= 'a' && z[i] <= 'z') || (z[i] >= '0' && z[i] <= '9') || z[i] == '-') || i >= 15)
			snprintf(err, sizeof(err), "zone must be up to 15 of a-z, 0-9 and - ('' for none)");
	if (!err[0] && !d)
		snprintf(err, sizeof(err), "no device '%s'", blobmsg_get_string(tb[DZ_DEVICE]));
	if (!err[0]) {
		snprintf(d->zone, sizeof(d->zone), "%s", z);
		dev_state_save();
	}
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", !err[0]);
	blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static struct ubus_method alert_methods[] = {
	UBUS_METHOD("list", m_alert_list, limit_policy),
};
static struct ubus_object_type alert_type = UBUS_OBJECT_TYPE("ag.alert", alert_methods);
static struct ubus_object alert_obj = {
	.name = "ag.alert", .type = &alert_type, .methods = alert_methods, .n_methods = ARRAY_SIZE(alert_methods),
};

static struct ubus_method domain_methods[] = {
	UBUS_METHOD("score", m_domain_score, score_policy),
};
static struct ubus_object_type domain_type = UBUS_OBJECT_TYPE("ag.domain", domain_methods);
static struct ubus_object domain_obj = {
	.name = "ag.domain", .type = &domain_type, .methods = domain_methods, .n_methods = ARRAY_SIZE(domain_methods),
};

static struct ubus_method ag_methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
};
static struct ubus_object_type ag_type = UBUS_OBJECT_TYPE("ag", ag_methods);
static struct ubus_object ag_obj = {
	.name = "ag", .type = &ag_type, .methods = ag_methods, .n_methods = ARRAY_SIZE(ag_methods),
};

static struct ubus_method dev_methods[] = {
	UBUS_METHOD_NOARG("list", m_device_list),
	UBUS_METHOD("zone", m_device_zone, zone_policy),
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
	    ubus_add_object(ctx, &dns_obj) || ubus_add_object(ctx, &flow_obj) ||
	    ubus_add_object(ctx, &alert_obj) || ubus_add_object(ctx, &domain_obj) ||
	    ubus_add_object(ctx, &decision_obj) || ubus_add_object(ctx, &plugin_obj) ||
	    ubus_add_object(ctx, &wifi_obj) || ubus_add_object(ctx, &model_obj) || ubus_add_object(ctx, &remote_obj) ||
	    ubus_add_object(ctx, &fed_obj))
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
	presence_tick();
	fastpath_tick();
	baseline_tick();
	act_tick();
	keys_tick();
	remote_tick();
	plugin_tick();
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
	signal(SIGPIPE, SIG_IGN);       /* a plug-in that went away must not take the brain with it */
	local_addrs_refresh();
	alerts_init();
	models_init();
	dev_state_load();
	act_init();
	remote_init();                  /* before the flow sensor: tunnel clients are part of the home */
	flow_init();
	sni_init();                     /* after the flow sensor: it names its connections */
	wifi_init();
	keys_init();
	plugin_init();

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
	plugin_stop_all();
	dev_state_save();
	AG_INFO("stopped, state saved");
	uloop_done();
	return 0;
}
