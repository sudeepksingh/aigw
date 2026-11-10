/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The data model: one place where AIGW OS is described in TR-181 terms, for
 * every management protocol. The USP (TR-369) and TR-069 adapters only
 * translate; the paths, values, instance numbers, write rules and events are
 * here, once.
 *
 *   ubus ag.dm
 *     schema                      every parameter, object, command and event
 *     instances {path}            the object instances under a path
 *     get {paths, origin}         values; a path ending in '.' means everything under it
 *     set {params, origin}        all or nothing
 *     operate {command, args, origin}
 *   event ag.dm.event {event, args}   e.g. Device.X_AIGW_.Alert!
 *
 * Adding a parameter is one line in params[] and a getter (see
 * docs/device-management.md). At most one {i} per path in this version.
 *
 * Who asks: "origin" is "local" (agctl) unless an adapter says otherwise
 * ("usp", "acs"). A management system may always read; it may approve, reject
 * and undo decisions; quarantining, blocking and changing the mode need
 * mgmt.actions 1; alert subjects and reasons (names devices looked up) need
 * mgmt.alert_details 1. Actions go through act_request, so every guardrail holds.
 *
 * Instance numbers: devices and access points are numbered from their MAC or
 * interface, the numbers saved with the state and never reused; alerts and
 * decisions use their own ids.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubus.h>

#include "agd.h"

#define DM_VERSION "1.0"
#define LIST_MAX   512
#define IMAP_MAX   1024
#define VAL_MAX    700

enum { T_STR, T_BOOL, T_UINT, T_INT, T_ULONG, T_TIME, T_DEC };
static const char *const type_names[] = {
	"string", "boolean", "unsignedInt", "int", "unsignedLong", "dateTime", "decimal",
};

enum { E_OK, E_NOT_FOUND, E_READ_ONLY, E_INVALID_VALUE, E_REFUSED, E_INTERNAL, E_INVALID_ARGS };
static const char *const err_names[] = {
	"ok", "not_found", "read_only", "invalid_value", "refused", "internal", "invalid_arguments",
};

/* ---------- instance numbers ---------- */

struct imap {
	int n;
	uint32_t next;
	struct { char key[40]; uint32_t num; } e[IMAP_MAX];
};
static struct imap hosts_map, ssid_map;

static uint32_t imap_find(const struct imap *m, const char *key)
{
	int i;

	for (i = 0; i < m->n; i++)
		if (!strcasecmp(m->e[i].key, key))
			return m->e[i].num;
	return 0;
}

static uint32_t imap_num(struct imap *m, const char *key)
{
	uint32_t num = imap_find(m, key);
	int i, low = 0;

	if (num)
		return num;
	if (m->n >= IMAP_MAX) {           /* full: forget the oldest number (it stays unused) */
		for (i = 1; i < m->n; i++)
			if (m->e[i].num < m->e[low].num)
				low = i;
		m->e[low] = m->e[--m->n];
	}
	snprintf(m->e[m->n].key, sizeof(m->e[m->n].key), "%s", key);
	m->e[m->n].num = ++m->next;
	return m->e[m->n++].num;
}

static const char *imap_key(const struct imap *m, uint32_t num)
{
	int i;

	for (i = 0; i < m->n; i++)
		if (m->e[i].num == num)
			return m->e[i].key;
	return NULL;
}

/* ---------- tables ---------- */

enum { TB_NONE = -1, TB_HOST, TB_ALERT, TB_DECISION, TB_SSID, TB_N };

