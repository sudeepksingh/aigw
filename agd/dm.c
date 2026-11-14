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

#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "agd.h"

#define DM_VERSION "1.6"
#define LIST_MAX   512
#define IMAP_MAX   1024
#define VAL_MAX    700

enum { T_STR, T_BOOL, T_UINT, T_INT, T_ULONG, T_TIME, T_DEC };
static const char *const type_names[] = {
	"string", "boolean", "unsignedInt", "int", "unsignedLong", "dateTime", "decimal",
};

enum { E_OK, E_NOT_FOUND, E_READ_ONLY, E_INVALID_VALUE, E_REFUSED, E_INTERNAL, E_INVALID_ARGS, E_PENDING };
static const char *const err_names[] = {
	"ok", "not_found", "read_only", "invalid_value", "refused", "internal", "invalid_arguments", "pending",
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

/* ---------- the vendor prefix ---------- */

/*
 * AIGW's own names (TR-106 vendor extensions) are written with X_AIGW_ below.
 * An ISP or OEM puts its own there with mgmt.vendor_prefix: X_<OUI>_ or
 * X_<domain, upper case, '.' as '-'>_, e.g. X_00D09E_ or X_EXAMPLE-COM_.
 * dm_init rewrites every path once; nothing else in agd knows the prefix.
 */
#define DM_VX "X_AIGW_"
static char vx[32] = DM_VX;

static int prefix_ok(const char *p)
{
	size_t i, l = strlen(p);

	if (l < 4 || l >= sizeof(vx) || strncmp(p, "X_", 2) || p[l - 1] != '_' || p[2] == '_')
		return 0;
	for (i = 2; i < l; i++)
		if (!isupper((unsigned char)p[i]) && !isdigit((unsigned char)p[i]) && p[i] != '-' && p[i] != '_')
			return 0;
	return 1;
}

/* s with X_AIGW_ as the configured prefix (s itself when it is X_AIGW_); made once, at start */
static const char *vx_path(const char *s)
{
	const char *at = strstr(s, DM_VX);
	char *out;
	size_t pre;

	if (!at || !strcmp(vx, DM_VX))
		return s;
	pre = (size_t)(at - s);
	if (!(out = malloc(strlen(s) + strlen(vx) + 1)))
		return s;
	snprintf(out, strlen(s) + strlen(vx) + 1, "%.*s%s%s", (int)pre, s, vx, at + strlen(DM_VX));
	return out;
}

/* ---------- tables ---------- */

enum { TB_NONE = -1, TB_HOST, TB_ALERT, TB_DECISION, TB_SSID, TB_RADIO, TB_AP, TB_FW, TB_HEAL, TB_N };

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

static int list_fw(uint32_t *out, int max)
{
	int n = fw_ids(out, max);

	qsort(out, (size_t)n, sizeof(*out), cmp_u32);
	return n;
}

/*
 * One access point = one radio, one SSID, one AccessPoint, with one instance
 * number (AIGW runs one access point per radio).
 */
static struct {
	const char *path;               /* the object, "{i}" for the instance */
	int (*list)(uint32_t *out, int max);
} tables[TB_N] = {
	[TB_HOST]     = { "Device.Hosts.Host.{i}.", list_hosts },
	[TB_ALERT]    = { "Device.X_AIGW_.Alert.{i}.", alerts_ids },
	[TB_DECISION] = { "Device.X_AIGW_.Decision.{i}.", act_ids },
	[TB_SSID]     = { "Device.WiFi.SSID.{i}.", list_ssids },
	[TB_RADIO]    = { "Device.WiFi.Radio.{i}.", list_ssids },
	[TB_AP]       = { "Device.WiFi.AccessPoint.{i}.", list_ssids },
	[TB_FW]       = { "Device.DeviceInfo.FirmwareImage.{i}.", list_fw },
	[TB_HEAL]     = { "Device.X_AIGW_.WiFiHealth.{i}.", list_ssids },
};

/* what an instance number stands for, resolved before a getter runs */
struct ictx {
	int table;
	uint32_t inst;
	struct device *dev;
	const struct wifi_ap *ap;
	struct fw_image fw;
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
	case TB_FW:
		return fw_find(inst, &c->fw);
	case TB_SSID:
	case TB_RADIO:
	case TB_AP:
	case TB_HEAL:
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

/* an OEM's or ISP's names when set (mgmt.manufacturer, model_name, product_class) */
GET(g_manufacturer) { (void)c; snprintf(buf, n, "%s", cfg.mgmt_manufacturer[0] ? cfg.mgmt_manufacturer : "AIGW OS project"); return 0; }
GET(g_model)
{
	(void)c;
	if (cfg.mgmt_model[0])
		snprintf(buf, n, "%s", cfg.mgmt_model);
	else
		snprintf(buf, n, "AIGW OS on %s", cfg.host);
	return 0;
}
GET(g_product) { (void)c; snprintf(buf, n, "%s", cfg.mgmt_product[0] ? cfg.mgmt_product : "AIGW OS gateway"); return 0; }
/* the firmware's version (its handler says), else AIGW OS's own */
GET(g_swver)
{
	(void)c;
	fw_version(buf, n);
	if (!buf[0])
		snprintf(buf, n, "%s", AG_VERSION);
	return 0;
}
GET(g_addsw) { (void)c; snprintf(buf, n, "AIGW OS %s", AG_VERSION); return 0; }
GET(g_dmver) { (void)c; snprintf(buf, n, "%s", DM_VERSION); return 0; }

/* the first LAN interface's MAC, as 12 upper-case hex digits ("" when none) */
static void lan_mac(char *out, size_t n)
{
	char ifc[64] = "", path[128], mac[32] = "";
	FILE *f;
	size_t i, j = 0;

	sscanf(cfg.lan_ifaces, "%63s", ifc);
	out[0] = '\0';
	if (!ifc[0])
		return;
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifc);
	if (!(f = fopen(path, "r")))
		return;
	if (!fgets(mac, sizeof(mac), f))
		mac[0] = '\0';
	fclose(f);
	for (i = 0; mac[i] && j + 1 < n; i++)
		if (isxdigit((unsigned char)mac[i]))
			out[j++] = (char)toupper((unsigned char)mac[i]);
	out[j] = '\0';
	if (j != 12 || !strcmp(out, "000000000000"))
		out[0] = '\0';
}

/* identity for USP and TR-069 alike: set, else the machine id, else the LAN MAC */
GET(g_serial)
{
	char id[40] = "";
	FILE *f;

	(void)c;
	if (cfg.mgmt_serial[0]) {
		snprintf(buf, n, "%s", cfg.mgmt_serial);
		return 0;
	}
	if ((f = fopen("/etc/machine-id", "r"))) {
		if (!fgets(id, sizeof(id), f))
			id[0] = '\0';
		fclose(f);
		id[strcspn(id, "\r\n")] = '\0';
	}
	if (strlen(id) >= 12) {
		snprintf(buf, n, "%.12s", id);
		return 0;
	}
	lan_mac(id, sizeof(id));
	snprintf(buf, n, "%s", id[0] ? id : "AIGW0000000000");
	return 0;
}

GET(g_oui)
{
	char mac[16];

	(void)c;
	if (cfg.mgmt_oui[0]) {
		snprintf(buf, n, "%s", cfg.mgmt_oui);
		return 0;
	}
	lan_mac(mac, sizeof(mac));
	snprintf(buf, n, "%.6s", mac[0] ? mac : "000000");
	return 0;
}
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
		snprintf(buf, n, "Device.%s.Alert.%s", vx, id);
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

/* what runs, or a value just given to the Wi-Fi handler until the access point has it */
static void wifi_val(const struct ictx *c, int what, const char *live, char *buf, size_t n)
{
	const char *p;

	wifiset_seen(c->ap->iface, what, live);
	p = wifiset_pending(c->ap->iface, what);
	snprintf(buf, n, "%s", p ? p : live);
}
GET(g_s_ssid) { wifi_val(c, WS_SSID, c->ap->ssid, buf, n); return 0; }
GET(g_s_lower) { snprintf(buf, n, "Device.WiFi.Radio.%u", c->inst); return 0; }
GET(g_radio_n) { (void)c; snprintf(buf, n, "%d", count(TB_RADIO)); return 0; }
GET(g_r_enable) { (void)c; snprintf(buf, n, "true"); return 0; }
GET(g_r_status) { (void)c; snprintf(buf, n, "Up"); return 0; }
GET(g_r_band) { snprintf(buf, n, "%s", c->ap->band == 5 ? "5GHz" : "2.4GHz"); return 0; }
GET(g_r_bands) { snprintf(buf, n, "%s", c->ap->band == 5 ? "5GHz" : "2.4GHz"); return 0; }
GET(g_r_chan)
{
	char live[16];

	snprintf(live, sizeof(live), "%d", c->ap->channel);
	wifi_val(c, WS_CHANNEL, live, buf, n);
	return 0;
}
GET(g_r_width)
{
	char live[16];

	snprintf(live, sizeof(live), "%dMHz", c->ap->width ? c->ap->width : 20);
	wifi_val(c, WS_WIDTH, live, buf, n);
	return 0;
}
GET(g_r_widths) { snprintf(buf, n, "%s", c->ap->band == 5 ? "20MHz,40MHz,80MHz,Auto" : "20MHz,40MHz,Auto"); return 0; }
GET(g_ap_n) { (void)c; snprintf(buf, n, "%d", count(TB_AP)); return 0; }
GET(g_ap_ssid) { snprintf(buf, n, "Device.WiFi.SSID.%u", c->inst); return 0; }
GET(g_ap_sta) { snprintf(buf, n, "%d", c->ap->stations); return 0; }
GET(g_ap_modes) { (void)c; snprintf(buf, n, "None,WPA2-Personal,WPA3-Personal,WPA2-WPA3-Personal"); return 0; }
GET(g_ap_mode)
{
	char live[32];

	wifi_mode(c->ap->iface, live, sizeof(live));
	wifi_val(c, WS_MODE, live, buf, n);
	return 0;
}
GET(g_ap_key) { (void)c; snprintf(buf, n, "%s", ""); return 0; }      /* TR-181: always reads empty */

/* Self-Healing Wi-Fi: what each radio's devices feel, and why */
GET(g_heal_n) { (void)c; snprintf(buf, n, "%d", count(TB_HEAL)); return 0; }
GET(g_heal_radio) { snprintf(buf, n, "Device.WiFi.Radio.%u", c->inst); return 0; }
static const struct heal_radio *heal_of(const struct ictx *c) { return heal_for(c->ap->iface); }
GET(g_heal_status) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%s", heal_status_name(r ? r->status : 0)); return 0; }
GET(g_heal_cause) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%s", heal_cause_name(r ? r->cause : 0)); return 0; }
GET(g_heal_expl) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%s", r ? r->explanation : ""); return 0; }
GET(g_heal_loss) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%d", r ? r->loss_pct : 0); return 0; }
GET(g_heal_rtt) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%d", r && r->rtt_ms > 0 ? r->rtt_ms : 0); return 0; }
GET(g_heal_meas) { const struct heal_radio *r = heal_of(c); snprintf(buf, n, "%d", r ? r->measured : 0); return 0; }
GET(g_heal_since) { const struct heal_radio *r = heal_of(c); fmt_time(r ? r->since : 0, buf, n); return 0; }

/* the healing report, per period: Stats.Day (1 day), Stats.Week (7), Stats.Month (30) */
static void hstats(const struct ictx *c, int days, struct heal_day *s)
{
	if (heal_report_of(c->ap->iface, days, s) < 0)
		memset(s, 0, sizeof(*s));
}
#define HSTAT(fn, days, expr) \
	GET(fn) { struct heal_day s; hstats(c, days, &s); snprintf(buf, n, "%u", (unsigned)(expr)); return 0; }
#define HSTATS(P, days) \
	HSTAT(g_hs_good_##P, days, s.secs[HEAL_GOOD]) \
	HSTAT(g_hs_degr_##P, days, s.secs[HEAL_DEGRADED]) \
	HSTAT(g_hs_poor_##P, days, s.secs[HEAL_POOR]) \
	HSTAT(g_hs_goodpct_##P, days, heal_day_good_pct(&s) < 0 ? 0 : heal_day_good_pct(&s)) \
	HSTAT(g_hs_found_##P, days, heal_day_problems(&s)) \
	HSTAT(g_hs_cong_##P, days, s.found[HC_CONGESTION]) \
	HSTAT(g_hs_cov_##P, days, s.found[HC_COVERAGE]) \
	HSTAT(g_hs_hop_##P, days, s.found[HC_HOPPING]) \
	HSTAT(g_hs_dev_##P, days, s.found[HC_DEVICE]) \
	HSTAT(g_hs_radio_##P, days, s.found[HC_RADIO]) \
	HSTAT(g_hs_kept_##P, days, s.kept) \
	HSTAT(g_hs_tried_##P, days, s.tried) \
	HSTAT(g_hs_band_##P, days, s.band_rules) \
	HSTAT(g_hs_fixes_##P, days, s.fixes) \
	HSTAT(g_hs_ttf_##P, days, s.fixes ? s.fix_secs / s.fixes : 0) \
	HSTAT(g_hs_lb_##P, days, s.kept ? s.loss_before / s.kept : 0) \
	HSTAT(g_hs_la_##P, days, s.kept ? s.loss_after / s.kept : 0)
HSTATS(day, 1)
HSTATS(week, 7)
HSTATS(month, 30)
GET(g_heal_hosts)
{
	const struct heal_radio *r = heal_of(c);
	char macs[256], *m, *save, ref[48];

	buf[0] = '\0';
	if (!r)
		return 0;
	snprintf(macs, sizeof(macs), "%s", r->affected);
	for (m = strtok_r(macs, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
		struct device *d = dev_find_mac(m, 0);

		/* its number, given now if nobody has listed the hosts yet */
		if (!d || !host_listed(d))
			continue;
		snprintf(ref, sizeof(ref), "Device.Hosts.Host.%u", imap_num(&hosts_map, d->mac));
		snprintf(buf + strlen(buf), n - strlen(buf), "%s%s", buf[0] ? "," : "", ref);
	}
	return 0;
}

GET(g_fw_n) { (void)c; snprintf(buf, n, "%d", count(TB_FW)); return 0; }
static void fw_ref(int which, char *buf, size_t n)
{
	struct fw_image all[FW_MAX];
	int i, k = fw_list(all, FW_MAX);

	snprintf(buf, n, "%s", "");
	for (i = 0; i < k; i++)
		if (which ? all[i].boot : all[i].active)
			snprintf(buf, n, "Device.DeviceInfo.FirmwareImage.%u", all[i].id);
}
GET(g_fw_active) { (void)c; fw_ref(0, buf, n); return 0; }
GET(g_fw_boot) { (void)c; fw_ref(1, buf, n); return 0; }
GET(g_fw_name) { snprintf(buf, n, "%s", c->fw.name); return 0; }
GET(g_fw_version) { snprintf(buf, n, "%s", c->fw.version); return 0; }
GET(g_fw_status) { snprintf(buf, n, "%s", c->fw.status); return 0; }
GET(g_fw_avail) { snprintf(buf, n, "%s", strcmp(c->fw.status, "NoImage") ? "true" : "false"); return 0; }
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

/* Wi-Fi settings: checked in both passes, staged on apply; m_set gives them to the handler */
static int wifi_set(const struct ictx *c, int what, const char *v, int apply, char *err, size_t en)
{
	if (req_remote && !cfg.mgmt_wifi) {
		snprintf(err, en, "changing Wi-Fi settings is not allowed for management systems (mgmt.wifi 0)");
		return E_REFUSED;
	}
	if (!wifi_handler()) {
		snprintf(err, en, "no Wi-Fi handler on this gateway (wifi.handler)");
		return E_REFUSED;
	}
	if (wifiset_check(c->ap->iface, what, v, err, en) < 0)
		return E_INVALID_VALUE;
	if (apply)
		wifiset_stage(c->ap->iface, what, v);
	return E_OK;
}
static int s_ssid(const struct ictx *c, const char *v, int a, char *e, size_t n) { return wifi_set(c, WS_SSID, v, a, e, n); }
static int s_key(const struct ictx *c, const char *v, int a, char *e, size_t n) { return wifi_set(c, WS_KEY, v, a, e, n); }
static int s_apmode(const struct ictx *c, const char *v, int a, char *e, size_t n) { return wifi_set(c, WS_MODE, v, a, e, n); }
static int s_chan(const struct ictx *c, const char *v, int a, char *e, size_t n) { return wifi_set(c, WS_CHANNEL, v, a, e, n); }
static int s_width(const struct ictx *c, const char *v, int a, char *e, size_t n) { return wifi_set(c, WS_WIDTH, v, a, e, n); }

/* the last diagnosis (Device.X_AIGW_.LastDiagnosis.): empty until one ran */
static const struct diag_finding *ld(const char **host, int64_t *at)
{
	const struct diag_result *r;
	const struct diag_input *in;

	*at = diag_last(&r, &in, host);
	return *at && r->n ? &r->f[0] : NULL;
}
GET(g_ld_time) { const char *h; int64_t at; (void)c; ld(&h, &at); fmt_time(at, buf, n); return 0; }
GET(g_ld_host) { const char *h; int64_t at; (void)c; ld(&h, &at); snprintf(buf, n, "%s", at ? h : ""); return 0; }
GET(g_ld_code) { const char *h; int64_t at; const struct diag_finding *f = ld(&h, &at); (void)c; snprintf(buf, n, "%s", f ? f->code : ""); return 0; }
GET(g_ld_sev) { const char *h; int64_t at; const struct diag_finding *f = ld(&h, &at); (void)c; snprintf(buf, n, "%s", f ? diag_sev_name(f->sev) : ""); return 0; }
GET(g_ld_summary) { const char *h; int64_t at; const struct diag_finding *f = ld(&h, &at); (void)c; snprintf(buf, n, "%s", f ? f->text : ""); return 0; }
GET(g_ld_advice) { const char *h; int64_t at; const struct diag_finding *f = ld(&h, &at); (void)c; snprintf(buf, n, "%s", f ? diag_advice(f->code) : ""); return 0; }
GET(g_ld_findings)
{
	const struct diag_result *r;
	const struct diag_input *in;
	const char *h;
	size_t len = 0;
	int i;

	(void)c;
	buf[0] = '\0';
	if (!diag_last(&r, &in, &h))
		return 0;
	for (i = 0; i < r->n && len < n; i++)
		len += (size_t)snprintf(buf + len, n - len, "%s%s", i ? "," : "", r->f[i].code);
	return 0;
}

/* proactive care (Device.X_AIGW_.Care.): the open problem, and the experience score per period */
static const struct care_state *cs_open(void) { const struct care_state *s = care_now(); return s->active ? s : NULL; }
GET(g_care_en) { (void)c; snprintf(buf, n, "%s", cfg.care ? "true" : "false"); return 0; }
GET(g_care_status) { (void)c; snprintf(buf, n, "%s", cs_open() ? "Problem" : "OK"); return 0; }
GET(g_care_last) { (void)c; fmt_time(care_now()->last_check, buf, n); return 0; }
GET(g_cp_code) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s ? s->code : ""); return 0; }
GET(g_cp_area) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s ? s->area : ""); return 0; }
GET(g_cp_sev) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s ? diag_sev_name(s->sev) : ""); return 0; }
GET(g_cp_summary) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s ? s->text : ""); return 0; }
GET(g_cp_advice) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s ? diag_advice(s->code) : ""); return 0; }
GET(g_cp_since) { const struct care_state *s = cs_open(); (void)c; fmt_time(s ? s->since : 0, buf, n); return 0; }
GET(g_cp_rep) { const struct care_state *s = cs_open(); (void)c; snprintf(buf, n, "%s", s && s->reported ? "true" : "false"); return 0; }
#define CSTAT(fn, days, expr) \
	GET(fn) { struct care_day s; (void)c; care_report_of(days, &s); snprintf(buf, n, "%u", (unsigned)(expr)); return 0; }
#define CSTATS(P, days) \
	CSTAT(g_cs_score_##P, days, care_score(&s) < 0 ? 0 : care_score(&s)) \
	CSTAT(g_cs_checks_##P, days, s.checks) \
	CSTAT(g_cs_probs_##P, days, s.problems) \
	CSTAT(g_cs_rep_##P, days, s.reported) \
	CSTAT(g_cs_fixed_##P, days, s.fixed) \
	CSTAT(g_cs_secs_##P, days, s.problem_secs) \
	GET(g_cs_cause_##P) { struct care_day s; (void)c; care_report_of(days, &s); \
		snprintf(buf, n, "%s", care_area_name(care_main_cause(&s))); return 0; }
CSTATS(day, 1)
CSTATS(week, 7)
CSTATS(month, 30)

/* ---------- the schema ---------- */

#define W 1                             /* writable */
#define N 2                             /* changes worth notifying (USP value change, CWMP active notification) */

static struct dm_param {
	const char *path;
	int type, flags, table;
	getter_t get;
	setter_t set;
} params[] = {
	{ "Device.DeviceInfo.Manufacturer", T_STR, 0, TB_NONE, g_manufacturer, NULL },
	{ "Device.DeviceInfo.ModelName", T_STR, 0, TB_NONE, g_model, NULL },
	{ "Device.DeviceInfo.ProductClass", T_STR, 0, TB_NONE, g_product, NULL },
	{ "Device.DeviceInfo.ManufacturerOUI", T_STR, 0, TB_NONE, g_oui, NULL },
	{ "Device.DeviceInfo.SerialNumber", T_STR, 0, TB_NONE, g_serial, NULL },
	{ "Device.DeviceInfo.SoftwareVersion", T_STR, N, TB_NONE, g_swver, NULL },
	{ "Device.DeviceInfo.AdditionalSoftwareVersion", T_STR, 0, TB_NONE, g_addsw, NULL },
	{ "Device.DeviceInfo.UpTime", T_UINT, 0, TB_NONE, g_uptime, NULL },
	{ "Device.DeviceInfo.X_AIGW_DataModelVersion", T_STR, 0, TB_NONE, g_dmver, NULL },
	{ "Device.DeviceInfo.FirmwareImageNumberOfEntries", T_UINT, 0, TB_NONE, g_fw_n, NULL },
	{ "Device.DeviceInfo.ActiveFirmwareImage", T_STR, 0, TB_NONE, g_fw_active, NULL },
	{ "Device.DeviceInfo.BootFirmwareImage", T_STR, 0, TB_NONE, g_fw_boot, NULL },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Name", T_STR, 0, TB_FW, g_fw_name, NULL },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Version", T_STR, 0, TB_FW, g_fw_version, NULL },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Available", T_BOOL, 0, TB_FW, g_fw_avail, NULL },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Status", T_STR, N, TB_FW, g_fw_status, NULL },

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

	{ "Device.WiFi.RadioNumberOfEntries", T_UINT, 0, TB_NONE, g_radio_n, NULL },
	{ "Device.WiFi.Radio.{i}.Enable", T_BOOL, 0, TB_RADIO, g_r_enable, NULL },
	{ "Device.WiFi.Radio.{i}.Status", T_STR, 0, TB_RADIO, g_r_status, NULL },
	{ "Device.WiFi.Radio.{i}.Name", T_STR, 0, TB_RADIO, g_s_name, NULL },
	{ "Device.WiFi.Radio.{i}.SupportedFrequencyBands", T_STR, 0, TB_RADIO, g_r_bands, NULL },
	{ "Device.WiFi.Radio.{i}.OperatingFrequencyBand", T_STR, 0, TB_RADIO, g_r_band, NULL },
	{ "Device.WiFi.Radio.{i}.Channel", T_UINT, W | N, TB_RADIO, g_r_chan, s_chan },
	{ "Device.WiFi.Radio.{i}.SupportedOperatingChannelBandwidths", T_STR, 0, TB_RADIO, g_r_widths, NULL },
	{ "Device.WiFi.Radio.{i}.OperatingChannelBandwidth", T_STR, W, TB_RADIO, g_r_width, s_width },
	{ "Device.WiFi.SSIDNumberOfEntries", T_UINT, 0, TB_NONE, g_ssid_n, NULL },
	{ "Device.WiFi.SSID.{i}.Name", T_STR, 0, TB_SSID, g_s_name, NULL },
	{ "Device.WiFi.SSID.{i}.LowerLayers", T_STR, 0, TB_SSID, g_s_lower, NULL },
	{ "Device.WiFi.SSID.{i}.SSID", T_STR, W, TB_SSID, g_s_ssid, s_ssid },
	{ "Device.WiFi.SSID.{i}.BSSID", T_STR, 0, TB_SSID, g_s_bssid, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Band", T_STR, 0, TB_SSID, g_s_band, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Channel", T_UINT, N, TB_SSID, g_s_chan, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Width", T_UINT, 0, TB_SSID, g_s_width, NULL },
	{ "Device.WiFi.SSID.{i}.X_AIGW_Stations", T_UINT, 0, TB_SSID, g_s_sta, NULL },
	{ "Device.WiFi.AccessPointNumberOfEntries", T_UINT, 0, TB_NONE, g_ap_n, NULL },
	{ "Device.WiFi.AccessPoint.{i}.SSIDReference", T_STR, 0, TB_AP, g_ap_ssid, NULL },
	{ "Device.WiFi.AccessPoint.{i}.AssociatedDeviceNumberOfEntries", T_UINT, 0, TB_AP, g_ap_sta, NULL },
	{ "Device.WiFi.AccessPoint.{i}.Security.ModesSupported", T_STR, 0, TB_AP, g_ap_modes, NULL },
	{ "Device.WiFi.AccessPoint.{i}.Security.ModeEnabled", T_STR, W, TB_AP, g_ap_mode, s_apmode },
	{ "Device.WiFi.AccessPoint.{i}.Security.KeyPassphrase", T_STR, W, TB_AP, g_ap_key, s_key },

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

	{ "Device.X_AIGW_.WiFiHealthNumberOfEntries", T_UINT, 0, TB_NONE, g_heal_n, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.Radio", T_STR, 0, TB_HEAL, g_heal_radio, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.Status", T_STR, N, TB_HEAL, g_heal_status, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.Cause", T_STR, N, TB_HEAL, g_heal_cause, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.Explanation", T_STR, 0, TB_HEAL, g_heal_expl, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.LossPercent", T_UINT, 0, TB_HEAL, g_heal_loss, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.LatencyMs", T_UINT, 0, TB_HEAL, g_heal_rtt, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.StationsMeasured", T_UINT, 0, TB_HEAL, g_heal_meas, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.AffectedHosts", T_STR, 0, TB_HEAL, g_heal_hosts, NULL },
	{ "Device.X_AIGW_.WiFiHealth.{i}.Since", T_TIME, 0, TB_HEAL, g_heal_since, NULL },
#define HROWS(O, P) \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".GoodSeconds", T_UINT, 0, TB_HEAL, g_hs_good_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".DegradedSeconds", T_UINT, 0, TB_HEAL, g_hs_degr_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".PoorSeconds", T_UINT, 0, TB_HEAL, g_hs_poor_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".GoodPercent", T_UINT, 0, TB_HEAL, g_hs_goodpct_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".ProblemsFound", T_UINT, 0, TB_HEAL, g_hs_found_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".CongestionFound", T_UINT, 0, TB_HEAL, g_hs_cong_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".WeakCoverageFound", T_UINT, 0, TB_HEAL, g_hs_cov_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".BandHoppingFound", T_UINT, 0, TB_HEAL, g_hs_hop_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".DeviceFound", T_UINT, 0, TB_HEAL, g_hs_dev_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".RadioFound", T_UINT, 0, TB_HEAL, g_hs_radio_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".ChannelTrialsKept", T_UINT, 0, TB_HEAL, g_hs_kept_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".ChannelTrialsTried", T_UINT, 0, TB_HEAL, g_hs_tried_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".BandRules", T_UINT, 0, TB_HEAL, g_hs_band_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".Fixes", T_UINT, 0, TB_HEAL, g_hs_fixes_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".MeanTimeToFix", T_UINT, 0, TB_HEAL, g_hs_ttf_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".LossBeforePercent", T_UINT, 0, TB_HEAL, g_hs_lb_##P, NULL }, \
	{ "Device.X_AIGW_.WiFiHealth.{i}.Stats." O ".LossAfterPercent", T_UINT, 0, TB_HEAL, g_hs_la_##P, NULL },
	HROWS("Day", day)
	HROWS("Week", week)
	HROWS("Month", month)
#undef HROWS

	{ "Device.X_AIGW_.LastDiagnosis.Time", T_TIME, 0, TB_NONE, g_ld_time, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Host", T_STR, 0, TB_NONE, g_ld_host, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Code", T_STR, N, TB_NONE, g_ld_code, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Severity", T_STR, 0, TB_NONE, g_ld_sev, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Summary", T_STR, 0, TB_NONE, g_ld_summary, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Advice", T_STR, 0, TB_NONE, g_ld_advice, NULL },
	{ "Device.X_AIGW_.LastDiagnosis.Findings", T_STR, 0, TB_NONE, g_ld_findings, NULL },

	{ "Device.X_AIGW_.Care.Enable", T_BOOL, 0, TB_NONE, g_care_en, NULL },
	{ "Device.X_AIGW_.Care.Status", T_STR, N, TB_NONE, g_care_status, NULL },
	{ "Device.X_AIGW_.Care.LastCheck", T_TIME, 0, TB_NONE, g_care_last, NULL },
	{ "Device.X_AIGW_.Care.Problem.Code", T_STR, N, TB_NONE, g_cp_code, NULL },
	{ "Device.X_AIGW_.Care.Problem.Area", T_STR, 0, TB_NONE, g_cp_area, NULL },
	{ "Device.X_AIGW_.Care.Problem.Severity", T_STR, 0, TB_NONE, g_cp_sev, NULL },
	{ "Device.X_AIGW_.Care.Problem.Summary", T_STR, 0, TB_NONE, g_cp_summary, NULL },
	{ "Device.X_AIGW_.Care.Problem.Advice", T_STR, 0, TB_NONE, g_cp_advice, NULL },
	{ "Device.X_AIGW_.Care.Problem.Since", T_TIME, 0, TB_NONE, g_cp_since, NULL },
	{ "Device.X_AIGW_.Care.Problem.ReportedToISP", T_BOOL, 0, TB_NONE, g_cp_rep, NULL },
#define CROWS(O, P) \
	{ "Device.X_AIGW_.Care.Stats." O ".ExperienceScore", T_UINT, 0, TB_NONE, g_cs_score_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".Checks", T_UINT, 0, TB_NONE, g_cs_checks_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".Problems", T_UINT, 0, TB_NONE, g_cs_probs_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".ReportedToISP", T_UINT, 0, TB_NONE, g_cs_rep_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".FixedByAIGW", T_UINT, 0, TB_NONE, g_cs_fixed_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".ProblemSeconds", T_UINT, 0, TB_NONE, g_cs_secs_##P, NULL }, \
	{ "Device.X_AIGW_.Care.Stats." O ".MainCause", T_STR, 0, TB_NONE, g_cs_cause_##P, NULL },
	CROWS("Day", day)
	CROWS("Week", week)
	CROWS("Month", month)
#undef CROWS
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
	char reason[160], who[64], ref[96];
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
	snprintf(ref, sizeof(ref), "Device.%s.Decision.%u", vx, id);
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

/* ---------- firmware (asynchronous: the answer comes when the handler is done) ---------- */

struct dm_async {
	struct ubus_request_data req;
	struct ubus_context *ctx;
	char cmd[160];
	char who[48];
};
static struct dm_async *cur_async;      /* the request an asynchronous command belongs to */

static void async_done(void *ctx, int ok, const char *msg)
{
	struct dm_async *a = ctx;
	struct blob_buf rb = { 0 };
	void *t;

	blob_buf_init(&rb, 0);
	t = blobmsg_open_table(&rb, "out");
	blobmsg_close_table(&rb, t);
	blobmsg_add_u8(&rb, "ok", ok);
	if (!ok) {
		blobmsg_add_string(&rb, "code", err_names[E_REFUSED]);
		blobmsg_add_string(&rb, "message", msg && *msg ? msg : "failed");
	}
	ubus_send_reply(a->ctx, &a->req, rb.head);
	ubus_complete_deferred_request(a->ctx, &a->req, 0);
	blob_buf_free(&rb);
	AG_INFO("dm: %s by %s: %s%s%s", a->cmd, a->who, ok ? "done" : "failed", ok ? "" : " -- ", ok ? "" : msg);
	free(a);
}

static int arg_true(struct blob_attr *a, const char *name)
{
	const char *v = arg(a, name);

	return v && (!strcmp(v, "true") || !strcmp(v, "1"));
}

static int fw_allowed(char *e, size_t n)
{
	if (req_remote && !cfg.mgmt_firmware) {
		snprintf(e, n, "firmware upgrades are not allowed for management systems (mgmt.firmware 0)");
		return 0;
	}
	return 1;
}

static int r_fw_download(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	struct fw_request r = { 0 };

	(void)o;
	if (!fw_allowed(e, n))
		return E_REFUSED;
	r.url = arg(a, "URL");
	r.user = arg(a, "Username");
	r.password = arg(a, "Password");
	r.size = arg(a, "FileSize");
	r.alg = arg(a, "CheckSumAlgorithm");
	r.sum = arg(a, "CheckSum");
	r.activate = arg_true(a, "AutoActivate");
	/* a TR-069 client saves its session and reboots by itself */
	r.reboot = strcmp(req_origin, "acs") != 0;
	if (!r.url || !*r.url) {
		snprintf(e, n, "URL is required");
		return E_INVALID_ARGS;
	}
	return fw_download(c->inst, &r, req_origin, async_done, cur_async, e, n) < 0 ? E_REFUSED : E_PENDING;
}

static int r_fw_activate(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	(void)a;
	(void)o;
	if (!fw_allowed(e, n))
		return E_REFUSED;
	return fw_activate(c->inst, strcmp(req_origin, "acs") != 0, req_origin, async_done, cur_async, e, n) < 0 ?
		E_REFUSED : E_PENDING;
}

/* ---------- Diagnose (asynchronous: about 3 s of measuring) ---------- */

/* the verdict as the command's outputs: flat values, the same for USP and TR-069 */
static void diag_out(struct blob_buf *bb, const struct diag_result *r, const struct diag_input *in)
{
	const struct diag_finding *v = r->n ? &r->f[0] : NULL;
	char codes[VAL_MAX] = "", num[16];
	size_t len = 0;
	int i;

	for (i = 0; i < r->n && len < sizeof(codes); i++)
		len += (size_t)snprintf(codes + len, sizeof(codes) - len, "%s%s", i ? "," : "", r->f[i].code);
	blobmsg_add_string(bb, "Code", v ? v->code : "OK");
	blobmsg_add_string(bb, "Area", v ? v->area : "All");
	blobmsg_add_string(bb, "Severity", diag_sev_name(v ? v->sev : DS_OK));
	blobmsg_add_string(bb, "Summary", v ? v->text : "");
	blobmsg_add_string(bb, "Advice", diag_advice(v ? v->code : "OK"));
	blobmsg_add_string(bb, "Findings", codes);
	snprintf(num, sizeof(num), "%d", in->inet.sent ? in->inet.lost * 100 / in->inet.sent : 100);
	blobmsg_add_string(bb, "InternetLossPercent", num);
	snprintf(num, sizeof(num), "%d", in->inet.rtt_ms < 0 ? 0 : in->inet.rtt_ms);
	blobmsg_add_string(bb, "InternetLatencyMs", num);
	snprintf(num, sizeof(num), "%d", in->gw_ping.rtt_ms < 0 ? 0 : in->gw_ping.rtt_ms);
	blobmsg_add_string(bb, "NextHopLatencyMs", num);
	snprintf(num, sizeof(num), "%d", in->dns == 1 ? in->dns_ms : 0);
	blobmsg_add_string(bb, "DNSLatencyMs", num);
}

static void diag_done(void *ctx, const struct diag_result *r, const struct diag_input *in)
{
	struct dm_async *a = ctx;
	struct blob_buf rb = { 0 };
	void *t;

	blob_buf_init(&rb, 0);
	t = blobmsg_open_table(&rb, "out");
	diag_out(&rb, r, in);
	blobmsg_close_table(&rb, t);
	blobmsg_add_u8(&rb, "ok", 1);
	ubus_send_reply(a->ctx, &a->req, rb.head);
	ubus_complete_deferred_request(a->ctx, &a->req, 0);
	blob_buf_free(&rb);
	AG_INFO("dm: %s by %s: done -- %s", a->cmd, a->who, r->n ? r->f[0].code : "OK");
	free(a);
}

static int r_diagnose(const struct ictx *c, struct blob_attr *a, struct blob_buf *o, char *e, size_t n)
{
	const char *v = arg(a, "Host"), *h = v && *v ? host_arg(v) : "";

	(void)c;
	(void)o;
	if (!h) {
		snprintf(e, n, "Host is a Device.Hosts.Host. path, or a MAC, IP or name");
		return E_INVALID_ARGS;
	}
	return diag_start(h, diag_done, cur_async, e, n) < 0 ? E_REFUSED : E_PENDING;
}

static struct dm_cmd {
	const char *path;
	int table;
	const char *in, *out;           /* argument names, comma-separated */
	run_t run;
	int action;                     /* changes something: needs mgmt.actions for a management system */
	int async;                      /* answers when done (USP: an asynchronous command) */
} cmds[] = {
	{ "Device.X_AIGW_.Decision.{i}.Approve()", TB_DECISION, "", "State", r_approve, 0, 0 },
	{ "Device.X_AIGW_.Decision.{i}.Reject()", TB_DECISION, "", "State", r_reject, 0, 0 },
	{ "Device.X_AIGW_.Decision.{i}.Undo()", TB_DECISION, "", "State", r_undo, 0, 0 },
	{ "Device.X_AIGW_.Quarantine()", TB_NONE, "Host,Duration", "Decision", r_quarantine, 1, 0 },
	{ "Device.X_AIGW_.Block()", TB_NONE, "Domain,Duration", "Decision", r_block, 1, 0 },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Download()", TB_FW,
	  "URL,AutoActivate,Username,Password,FileSize,CheckSumAlgorithm,CheckSum", "", r_fw_download, 0, 1 },
	{ "Device.DeviceInfo.FirmwareImage.{i}.Activate()", TB_FW, "", "", r_fw_activate, 0, 1 },
	{ "Device.X_AIGW_.Diagnose()", TB_NONE, "Host",
	  "Code,Area,Severity,Summary,Advice,Findings,InternetLossPercent,InternetLatencyMs,NextHopLatencyMs,DNSLatencyMs",
	  r_diagnose, 0, 1 },
};
#define N_CMDS ((int)(sizeof(cmds) / sizeof(cmds[0])))

static struct {
	const char *path;
	const char *args;
} events[] = {
	{ "Device.X_AIGW_.Alert!", "Alert,Kind,Severity,Host,Subject,Reason" },
	{ "Device.X_AIGW_.DecisionChanged!", "Decision,Action,State,Host" },
	{ "Device.X_AIGW_.WiFiHealthChanged!", "WiFiHealth,Radio,Status,Cause,Explanation" },
	{ "Device.X_AIGW_.DiagnosisDone!", "Host,Code,Severity,Summary" },
	{ "Device.X_AIGW_.ProblemDetected!", "Code,Area,Severity,Summary,Advice,Since" },
	{ "Device.X_AIGW_.ProblemCleared!", "Code,Area,Since,DurationSeconds" },
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
	blobmsg_add_string(&b, "vendor_prefix", vx);
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
		blobmsg_add_u8(&b, "async", cmds[i].async != 0);
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

enum { P_PATH, P_PATHS, P_PARAMS, P_COMMAND, P_ARGS, P_ORIGIN, P_CHECK, __P_MAX };
static const struct blobmsg_policy p_policy[__P_MAX] = {
	[P_PATH] = { "path", BLOBMSG_TYPE_STRING },
	[P_PATHS] = { "paths", BLOBMSG_TYPE_ARRAY },
	[P_PARAMS] = { "params", BLOBMSG_TYPE_TABLE },
	[P_COMMAND] = { "command", BLOBMSG_TYPE_STRING },
	[P_ARGS] = { "args", BLOBMSG_TYPE_TABLE },
	[P_ORIGIN] = { "origin", BLOBMSG_TYPE_STRING },
	[P_CHECK] = { "check", BLOBMSG_TYPE_BOOL },
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
	int pass, passes, bad = 0;

	blobmsg_parse(p_policy, __P_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[P_PARAMS])
		return UBUS_STATUS_INVALID_ARGUMENT;
	set_origin(tb[P_ORIGIN]);
	/* "check": only say whether the values would be taken (TR-069 sets one value at a time, then commits) */
	passes = tb[P_CHECK] && blobmsg_get_bool(tb[P_CHECK]) ? 1 : 2;
	blob_buf_init(&b, 0);
	t = blobmsg_open_table(&b, "errors");
	/* pass 0 checks every value, pass 1 applies them: all or nothing */
	for (pass = 0; pass < passes && !bad; pass++)
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
	/* Wi-Fi settings: one handler call per access point, with all this request changed */
	if (bad || passes < 2)
		wifiset_drop();
	else
		wifiset_flush(req_origin);
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
			if (cmds[i].async && !(cur_async = calloc(1, sizeof(*cur_async)))) {
				rc = E_INTERNAL;
				break;
			}
			if (cur_async) {
				snprintf(cur_async->cmd, sizeof(cur_async->cmd), "%s", q);
				snprintf(cur_async->who, sizeof(cur_async->who), "%s", req_origin);
			}
			rc = cmds[i].run(&c, tb[P_ARGS], &b, err, sizeof(err));
			if (rc == E_PENDING) {
				/* the handler answers later (async_done) */
				cur_async->ctx = ctx;
				ubus_defer_request(ctx, req, &cur_async->req);
				cur_async = NULL;
				AG_INFO("dm: %s by %s: started", q, req_origin);
				return 0;
			}
			free(cur_async);
			cur_async = NULL;
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

/*
 * system {action: reboot | factory_reset, origin}: what Device.Reboot() and
 * Device.FactoryReset() do. The protocol clients own those commands (OB-USP-
 * Agent, icwmp); they come here so that who may, and what it means on this
 * device (the firmware handler), is decided in one place.
 */
enum { S_ACTION, S_ORIGIN, __S_MAX };
static const struct blobmsg_policy s_policy[__S_MAX] = {
	[S_ACTION] = { "action", BLOBMSG_TYPE_STRING },
	[S_ORIGIN] = { "origin", BLOBMSG_TYPE_STRING },
};

static int m_system(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__S_MAX];
	const char *action;
	char err[200] = "";
	int rc = E_OK;

	blobmsg_parse(s_policy, __S_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[S_ACTION])
		return UBUS_STATUS_INVALID_ARGUMENT;
	action = blobmsg_get_string(tb[S_ACTION]);
	set_origin(tb[S_ORIGIN]);
	if (req_remote && !cfg.mgmt_reset) {
		snprintf(err, sizeof(err), "%s is not allowed for management systems (mgmt.reset 0)", action);
		rc = E_REFUSED;
	} else if (fw_system(action, req_origin, err, sizeof(err)) < 0)
		rc = E_REFUSED;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", rc == E_OK);
	if (rc != E_OK) {
		blobmsg_add_string(&b, "code", err_names[rc]);
		blobmsg_add_string(&b, "message", err);
		AG_INFO("dm: %s by %s: %s", action, req_origin, err);
	}
	return ubus_send_reply(ctx, req, b.head);
}

static const struct ubus_method dm_methods[] = {
	UBUS_METHOD("system", m_system, s_policy),
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
	char ref[96], kind[16], sev[8], mac[24], host[48], subj[NAME_LEN], reason[VAL_MAX];
	int details = cfg.mgmt_alert_details;

	snprintf(ref, sizeof(ref), "Device.%s.Alert.%u", vx, id);
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

		emit(events[0].path, kv);   /* Alert! */
	}
}

void dm_on_decision(uint32_t id, const char *state)
{
	struct ictx c = { .table = TB_DECISION, .inst = id };
	char ref[96], action[48] = "", host[48] = "";
	int saved = req_remote;

	snprintf(ref, sizeof(ref), "Device.%s.Decision.%u", vx, id);
	act_field(id, "action", action, sizeof(action));
	req_remote = 1;
	g_d_host(&c, host, sizeof(host));
	req_remote = saved;
	{
		const char *kv[] = { "Decision", ref, "Action", action, "State", state, "Host", host, NULL };

		emit(events[1].path, kv);   /* DecisionChanged! */
	}
}

void dm_on_heal(const char *iface)
{
	const struct heal_radio *r = heal_for(iface);
	uint32_t num = imap_find(&ssid_map, iface);
	char ref[96], radio[48];

	if (!r || !num)
		return;
	snprintf(ref, sizeof(ref), "Device.%s.WiFiHealth.%u", vx, num);
	snprintf(radio, sizeof(radio), "Device.WiFi.Radio.%u", num);
	{
		const char *kv[] = { "WiFiHealth", ref, "Radio", radio, "Status", heal_status_name(r->status),
				     "Cause", heal_cause_name(r->cause), "Explanation", r->explanation, NULL };

		emit(events[2].path, kv);   /* WiFiHealthChanged! */
	}
}

/* diag.c: a diagnosis is done (asked from anywhere): DiagnosisDone! */
void dm_on_diag(const struct diag_result *r, const struct diag_input *in, const char *mac)
{
	char ref[96] = "";
	const char *kv[] = { "Host", ref, "Code", r->n ? r->f[0].code : "OK",
			     "Severity", diag_sev_name(r->n ? r->f[0].sev : DS_OK), "Summary", r->n ? r->f[0].text : "", NULL };

	if (in->host.found && mac && *mac)
		host_ref(mac, ref, sizeof(ref));
	emit(events[3].path, kv);           /* DiagnosisDone! */
}

/* care.c: a problem the ISP must hear of (ProblemDetected!), and its end (ProblemCleared!) */
void dm_on_care(int detected)
{
	const struct care_state *s = care_now();
	char since[40], dur[24];

	fmt_time(s->since, since, sizeof(since));
	snprintf(dur, sizeof(dur), "%ld", s->duration);
	if (detected) {
		const char *kv[] = { "Code", s->code, "Area", s->area, "Severity", diag_sev_name(s->sev),
				     "Summary", s->text, "Advice", diag_advice(s->code), "Since", since, NULL };

		emit(events[4].path, kv);   /* ProblemDetected! */
	} else {
		const char *kv[] = { "Code", s->code, "Area", s->area, "Since", since, "DurationSeconds", dur, NULL };

		emit(events[5].path, kv);   /* ProblemCleared! */
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
	static int done;
	int i;

	memset(&hosts_map, 0, sizeof(hosts_map));
	memset(&ssid_map, 0, sizeof(ssid_map));
	if (cfg.mgmt_prefix[0] && !prefix_ok(cfg.mgmt_prefix))
		AG_WARN("dm: mgmt.vendor_prefix '%s' is not X_<OUI or DOMAIN-NAME>_ (upper case): using %s",
			cfg.mgmt_prefix, DM_VX);
	else if (cfg.mgmt_prefix[0] && !done)
		snprintf(vx, sizeof(vx), "%s", cfg.mgmt_prefix);
	if (!done) {
		for (i = 0; i < N_PARAMS; i++)
			params[i].path = vx_path(params[i].path);
		for (i = 0; i < TB_N; i++)
			tables[i].path = vx_path(tables[i].path);
		for (i = 0; i < N_CMDS; i++)
			cmds[i].path = vx_path(cmds[i].path);
		for (i = 0; i < N_EVENTS; i++)
			events[i].path = vx_path(events[i].path);
		done = 1;
	}
	AG_INFO("dm: data model %s, vendor prefix %s, %d parameters, %d commands, %d events (management: %s, actions %s, alert details %s)",
		DM_VERSION, vx, N_PARAMS, N_CMDS, N_EVENTS, cfg.mgmt_enabled ? "on" : "off",
		cfg.mgmt_actions ? "allowed" : "not allowed", cfg.mgmt_alert_details ? "shared" : "kept home");
}
