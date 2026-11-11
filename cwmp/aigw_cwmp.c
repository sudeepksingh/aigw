/* SPDX-License-Identifier: Apache-2.0 */
/*
 * aigw-cwmp-dm: AIGW OS's data model for icwmp (TR-069 / CWMP).
 *
 * icwmp (BSD-3-Clause, iopsys), run unmodified, reaches its data model only
 * through the ubus object "bbfdm" (get, schema, set, add, del, operate) and
 * applies writes with "bbf.config" (commit, revert). This daemon provides both,
 * in front of agd's data layer (ubus ag.dm): the paths, values, instance
 * numbers and write rules are agd's, the same ones a USP controller sees.
 * What an ACS may do is decided in agd (mgmt.* in /etc/config/aigw), with
 * origin "acs".
 *
 * Here, and only here, is what TR-069 needs and USP doesn't:
 *   Device.ManagementServer.*   the ACS settings, kept in icwmp's own config
 *                               (/etc/config/cwmp) and in the 'cwmp' section of
 *                               /etc/config/aigw, so they survive a restart
 *   Device.RootDataModelVersion, DeviceInfo.ProvisioningCode
 *   Device.X_AIGW_.Request.     commands, which CWMP doesn't have: set Command
 *                               (and its inputs) in one SetParameterValues; it
 *                               runs when icwmp commits, Status and the outputs
 *                               tell the result
 *   Device.X_AIGW_.LastEvent.   agd's events, for active notification on Count
 *
 * A SetParameterValues is all or nothing: every value is checked when icwmp
 * sets it (ag.dm set with "check"), and applied, in one ag.dm set, on commit.
 *
 * On a PC (no netifd) it also answers network.interface.<wan> status, which
 * icwmp asks for its WAN device, when aigw.cwmp.wan_device is set.
 *
 * "X_AIGW_" above is agd's vendor prefix: whatever mgmt.vendor_prefix says.
 *
 *   aigw-cwmp-dm [-s ubus socket] [-n notify.json]
 *       (AIGW_UBUS, AIGW_UCI_CONFDIR; AIGW_CWMP_CONFDIR for cwmp's)
 *       -n: write icwmp's custom notifications (active on LastEvent.Count)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <uci.h>

#include "ag/ag.h"

#define ORIGIN   "acs"
#define TIMEOUT  10000
#define PATH_MAX_ 256
#define VAL_MAX  768
#define LP_MAX   96
#define PEND_MAX 64

enum { F_DENIED = 9001, F_INTERNAL = 9002, F_ARGS = 9003, F_NAME = 9005, F_VALUE = 9007, F_READONLY = 9008 };

static struct ubus_context *ctx;        /* serves bbfdm and bbf.config, follows agd's events */
static struct ubus_context *out;        /* calls to agd (synchronous, not on the loop) */
static const char *ubus_path;
static const char *cwmp_confdir;        /* icwmp's config: /etc/config unless AIGW_CWMP_CONFDIR */
static struct blob_attr *schema;        /* ag.dm schema, kept: names point into it */
static char wan[64] = "wan", wan_dev[IFNAMSIZ];

/* ---------- small helpers ---------- */

static struct blob_attr *field(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return NULL;
	blobmsg_for_each_attr(cur, tbl, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static struct blob_attr *top(struct blob_attr *msg, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!msg)
		return NULL;
	blob_for_each_attr(cur, msg, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static const char *str(struct blob_attr *a)
{
	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "";
}

static int ends_dot(const char *p)
{
	size_t n = strlen(p);

	return n && p[n - 1] == '.';
}

static void fmt_now(char *buf, size_t n)
{
	time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* a concrete path against a template with at most one {i} */
static int tmpl_match(const char *q, const char *pat)
{
	const char *ip = strstr(pat, "{i}");
	size_t pre;
	const char *s;

	if (!ip)
		return !strcmp(q, pat);
	pre = (size_t)(ip - pat);
	if (strncmp(q, pat, pre) || !isdigit((unsigned char)q[pre]))
		return 0;
	for (s = q + pre; isdigit((unsigned char)*s); s++)
		;
	return !strcmp(s, ip + 3);
}

static const char *xsd(const char *t)
{
	if (!strcmp(t, "boolean")) return "xsd:boolean";
	if (!strcmp(t, "unsignedInt")) return "xsd:unsignedInt";
	if (!strcmp(t, "int")) return "xsd:int";
	if (!strcmp(t, "unsignedLong")) return "xsd:unsignedLong";
	if (!strcmp(t, "dateTime")) return "xsd:dateTime";
	if (!strcmp(t, "decimal")) return "xsd:decimal";
	if (!strcmp(t, "object")) return "xsd:object";
	return "xsd:string";
}

static int cwmp_fault(const char *code)
{
	if (!strcmp(code, "not_found")) return F_NAME;
	if (!strcmp(code, "read_only")) return F_READONLY;
	if (!strcmp(code, "invalid_value")) return F_VALUE;
	if (!strcmp(code, "refused")) return F_DENIED;
	if (!strcmp(code, "invalid_arguments")) return F_ARGS;
	return F_INTERNAL;
}

/* ---------- uci: icwmp's config and AIGW's ---------- */

static int uci_get_opt(const char *confdir, const char *pkg, const char *sec, const char *opt, char *buf, size_t n)
{
	struct uci_context *u = uci_alloc_context();
	struct uci_ptr p = { 0 };
	char spec[160];
	int rc = -1;

	buf[0] = '\0';
	if (!u)
		return -1;
	if (confdir)
		uci_set_confdir(u, confdir);
	snprintf(spec, sizeof(spec), "%s.%s.%s", pkg, sec, opt);
	if (uci_lookup_ptr(u, &p, spec, true) == UCI_OK && p.o && p.o->type == UCI_TYPE_STRING) {
		snprintf(buf, n, "%s", p.o->v.string);
		rc = 0;
	}
	uci_free_context(u);
	return rc;
}

/* sets pkg.sec.opt (the section is made, of type sec, when missing) */
static int uci_set_opt(const char *confdir, const char *pkg, const char *sec, const char *opt, const char *val)
{
	struct uci_context *u = uci_alloc_context();
	struct uci_ptr p = { 0 };
	char spec[160];
	int rc = -1;

	if (!u)
		return -1;
	if (confdir)
		uci_set_confdir(u, confdir);
	snprintf(spec, sizeof(spec), "%s.%s", pkg, sec);
	if (uci_lookup_ptr(u, &p, spec, true) != UCI_OK || !p.p)
		goto done;
	if (!p.s) {
		p.value = sec;
		if (uci_set(u, &p) != UCI_OK)
			goto done;
	}
	memset(&p, 0, sizeof(p));
	snprintf(spec, sizeof(spec), "%s.%s.%s", pkg, sec, opt);
	if (uci_lookup_ptr(u, &p, spec, true) != UCI_OK)
		goto done;
	p.value = val;
	if (uci_set(u, &p) == UCI_OK && uci_save(u, p.p) == UCI_OK && uci_commit(u, &p.p, false) == UCI_OK)
		rc = 0;
done:
	uci_free_context(u);
	return rc;
}

static const char *aigw_confdir(void)
{
	const char *d = getenv("AIGW_UCI_CONFDIR");

	return d && *d ? d : NULL;
}

/* ---------- calls to agd ---------- */

struct reply {
	struct blob_attr *msg;
};

static void reply_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct reply *r = req->priv;

	(void)type;
	free(r->msg);
	r->msg = NULL;
	if (msg && (r->msg = malloc(blob_pad_len(msg))))
		memcpy(r->msg, msg, blob_pad_len(msg));
}

static struct blob_attr *call_t(const char *obj, const char *method, struct blob_attr *args, int timeout);

static struct blob_attr *call(const char *obj, const char *method, struct blob_attr *args)
{
	return call_t(obj, method, args, TIMEOUT);
}

static struct blob_attr *call_t(const char *obj, const char *method, struct blob_attr *args, int timeout)
{
	struct reply r = { NULL };
	uint32_t id;
	int rc;

	if (!out && !(out = ubus_connect(ubus_path)))
		return NULL;
	if (ubus_lookup_id(out, obj, &id)) {
		if (ubus_reconnect(out, ubus_path) || ubus_lookup_id(out, obj, &id))
			return NULL;
	}
	rc = ubus_invoke(out, id, method, args, reply_cb, &r, timeout);
	if (rc) {
		AG_WARN("%s %s: %s", obj, method, ubus_strerror(rc));
		free(r.msg);
		return NULL;
	}
	return r.msg;
}

static struct blob_attr *dm_get(const char *path)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r;
	void *a;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	a = blobmsg_open_array(&b, "paths");
	blobmsg_add_string(&b, NULL, path);
	blobmsg_close_array(&b, a);
	r = call("ag.dm", "get", b.head);
	blob_buf_free(&b);
	return r;
}

/* ---------- the schema agd serves ---------- */

static struct blob_attr *schema_param(const char *path)
{
	struct blob_attr *cur;
	size_t rem;

	if (top(schema, "params"))
		blobmsg_for_each_attr(cur, top(schema, "params"), rem)
			if (tmpl_match(path, str(field(cur, "path"))))
				return cur;
	return NULL;
}

static struct blob_attr *schema_cmd(const char *path)
{
	struct blob_attr *cur;
	size_t rem;

	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem)
			if (tmpl_match(path, str(field(cur, "path"))))
				return cur;
	return NULL;
}

/* "Quarantine", "Device.X_AIGW_.Quarantine" or "...Decision.3.Approve()" -> the command path */
static int resolve_cmd(const char *v, char *path, size_t n)
{
	struct blob_attr *cur;
	char name[PATH_MAX_];
	size_t rem, l;

	snprintf(name, sizeof(name), "%s", v);
	l = strlen(name);
	if (l > 2 && !strcmp(name + l - 2, "()"))
		name[l - 2] = '\0';
	if (!name[0])
		return -1;
	if (strchr(name, '.')) {
		snprintf(path, n, "%s()", name);
		return schema_cmd(path) ? 0 : -1;
	}
	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem) {
			const char *p = str(field(cur, "path")), *dot = strrchr(p, '.');

			if (!strstr(p, "{i}") && dot && !strncmp(dot + 1, name, strlen(name)) &&
			    !strcmp(dot + 1 + strlen(name), "()")) {
				snprintf(path, n, "%s", p);
				return 0;
			}
		}
	return -1;
}