static int host_listed(const struct device *d)
{
	return d->used && d->mac[0] && !is_local(d->ip);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static int list_hosts(uint32_t *out, int max)
{
	int i, n = 0;

	for (i = 0; i < DEV_MAX && n < max; i++)
		if (host_listed(&devs[i]))
			out[n++] = imap_num(&hosts_map, devs[i].mac);
	qsort(out, (size_t)n, sizeof(*out), cmp_u32);
	return n;
}

static int list_ssids(uint32_t *out, int max)
{
	int i, n = 0;

	for (i = 0; i < wifi_n_aps && n < max; i++)
		out[n++] = imap_num(&ssid_map, wifi_aps[i].iface);
	qsort(out, (size_t)n, sizeof(*out), cmp_u32);
	return n;
}

static const struct {
	const char *path;               /* the object, "{i}" for the instance */
	int (*list)(uint32_t *out, int max);
} tables[TB_N] = {
	[TB_HOST]     = { "Device.Hosts.Host.{i}.", list_hosts },
	[TB_ALERT]    = { "Device.X_AIGW_.Alert.{i}.", alerts_ids },
	[TB_DECISION] = { "Device.X_AIGW_.Decision.{i}.", act_ids },
	[TB_SSID]     = { "Device.WiFi.SSID.{i}.", list_ssids },
};

/* what an instance number stands for, resolved before a getter runs */
struct ictx {
	int table;
	uint32_t inst;
	struct device *dev;
	const struct wifi_ap *ap;
};

static int resolve(int table, uint32_t inst, struct ictx *c)
{
	char tmp[8];
	const char *k;
	int i;

	memset(c, 0, sizeof(*c));
	c->table = table;
	c->inst = inst;
	switch (table) {
	case TB_NONE:
		return 0;
	case TB_HOST:
		k = imap_key(&hosts_map, inst);
		c->dev = k ? dev_find_mac(k, 0) : NULL;
		return c->dev && host_listed(c->dev) ? 0 : -1;
	case TB_SSID:
		k = imap_key(&ssid_map, inst);
		for (i = 0; k && i < wifi_n_aps; i++)
			if (!strcmp(wifi_aps[i].iface, k))
				c->ap = &wifi_aps[i];
		return c->ap ? 0 : -1;
	case TB_ALERT:
		return alerts_field(inst, "kind", tmp, sizeof(tmp));
	case TB_DECISION:
		return act_field(inst, "action", tmp, sizeof(tmp));
	}
	return -1;
}

/* ---------- the request being served ---------- */

static int req_remote;                  /* asked by a management system, not agctl */
static char req_origin[48] = "local";

static int hide_details(void)
{
	return req_remote && !cfg.mgmt_alert_details;
}

/* ---------- formatting ---------- */

static void fmt_time(int64_t t, char *buf, size_t n)
{
	struct tm tm;
	time_t tt = (time_t)t;

	if (t <= 0 || !gmtime_r(&tt, &tm)) {
		snprintf(buf, n, "0001-01-01T00:00:00Z");   /* TR-106: unknown time */
		return;
	}
	strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void fmt_time_str(const char *secs, char *buf, size_t n)
{
	fmt_time(secs ? strtoll(secs, NULL, 10) : 0, buf, n);
}

static void host_ref(const char *mac, char *buf, size_t n)
{
	uint32_t num = mac && *mac ? imap_find(&hosts_map, mac) : 0;

	if (num)
		snprintf(buf, n, "Device.Hosts.Host.%u", num);
	else
		snprintf(buf, n, "%s", "");
}

static int count(int table)
{
	uint32_t ids[LIST_MAX];

	return tables[table].list(ids, LIST_MAX);
}

/* ---------- getters ---------- */

#define GET(name) static int name(const struct ictx *c, char *buf, size_t n)
typedef int (*getter_t)(const struct ictx *c, char *buf, size_t n);
typedef int (*setter_t)(const struct ictx *c, const char *v, int apply, char *err, size_t en);

GET(g_manufacturer) { (void)c; snprintf(buf, n, "AIGW OS project"); return 0; }
GET(g_model) { (void)c; snprintf(buf, n, "AIGW OS on %s", cfg.host); return 0; }
GET(g_product) { (void)c; snprintf(buf, n, "AIGW OS gateway"); return 0; }
GET(g_swver) { (void)c; snprintf(buf, n, "%s", AG_VERSION); return 0; }
GET(g_dmver) { (void)c; snprintf(buf, n, "%s", DM_VERSION); return 0; }
GET(g_uptime)
{
	double up = 0;
	FILE *f = fopen("/proc/uptime", "r");

	(void)c;
	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0;
		fclose(f);
	}
	snprintf(buf, n, "%u", (unsigned)up);
	return 0;
}

GET(g_host_n) { (void)c; snprintf(buf, n, "%d", count(TB_HOST)); return 0; }
GET(g_h_mac) { snprintf(buf, n, "%s", c->dev->mac); return 0; }
GET(g_h_ip) { snprintf(buf, n, "%s", c->dev->ip); return 0; }
GET(g_h_name) { snprintf(buf, n, "%s", c->dev->hostname); return 0; }
GET(g_h_active) { snprintf(buf, n, "%s", c->dev->present || c->dev->wifi_connected ? "true" : "false"); return 0; }
GET(g_h_type) { snprintf(buf, n, "%s", c->dev->type); return 0; }
GET(g_h_zone) { snprintf(buf, n, "%s", c->dev->zone); return 0; }
GET(g_h_rdown) { snprintf(buf, n, "%llu", (unsigned long long)c->dev->rate_down); return 0; }
GET(g_h_rup) { snprintf(buf, n, "%llu", (unsigned long long)c->dev->rate_up); return 0; }
GET(g_h_bdown) { snprintf(buf, n, "%llu", (unsigned long long)c->dev->bytes_down); return 0; }
GET(g_h_bup) { snprintf(buf, n, "%llu", (unsigned long long)c->dev->bytes_up); return 0; }
GET(g_h_band) { snprintf(buf, n, "%d", c->dev->wifi_connected ? c->dev->wifi_band : 0); return 0; }
GET(g_h_signal)
{
	snprintf(buf, n, "%d", c->dev->wifi_connected ? (int)(c->dev->wifi_sig_avg - (c->dev->wifi_sig_avg < 0 ? 0.5 : -0.5)) : 0);
	return 0;
}
GET(g_h_first) { fmt_time(c->dev->first_seen, buf, n); return 0; }
GET(g_h_last) { fmt_time(c->dev->last_seen, buf, n); return 0; }

GET(g_mode) { (void)c; snprintf(buf, n, "%s", ag_mode_str(cfg.act_mode)); return 0; }
GET(g_alert_n) { (void)c; snprintf(buf, n, "%d", count(TB_ALERT)); return 0; }
GET(g_dec_n) { (void)c; snprintf(buf, n, "%d", count(TB_DECISION)); return 0; }
GET(g_actions) { (void)c; snprintf(buf, n, "%s", cfg.mgmt_actions ? "true" : "false"); return 0; }
GET(g_details) { (void)c; snprintf(buf, n, "%s", cfg.mgmt_alert_details ? "true" : "false"); return 0; }

static int alert_str(const struct ictx *c, const char *f, char *buf, size_t n)
{
	return alerts_field(c->inst, f, buf, n);
}
GET(g_a_kind) { return alert_str(c, "kind", buf, n); }
GET(g_a_sev) { return alert_str(c, "severity", buf, n); }
GET(g_a_count) { return alert_str(c, "count", buf, n); }
GET(g_a_score) { return alert_str(c, "score", buf, n); }
GET(g_a_subject)
{
	if (hide_details()) {
		snprintf(buf, n, "%s", "");
		return 0;
	}
	return alert_str(c, "subject", buf, n);
}
GET(g_a_reason)
{
	if (hide_details()) {
		snprintf(buf, n, "%s", "");
		return 0;
	}
	return alert_str(c, "reason", buf, n);
}
GET(g_a_host)
{
	char mac[24];

	if (alerts_field(c->inst, "mac", mac, sizeof(mac)) < 0)
		return -1;
	host_ref(mac, buf, n);
	return 0;
}
GET(g_a_first)
{
	char t[24];

	if (alerts_field(c->inst, "first", t, sizeof(t)) < 0)
		return -1;
	fmt_time_str(t, buf, n);
	return 0;
}
GET(g_a_last)
{
	char t[24];

	if (alerts_field(c->inst, "last", t, sizeof(t)) < 0)
		return -1;
	fmt_time_str(t, buf, n);
	return 0;
}

static int dec_str(const struct ictx *c, const char *f, char *buf, size_t n)
{
	return act_field(c->inst, f, buf, n);
}
GET(g_d_action) { return dec_str(c, "action", buf, n); }
GET(g_d_state) { return dec_str(c, "state", buf, n); }
GET(g_d_origin) { return dec_str(c, "origin", buf, n); }
GET(g_d_note) { return dec_str(c, "note", buf, n); }
GET(g_d_target)
{
	char action[48];

	/* a blocked domain says what someone looked up */
	if (hide_details() && act_field(c->inst, "action", action, sizeof(action)) == 0 && !strcmp(action, "block")) {
		snprintf(buf, n, "%s", "");
		return 0;
	}
	return dec_str(c, "target", buf, n);
}
GET(g_d_reason)
{
	if (hide_details()) {
		snprintf(buf, n, "%s", "");
		return 0;
	}
	return dec_str(c, "reason", buf, n);
}
GET(g_d_auto)
{
	char v[8];

	if (act_field(c->inst, "automatic", v, sizeof(v)) < 0)
		return -1;
	snprintf(buf, n, "%s", atoi(v) ? "true" : "false");
	return 0;
}
GET(g_d_created)
{
	char t[24];

	if (act_field(c->inst, "created", t, sizeof(t)) < 0)
		return -1;
	fmt_time_str(t, buf, n);
	return 0;
}
GET(g_d_expires)
{
	char t[24];

	if (act_field(c->inst, "expires", t, sizeof(t)) < 0)
		return -1;
	fmt_time_str(t, buf, n);
	return 0;
}
GET(g_d_alert)
{
	char id[16], kind[8];

	if (act_field(c->inst, "alert", id, sizeof(id)) < 0)
		return -1;
	if (strtoul(id, NULL, 10) && alerts_field((uint32_t)strtoul(id, NULL, 10), "kind", kind, sizeof(kind)) == 0)
		snprintf(buf, n, "Device.X_AIGW_.Alert.%s", id);
	else
		snprintf(buf, n, "%s", "");
	return 0;
}
GET(g_d_host)
{
	char action[48], target[NAME_LEN];
	struct device *d;

	if (act_field(c->inst, "action", action, sizeof(action)) < 0 ||
	    act_field(c->inst, "target", target, sizeof(target)) < 0)
		return -1;
	d = strchr(target, ':') ? dev_find_mac(target, 0) : dev_find_ip(target, 0);
	host_ref(d ? d->mac : "", buf, n);
	return 0;
}

GET(g_ssid_n) { (void)c; snprintf(buf, n, "%d", count(TB_SSID)); return 0; }
GET(g_s_name) { snprintf(buf, n, "%s", c->ap->iface); return 0; }
GET(g_s_ssid) { snprintf(buf, n, "%s", c->ap->ssid); return 0; }
GET(g_s_bssid) { snprintf(buf, n, "%s", c->ap->bssid); return 0; }
GET(g_s_band) { snprintf(buf, n, "%s", c->ap->band == 5 ? "5GHz" : c->ap->band == 2 ? "2.4GHz" : ""); return 0; }
GET(g_s_chan) { snprintf(buf, n, "%d", c->ap->channel); return 0; }
GET(g_s_width) { snprintf(buf, n, "%d", c->ap->width); return 0; }
GET(g_s_sta) { snprintf(buf, n, "%d", c->ap->stations); return 0; }

/* ---------- setters ---------- */

static int s_mode(const struct ictx *c, const char *v, int apply, char *err, size_t en)
{
	(void)c;
	if (strcmp(v, "off") && strcmp(v, "advise") && strcmp(v, "enforce")) {
		snprintf(err, en, "Mode is off, advise or enforce");
		return E_INVALID_VALUE;
	}
	if (req_remote && !cfg.mgmt_actions) {
		snprintf(err, en, "changing the mode is not allowed for management systems (mgmt.actions 0)");
		return E_REFUSED;
	}
	if (!apply)
		return E_OK;
	cfg.act_mode = ag_mode_parse(v, cfg.act_mode);
	if (ag_config_set("act", "mode", v) < 0)
		AG_WARN("dm: mode %s set, but not saved in the config", v);
	AG_INFO("dm: mode set to %s by %s", v, req_origin);
	return E_OK;
}

/* ---------- the schema ---------- */

#define W 1                             /* writable */
#define N 2                             /* changes worth notifying (USP value change, CWMP active notification) */

static const struct dm_param {
	const char *path;
	int type, flags, table;
	getter_t get;
	setter_t set;
} params[] = {
	{ "Device.DeviceInfo.Manufacturer", T_STR, 0, TB_NONE, g_manufacturer, NULL },
	{ "Device.DeviceInfo.ModelName", T_STR, 0, TB_NONE, g_model, NULL },
	{ "Device.DeviceInfo.ProductClass", T_STR, 0, TB_NONE, g_product, NULL },
	{ "Device.DeviceInfo.SoftwareVersion", T_STR, 0, TB_NONE, g_swver, NULL },
	{ "Device.DeviceInfo.UpTime", T_UINT, 0, TB_NONE, g_uptime, NULL },
	{ "Device.DeviceInfo.X_AIGW_DataModelVersion", T_STR, 0, TB_NONE, g_dmver, NULL },

	{ "Device.Hosts.HostNumberOfEntries", T_UINT, N, TB_NONE, g_host_n, NULL },
	{ "Device.Hosts.Host.{i}.PhysAddress", T_STR, 0, TB_HOST, g_h_mac, NULL },
	{ "Device.Hosts.Host.{i}.IPAddress", T_STR, N, TB_HOST, g_h_ip, NULL },
	{ "Device.Hosts.Host.{i}.HostName", T_STR, N, TB_HOST, g_h_name, NULL },
	{ "Device.Hosts.Host.{i}.Active", T_BOOL, N, TB_HOST, g_h_active, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_Type", T_STR, 0, TB_HOST, g_h_type, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_Zone", T_STR, 0, TB_HOST, g_h_zone, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_FirstSeen", T_TIME, 0, TB_HOST, g_h_first, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_LastSeen", T_TIME, 0, TB_HOST, g_h_last, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_RateDown", T_ULONG, 0, TB_HOST, g_h_rdown, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_RateUp", T_ULONG, 0, TB_HOST, g_h_rup, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_BytesDown", T_ULONG, 0, TB_HOST, g_h_bdown, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_BytesUp", T_ULONG, 0, TB_HOST, g_h_bup, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_WiFiBand", T_UINT, 0, TB_HOST, g_h_band, NULL },
	{ "Device.Hosts.Host.{i}.X_AIGW_WiFiSignal", T_INT, 0, TB_HOST, g_h_signal, NULL },

	{ "Device.WiFi.SSIDNumberOfEntries", T_UINT, 0, TB_NONE, g_ssid_n, NULL },
	{ "Device.WiFi.SSID.{i}.Name", T_STR, 0, TB_SSID, g_s_name, NULL },
	{ "Device.WiFi.SSID.{i}.SSID", T_STR, 0, TB_SSID, g_s_ssid, NULL },
	{ "Device.WiFi.SSID.{i}.BSSID", T_STR, 0, TB_SSID, g_s_bssid, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Band", T_STR, 0, TB_SSID, g_s_band, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Channel", T_UINT, N, TB_SSID, g_s_chan, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Width", T_UINT, 0, TB_SSID, g_s_width, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Stations", T_UINT, 0, TB_SSID, g_s_sta, NULL },

	{ "Device.X_AIGW_.Mode", T_STR, W | N, TB_NONE, g_mode, s_mode },
	{ "Device.X_AIGW_.ActionsAllowed", T_BOOL, 0, TB_NONE, g_actions, NULL },
	{ "Device.X_AIGW_.AlertDetails", T_BOOL, 0, TB_NONE, g_details, NULL },
	{ "Device.X_AIGW_.AlertNumberOfEntries", T_UINT, N, TB_NONE, g_alert_n, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Kind", T_STR, 0, TB_ALERT, g_a_kind, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Severity", T_STR, 0, TB_ALERT, g_a_sev, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Host", T_STR, 0, TB_ALERT, g_a_host, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Subject", T_STR, 0, TB_ALERT, g_a_subject, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Reason", T_STR, 0, TB_ALERT, g_a_reason, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Score", T_DEC, 0, TB_ALERT, g_a_score, NULL },
	{ "Device.X_AIGW_.Alert.{i}.FirstSeen", T_TIME, 0, TB_ALERT, g_a_first, NULL },
	{ "Device.X_AIGW_.Alert.{i}.LastSeen", T_TIME, 0, TB_ALERT, g_a_last, NULL },
	{ "Device.X_AIGW_.Alert.{i}.Count", T_UINT, 0, TB_ALERT, g_a_count, NULL },
	{ "Device.X_AIGW_.DecisionNumberOfEntries", T_UINT, N, TB_NONE, g_dec_n, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Action", T_STR, 0, TB_DECISION, g_d_action, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Target", T_STR, 0, TB_DECISION, g_d_target, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Host", T_STR, 0, TB_DECISION, g_d_host, NULL },
	{ "Device.X_AIGW_.Decision.{i}.State", T_STR, N, TB_DECISION, g_d_state, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Origin", T_STR, 0, TB_DECISION, g_d_origin, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Reason", T_STR, 0, TB_DECISION, g_d_reason, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Note", T_STR, 0, TB_DECISION, g_d_note, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Alert", T_STR, 0, TB_DECISION, g_d_alert, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Automatic", T_BOOL, 0, TB_DECISION, g_d_auto, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Created", T_TIME, 0, TB_DECISION, g_d_created, NULL },
	{ "Device.X_AIGW_.Decision.{i}.Expires", T_TIME, 0, TB_DECISION, g_d_expires, NULL },
};
#define N_PARAMS ((int)(sizeof(params) / sizeof(params[0])))

/* ---------- commands ---------- */

typedef int (*run_t)(const struct ictx *c, struct blob_attr *args, struct blob_buf *out, char *err, size_t en);

static const char *arg(struct blob_attr *args, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!args)
		return NULL;
	blobmsg_for_each_attr(cur, args, rem)
		if (!strcmp(blobmsg_name(cur), name) && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			return blobmsg_get_string(cur);
	return NULL;
}

static int duration(struct blob_attr *args, int *ttl, char *err, size_t en)
{
	const char *v = arg(args, "Duration");
	char *end;
	long s;

	*ttl = 0;                       /* the action's default */
	if (!v || !*v)
		return E_OK;
	s = strtol(v, &end, 10);
	if (*end || s < 0 || s > 86400 * 31) {
		snprintf(err, en, "Duration is a number of seconds");
		return E_INVALID_ARGS;
	}
	*ttl = (int)s;
	return E_OK;
}

static int answer(const struct ictx *c, int (*fn)(uint32_t, char *, size_t), struct blob_buf *out, char *err, size_t en)
{
	char state[16];
	int rc;

	act_answered_by(req_remote ? req_origin : "you");
	rc = fn(c->inst, err, en);
	act_answered_by("you");
	if (rc < 0)
		return E_REFUSED;
	if (act_field(c->inst, "state", state, sizeof(state)) == 0)
		blobmsg_add_string(out, "State", state);
	return E_OK;
}

static int approve_default(uint32_t id, char *err, size_t en)
{
	return act_approve(id, 0, err, en);
}

static int r_approve(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	(void)a;
	return answer(c, approve_default, o, e, n);
}

static int r_reject(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	(void)a;
	return answer(c, act_reject, o, e, n);
}

static int r_undo(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	(void)a;
	return answer(c, act_revert, o, e, n);
}

static int request(const char *action, const char *what, int ttl, struct blob_buf *out, char *err, size_t en)
{
	struct act_opts o = { 0 };
	char reason[160], who[64], ref[48];
	uint32_t id = 0;

	if (req_remote && !cfg.mgmt_actions) {
		snprintf(err, en, "%s is not allowed for management systems (mgmt.actions 0)", action);
		return E_REFUSED;
	}
	snprintf(who, sizeof(who), "%s", req_remote ? req_origin : "you");
	o.origin = who;
	snprintf(reason, sizeof(reason), "Asked by %s through the data model.", who);
	if (act_request(action, what, ttl, &o, reason, &id, err, en) < 0)
		return E_REFUSED;
	snprintf(ref, sizeof(ref), "Device.X_AIGW_.Decision.%u", id);
	blobmsg_add_string(out, "Decision", ref);
	return E_OK;
}

/* a Host argument: a Device.Hosts.Host.N path, or a MAC, IP or name */
static const char *host_arg(const char *v)
{
	static char mac[24];
	const char *p = "Device.Hosts.Host.";
	struct device *d;

	if (!v || !*v)
		return NULL;
	if (!strncmp(v, p, strlen(p))) {
		const char *k = imap_key(&hosts_map, (uint32_t)strtoul(v + strlen(p), NULL, 10));

		d = k ? dev_find_mac(k, 0) : NULL;
		if (!d)
			return NULL;
		snprintf(mac, sizeof(mac), "%s", d->mac);
		return mac;
	}
	return v;
}

static int r_quarantine(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	const char *h = host_arg(arg(a, "Host"));
	int ttl, rc;

	(void)c;
	if (!h) {
		snprintf(e, n, "Host is a Device.Hosts.Host. path, or a MAC, IP or name");
		return E_INVALID_ARGS;
	}
	if ((rc = duration(a, &ttl, e, n)) != E_OK)
		return rc;
	return request("quarantine", h, ttl, o, e, n);
}

static int r_block(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	const char *dom = arg(a, "Domain");
	int ttl, rc;

	(void)c;
	if (!dom || !*dom) {
		snprintf(e, n, "Domain is required");
		return E_INVALID_ARGS;
	}
	if ((rc = duration(a, &ttl, e, n)) != E_OK)
		return rc;
	return request("block", dom, ttl, o, e, n);
}

static const struct dm_cmd {
	const char *path;
	int table;
	const char *in, *out;           /* argument names, comma-separated */
	run_t run;
	int action;                     /* changes something: needs mgmt.actions for a management system */
} cmds[] = {
	{ "Device.X_AIGW_.Decision.{i}.Approve()", TB_DECISION, "", "State", r_approve, 0 },
	{ "Device.X_AIGW_.Decision.{i}.Reject()", TB_DECISION, "", "State", r_reject, 0 },
	{ "Device.X_AIGW_.Decision.{i}.Undo()", TB_DECISION, "", "State", r_undo, 0 },
	{ "Device.X_AIGW_.Quarantine()", TB_NONE, "Host,Duration", "Decision", r_quarantine, 1 },
	{ "Device.X_AIGW_.Block()", TB_NONE, "Domain,Duration", "Decision", r_block, 1 },
};
#define N_CMDS ((int)(sizeof(cmds) / sizeof(cmds[0])))

static const struct {
	const char *path;
	const char *args;
} events[] = {
	{ "Device.X_AIGW_.Alert!", "Alert,Kind,Severity,Host,Subject,Reason" },
	{ "Device.X_AIGW_.DecisionChanged!", "Decision,Action,State,Host" },
};
#define N_EVENTS ((int)(sizeof(events) / sizeof(events[0])))

/* ---------- paths ---------- */

/* a concrete path against a pattern with at most one {i}; 1 when it is that parameter */
static int match_exact(const char *q, const char *pat, uint32_t *inst)
{
	const char *ip = strstr(pat, "{i}");
	size_t pre;
	unsigned long v;
	char *end;

	*inst = 0;
	if (!ip)
		return !strcmp(q, pat);
	pre = (size_t)(ip - pat);
	if (strncmp(q, pat, pre) || !isdigit((unsigned char)q[pre]))
		return 0;
	v = strtoul(q + pre, &end, 10);
	if (!v || v > 0xffffffffUL || strcmp(end, ip + 3))
		return 0;
	*inst = (uint32_t)v;
	return 1;
}

/* a partial path (ending in '.') against a pattern: 0 not under it, 1 every instance, 2 one (*inst) */
static int match_partial(const char *q, const char *pat, uint32_t *inst)
{
	const char *ip = strstr(pat, "{i}");
	size_t ql = strlen(q), pre;
	unsigned long v;
	char *end;

	*inst = 0;
	if (!ip || ql <= (size_t)(ip - pat))
		return !strncmp(q, pat, ql) ? 1 : 0;
	pre = (size_t)(ip - pat);
	if (strncmp(q, pat, pre) || !isdigit((unsigned char)q[pre]))
		return 0;
	v = strtoul(q + pre, &end, 10);
	if (!v || v > 0xffffffffUL || strncmp(end, ip + 3, strlen(end)))
		return 0;
	*inst = (uint32_t)v;
	return 2;
}

static void concrete(const char *pat, uint32_t inst, char *out, size_t n)
{
	const char *ip = strstr(pat, "{i}");

	if (!ip)
		snprintf(out, n, "%s", pat);
	else
		snprintf(out, n, "%.*s%u%s", (int)(ip - pat), pat, inst, ip + 3);
}

/* ---------- ubus methods ---------- */

static struct blob_buf b;

static void set_origin(struct blob_attr *o)
{
	const char *s = o && blobmsg_type(o) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(o) : "";

	snprintf(req_origin, sizeof(req_origin), "%s", *s ? s : "local");
	req_remote = strcmp(req_origin, "local") != 0;
}

static void err_add(struct blob_buf *bb, const char *path, int code, const char *msg)
{
	void *t = blobmsg_open_table(bb, path);

	blobmsg_add_string(bb, "code", err_names[code]);
	blobmsg_add_string(bb, "message", msg ? msg : err_names[code]);
	blobmsg_close_table(bb, t);
}

static int m_schema(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	void *a, *t, *l;
	char tmp[128], *s, *save;
	int i;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", DM_VERSION);
	a = blobmsg_open_array(&b, "params");
	for (i = 0; i < N_PARAMS; i++) {
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "path", params[i].path);
		blobmsg_add_string(&b, "type", type_names[params[i].type]);
		blobmsg_add_u8(&b, "writable", (params[i].flags & W) != 0);
		blobmsg_add_u8(&b, "notify", (params[i].flags & N) != 0);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "objects");
	for (i = 0; i < TB_N; i++) {
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "path", tables[i].path);
		blobmsg_add_u8(&b, "writable", 0);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "commands");
	for (i = 0; i < N_CMDS; i++) {
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "path", cmds[i].path);
		l = blobmsg_open_array(&b, "in");
		snprintf(tmp, sizeof(tmp), "%s", cmds[i].in);
		for (s = strtok_r(tmp, ",", &save); s; s = strtok_r(NULL, ",", &save))
			blobmsg_add_string(&b, NULL, s);
		blobmsg_close_array(&b, l);
		l = blobmsg_open_array(&b, "out");
		snprintf(tmp, sizeof(tmp), "%s", cmds[i].out);
		for (s = strtok_r(tmp, ",", &save); s; s = strtok_r(NULL, ",", &save))
			blobmsg_add_string(&b, NULL, s);
		blobmsg_close_array(&b, l);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
	a = blobmsg_open_array(&b, "events");
	for (i = 0; i < N_EVENTS; i++) {
		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "path", events[i].path);
		l = blobmsg_open_array(&b, "args");
		snprintf(tmp, sizeof(tmp), "%s", events[i].args);
		for (s = strtok_r(tmp, ",", &save); s; s = strtok_r(NULL, ",", &save))
			blobmsg_add_string(&b, NULL, s);
		blobmsg_close_array(&b, l);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

enum { P_PATH, P_PATHS, P_PARAMS, P_COMMAND, P_ARGS, P_ORIGIN, __P_MAX };
static const struct blobmsg_policy p_policy[__P_MAX] = {
	[P_PATH] = { "path", BLOBMSG_TYPE_STRING },
	[P_PATHS] = { "paths", BLOBMSG_TYPE_ARRAY },
	[P_PARAMS] = { "params", BLOBMSG_TYPE_TABLE },
	[P_COMMAND] = { "command", BLOBMSG_TYPE_STRING },
	[P_ARGS] = { "args", BLOBMSG_TYPE_TABLE },
	[P_ORIGIN] = { "origin", BLOBMSG_TYPE_STRING },
};

static int m_instances(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		       const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__P_MAX];
	const char *q;
	uint32_t ids[LIST_MAX];
	char path[160];
	void *a;
	int i, j, n;

	blobmsg_parse(p_policy, __P_MAX, tb, blob_data(msg), blob_len(msg));
	q = tb[P_PATH] ? blobmsg_get_string(tb[P_PATH]) : "Device.";
	blob_buf_init(&b, 0);
	a = blobmsg_open_array(&b, "instances");
	for (i = 0; i < TB_N; i++) {
		uint32_t one;
		int m = match_partial(q, tables[i].path, &one);

		if (!m)
			continue;
		n = tables[i].list(ids, LIST_MAX);
		for (j = 0; j < n; j++)
			if (m == 1 || ids[j] == one) {
				concrete(tables[i].path, ids[j], path, sizeof(path));
				blobmsg_add_string(&b, NULL, path);
			}
	}
	blobmsg_close_array(&b, a);
	return ubus_send_reply(ctx, req, b.head);
}

static void get_one(struct blob_buf *bb, int pi, uint32_t inst, int *found)
{
	struct ictx c;
	char path[200], val[VAL_MAX];

	if (resolve(params[pi].table, inst, &c) < 0)
		return;
	val[0] = '\0';
	if (params[pi].get(&c, val, sizeof(val)) < 0)
		return;
	concrete(params[pi].path, inst, path, sizeof(path));
	blobmsg_add_string(bb, path, val);
	(*found)++;
}

static int m_get(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__P_MAX], *cur;
	struct blob_buf errs = { 0 };
	size_t rem;
	void *vals;
	int nerr = 0;

	blobmsg_parse(p_policy, __P_MAX, tb, blob_data(msg), blob_len(msg));
	set_origin(tb[P_ORIGIN]);
	blob_buf_init(&b, 0);
	blob_buf_init(&errs, 0);
	vals = blobmsg_open_table(&b, "values");
	if (tb[P_PATHS])
		blobmsg_for_each_attr(cur, tb[P_PATHS], rem) {
			const char *q = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : "";
			size_t ql = strlen(q);
			int i, found = 0, valid = 0;
			uint32_t inst;

			if (ql && q[ql - 1] == '.') {
				for (i = 0; i < N_PARAMS; i++) {
					int m = match_partial(q, params[i].path, &inst);

					if (m == 1 && params[i].table == TB_NONE) {
						valid = 1;
						get_one(&b, i, 0, &found);
					} else if (m == 1) {
						uint32_t ids[LIST_MAX];
						int j, n = tables[params[i].table].list(ids, LIST_MAX);

						valid = 1;
						for (j = 0; j < n; j++)
							get_one(&b, i, ids[j], &found);
					} else if (m == 2) {
						struct ictx c;

						if (resolve(params[i].table, inst, &c) == 0)
							valid = 1;
						get_one(&b, i, inst, &found);
					}
				}
			} else {
				for (i = 0; i < N_PARAMS; i++)
					if (match_exact(q, params[i].path, &inst)) {
						valid = 1;
						get_one(&b, i, inst, &found);
						if (!found)
							valid = 0;  /* the instance is gone */
						break;
					}
			}
			if (!valid) {
				err_add(&errs, q, E_NOT_FOUND, "no such path in the data model");
				nerr++;
			}
		}
	blobmsg_close_table(&b, vals);
	if (nerr) {
		struct blob_attr *e;

		vals = blobmsg_open_table(&b, "errors");
		blob_for_each_attr(e, errs.head, rem)
			blobmsg_add_field(&b, blobmsg_type(e), blobmsg_name(e), blobmsg_data(e), blobmsg_data_len(e));
		blobmsg_close_table(&b, vals);
	}
	blob_buf_free(&errs);
	return ubus_send_reply(ctx, req, b.head);
}

static int find_param(const char *q, uint32_t *inst)
{
	int i;

	for (i = 0; i < N_PARAMS; i++)
		if (match_exact(q, params[i].path, inst))
			return i;
	return -1;
}

static int m_set(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__P_MAX], *cur;
	size_t rem;
	void *t;
	int pass, bad = 0;

	blobmsg_parse(p_policy, __P_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[P_PARAMS])
		return UBUS_STATUS_INVALID_ARGUMENT;
	set_origin(tb[P_ORIGIN]);
	blob_buf_init(&b, 0);
	t = blobmsg_open_table(&b, "errors");
	/* pass 0 checks every value, pass 1 applies them: all or nothing */
	for (pass = 0; pass < 2 && !bad; pass++)
		blobmsg_for_each_attr(cur, tb[P_PARAMS], rem) {
			const char *q = blobmsg_name(cur);
			const char *v = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : NULL;
			char err[200] = "";
			struct ictx c;
			uint32_t inst;
			int pi = find_param(q, &inst), rc;

			if (pi < 0 || resolve(params[pi].table, inst, &c) < 0)
				rc = E_NOT_FOUND;
			else if (!(params[pi].flags & W) || !params[pi].set)
				rc = E_READ_ONLY;
			else if (!v)
				rc = E_INVALID_VALUE;
			else
				rc = params[pi].set(&c, v, pass, err, sizeof(err));
			if (rc != E_OK) {
				err_add(&b, q, rc, err[0] ? err : NULL);
				bad++;
			}
		}
	blobmsg_close_table(&b, t);
	blobmsg_add_u8(&b, "ok", !bad);
	return ubus_send_reply(ctx, req, b.head);
}