/* ---------- this adapter's own parameters ---------- */

enum { K_UCI, K_FIXED, K_CRURL, K_PKEY, K_HWVER, K_REQ_CMD, K_REQ_IN, K_REQ_OUT, K_REQ_STATUS, K_REQ_MSG,
       K_EV_NAME, K_EV_TIME, K_EV_COUNT, K_EV_ARG };
enum { V_STR, V_BOOL, V_UINT, V_URL };

struct lparam {
	char path[PATH_MAX_];
	const char *type;               /* schema type name */
	int writable, kind;
	/* K_UCI: where it lives */
	const char *sec, *opt, *aigw_opt, *def;
	int check, secret;
	char val[VAL_MAX];              /* K_FIXED and the request / event values */
};
static struct lparam lp[LP_MAX];
static int nlp;
static unsigned ev_count;

static struct lparam *lp_add(const char *path, const char *type, int writable, int kind)
{
	struct lparam *p;
	int i;

	for (i = 0; i < nlp; i++)
		if (!strcmp(lp[i].path, path))
			return &lp[i];      /* an input and an output of the same name: one parameter */
	if (nlp >= LP_MAX)
		return NULL;
	p = &lp[nlp++];
	memset(p, 0, sizeof(*p));
	snprintf(p->path, sizeof(p->path), "%s", path);
	p->type = type;
	p->writable = writable;
	p->kind = kind;
	return p;
}

static void lp_uci(const char *name, const char *type, int writable, const char *sec, const char *opt,
		   const char *aigw_opt, const char *def, int check, int secret)
{
	struct lparam *p = lp_add(name, type, writable, K_UCI);

	if (!p)
		return;
	p->sec = sec;
	p->opt = opt;
	p->aigw_opt = aigw_opt;
	p->def = def;
	p->check = check;
	p->secret = secret;
}

static void lp_fixed(const char *name, const char *type, const char *v)
{
	struct lparam *p = lp_add(name, type, 0, K_FIXED);

	if (p)
		snprintf(p->val, sizeof(p->val), "%s", v);
}

static struct lparam *lp_find(const char *path)
{
	int i;

	for (i = 0; i < nlp; i++)
		if (!strcmp(lp[i].path, path))
			return &lp[i];
	return NULL;
}

/* under the vendor prefix agd uses (mgmt.vendor_prefix), set from its schema */
static char req_base[64] = "Device.X_AIGW_.Request.", ev_base[64] = "Device.X_AIGW_.LastEvent.";

static const char *under(const char *base, const char *name)
{
	static char buf[4][PATH_MAX_];
	static int k;

	k = (k + 1) % 4;
	snprintf(buf[k], sizeof(buf[k]), "%s%s", base, name);
	return buf[k];
}
#define REQ(name) under(req_base, name)
#define EV(name)  under(ev_base, name)