static int m_operate(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		     const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__P_MAX];
	const char *q;
	char err[240] = "";
	struct ictx c;
	uint32_t inst;
	void *t;
	int i, rc = E_NOT_FOUND;

	blobmsg_parse(p_policy, __P_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[P_COMMAND])
		return UBUS_STATUS_INVALID_ARGUMENT;
	q = blobmsg_get_string(tb[P_COMMAND]);
	set_origin(tb[P_ORIGIN]);
	blob_buf_init(&b, 0);
	t = blobmsg_open_table(&b, "out");
	for (i = 0; i < N_CMDS; i++)
		if (match_exact(q, cmds[i].path, &inst)) {
			if (resolve(cmds[i].table, inst, &c) < 0)
				break;
			rc = cmds[i].run(&c, tb[P_ARGS], &b, err, sizeof(err));
			break;
		}
	blobmsg_close_table(&b, t);
	blobmsg_add_u8(&b, "ok", rc == E_OK);
	if (rc != E_OK) {
		blobmsg_add_string(&b, "code", err_names[rc]);
		blobmsg_add_string(&b, "message", err[0] ? err : rc == E_NOT_FOUND ? "no such command or instance" : err_names[rc]);
	}
	AG_INFO("dm: %s by %s: %s%s%s", q, req_origin, err_names[rc], err[0] ? " -- " : "", err);
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method dm_methods[] = {
	UBUS_METHOD_NOARG("schema", m_schema),
	UBUS_METHOD("instances", m_instances, p_policy),
	UBUS_METHOD("get", m_get, p_policy),
	UBUS_METHOD("set", m_set, p_policy),
	UBUS_METHOD("operate", m_operate, p_policy),
};
static struct ubus_object_type dm_type = UBUS_OBJECT_TYPE("ag.dm", dm_methods);
static struct ubus_object dm_obj = {
	.name = "ag.dm",
	.type = &dm_type,
	.methods = dm_methods,
	.n_methods = ARRAY_SIZE(dm_methods),
};

struct ubus_object *dm_ubus_object(void)
{
	return &dm_obj;
}

/* ---------- events ---------- */

static void emit(const char *event, const char *const *kv)
{
	struct blob_buf eb = { 0 };
	void *t;

	blob_buf_init(&eb, 0);
	blobmsg_add_string(&eb, "event", event);
	t = blobmsg_open_table(&eb, "args");
	for (; kv[0]; kv += 2)
		blobmsg_add_string(&eb, kv[0], kv[1] ? kv[1] : "");
	blobmsg_close_table(&eb, t);
	agd_emit(AG_EV_DM, &eb);
	blob_buf_free(&eb);
}

void dm_on_alert(uint32_t id)
{
	char ref[48], kind[16], sev[8], mac[24], host[48], subj[NAME_LEN], reason[VAL_MAX];
	int details = cfg.mgmt_alert_details;

	snprintf(ref, sizeof(ref), "Device.X_AIGW_.Alert.%u", id);
	if (alerts_field(id, "kind", kind, sizeof(kind)) < 0)
		return;
	alerts_field(id, "severity", sev, sizeof(sev));
	alerts_field(id, "mac", mac, sizeof(mac));
	host_ref(mac, host, sizeof(host));
	if (!details || alerts_field(id, "subject", subj, sizeof(subj)) < 0)
		subj[0] = '\0';
	if (!details || alerts_field(id, "reason", reason, sizeof(reason)) < 0)
		reason[0] = '\0';
	{
		const char *kv[] = { "Alert", ref, "Kind", kind, "Severity", sev, "Host", host,
				     "Subject", subj, "Reason", reason, NULL };

		emit("Device.X_AIGW_.Alert!", kv);
	}
}