static void lp_init(void)
{
	struct blob_attr *cur, *n;
	size_t rem, r2;

	nlp = 0;
	lp_fixed("Device.RootDataModelVersion", "string", "2.18");
	/* every parameter icwmp puts in each Inform must exist: one that fails makes
	   it fail the ones after it in the same Inform */
	lp_add("Device.DeviceInfo.HardwareVersion", "string", 0, K_HWVER);
	lp_uci("Device.DeviceInfo.ProvisioningCode", "string", 1, "cpe", "provisioning_code", "provisioning_code", "", V_STR, 0);
	lp_uci("Device.ManagementServer.URL", "string", 1, "acs", "url", "url", "", V_URL, 0);
	lp_uci("Device.ManagementServer.Username", "string", 1, "acs", "userid", "username", "", V_STR, 0);
	lp_uci("Device.ManagementServer.Password", "string", 1, "acs", "passwd", "password", "", V_STR, 1);
	lp_uci("Device.ManagementServer.PeriodicInformEnable", "boolean", 1, "acs", "periodic_inform_enable",
	       "periodic_enable", "1", V_BOOL, 0);
	lp_uci("Device.ManagementServer.PeriodicInformInterval", "unsignedInt", 1, "acs", "periodic_inform_interval",
	       "periodic_interval", "86400", V_UINT, 0);
	lp_add("Device.ManagementServer.ParameterKey", "string", 0, K_PKEY);
	lp_add("Device.ManagementServer.ConnectionRequestURL", "string", 0, K_CRURL);
	lp_uci("Device.ManagementServer.ConnectionRequestUsername", "string", 1, "cpe", "userid", "cr_username", "", V_STR, 0);
	lp_uci("Device.ManagementServer.ConnectionRequestPassword", "string", 1, "cpe", "passwd", "cr_password", "", V_STR, 1);
	lp_fixed("Device.ManagementServer.UpgradesManaged", "boolean", "false");
	lp_fixed("Device.ManagementServer.AliasBasedAddressing", "boolean", "false");

	/* commands -> Request.: Command, every input (writable), Status, Message, every output */
	lp_add(REQ("Command"), "string", 1, K_REQ_CMD);
	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem) {
			if ((n = field(cur, "in")))
				blobmsg_for_each_attr(n, field(cur, "in"), r2)
					lp_add(REQ(str(n)), "string", 1, K_REQ_IN);
		}
	lp_add(REQ("Status"), "string", 0, K_REQ_STATUS);
	snprintf(lp_find(REQ("Status"))->val, VAL_MAX, "None");
	lp_add(REQ("Message"), "string", 0, K_REQ_MSG);
	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem) {
			if ((n = field(cur, "out")))
				blobmsg_for_each_attr(n, field(cur, "out"), r2)
					lp_add(REQ(str(n)), "string", 0, K_REQ_OUT);
		}

	/* events -> LastEvent.: Name, Time, Count and every argument */
	lp_add(EV("Name"), "string", 0, K_EV_NAME);
	lp_add(EV("Time"), "dateTime", 0, K_EV_TIME);
	snprintf(lp_find(EV("Time"))->val, VAL_MAX, "0001-01-01T00:00:00Z");
	lp_add(EV("Count"), "unsignedInt", 0, K_EV_COUNT);
	if (top(schema, "events"))
		blobmsg_for_each_attr(cur, top(schema, "events"), rem) {
			if ((n = field(cur, "args")))
				blobmsg_for_each_attr(n, field(cur, "args"), r2)
					lp_add(EV(str(n)), "string", 0, K_EV_ARG);
		}
}

/* icwmp's custom notifications: active notification on LastEvent.Count */
static int write_notify(const char *file)
{
	char tmp[PATH_MAX_ + 8];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	if (!(f = fopen(tmp, "w")))
		return -1;
	fprintf(f, "{ \"custom_notification\": [ { \"parameter\": \"%s\", \"notify_type\": \"2\" } ] }\n", EV("Count"));
	if (fclose(f) || rename(tmp, file))
		return -1;
	return 0;
}

/* the WAN device: configured, or netifd's l3_device for the WAN interface */
static void wan_device(char *dev, size_t n)
{
	struct blob_attr *r;
	char obj[96];

	if (wan_dev[0]) {
		snprintf(dev, n, "%s", wan_dev);
		return;
	}
	dev[0] = '\0';
	snprintf(obj, sizeof(obj), "network.interface.%s", wan);
	if ((r = call(obj, "status", NULL))) {
		snprintf(dev, n, "%s", str(top(r, "l3_device")));
		free(r);
	}
}

static int ipv4_of(const char *dev, char *ip, size_t n, int *prefix)
{
	struct ifaddrs *ifa, *i;
	int rc = -1;

	ip[0] = '\0';
	if (!dev[0] || getifaddrs(&ifa))
		return -1;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, dev)) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, (socklen_t)n);
			if (prefix && i->ifa_netmask) {
				uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);

				for (*prefix = 0; m & 0x80000000u; m <<= 1)
					(*prefix)++;
			}
			rc = 0;
			break;
		}
	freeifaddrs(ifa);
	return rc;
}

static void lp_value(const struct lparam *p, char *buf, size_t n)
{
	char dev[IFNAMSIZ], ip[64], port[16], path[128];

	buf[0] = '\0';
	switch (p->kind) {
	case K_UCI:
		if (p->secret)
			return;                 /* passwords read as empty (TR-069) */
		if (uci_get_opt(cwmp_confdir, "cwmp", p->sec, p->opt, buf, n) < 0 || !buf[0])
			snprintf(buf, n, "%s", p->def);
		if (p->check == V_BOOL)
			snprintf(buf, n, "%s", (!strcmp(buf, "1") || !strcasecmp(buf, "true")) ? "true" : "false");
		return;
	case K_PKEY:
		uci_get_opt(cwmp_confdir, "cwmp", "cpe", "ParameterKey", buf, n);
		return;
	case K_HWVER: {
		/* the board: OpenWRT's model name, else the PC's */
		static const char *const src[] = { "/tmp/sysinfo/model", "/sys/class/dmi/id/product_name", NULL };
		FILE *f;
		int i;

		for (i = 0; src[i] && !buf[0]; i++)
			if ((f = fopen(src[i], "r"))) {
				if (!fgets(buf, (int)n, f))
					buf[0] = '\0';
				fclose(f);
				buf[strcspn(buf, "\r\n")] = '\0';
			}
		return;
	}
	case K_CRURL:
		wan_device(dev, sizeof(dev));
		if (ipv4_of(dev, ip, sizeof(ip), NULL) < 0)
			return;
		if (uci_get_opt(cwmp_confdir, "cwmp", "cpe", "port", port, sizeof(port)) < 0 || !port[0])
			snprintf(port, sizeof(port), "7547");
		uci_get_opt(cwmp_confdir, "cwmp", "cpe", "path", path, sizeof(path));
		snprintf(buf, n, "http://%s:%s/%s", ip, port, path[0] == '/' ? path + 1 : path);
		return;
	case K_EV_COUNT:
		snprintf(buf, n, "%u", ev_count);
		return;
	default:
		snprintf(buf, n, "%s", p->val);
	}
}

static int lp_check(const struct lparam *p, const char *v, char *err, size_t en)
{
	const char *s;

	if (!p->writable) {
		snprintf(err, en, "%.200s is not writable", p->path);
		return F_READONLY;
	}
	if (strlen(v) > 256 || strchr(v, '\n')) {
		snprintf(err, en, "at most 256 characters, one line");
		return F_VALUE;
	}
	if (p->kind == K_REQ_CMD) {
		char cmd[PATH_MAX_];

		if (*v && resolve_cmd(v, cmd, sizeof(cmd)) < 0) {
			snprintf(err, en, "no such command: %.200s", v);
			return F_VALUE;
		}
		return 0;
	}
	if (p->kind != K_UCI)
		return 0;
	switch (p->check) {
	case V_BOOL:
		if (strcmp(v, "0") && strcmp(v, "1") && strcasecmp(v, "true") && strcasecmp(v, "false")) {
			snprintf(err, en, "a boolean");
			return F_VALUE;
		}
		break;
	case V_UINT:
		for (s = v; isdigit((unsigned char)*s); s++)
			;
		if (!*v || *s || strtoul(v, NULL, 10) < 1) {
			snprintf(err, en, "a number of seconds, at least 1");
			return F_VALUE;
		}
		break;
	case V_URL:
		if (*v && strncasecmp(v, "http://", 7) && strncasecmp(v, "https://", 8)) {
			snprintf(err, en, "an http:// or https:// URL");
			return F_VALUE;
		}
		break;
	}
	return 0;
}

/* ---------- pending writes: checked at set, applied at commit ---------- */

static struct {
	char path[PATH_MAX_];
	char val[VAL_MAX];
	int local;
} pend[PEND_MAX];
static int npend;
static time_t pend_time;

static void pend_add(const char *path, const char *v, int local)
{
	int i;

	for (i = 0; i < npend; i++)
		if (!strcmp(pend[i].path, path))
			break;
	if (i == npend) {
		if (npend >= PEND_MAX)
			return;
		npend++;
	}
	snprintf(pend[i].path, sizeof(pend[i].path), "%s", path);
	snprintf(pend[i].val, sizeof(pend[i].val), "%s", v);
	pend[i].local = local;
	pend_time = time(NULL);
}

static void reload_done(struct ubus_request *req, int ret)
{
	free(req);
}

static struct uloop_timeout reload_timer;

/* icwmp re-reads its config; during a session it does that when the session ends */
static void reload_icwmp(struct uloop_timeout *t)
{
	struct ubus_request *req = calloc(1, sizeof(*req));
	struct blob_buf b = { 0 };
	uint32_t id;

	(void)t;
	if (!req)
		return;
	if (ubus_lookup_id(ctx, "tr069", &id)) {
		free(req);
		return;
	}
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "command", "reload");
	if (ubus_invoke_async(ctx, id, "command", b.head, req) == 0) {
		req->complete_cb = reload_done;
		ubus_complete_request_async(ctx, req);
	} else {
		free(req);
	}
	blob_buf_free(&b);
}

static void run_request(const char *cmdval)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r, *c, *cur, *o;
	char cmd[PATH_MAX_];
	struct lparam *st = lp_find(REQ("Status")), *msg = lp_find(REQ("Message"));
	size_t rem;
	void *t;
	int i, ok;

	for (i = 0; i < nlp; i++)
		if (lp[i].kind == K_REQ_OUT)
			lp[i].val[0] = '\0';
	if (resolve_cmd(cmdval, cmd, sizeof(cmd)) < 0 || !(c = schema_cmd(cmd))) {
		snprintf(st->val, VAL_MAX, "Error");
		snprintf(msg->val, VAL_MAX, "no such command: %.200s", cmdval);
		return;
	}
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	blobmsg_add_string(&b, "command", cmd);
	t = blobmsg_open_table(&b, "args");
	if (field(c, "in"))
		blobmsg_for_each_attr(cur, field(c, "in"), rem) {
			struct lparam *in;

			if ((in = lp_find(REQ(str(cur)))) && in->val[0])
				blobmsg_add_string(&b, str(cur), in->val);
		}
	blobmsg_close_table(&b, t);
	r = call("ag.dm", "operate", b.head);
	blob_buf_free(&b);
	ok = r && top(r, "ok") && blobmsg_get_u8(top(r, "ok"));
	snprintf(st->val, VAL_MAX, "%s", ok ? "Done" : !r ? "Error" : !strcmp(str(top(r, "code")), "refused") ? "Refused" : "Error");
	snprintf(msg->val, VAL_MAX, "%s", !r ? "AIGW OS (agd) is not answering" : ok ? "" : str(top(r, "message")));
	if (ok && (o = top(r, "out")))
		blobmsg_for_each_attr(cur, o, rem) {
			struct lparam *op;

			if ((op = lp_find(REQ(blobmsg_name(cur)))))
				snprintf(op->val, VAL_MAX, "%s", str(cur));
		}
	/* inputs are used once */
	for (i = 0; i < nlp; i++)
		if (lp[i].kind == K_REQ_IN || lp[i].kind == K_REQ_CMD)
			lp[i].val[0] = '\0';
	AG_INFO("request %s: %s%s%s", cmd, st->val, msg->val[0] ? " -- " : "", msg->val);
	free(r);
}

static void commit(void)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r;
	const char *cmd = NULL;
	int i, ndm = 0, cwmp_changed = 0;
	void *t;

	/* agd's values first, all in one set */
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	t = blobmsg_open_table(&b, "params");
	for (i = 0; i < npend; i++)
		if (!pend[i].local) {
			blobmsg_add_string(&b, pend[i].path, pend[i].val);
			ndm++;
		}
	blobmsg_close_table(&b, t);
	if (ndm) {
		r = call("ag.dm", "set", b.head);
		if (!r || !top(r, "ok") || !blobmsg_get_u8(top(r, "ok")))
			AG_WARN("commit: agd did not take the %d value(s) it had checked", ndm);
		free(r);
	}
	blob_buf_free(&b);
	for (i = 0; i < npend; i++) {
		struct lparam *p;

		if (!pend[i].local || !(p = lp_find(pend[i].path)))
			continue;
		if (p->kind == K_UCI) {
			const char *v = pend[i].val;

			if (p->check == V_BOOL)
				v = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? "1" : "0";
			if (uci_set_opt(cwmp_confdir, "cwmp", p->sec, p->opt, v) < 0)
				AG_WARN("could not save %s in icwmp's config", p->path);
			if (p->aigw_opt && uci_set_opt(aigw_confdir(), "aigw", "cwmp", p->aigw_opt, v) < 0)
				AG_WARN("could not save %s in /etc/config/aigw", p->path);
			cwmp_changed = 1;
			AG_INFO("%s set by the ACS", p->path);
		} else if (p->kind == K_REQ_CMD) {
			cmd = pend[i].val;
		} else if (p->kind == K_REQ_IN) {
			snprintf(p->val, VAL_MAX, "%s", pend[i].val);
		}
	}
	if (cmd && *cmd)
		run_request(cmd);
	if (cwmp_changed) {
		reload_timer.cb = reload_icwmp;
		uloop_timeout_set(&reload_timer, 200);
	}
	npend = 0;
}

/* ---------- names (GetParameterNames) ---------- */

struct name {
	char path[PATH_MAX_];
	const char *type;
	int writable;
};
static struct name *names;
static int nnames, cap;

static void name_add(const char *path, const char *type, int writable)
{
	if (nnames >= cap) {
		struct name *n = realloc(names, sizeof(*names) * (size_t)(cap ? cap * 2 : 256));

		if (!n)
			return;
		names = n;
		cap = cap ? cap * 2 : 256;
	}
	snprintf(names[nnames].path, PATH_MAX_, "%s", path);
	names[nnames].type = type;
	names[nnames].writable = writable;
	nnames++;
}

/* every object above a name ("Device.", "Device.Hosts.", ...) */
static void parents_add(const char *path)
{
	char p[PATH_MAX_];
	const char *s;

	for (s = strchr(path, '.'); s; s = strchr(s + 1, '.')) {
		if (!s[1])
			break;              /* the name itself is an object: added by the caller */
		snprintf(p, sizeof(p), "%.*s", (int)(s - path + 1), path);
		name_add(p, "object", 0);
	}
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(((const struct name *)a)->path, ((const struct name *)b)->path);
}