void dm_on_decision(uint32_t id, const char *state)
{
	struct ictx c = { .table = TB_DECISION, .inst = id };
	char ref[48], action[48] = "", host[48] = "";
	int saved = req_remote;

	snprintf(ref, sizeof(ref), "Device.X_AIGW_.Decision.%u", id);
	act_field(id, "action", action, sizeof(action));
	req_remote = 1;
	g_d_host(&c, host, sizeof(host));
	req_remote = saved;
	{
		const char *kv[] = { "Decision", ref, "Action", action, "State", state, "Host", host, NULL };

		emit("Device.X_AIGW_.DecisionChanged!", kv);
	}
}

/* ---------- state ---------- */

static void imap_save(struct blob_buf *bb, const char *name, const struct imap *m)
{
	char line[64];
	void *a;
	int i;

	a = blobmsg_open_array(bb, name);
	for (i = 0; i < m->n; i++) {
		snprintf(line, sizeof(line), "%u %s", m->e[i].num, m->e[i].key);
		blobmsg_add_string(bb, NULL, line);
	}
	blobmsg_close_array(bb, a);
}

void dm_save(struct blob_buf *bb)
{
	void *t = blobmsg_open_table(bb, "dm_instances");

	blobmsg_add_u32(bb, "host_next", hosts_map.next);
	blobmsg_add_u32(bb, "ssid_next", ssid_map.next);
	imap_save(bb, "hosts", &hosts_map);
	imap_save(bb, "ssids", &ssid_map);
	blobmsg_close_table(bb, t);
}