/* the instances agd has under a table template, e.g. Device.Hosts.Host.{i}. */
static struct blob_attr *instances(const char *tmpl)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r;
	char q[PATH_MAX_];
	const char *ip = strstr(tmpl, "{i}");

	snprintf(q, sizeof(q), "%.*s", ip ? (int)(ip - tmpl) : (int)strlen(tmpl), tmpl);
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "path", q);
	r = call("ag.dm", "instances", b.head);
	blob_buf_free(&b);
	return r;
}

static void expand(const char *tmpl, const char *type, int writable, int templates, struct blob_attr *inst)
{
	struct blob_attr *cur;
	const char *ip = strstr(tmpl, "{i}");
	size_t rem;

	if (!ip || templates) {
		parents_add(tmpl);
		name_add(tmpl, type, writable);
		return;
	}
	if (!inst || !top(inst, "instances"))
		return;
	blobmsg_for_each_attr(cur, top(inst, "instances"), rem) {
		const char *ins = str(cur);         /* "Device.Hosts.Host.3." */
		char p[PATH_MAX_];
		size_t pre = (size_t)(ip - tmpl);

		if (strncmp(ins, tmpl, pre))
			continue;
		snprintf(p, sizeof(p), "%s%s", ins, ip + 4);   /* the rest after "{i}." */
		parents_add(p);
		name_add(p, type, writable);
	}
}

/* every name in the data model; templates: {i} kept (proto usp) */
static void names_build(int templates)
{
	struct blob_attr *cur, *objs = top(schema, "objects");
	struct blob_attr *inst[16] = { 0 };
	const char *tmpl[16];
	size_t rem;
	int i, j, nt = 0, k;

	nnames = 0;
	if (objs)
		blobmsg_for_each_attr(cur, objs, rem)
			if (nt < 16) {
				tmpl[nt] = str(field(cur, "path"));
				inst[nt] = templates ? NULL : instances(tmpl[nt]);
				expand(tmpl[nt], "object", 0, templates, inst[nt]);
				if (!templates) {
					/* the table object itself: Device.Hosts.Host. */
					char t[PATH_MAX_];
					const char *ip = strstr(tmpl[nt], "{i}");

					snprintf(t, sizeof(t), "%.*s", (int)(ip - tmpl[nt]), tmpl[nt]);
					parents_add(t);
					name_add(t, "object", 0);
				}
				nt++;
			}
	if (top(schema, "params"))
		blobmsg_for_each_attr(cur, top(schema, "params"), rem) {
			const char *p = str(field(cur, "path"));
			struct blob_attr *w = field(cur, "writable");
			struct blob_attr *in = NULL;

			for (j = 0; j < nt; j++) {
				const char *ip = strstr(tmpl[j], "{i}");

				if (ip && !strncmp(p, tmpl[j], (size_t)(ip - tmpl[j]) + 4))
					in = inst[j];
			}
			expand(p, str(field(cur, "type")), w && blobmsg_get_u8(w), templates, in);
		}
	for (i = 0; i < nlp; i++) {
		parents_add(lp[i].path);
		name_add(lp[i].path, lp[i].type, lp[i].writable);
	}
	for (j = 0; j < nt; j++)
		free(inst[j]);
	qsort(names, (size_t)nnames, sizeof(*names), name_cmp);
	for (i = 0, k = 0; i < nnames; i++)
		if (!k || strcmp(names[k - 1].path, names[i].path))
			names[k++] = names[i];
	nnames = k;
}

/* ---------- bbfdm: what icwmp calls ---------- */

static struct blob_buf rb;

enum { A_PATH, A_VALUE, A_FIRST, A_OPT, __A_MAX };
static const struct blobmsg_policy a_policy[__A_MAX] = {
	[A_PATH] = { "path", BLOBMSG_TYPE_STRING },
	[A_VALUE] = { "value", BLOBMSG_TYPE_STRING },
	[A_FIRST] = { "first_level", BLOBMSG_TYPE_BOOL },
	[A_OPT] = { "optional", BLOBMSG_TYPE_TABLE },
};

static const char *req_path(struct blob_attr **tb)
{
	const char *p = tb[A_PATH] ? blobmsg_get_string(tb[A_PATH]) : "";

	return *p ? p : "Device.";
}

static void result_fault(const char *path, int fault, const char *msg)
{
	void *a = blobmsg_open_array(&rb, "results"), *t = blobmsg_open_table(&rb, NULL);

	blobmsg_add_string(&rb, "path", path);
	blobmsg_add_u32(&rb, "fault", (uint32_t)fault);
	if (msg)
		blobmsg_add_string(&rb, "fault_msg", msg);
	blobmsg_close_table(&rb, t);
	blobmsg_close_array(&rb, a);
}

static void result_value(const char *path, const char *val, const char *type)
{
	void *t = blobmsg_open_table(&rb, NULL);

	blobmsg_add_string(&rb, "path", path);
	blobmsg_add_string(&rb, "data", val);
	blobmsg_add_string(&rb, "type", xsd(type));
	blobmsg_close_table(&rb, t);
}

static int m_get(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__A_MAX], *r, *vals, *cur;
	const char *q;
	size_t rem;
	void *a;
	int i, found = 0, agd_ok;

	blobmsg_parse(a_policy, __A_MAX, tb, blob_data(msg), blob_len(msg));
	q = req_path(tb);
	blob_buf_init(&rb, 0);
	if (strchr(q, '*') || strchr(q, '[')) {
		result_fault(q, F_NAME, "wildcards and aliases are not supported");
		return ubus_send_reply(c, req, rb.head);
	}
	r = dm_get(q);
	agd_ok = r && !field(top(r, "errors"), q);
	vals = r ? top(r, "values") : NULL;
	a = blobmsg_open_array(&rb, "results");
	if (agd_ok && vals)
		blobmsg_for_each_attr(cur, vals, rem) {
			struct blob_attr *sp = schema_param(blobmsg_name(cur));

			result_value(blobmsg_name(cur), str(cur), sp ? str(field(sp, "type")) : "string");
			found++;
		}
	for (i = 0; i < nlp; i++)
		if (ends_dot(q) ? !strncmp(lp[i].path, q, strlen(q)) : !strcmp(lp[i].path, q)) {
			char v[VAL_MAX];

			lp_value(&lp[i], v, sizeof(v));
			result_value(lp[i].path, v, lp[i].type);
			found++;
		}
	blobmsg_close_array(&rb, a);
	free(r);
	if (!found && !agd_ok) {
		blob_buf_init(&rb, 0);
		result_fault(q, r ? F_NAME : F_INTERNAL, r ? "no such parameter" : "AIGW OS (agd) is not answering");
	}
	return ubus_send_reply(c, req, rb.head);
}

static int m_schema(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__A_MAX], *o[1];
	static const struct blobmsg_policy op = { "proto", BLOBMSG_TYPE_STRING };
	const char *q;
	size_t ql;
	void *a;
	int i, first, templates = 0, found = 0;

	blobmsg_parse(a_policy, __A_MAX, tb, blob_data(msg), blob_len(msg));
	first = tb[A_FIRST] && blobmsg_get_bool(tb[A_FIRST]);
	if (tb[A_OPT]) {
		blobmsg_parse(&op, 1, o, blobmsg_data(tb[A_OPT]), blobmsg_len(tb[A_OPT]));
		templates = o[0] && !strcmp(blobmsg_get_string(o[0]), "usp");
	}
	blob_buf_init(&rb, 0);
	/* an empty path with NextLevel: just the root object */
	if ((!tb[A_PATH] || !*blobmsg_get_string(tb[A_PATH])) && first) {
		a = blobmsg_open_array(&rb, "results");
		result_value("Device.", "0", "object");
		blobmsg_close_array(&rb, a);
		return ubus_send_reply(c, req, rb.head);
	}
	q = req_path(tb);
	ql = strlen(q);
	if (!ends_dot(q) && first) {
		result_fault(q, F_ARGS, "NextLevel true needs an object path");
		return ubus_send_reply(c, req, rb.head);
	}
	names_build(templates);
	a = blobmsg_open_array(&rb, "results");
	for (i = 0; i < nnames; i++) {
		const char *p = names[i].path, *rest;

		if (!ends_dot(q)) {
			if (strcmp(p, q))
				continue;
		} else {
			if (strncmp(p, q, ql))
				continue;
			rest = p + ql;
			if (first) {
				const char *dot = strchr(rest, '.');

				/* direct children only: "Name" or "Name." */
				if (!*rest || (dot && dot[1]))
					continue;
			}
		}
		result_value(p, names[i].writable ? "1" : "0", names[i].type);
		found++;
	}
	blobmsg_close_array(&rb, a);
	if (!found) {
		blob_buf_init(&rb, 0);
		result_fault(q, F_NAME, "no such object or parameter");
	}
	return ubus_send_reply(c, req, rb.head);
}

static int m_set(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__A_MAX];
	struct lparam *p;
	const char *q, *v;
	char err[240] = "";
	int fault = 0;
	void *a, *t;

	blobmsg_parse(a_policy, __A_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[A_PATH] || !tb[A_VALUE])
		return UBUS_STATUS_INVALID_ARGUMENT;
	q = blobmsg_get_string(tb[A_PATH]);
	v = blobmsg_get_string(tb[A_VALUE]);
	/* left over from a SetParameterValues that never finished */
	if (npend && time(NULL) - pend_time > 60)
		npend = 0;
	if ((p = lp_find(q))) {
		if (!(fault = lp_check(p, v, err, sizeof(err))))
			pend_add(q, v, 1);
	} else if (schema_param(q)) {
		struct blob_buf b = { 0 };
		struct blob_attr *r, *e;

		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "origin", ORIGIN);
		blobmsg_add_u8(&b, "check", 1);
		t = blobmsg_open_table(&b, "params");
		blobmsg_add_string(&b, q, v);
		blobmsg_close_table(&b, t);
		r = call("ag.dm", "set", b.head);
		blob_buf_free(&b);
		if (!r) {
			fault = F_INTERNAL;
			snprintf(err, sizeof(err), "AIGW OS (agd) is not answering");
		} else if ((e = field(top(r, "errors"), q))) {
			fault = cwmp_fault(str(field(e, "code")));
			snprintf(err, sizeof(err), "%.230s", str(field(e, "message")));
		} else {
			pend_add(q, v, 0);
		}
		free(r);
	} else {
		fault = F_NAME;
		snprintf(err, sizeof(err), "no such parameter");
	}
	blob_buf_init(&rb, 0);
	if (fault) {
		AG_INFO("set %s refused (%d): %s", q, fault, err);
		result_fault(q, fault, err);
	} else {
		a = blobmsg_open_array(&rb, "results");
		t = blobmsg_open_table(&rb, NULL);
		blobmsg_add_string(&rb, "path", q);
		blobmsg_add_string(&rb, "data", "1");
		blobmsg_close_table(&rb, t);
		blobmsg_close_array(&rb, a);
	}
	/* so icwmp commits (or reverts) through bbf.config when the RPC ends */
	a = blobmsg_open_array(&rb, "modified_uci");
	blobmsg_add_string(&rb, NULL, "/etc/config/aigw");
	blobmsg_close_array(&rb, a);
	return ubus_send_reply(c, req, rb.head);
}

static int m_addel(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		   const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__A_MAX];

	blobmsg_parse(a_policy, __A_MAX, tb, blob_data(msg), blob_len(msg));
	blob_buf_init(&rb, 0);
	result_fault(req_path(tb), F_NAME, "AIGW OS's objects are not created or deleted by an ACS");
	return ubus_send_reply(c, req, rb.head);
}

/* ---------- firmware: what icwmp asks for a Download (file type 1) ---------- */

/*
 * icwmp downloads the image itself, checks it with "sysupgrade -T", then:
 *   fwbank dump                      picks an image that isn't running
 *   bbfdm operate FirmwareImage.N.Download() {URL: file://...}
 *   fwbank set_bootbank {bank: N}    makes it boot next
 *   system reboot, and after the boot a TransferComplete
 * Download() and Activate() are agd's (its firmware handler); the bank calls
 * are answered from Device.DeviceInfo.FirmwareImage.
 */
#define FW_TIMEOUT (10 * 60 * 1000)
#define FW_PREFIX  "Device.DeviceInfo.FirmwareImage."

enum { O_PATH, O_INPUT, __O_MAX };
static const struct blobmsg_policy o_policy[__O_MAX] = {
	[O_PATH] = { "path", BLOBMSG_TYPE_STRING },
	[O_INPUT] = { "input", BLOBMSG_TYPE_TABLE },
};

/* icwmp looks for a fault inside each result's output */
static void op_result(const char *path, int fault, const char *msg)
{
	void *a = blobmsg_open_array(&rb, "results"), *t = blobmsg_open_table(&rb, NULL), *o, *f;

	blobmsg_add_string(&rb, "path", path);
	o = blobmsg_open_array(&rb, "output");
	if (fault) {
		f = blobmsg_open_table(&rb, NULL);
		blobmsg_add_u32(&rb, "fault", (uint32_t)fault);
		blobmsg_add_string(&rb, "fault_msg", msg ? msg : "");
		blobmsg_close_table(&rb, f);
	}
	blobmsg_close_array(&rb, o);
	blobmsg_close_table(&rb, t);
	blobmsg_close_array(&rb, a);
}