static void imap_load(struct imap *m, struct blob_attr *arr)
{
	struct blob_attr *e;
	size_t rem;

	m->n = 0;
	blobmsg_for_each_attr(e, arr, rem) {
		char key[40];
		unsigned num;

		if (blobmsg_type(e) != BLOBMSG_TYPE_STRING || m->n >= IMAP_MAX ||
		    sscanf(blobmsg_get_string(e), "%u %39s", &num, key) != 2 || !num || imap_find(m, key))
			continue;
		snprintf(m->e[m->n].key, sizeof(m->e[m->n].key), "%s", key);
		m->e[m->n++].num = num;
		if (num > m->next)
			m->next = num;
	}
}

void dm_load(struct blob_attr *tbl)
{
	struct blob_attr *cur;
	size_t rem;
	uint32_t hn = 0, sn = 0;

	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "hosts") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)
			imap_load(&hosts_map, cur);
		else if (!strcmp(k, "ssids") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)
			imap_load(&ssid_map, cur);
		else if (!strcmp(k, "host_next") && blobmsg_type(cur) == BLOBMSG_TYPE_INT32)
			hn = blobmsg_get_u32(cur);
		else if (!strcmp(k, "ssid_next") && blobmsg_type(cur) == BLOBMSG_TYPE_INT32)
			sn = blobmsg_get_u32(cur);
	}
	if (hn > hosts_map.next)
		hosts_map.next = hn;
	if (sn > ssid_map.next)
		ssid_map.next = sn;
	AG_INFO("dm: restored %d device and %d access point instance numbers", hosts_map.n, ssid_map.n);
}

void dm_init(void)
{
	memset(&hosts_map, 0, sizeof(hosts_map));
	memset(&ssid_map, 0, sizeof(ssid_map));
	AG_INFO("dm: data model %s, %d parameters, %d commands, %d events (management: %s, actions %s, alert details %s)",
		DM_VERSION, N_PARAMS, N_CMDS, N_EVENTS, cfg.mgmt_enabled ? "on" : "off",
		cfg.mgmt_actions ? "allowed" : "not allowed", cfg.mgmt_alert_details ? "shared" : "kept home");
}