/* FirmwareImage.N.Download() / Activate(): agd's answer, or a fault message */
static int fw_operate(const char *path, struct blob_attr *input, char *err, size_t en)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r, *cur;
	size_t rem;
	void *t;
	int ok;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	blobmsg_add_string(&b, "command", path);
	t = blobmsg_open_table(&b, "args");
	if (input)
		blobmsg_for_each_attr(cur, input, rem) {
			char v[32];

			if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				blobmsg_add_string(&b, blobmsg_name(cur), blobmsg_get_string(cur));
			else if (blobmsg_type(cur) == BLOBMSG_TYPE_BOOL) {
				blobmsg_add_string(&b, blobmsg_name(cur), blobmsg_get_bool(cur) ? "true" : "false");
			} else if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32) {
				snprintf(v, sizeof(v), "%u", blobmsg_get_u32(cur));
				blobmsg_add_string(&b, blobmsg_name(cur), v);
			}
		}
	blobmsg_close_table(&b, t);
	r = call_t("ag.dm", "operate", b.head, FW_TIMEOUT);
	blob_buf_free(&b);
	ok = r && top(r, "ok") && blobmsg_get_u8(top(r, "ok"));
	snprintf(err, en, "%s", !r ? "AIGW OS (agd) is not answering" : str(top(r, "message")));
	free(r);
	return ok ? 0 : -1;
}

static int m_operate(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		     const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__O_MAX];
	const char *p;
	char err[256];
	size_t l;

	blobmsg_parse(o_policy, __O_MAX, tb, blob_data(msg), blob_len(msg));
	p = tb[O_PATH] ? blobmsg_get_string(tb[O_PATH]) : "";
	l = strlen(p);
	blob_buf_init(&rb, 0);
	if (!strncmp(p, FW_PREFIX, strlen(FW_PREFIX)) &&
	    ((l > 11 && !strcmp(p + l - 11, ".Download()")) || (l > 11 && !strcmp(p + l - 11, ".Activate()")))) {
		AG_INFO("%s asked by the ACS", p);
		if (fw_operate(p, tb[O_INPUT], err, sizeof(err)) == 0)
			op_result(p, 0, NULL);
		else {
			AG_WARN("%s: %s", p, err);
			op_result(p, 9010, err);        /* download failure */
		}
		return ubus_send_reply(c, req, rb.head);
	}
	snprintf(err, sizeof(err), "not supported: use %s", req_base);
	op_result(p, F_ARGS, err);
	return ubus_send_reply(c, req, rb.head);
}

/* icwmp's session: kept in flash before an image is activated, so the
   TransferComplete survives the reboot (aigw-cwmp-run puts it back) */
static void keep_session(void)
{
	static const char *const files[] = { "icwmpd_backup_session.xml", "dm_enabled_notify",
					     "icwmpd_notify_import_marker", NULL };
	const char *run = getenv("AIGW_ICWMP_RUN"), *keep = getenv("AIGW_ICWMP_KEEP");
	char src[256], dst[256], buf[4096];
	int i;

	run = run && *run ? run : "/var/run/icwmpd";
	keep = keep && *keep ? keep : "/etc/icwmpd";
	mkdir(keep, 0700);
	for (i = 0; files[i]; i++) {
		FILE *in, *o;
		size_t n;

		snprintf(src, sizeof(src), "%s/%s", run, files[i]);
		snprintf(dst, sizeof(dst), "%s/%s", keep, files[i]);
		if (!(in = fopen(src, "rb")))
			continue;
		if ((o = fopen(dst, "wb"))) {
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				if (fwrite(buf, 1, n, o) != n)
					break;
			fclose(o);
		}
		fclose(in);
	}
}

static int m_fw_dump(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		     const char *method, struct blob_attr *msg)
{
	struct blob_buf b = { 0 };
	struct blob_attr *inst, *vals = NULL, *r = NULL, *cur;
	char active[96] = "", boot[96] = "";
	size_t rem;
	void *a, *t;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "path", FW_PREFIX);
	inst = call("ag.dm", "instances", b.head);
	blob_buf_free(&b);
	if ((r = dm_get("Device.DeviceInfo.")))
		vals = top(r, "values");
	snprintf(active, sizeof(active), "%s.", str(field(vals, "Device.DeviceInfo.ActiveFirmwareImage")));
	snprintf(boot, sizeof(boot), "%s.", str(field(vals, "Device.DeviceInfo.BootFirmwareImage")));
	blob_buf_init(&rb, 0);
	a = blobmsg_open_array(&rb, "bank");
	if (inst && top(inst, "instances"))
		blobmsg_for_each_attr(cur, top(inst, "instances"), rem) {
			const char *p = str(cur);       /* Device.DeviceInfo.FirmwareImage.N. */
			char k[160];
			const char *ver;

			t = blobmsg_open_table(&rb, NULL);
			snprintf(k, sizeof(k), "%sName", p);
			blobmsg_add_string(&rb, "name", str(field(vals, k)));
			blobmsg_add_u32(&rb, "id", (uint32_t)strtoul(p + strlen(FW_PREFIX), NULL, 10));
			blobmsg_add_u8(&rb, "active", !strcmp(p, active));
			blobmsg_add_u8(&rb, "boot", !strcmp(p, boot));
			blobmsg_add_u8(&rb, "upgrade", 0);
			snprintf(k, sizeof(k), "%sVersion", p);
			ver = str(field(vals, k));
			blobmsg_add_string(&rb, "fwver", ver);
			blobmsg_add_string(&rb, "swver", ver);
			snprintf(k, sizeof(k), "%sStatus", p);
			blobmsg_add_string(&rb, "status", str(field(vals, k)));
			blobmsg_close_table(&rb, t);
		}
	blobmsg_close_array(&rb, a);
	free(inst);
	free(r);
	return ubus_send_reply(c, req, rb.head);
}

enum { B_BANK, __B_MAX };
static const struct blobmsg_policy b_policy[__B_MAX] = {
	[B_BANK] = { "bank", BLOBMSG_TYPE_INT32 },
};

static int m_fw_bootbank(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
			 const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__B_MAX];
	char path[96], err[256];
	int ok = 0;

	blobmsg_parse(b_policy, __B_MAX, tb, blob_data(msg), blob_len(msg));
	blob_buf_init(&rb, 0);
	if (tb[B_BANK]) {
		snprintf(path, sizeof(path), FW_PREFIX "%u.Activate()", blobmsg_get_u32(tb[B_BANK]));
		keep_session();
		AG_INFO("%s asked by the ACS", path);
		ok = fw_operate(path, NULL, err, sizeof(err)) == 0;
		if (!ok)
			AG_WARN("%s: %s", path, err);
	}
	blobmsg_add_u8(&rb, "success", ok);
	return ubus_send_reply(c, req, rb.head);
}

static const struct ubus_method fw_methods[] = {
	UBUS_METHOD_NOARG("dump", m_fw_dump),
	UBUS_METHOD("set_bootbank", m_fw_bootbank, b_policy),
};
static struct ubus_object_type fw_type = UBUS_OBJECT_TYPE("fwbank", fw_methods);
static struct ubus_object fw_obj = {
	.name = "fwbank", .type = &fw_type, .methods = fw_methods, .n_methods = ARRAY_SIZE(fw_methods),
};

static const struct ubus_method bbfdm_methods[] = {
	UBUS_METHOD("get", m_get, a_policy),
	UBUS_METHOD("schema", m_schema, a_policy),
	UBUS_METHOD("set", m_set, a_policy),
	UBUS_METHOD("add", m_addel, a_policy),
	UBUS_METHOD("del", m_addel, a_policy),
	UBUS_METHOD("operate", m_operate, o_policy),
};
static struct ubus_object_type bbfdm_type = UBUS_OBJECT_TYPE("bbfdm", bbfdm_methods);
static struct ubus_object bbfdm_obj = {
	.name = "bbfdm", .type = &bbfdm_type, .methods = bbfdm_methods, .n_methods = ARRAY_SIZE(bbfdm_methods),
};

static int m_commit(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		    const char *method, struct blob_attr *msg)
{
	if (!strcmp(method, "commit"))
		commit();
	else
		npend = 0;
	blob_buf_init(&rb, 0);
	blobmsg_add_u8(&rb, "status", 1);
	return ubus_send_reply(c, req, rb.head);
}

static const struct ubus_method cfg_methods[] = {
	UBUS_METHOD_NOARG("commit", m_commit),
	UBUS_METHOD_NOARG("revert", m_commit),
};
static struct ubus_object_type cfg_type = UBUS_OBJECT_TYPE("bbf.config", cfg_methods);
static struct ubus_object cfg_obj = {
	.name = "bbf.config", .type = &cfg_type, .methods = cfg_methods, .n_methods = ARRAY_SIZE(cfg_methods),
};

/* ---------- network.interface.<wan>, on a PC ---------- */

static int m_ifstatus(struct ubus_context *c, struct ubus_object *obj, struct ubus_request_data *req,
		      const char *method, struct blob_attr *msg)
{
	char ip[64];
	int prefix = 0;
	void *a, *t;

	blob_buf_init(&rb, 0);
	blobmsg_add_u8(&rb, "up", ipv4_of(wan_dev, ip, sizeof(ip), &prefix) == 0);
	blobmsg_add_string(&rb, "l3_device", wan_dev);
	blobmsg_add_string(&rb, "device", wan_dev);
	a = blobmsg_open_array(&rb, "ipv4-address");
	if (ip[0]) {
		t = blobmsg_open_table(&rb, NULL);
		blobmsg_add_string(&rb, "address", ip);
		blobmsg_add_u32(&rb, "mask", (uint32_t)prefix);
		blobmsg_close_table(&rb, t);
	}
	blobmsg_close_array(&rb, a);
	return ubus_send_reply(c, req, rb.head);
}

static const struct ubus_method if_methods[] = {
	UBUS_METHOD_NOARG("status", m_ifstatus),
};
static char if_name[96];
static struct ubus_object_type if_type = UBUS_OBJECT_TYPE("aigw-cwmp-iface", if_methods);
static struct ubus_object if_obj = {
	.type = &if_type, .methods = if_methods, .n_methods = ARRAY_SIZE(if_methods),
};

/* ---------- agd's events -> LastEvent. ---------- */

static void on_event(struct ubus_context *c, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg)
{
	struct blob_attr *name = top(msg, "event"), *args = top(msg, "args"), *cur;
	size_t rem;
	int i;

	(void)c;
	(void)ev;
	(void)type;
	if (!name || !*str(name))
		return;
	for (i = 0; i < nlp; i++)
		if (lp[i].kind == K_EV_ARG)
			lp[i].val[0] = '\0';
	snprintf(lp_find(EV("Name"))->val, VAL_MAX, "%s", str(name));
	fmt_now(lp_find(EV("Time"))->val, VAL_MAX);
	if (args)
		blobmsg_for_each_attr(cur, args, rem) {
			struct lparam *p;

			if ((p = lp_find(EV(blobmsg_name(cur)))) && p->kind == K_EV_ARG)
				snprintf(p->val, VAL_MAX, "%s", str(cur));
		}
	ev_count++;                     /* last: an ACS watching Count reads a complete event */
	AG_DEBUG("event %s (%u)", str(name), ev_count);
}

static struct ubus_event_handler ev_handler = { .cb = on_event };

/* ---------- main ---------- */

static void read_config(void)
{
	char v[64];

	if (uci_get_opt(aigw_confdir(), "aigw", "cwmp", "wan", v, sizeof(v)) == 0 && v[0])
		snprintf(wan, sizeof(wan), "%s", v);
	if (uci_get_opt(aigw_confdir(), "aigw", "cwmp", "wan_device", v, sizeof(v)) == 0)
		snprintf(wan_dev, sizeof(wan_dev), "%.*s", IFNAMSIZ - 1, v);
	if (uci_get_opt(aigw_confdir(), "aigw", "main", "log_level", v, 16) == 0 && v[0])
		ag_log_init("aigw-cwmp", ag_log_level_parse(v));
}

int main(int argc, char **argv)
{
	const char *notify = NULL, *vx;
	uint32_t id;
	int opt, i;

	ag_log_init("aigw-cwmp", AG_LOG_INFO);
	ubus_path = getenv("AIGW_UBUS");
	while ((opt = getopt(argc, argv, "s:n:")) != -1)
		if (opt == 's')
			ubus_path = optarg;
		else if (opt == 'n')
			notify = optarg;
	if (ubus_path && !*ubus_path)
		ubus_path = NULL;
	cwmp_confdir = getenv("AIGW_CWMP_CONFDIR");
	if (cwmp_confdir && !*cwmp_confdir)
		cwmp_confdir = NULL;
	read_config();

	/* agd may still be starting */
	for (i = 0; i < 60 && !(schema = call("ag.dm", "schema", NULL)); i++)
		sleep(1);
	if (!schema) {
		AG_ERR("no AIGW OS data model on ubus (is agd running?)");
		return 1;
	}
	vx = str(top(schema, "vendor_prefix"));
	if (*vx) {
		snprintf(req_base, sizeof(req_base), "Device.%.40s.Request.", vx);
		snprintf(ev_base, sizeof(ev_base), "Device.%.40s.LastEvent.", vx);
	}
	lp_init();
	if (notify && write_notify(notify) < 0)
		AG_WARN("cannot write %s", notify);

	uloop_init();
	if (!(ctx = ubus_connect(ubus_path))) {
		AG_ERR("cannot connect to ubus");
		return 1;
	}
	ubus_add_uloop(ctx);
	if (ubus_add_object(ctx, &bbfdm_obj) || ubus_add_object(ctx, &cfg_obj)) {
		AG_ERR("cannot publish bbfdm / bbf.config (is another data model daemon, bbfdmd, running?)");
		return 1;
	}
	/* firmware banks, unless the platform has its own (iopsys fwbank) */
	if (ubus_lookup_id(ctx, "fwbank", &id) && ubus_add_object(ctx, &fw_obj))
		AG_WARN("cannot publish fwbank: firmware downloads from the ACS won't work");
	snprintf(if_name, sizeof(if_name), "network.interface.%s", wan);
	if (wan_dev[0] && ubus_lookup_id(ctx, if_name, &id)) {
		if_obj.name = if_name;
		if (ubus_add_object(ctx, &if_obj))
			AG_WARN("cannot publish %s", if_name);
	}
	if (ubus_register_event_handler(ctx, &ev_handler, "ag.dm.event"))
		AG_WARN("cannot follow AIGW OS events");
	AG_INFO("AIGW OS data model %s for TR-069: %d of its own parameters; WAN %s%s%s",
		str(top(schema, "version")), nlp, wan, wan_dev[0] ? " on " : "", wan_dev);
	uloop_run();
	ubus_free(ctx);
	if (out)
		ubus_free(out);
	uloop_done();
	return 0;
}
