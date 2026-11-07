/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Plug-in host (phase 4, "plug & IoT"): sensors, actuators, skills and
 * connectors that wire themselves in, with no configuration.
 *
 *   drop a folder with a manifest.json into a plug-in directory
 *     (a package does this: opkg install aigw-mqtt; or: agctl plugin add <dir>)
 *        │  found within seconds
 *        ▼
 *   check ── manifest valid · runs on this host · trusted? · needs met (binary,
 *        │   file, TCP service, another plug-in's events) ── else "waiting"
 *        ▼
 *   wire ─── its events reach the brain and other plug-ins, its actions become
 *        │   decisions (with the usual guardrails), a skill's policies join the
 *        │   policy table
 *        ▼
 *   run ──── a child process speaking JSON lines on stdin/stdout, as an
 *            unprivileged user, restarted when it crashes, stopped when it goes
 *            over its memory or CPU budget
 *
 * Trust: plug-ins in plugins.dir come from packages (installed by root through
 * the package manager) and get the capabilities their manifest declares.
 * Plug-ins added to plugins.user_dir run too, but privileged capabilities
 * (acting as a person, reading the device list, seeing every DNS lookup or
 * connection) wait until you allow them: agctl plugin allow <name>.
 *
 * Protocol (one JSON object per line):
 *   agd -> plug-in   {"hello": {name, version, host, config, granted, denied}}
 *                    {"event": {"type": "ag.wifi", "body": {...}}}
 *                    {"act": {id, action, target, device, ttl, reason}}  {"undo": {...}}
 *                    {"reply": n, "data": {...}, "error": ""}
 *   plug-in -> agd   {"emit": "iot.door", "body": {...}}
 *                    {"alert": {kind, device, subject, reason, severity}}
 *                    {"propose": {action, target, ttl, reason}}     waits for a person
 *                    {"request": {action, target, ttl, reason}}     acts, as a person would
 *                    {"request": {"undo": id}}
 *                    {"done": {id, ok, error}}                      actuator result
 *                    {"query": "devices" | "decisions" | "alerts" | "models" | "status", "id": n}
 *                    {"model": {file, sig, id}}     offer a signed model update (capability
 *                                                   model.offer; files in its own $AIGW_STATE)
 *                    {"log": "text"}
 *
 * Each plug-in runs in its folder with $AIGW_STATE, a directory only it (and
 * root) can write. "needs" may also name a setting ("config:url"): the
 * plug-in waits until you set it (agctl plugin config <name> url <value>).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <uci.h>

#include "agd.h"

#define PLUGIN_MAX     32
#define LIST_MAX       8
#define CONF_MAX       16
#define POLICY_MAX     4
#define RBUF           16384
#define OBUF_MAX       (1024 * 1024)
#define CRASH_LIMIT    5
#define CRASH_WINDOW   600
#define DEFAULT_RAM_KB 16384
#define DEFAULT_CPU    10
#define LOG_PER_MIN    30

enum pstate { P_INVALID, P_UNSUPPORTED, P_DISABLED, P_WAITING, P_RUNNING, P_ACTIVE, P_FAILED };
static const char *const state_names[] = {
	"invalid", "unsupported", "disabled", "waiting", "running", "active", "failed",
};

struct kv { char k[32]; char v[128]; };

struct plugin {
	int used, seen;
	char name[32], kind[12], version[16], description[160];
	char dir[256], exec[300], manifest[300];
	int system;                         /* from plugins.dir (a package) */
	int64_t mtime;                      /* manifest, to notice updates */
	char emits[LIST_MAX][48];       int n_emits;
	char subs[LIST_MAX][48];        int n_subs;
	char actions[LIST_MAX][24];     int n_actions;
	uint8_t action_device[LIST_MAX];    /* 1 = target is a device, 0 = free text */
	char caps[LIST_MAX][24];        int n_caps;
	char needs[LIST_MAX][96];       int n_needs;
	char alerts[LIST_MAX][32];      int n_alerts;
	struct kv conf[CONF_MAX];       int n_conf;
	struct ag_policy policies[POLICY_MAX]; int n_policies;
	char card_title[64], card_text[200];
	int ram_kb, cpu_pct;

	int state;
	char why[200];
	int allowed;                        /* you allowed its privileged capabilities */
	int disabled;

	/* process */
	pid_t pid;
	struct uloop_process proc;
	struct uloop_fd out, err, in;
	struct uloop_timeout restart;
	int stopping;
	int64_t stop_at;
	char rbuf[RBUF];  size_t rlen; int rskip;
	char ebuf[1024];  size_t elen;
	char *obuf;       size_t olen, ocap;
	int64_t started;
	int64_t crashes[CRASH_LIMIT];
	unsigned restarts;
	int log_min, log_n;

	/* budget */
	unsigned long rss_kb;
	double cpu_now;
	unsigned long long cpu_ticks;
	uint64_t cpu_at_us;
	int over_cpu;

	uint64_t events_in, events_out, dropped;
	int64_t last_event;
};

static struct plugin plugins[PLUGIN_MAX];

static void evaluate(struct plugin *p);
static void stop(struct plugin *p, int state, const char *why);

/* ---------- small helpers ---------- */

static int trusted(const struct plugin *p)
{
	return p->system || p->allowed;
}

static int priv_cap(const char *cap)
{
	return !strcmp(cap, "decision.request") || !strcmp(cap, "devices.read") || !strcmp(cap, "model.offer");
}

static void state_dir(const struct plugin *p, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", cfg.plugin_state, p->name);
}

static int priv_sub(const char *type)
{
	return !strcmp(type, "*") || !strcmp(type, AG_EV_DNS) || !strcmp(type, AG_EV_FLOW) ||
	       !strcmp(type, "ag.*");
}

static int has_cap(const struct plugin *p, const char *cap)
{
	int i;

	for (i = 0; i < p->n_caps; i++)
		if (!strcmp(p->caps[i], cap))
			return !priv_cap(cap) || trusted(p);
	return 0;
}

/* "iot.mqtt" matches iot.mqtt and iot.mqtt.<anything>; "iot.*" matches iot.<anything>; "*" all */
static int type_match(const char *pattern, const char *type)
{
	size_t n = strlen(pattern);

	if (!strcmp(pattern, "*"))
		return 1;
	if (n >= 2 && pattern[n - 1] == '*' && pattern[n - 2] == '.')
		return !strncmp(pattern, type, n - 1);
	return !strcmp(pattern, type) || (!strncmp(pattern, type, n) && type[n] == '.');
}

static struct plugin *find(const char *name)
{
	int i;

	for (i = 0; i < PLUGIN_MAX; i++)
		if (plugins[i].used && !strcmp(plugins[i].name, name))
			return &plugins[i];
	return NULL;
}

static void lifecycle_event(const struct plugin *p)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "plugin", p->name);
	blobmsg_add_string(&b, "kind", p->kind);
	blobmsg_add_string(&b, "state", state_names[p->state]);
	blobmsg_add_string(&b, "why", p->why);
	agd_emit(AG_EV_PLUGIN, &b);
	blob_buf_free(&b);
}

static void set_state(struct plugin *p, int state, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(struct plugin *p, int state, const char *fmt, ...)
{
	char why[200] = "";
	va_list ap;

	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(why, sizeof(why), fmt, ap);
		va_end(ap);
	}
	if (p->state == state && !strcmp(p->why, why))
		return;
	p->state = state;
	snprintf(p->why, sizeof(p->why), "%s", why);
	AG_INFO("plug-in %s: %s%s%s", p->name, state_names[state], why[0] ? " -- " : "", why);
	lifecycle_event(p);
}

/* ---------- output to the plug-in (non-blocking, buffered) ---------- */

static void flush_out(struct plugin *p)
{
	while (p->olen && p->in.fd >= 0) {
		ssize_t w = write(p->in.fd, p->obuf, p->olen);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				p->olen = 0;    /* plug-in gone; its exit is handled elsewhere */
			break;
		}
		memmove(p->obuf, p->obuf + w, p->olen - (size_t)w);
		p->olen -= (size_t)w;
	}
	if (p->in.fd >= 0)
		uloop_fd_add(&p->in, p->olen ? ULOOP_WRITE : 0);
}

static void in_cb(struct uloop_fd *u, unsigned int events)
{
	flush_out(container_of(u, struct plugin, in));
}

static void send_blob(struct plugin *p, struct blob_buf *b)
{
	char *json;
	size_t len;

	if (p->state != P_RUNNING || p->in.fd < 0)
		return;
	json = blobmsg_format_json(b->head, true);
	if (!json)
		return;
	len = strlen(json);
	if (p->olen + len + 1 > p->ocap) {
		size_t cap = p->ocap ? p->ocap : 65536;
		char *nb;

		while (cap < p->olen + len + 1)
			cap *= 2;
		if (cap > OBUF_MAX || !(nb = realloc(p->obuf, cap))) {
			p->dropped++;       /* the plug-in isn't keeping up: drop, never block the brain */
			free(json);
			return;
		}
		p->obuf = nb;
		p->ocap = cap;
	}
	memcpy(p->obuf + p->olen, json, len);
	p->obuf[p->olen + len] = '\n';
	p->olen += len + 1;
	free(json);
	p->events_out++;
	flush_out(p);
}

static void reply(struct plugin *p, int id, struct blob_buf *data, const char *error)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "reply", (uint32_t)id);
	if (data)
		blobmsg_add_field(&b, BLOBMSG_TYPE_TABLE, "data", blob_data(data->head), blob_len(data->head));
	blobmsg_add_string(&b, "error", error ? error : "");
	send_blob(p, &b);
	blob_buf_free(&b);
}

/* ---------- messages from the plug-in ---------- */

static struct blob_attr *get(struct blob_attr *tbl, const char *name)
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

static const char *gstr(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = get(tbl, name);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : NULL;
}

static long gnum(struct blob_attr *tbl, const char *name, long dflt)
{
	struct blob_attr *a = get(tbl, name);

	if (!a)
		return dflt;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8:  return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	case BLOBMSG_TYPE_STRING: return atol(blobmsg_get_string(a));
	default: return dflt;
	}
}

static void on_emit(struct plugin *p, const char *type, struct blob_attr *body)
{
	struct blob_buf b = { 0 };
	struct device *d = NULL;
	const char *who;
	int i, ok = 0;

	for (i = 0; i < p->n_emits && !ok; i++)
		ok = type_match(p->emits[i], type);
	if (!ok || !strncmp(type, "ag.", 3)) {
		AG_DEBUG("plug-in %s: event type '%s' not in its manifest, dropped", p->name, type);
		return;
	}
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "plugin", p->name);
	if (body && blobmsg_type(body) == BLOBMSG_TYPE_TABLE) {
		struct blob_attr *cur;
		size_t rem;

		blobmsg_for_each_attr(cur, body, rem)
			if (strcmp(blobmsg_name(cur), "plugin"))
				blobmsg_add_blob(&b, cur);
	}
	/* telemetry about a device becomes part of what AIGW knows about it */
	if ((who = gstr(body, "mac")) || (who = gstr(body, "ip")) || (who = gstr(body, "device")))
		d = act_find_device(who);
	if (d) {
		d->plugin_events++;
		snprintf(d->plugin_last, sizeof(d->plugin_last), "%s", type);
		d->plugin_last_ts = ag_wall_s();
	}
	p->last_event = ag_wall_s();
	agd_emit_from(type, &b, p);
	blob_buf_free(&b);
}

static void on_alert(struct plugin *p, struct blob_attr *a)
{
	const char *kind = gstr(a, "kind"), *dev = gstr(a, "device"), *reason = gstr(a, "reason");
	const char *sev = gstr(a, "severity"), *subject = gstr(a, "subject");
	struct device *d = dev ? act_find_device(dev) : NULL;
	char why[400];
	int i, ok = 0;

	for (i = 0; kind && i < p->n_alerts && !ok; i++)
		ok = !strcmp(p->alerts[i], kind);
	if (!ok) {
		AG_INFO("plug-in %s: alert kind '%s' not in its manifest, dropped", p->name, kind ? kind : "?");
		return;
	}
	snprintf(why, sizeof(why), "%.330s (reported by plug-in %s)", reason ? reason : kind, p->name);
	alerts_raise(kind, sev && !strcmp(sev, "warning") ? "warning" : "info", d ? d->ip : (dev ? dev : ""),
		     subject ? subject : p->name, 0, why);
}

static void on_decision(struct plugin *p, struct blob_attr *a, int request)
{
	char err[256] = "", origin[48];
	const char *action = gstr(a, "action"), *target = gstr(a, "target"), *reason = gstr(a, "reason");
	uint32_t id = 0;
	int rc;

	snprintf(origin, sizeof(origin), "plug-in %s", p->name);
	if (!request && !has_cap(p, "decision.propose")) {
		snprintf(err, sizeof(err), "plug-in %s has no decision.propose capability in its manifest", p->name);
		rc = -1;
	} else if (request && !has_cap(p, "decision.request")) {
		snprintf(err, sizeof(err), "plug-in %s may not act for you (capability decision.request%s)", p->name,
			 trusted(p) ? " not in its manifest" : " needs: agctl plugin allow");
		rc = -1;
	} else if (request && get(a, "undo")) {
		id = (uint32_t)gnum(a, "undo", 0);
		rc = act_revert(id, err, sizeof(err));
	} else if (!action || !target) {
		snprintf(err, sizeof(err), "needs action and target");
		rc = -1;
	} else if (request) {
		struct act_opts o = { 0 };

		o.origin = origin;
		o.rate_kbps = (int)gnum(a, "rate", 0);
		o.device = gstr(a, "device");
		rc = act_request(action, target, (int)gnum(a, "ttl", 0), &o, reason, &id, err, sizeof(err));
	} else {
		rc = act_propose(action, target, (int)gnum(a, "ttl", 0), origin, reason, &id, err, sizeof(err));
	}
	if (rc == 0)
		dev_state_save();
	if (get(a, "id")) {                 /* the plug-in wants to know how it went */
		struct blob_buf d = { 0 };

		blob_buf_init(&d, 0);
		blobmsg_add_u8(&d, "ok", rc == 0);
		blobmsg_add_u32(&d, "decision", id);
		reply(p, (int)gnum(a, "id", 0), &d, err);
		blob_buf_free(&d);
	} else if (rc) {
		AG_INFO("plug-in %s: %s refused: %s", p->name, request ? "request" : "proposal", err);
	}
}

/* a path the plug-in hands over must lie in its own state directory */
static int own_file(const struct plugin *p, const char *path, char *real, size_t n)
{
	char dir[300], rdir[PATH_MAX], rp[PATH_MAX];
	size_t l;

	state_dir(p, dir, sizeof(dir));
	if (!path || !realpath(dir, rdir) || !realpath(path, rp))
		return 0;
	l = strlen(rdir);
	if (strncmp(rp, rdir, l) || rp[l] != '/')
		return 0;
	snprintf(real, n, "%s", rp);
	return 1;
}

static void on_model(struct plugin *p, struct blob_attr *a)
{
	char err[256] = "", origin[48], file[PATH_MAX] = "", sig[PATH_MAX] = "";
	const char *sg = gstr(a, "sig");
	int rc = -1, version = 0;

	snprintf(origin, sizeof(origin), "plug-in %s", p->name);
	if (!has_cap(p, "model.offer"))
		snprintf(err, sizeof(err), "plug-in %s may not offer models (capability model.offer%s)", p->name,
			 trusted(p) ? " not in its manifest" : " needs: agctl plugin allow");
	else if (!own_file(p, gstr(a, "file"), file, sizeof(file)) || (sg && *sg && !own_file(p, sg, sig, sizeof(sig))))
		snprintf(err, sizeof(err), "model files must be in the plug-in's own directory ($AIGW_STATE)");
	else
		rc = models_offer(file, sig[0] ? sig : NULL, origin, 0, &version, err, sizeof(err));
	if (get(a, "id")) {
		struct blob_buf d = { 0 };

		blob_buf_init(&d, 0);
		blobmsg_add_u8(&d, "ok", rc == 0);
		blobmsg_add_u32(&d, "version", (uint32_t)version);
		reply(p, (int)gnum(a, "id", 0), &d, err);
		blob_buf_free(&d);
	} else if (rc) {
		AG_INFO("plug-in %s: model refused: %s", p->name, err);
	}
}

static void on_query(struct plugin *p, const char *what, int id)
{
	struct blob_buf d = { 0 };
	int i;

	blob_buf_init(&d, 0);
	if (!strcmp(what, "status")) {
		blobmsg_add_string(&d, "version", AG_VERSION);
		blobmsg_add_string(&d, "host", cfg.host);
		blobmsg_add_string(&d, "mode", ag_mode_str(cfg.act_mode));
		blobmsg_add_u32(&d, "devices", (uint32_t)dev_count());
		blobmsg_add_u64(&d, "alerts", alerts_total());
		blobmsg_add_u32(&d, "waiting", (uint32_t)act_count(0));
		blobmsg_add_u32(&d, "active", (uint32_t)act_count(1));
		blobmsg_add_u32(&d, "plugins", (uint32_t)plugin_count(1));
		blobmsg_add_u64(&d, "uptime_s", (uint64_t)(ag_wall_s() - started));
	} else if (!strcmp(what, "models")) {
		models_blob(&d, "models");
	} else if (!has_cap(p, "devices.read")) {
		blob_buf_free(&d);
		reply(p, id, NULL, trusted(p) ? "capability devices.read not in the manifest" :
		      "devices.read needs your OK: agctl plugin allow");
		return;
	} else if (!strcmp(what, "devices")) {
		void *arr = blobmsg_open_array(&d, "devices"), *t;

		for (i = 0; i < DEV_MAX; i++) {
			if (!devs[i].used)
				continue;
			t = blobmsg_open_table(&d, NULL);
			dev_blob(&d, &devs[i], 0);
			blobmsg_close_table(&d, t);
		}
		blobmsg_close_array(&d, arr);
	} else if (!strcmp(what, "decisions")) {
		act_blob(&d, 50, "decisions");
	} else if (!strcmp(what, "alerts")) {
		alerts_blob(&d, 50, "alerts");
	} else {
		blob_buf_free(&d);
		reply(p, id, NULL, "unknown query (status, devices, decisions, alerts, models)");
		return;
	}
	reply(p, id, &d, NULL);
	blob_buf_free(&d);
}

static void handle_line(struct plugin *p, char *line)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	size_t rem;

	if (!*line)
		return;
	p->events_in++;
	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, line)) {
		AG_DEBUG("plug-in %s: not a JSON object: %.80s", p->name, line);
		blob_buf_free(&b);
		return;
	}
	blob_for_each_attr(cur, b.head, rem) {
		const char *key = blobmsg_name(cur);
		struct blob_attr *body = blobmsg_type(cur) == BLOBMSG_TYPE_TABLE ? cur : NULL;

		if (!strcmp(key, "emit") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			on_emit(p, blobmsg_get_string(cur), get(b.head, "body"));
		else if (!strcmp(key, "alert") && body)
			on_alert(p, body);
		else if ((!strcmp(key, "propose") || !strcmp(key, "request")) && body)
			on_decision(p, body, key[0] == 'r');
		else if (!strcmp(key, "done") && body)
			act_plugin_result((uint32_t)gnum(body, "id", 0), (int)gnum(body, "ok", 0), gstr(body, "error"));
		else if (!strcmp(key, "query") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			on_query(p, blobmsg_get_string(cur), (int)gnum(b.head, "id", 0));
		else if (!strcmp(key, "model") && body)
			on_model(p, body);
		else if (!strcmp(key, "log") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			AG_INFO("plug-in %s: %.300s", p->name, blobmsg_get_string(cur));
		else
			continue;
		break;                       /* one message per line */
	}
	blob_buf_free(&b);
}

static void out_cb(struct uloop_fd *u, unsigned int events)
{
	struct plugin *p = container_of(u, struct plugin, out);

	for (;;) {
		ssize_t n = read(u->fd, p->rbuf + p->rlen, sizeof(p->rbuf) - 1 - p->rlen);
		char *start, *nl;

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		p->rlen += (size_t)n;
		p->rbuf[p->rlen] = '\0';
		start = p->rbuf;
		while ((nl = strchr(start, '\n'))) {
			*nl = '\0';
			if (!p->rskip)
				handle_line(p, start);
			p->rskip = 0;
			start = nl + 1;
		}
		p->rlen -= (size_t)(start - p->rbuf);
		memmove(p->rbuf, start, p->rlen);
		if (p->rlen == sizeof(p->rbuf) - 1) {   /* a line too long: skip it */
			p->rlen = 0;
			p->rskip = 1;
		}
		if (!p->used)
			return;
	}
}

static void err_cb(struct uloop_fd *u, unsigned int events)
{
	struct plugin *p = container_of(u, struct plugin, err);
	int minute = (int)(ag_wall_s() / 60);

	for (;;) {
		ssize_t n = read(u->fd, p->ebuf + p->elen, sizeof(p->ebuf) - 1 - p->elen);
		char *start, *nl;

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		p->elen += (size_t)n;
		p->ebuf[p->elen] = '\0';
		start = p->ebuf;
		while ((nl = strchr(start, '\n'))) {
			*nl = '\0';
			if (p->log_min != minute) {
				p->log_min = minute;
				p->log_n = 0;
			}
			if (p->log_n++ < LOG_PER_MIN && *start)
				AG_INFO("plug-in %s: %.300s", p->name, start);
			start = nl + 1;
		}
		p->elen -= (size_t)(start - p->ebuf);
		memmove(p->ebuf, start, p->elen);
		if (p->elen == sizeof(p->ebuf) - 1)
			p->elen = 0;
	}
}

/* ---------- process supervision ---------- */

static void close_fd(struct uloop_fd *u)
{
	if (u->fd >= 0) {
		uloop_fd_delete(u);
		close(u->fd);
		u->fd = -1;
	}
}

static void restart_cb(struct uloop_timeout *t)
{
	struct plugin *p = container_of(t, struct plugin, restart);

	if (p->used && p->state == P_WAITING)
		evaluate(p);
}

static void proc_cb(struct uloop_process *up, int status)
{
	struct plugin *p = container_of(up, struct plugin, proc);
	int64_t now = ag_wall_s();
	char how[64];
	int i, recent = 0;
	unsigned delay;

	out_cb(&p->out, 0);                 /* last words */
	close_fd(&p->out);
	close_fd(&p->err);
	close_fd(&p->in);
	p->pid = 0;
	p->olen = 0;
	if (WIFEXITED(status))
		snprintf(how, sizeof(how), "exited with code %d", WEXITSTATUS(status));
	else
		snprintf(how, sizeof(how), "killed by signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	if (p->stopping) {
		p->stopping = 0;            /* stop() already set the state */
		if (p->state == P_WAITING)
			evaluate(p);            /* e.g. restart with new permissions */
		return;
	}
	/* a crash: its actions stay in effect and are sent again when it is back */
	memmove(p->crashes + 1, p->crashes, sizeof(p->crashes) - sizeof(p->crashes[0]));
	p->crashes[0] = now;
	for (i = 0; i < CRASH_LIMIT; i++)
		recent += p->crashes[i] && now - p->crashes[i] < CRASH_WINDOW;
	if (recent >= CRASH_LIMIT) {
		set_state(p, P_FAILED, "%s; stopped %d times in %d minutes (agctl plugin enable %s to try again)",
			  how, CRASH_LIMIT, CRASH_WINDOW / 60, p->name);
		return;
	}
	delay = 1u << (recent > 6 ? 6 : recent - 1);
	if (delay > 60)
		delay = 60;
	p->restarts++;
	set_state(p, P_WAITING, "%s; restarting in %us", how, delay);
	p->restart.cb = restart_cb;
	uloop_timeout_set(&p->restart, (int)delay * 1000);
}

static void stop(struct plugin *p, int state, const char *why)
{
	uloop_timeout_cancel(&p->restart);
	if (p->pid > 0) {
		p->stopping = 1;
		p->stop_at = ag_wall_s();
		kill(-p->pid, SIGTERM);     /* its own process group: helpers it started go too */
		kill(p->pid, SIGTERM);
	}
	set_state(p, state, "%s", why ? why : "");
	if (state == P_DISABLED || state == P_FAILED)
		act_plugin_gone(p->name);   /* nobody left to carry out or undo its actions */
}

static void send_hello(struct plugin *p)
{
	struct blob_buf b = { 0 };
	void *h, *t, *g;
	int i;

	blob_buf_init(&b, 0);
	h = blobmsg_open_table(&b, "hello");
	blobmsg_add_string(&b, "name", p->name);
	blobmsg_add_string(&b, "version", p->version);
	blobmsg_add_string(&b, "aigw", AG_VERSION);
	blobmsg_add_string(&b, "host", cfg.host);
	t = blobmsg_open_table(&b, "config");
	for (i = 0; i < p->n_conf; i++)
		blobmsg_add_string(&b, p->conf[i].k, p->conf[i].v);
	blobmsg_close_table(&b, t);
	g = blobmsg_open_array(&b, "granted");
	for (i = 0; i < p->n_caps; i++)
		if (has_cap(p, p->caps[i]))
			blobmsg_add_string(&b, NULL, p->caps[i]);
	blobmsg_close_array(&b, g);
	g = blobmsg_open_array(&b, "denied");
	for (i = 0; i < p->n_caps; i++)
		if (!has_cap(p, p->caps[i]))
			blobmsg_add_string(&b, NULL, p->caps[i]);
	blobmsg_close_array(&b, g);
	blobmsg_close_table(&b, h);
	send_blob(p, &b);
	blob_buf_free(&b);
}

static int start(struct plugin *p)
{
	int pin[2] = { -1, -1 }, pout[2] = { -1, -1 }, perr[2] = { -1, -1 };
	struct passwd *pw = NULL;
	char env_name[64], env_dir[300], env_host[48], env_state[320], sdir[300], *envp[9], *argv[2];
	pid_t pid;

	if (getuid() == 0 && cfg.plugin_user[0] && !(pw = getpwnam(cfg.plugin_user))) {
		set_state(p, P_FAILED, "user '%s' (plugins.user) does not exist", cfg.plugin_user);
		return -1;
	}
	/* its own writable directory: downloads, caches; nobody else's plug-in can touch it */
	state_dir(p, sdir, sizeof(sdir));
	if ((mkdir(cfg.plugin_state, 0755) && errno != EEXIST) || (mkdir(sdir, 0700) && errno != EEXIST) ||
	    (pw && chown(sdir, pw->pw_uid, pw->pw_gid)) || chmod(sdir, 0700)) {
		set_state(p, P_FAILED, "cannot prepare %s: %s", sdir, strerror(errno));
		return -1;
	}
	if (pipe(pin) || pipe(pout) || pipe(perr)) {
		set_state(p, P_FAILED, "pipe: %s", strerror(errno));
		goto fail;
	}
	snprintf(env_name, sizeof(env_name), "AIGW_PLUGIN=%s", p->name);
	snprintf(env_dir, sizeof(env_dir), "AIGW_PLUGIN_DIR=%s", p->dir);
	snprintf(env_host, sizeof(env_host), "AIGW_HOST=%s", cfg.host);
	snprintf(env_state, sizeof(env_state), "AIGW_STATE=%s", sdir);
	envp[0] = (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	envp[1] = env_name;
	envp[2] = env_dir;
	envp[3] = env_host;
	envp[4] = env_state;
	envp[5] = (char *)"HOME=/tmp";
	envp[6] = (char *)"LANG=C";
	envp[7] = NULL;
	argv[0] = p->exec;
	argv[1] = NULL;

	pid = fork();
	if (pid < 0) {
		set_state(p, P_FAILED, "fork: %s", strerror(errno));
		goto fail;
	}
	if (pid == 0) {
		struct rlimit core = { 0, 0 };
		sigset_t none;
		int fd;

		setpgid(0, 0);
		dup2(pin[0], 0);
		dup2(pout[1], 1);
		dup2(perr[1], 2);
		for (fd = 3; fd < 1024; fd++)
			close(fd);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		setrlimit(RLIMIT_CORE, &core);
		if (chdir(p->dir) != 0)
			_exit(126);
		if (pw && (setgroups(0, NULL) || setgid(pw->pw_gid) || setuid(pw->pw_uid))) {
			fprintf(stderr, "cannot switch to user %s\n", cfg.plugin_user);
			_exit(126);
		}
		execve(p->exec, argv, envp);
		fprintf(stderr, "cannot run %s: %s\n", p->exec, strerror(errno));
		_exit(127);
	}
	setpgid(pid, pid);
	close(pin[0]);
	close(pout[1]);
	close(perr[1]);
	p->pid = pid;
	p->in.fd = pin[1];
	p->out.fd = pout[0];
	p->err.fd = perr[0];
	fcntl(p->in.fd, F_SETFL, O_NONBLOCK);
	fcntl(p->out.fd, F_SETFL, O_NONBLOCK);
	fcntl(p->err.fd, F_SETFL, O_NONBLOCK);
	fcntl(p->in.fd, F_SETFD, FD_CLOEXEC);
	fcntl(p->out.fd, F_SETFD, FD_CLOEXEC);
	fcntl(p->err.fd, F_SETFD, FD_CLOEXEC);
	p->in.cb = in_cb;
	p->out.cb = out_cb;
	p->err.cb = err_cb;
	uloop_fd_add(&p->out, ULOOP_READ);
	uloop_fd_add(&p->err, ULOOP_READ);
	p->proc.pid = pid;
	p->proc.cb = proc_cb;
	uloop_process_add(&p->proc);
	p->started = ag_wall_s();
	p->rlen = p->elen = p->olen = 0;
	p->rskip = 0;
	p->cpu_ticks = 0;
	p->cpu_at_us = 0;
	p->over_cpu = 0;
	p->rss_kb = 0;
	p->cpu_now = 0;
	{
		int n = 0, i;

		for (i = 0; i < p->n_caps; i++)
			n += !has_cap(p, p->caps[i]);
		set_state(p, P_RUNNING, n ? "running; %d capabilit%s wait%s for your OK (agctl plugin allow %s)" : NULL,
			  n, n == 1 ? "y" : "ies", n == 1 ? "s" : "", p->name);
	}
	send_hello(p);
	act_plugin_started(p->name);
	return 0;
fail:
	if (pin[0] >= 0) { close(pin[0]); close(pin[1]); }
	if (pout[0] >= 0) { close(pout[0]); close(pout[1]); }
	if (perr[0] >= 0) { close(perr[0]); close(perr[1]); }
	return -1;
}

/* ---------- needs ---------- */

static void substitute(const struct plugin *p, const char *in, char *out, size_t n)
{
	size_t len = 0;

	while (*in && len + 1 < n) {
		const char *end;
		int i;

		if (in[0] == '$' && in[1] == '{' && (end = strchr(in, '}'))) {
			char key[32];

			snprintf(key, sizeof(key), "%.*s", (int)(end - in - 2), in + 2);
			for (i = 0; i < p->n_conf; i++)
				if (!strcmp(p->conf[i].k, key))
					len += (size_t)snprintf(out + len, n - len, "%s", p->conf[i].v);
			if (len >= n)
				len = n - 1;
			in = end + 1;
			continue;
		}
		out[len++] = *in++;
	}
	out[len] = '\0';
}

static int in_path(const char *bin)
{
	char path[1024], *dir, *save = NULL, full[1300];
	const char *env = getenv("PATH");

	snprintf(path, sizeof(path), "%s:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		 env ? env : "");
	for (dir = strtok_r(path, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		snprintf(full, sizeof(full), "%s/%s", dir, bin);
		if (access(full, X_OK) == 0)
			return 1;
	}
	return 0;
}

/* numeric address or localhost only: never a DNS lookup from the brain's loop */
static int tcp_open(const char *hostport)
{
	char host[64];
	const char *colon = strrchr(hostport, ':');
	struct sockaddr_storage ss;
	struct pollfd pfd;
	socklen_t len;
	int fd, port, err = 0, ok = 0;

	if (!colon || colon == hostport || (port = atoi(colon + 1)) <= 0 || port > 65535)
		return 0;
	snprintf(host, sizeof(host), "%.*s", (int)(colon - hostport), hostport);
	if (host[0] == '[') {
		memmove(host, host + 1, strlen(host));
		host[strcspn(host, "]")] = '\0';
	}
	if (!strcmp(host, "localhost"))
		snprintf(host, sizeof(host), "127.0.0.1");
	memset(&ss, 0, sizeof(ss));
	if (inet_pton(AF_INET, host, &((struct sockaddr_in *)&ss)->sin_addr) == 1) {
		((struct sockaddr_in *)&ss)->sin_family = AF_INET;
		((struct sockaddr_in *)&ss)->sin_port = htons((uint16_t)port);
		len = sizeof(struct sockaddr_in);
	} else if (inet_pton(AF_INET6, host, &((struct sockaddr_in6 *)&ss)->sin6_addr) == 1) {
		((struct sockaddr_in6 *)&ss)->sin6_family = AF_INET6;
		((struct sockaddr_in6 *)&ss)->sin6_port = htons((uint16_t)port);
		len = sizeof(struct sockaddr_in6);
	} else {
		return 1;                    /* a name: the brain doesn't resolve it; the plug-in will try */
	}
	fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	if (connect(fd, (struct sockaddr *)&ss, len) == 0) {
		ok = 1;
	} else if (errno == EINPROGRESS) {
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, 300) == 1) {
			len = sizeof(err);
			ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
		}
	}
	close(fd);
	return ok;
}

static int event_available(const struct plugin *self, const char *type)
{
	static const char *const builtin[] = {
		AG_EV_DNS, AG_EV_FLOW, AG_EV_WIFI, AG_EV_DHCP, AG_EV_ALERT, AG_EV_DECISION, AG_EV_PLUGIN, NULL
	};
	int i, j;

	for (i = 0; builtin[i]; i++)
		if (!strcmp(builtin[i], type))
			return 1;
	for (i = 0; i < PLUGIN_MAX; i++) {
		const struct plugin *o = &plugins[i];

		if (!o->used || o == self || o->state != P_RUNNING)
			continue;
		for (j = 0; j < o->n_emits; j++)
			if (type_match(o->emits[j], type) || type_match(type, o->emits[j]))
				return 1;
	}
	return 0;
}

static int conf_set(const struct plugin *p, const char *key)
{
	int i;

	for (i = 0; i < p->n_conf; i++)
		if (!strcmp(p->conf[i].k, key))
			return p->conf[i].v[0] != '\0';
	return 0;
}

/* NULL when everything it needs is here, else what is missing */
static const char *missing(const struct plugin *p, char *buf, size_t n)
{
	int i;

	for (i = 0; i < p->n_needs; i++) {
		char need[160];
		const char *v;

		substitute(p, p->needs[i], need, sizeof(need));
		v = strchr(need, ':') + 1;
		if (!strncmp(need, "bin:", 4) && !in_path(v))
			snprintf(buf, n, "program '%s' is not installed", v);
		else if (!strncmp(need, "file:", 5) && access(v, F_OK))
			snprintf(buf, n, "%s is not there (yet)", v);
		else if (!strncmp(need, "tcp:", 4) && !tcp_open(v))
			snprintf(buf, n, "nothing answers on %s (yet)", v);
		else if (!strncmp(need, "event:", 6) && !event_available(p, v))
			snprintf(buf, n, "no running plug-in provides '%s' events", v);
		else if (!strncmp(need, "config:", 7) && !conf_set(p, v))
			snprintf(buf, n, "setting '%s' is not set (agctl plugin config %s %s <value>)", v, p->name, v);
		else
			continue;
		return buf;
	}
	return NULL;
}

/* ---------- manifest ---------- */

static int read_list(struct blob_attr *arr, char *out, int max, size_t width, int *count)
{
	struct blob_attr *cur;
	size_t rem;

	*count = 0;
	if (!arr)
		return 0;
	if (blobmsg_type(arr) != BLOBMSG_TYPE_ARRAY)
		return -1;
	blobmsg_for_each_attr(cur, arr, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING || *count >= max ||
		    strlen(blobmsg_get_string(cur)) >= width)
			return -1;
		snprintf(out + (size_t)*count * width, width, "%s", blobmsg_get_string(cur));
		(*count)++;
	}
	return 0;
}

static int safe_file(const char *path, int system, char *err, size_t n)
{
	struct stat st;

	if (stat(path, &st) != 0) {
		snprintf(err, n, "%s: %s", path, strerror(errno));
		return -1;
	}
	/* code that agd runs must not be changeable by ordinary users */
	if (!system && ((st.st_mode & 022) || (st.st_uid != 0 && st.st_uid != getuid()))) {
		snprintf(err, n, "%s is writable by others or not owned by root", path);
		return -1;
	}
	return 0;
}

static int load_manifest(struct plugin *p, char *err, size_t n)
{
	struct blob_buf b = { 0 };
	struct blob_attr *m, *a, *cur;
	const char *s;
	size_t rem;
	int i, rc = -1;

	blob_buf_init(&b, 0);
	if (safe_file(p->manifest, p->system, err, n))
		goto out;
	if (!blobmsg_add_json_from_file(&b, p->manifest)) {
		snprintf(err, n, "manifest.json is not valid JSON");
		goto out;
	}
	m = b.head;
	s = gstr(m, "name");
	if (!s || strcmp(s, p->name)) {
		snprintf(err, n, "manifest name '%s' must match its folder '%s'", s ? s : "", p->name);
		goto out;
	}
	s = gstr(m, "kind");
	if (!s || (strcmp(s, "sensor") && strcmp(s, "actuator") && strcmp(s, "skill") && strcmp(s, "connector"))) {
		snprintf(err, n, "kind must be sensor, actuator, skill or connector");
		goto out;
	}
	snprintf(p->kind, sizeof(p->kind), "%s", s);
	snprintf(p->version, sizeof(p->version), "%s", (s = gstr(m, "version")) ? s : "0");
	snprintf(p->description, sizeof(p->description), "%s", (s = gstr(m, "description")) ? s : "");
	if (read_list(get(m, "emits"), &p->emits[0][0], LIST_MAX, sizeof(p->emits[0]), &p->n_emits) ||
	    read_list(get(m, "subscribes"), &p->subs[0][0], LIST_MAX, sizeof(p->subs[0]), &p->n_subs) ||
	    read_list(get(m, "caps"), &p->caps[0][0], LIST_MAX, sizeof(p->caps[0]), &p->n_caps) ||
	    read_list(get(m, "needs"), &p->needs[0][0], LIST_MAX, sizeof(p->needs[0]), &p->n_needs) ||
	    read_list(get(m, "alerts"), &p->alerts[0][0], LIST_MAX, sizeof(p->alerts[0]), &p->n_alerts)) {
		snprintf(err, n, "emits, subscribes, caps, needs and alerts must be short lists of strings");
		goto out;
	}
	for (i = 0; i < p->n_emits; i++)
		if (!strncmp(p->emits[i], "ag.", 3) || !strcmp(p->emits[i], "*")) {
			snprintf(err, n, "plug-ins can't emit core events (%s)", p->emits[i]);
			goto out;
		}
	for (i = 0; i < p->n_needs; i++)
		if (strncmp(p->needs[i], "bin:", 4) && strncmp(p->needs[i], "file:", 5) &&
		    strncmp(p->needs[i], "tcp:", 4) && strncmp(p->needs[i], "event:", 6) &&
		    strncmp(p->needs[i], "config:", 7)) {
			snprintf(err, n, "unknown need '%s' (bin:, file:, tcp:, event:, config:)", p->needs[i]);
			goto out;
		}
	for (i = 0; i < p->n_caps; i++)
		if (strcmp(p->caps[i], "decision.request") && strcmp(p->caps[i], "decision.propose") &&
		    strcmp(p->caps[i], "devices.read") && strcmp(p->caps[i], "alert.raise") &&
		    strcmp(p->caps[i], "model.offer")) {
			snprintf(err, n, "unknown capability '%s'", p->caps[i]);
			goto out;
		}

	/* actions: "name" (about a device) or {"name": "x", "subject": "device"|"none"} */
	p->n_actions = 0;
	if ((a = get(m, "actions"))) {
		blobmsg_for_each_attr(cur, a, rem) {
			const char *an = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : gstr(cur, "name");
			const char *subj = blobmsg_type(cur) == BLOBMSG_TYPE_TABLE ? gstr(cur, "subject") : NULL;
			char full[64];

			snprintf(full, sizeof(full), "%s.%s", p->name, an ? an : "");
			if (!an || p->n_actions >= LIST_MAX || !ag_policy_action_ok(full)) {
				snprintf(err, n, "bad action '%s' (a-z, 0-9, - and _, up to 23)", an ? an : "?");
				goto out;
			}
			snprintf(p->actions[p->n_actions], sizeof(p->actions[0]), "%s", an);
			p->action_device[p->n_actions++] = !subj || strcmp(subj, "none");
		}
	}

	/* skill: policies */
	p->n_policies = 0;
	if ((a = get(m, "policies"))) {
		blobmsg_for_each_attr(cur, a, rem) {
			struct ag_policy *pol;
			const char *alert = gstr(cur, "alert"), *action = gstr(cur, "action"), *zone = gstr(cur, "zone");

			if (p->n_policies >= POLICY_MAX || !alert || !ag_policy_action_ok(action)) {
				snprintf(err, n, "each policy needs an alert and an allowed action (never block)");
				goto out;
			}
			pol = &p->policies[p->n_policies++];
			memset(pol, 0, sizeof(*pol));
			snprintf(pol->name, sizeof(pol->name), "%s", p->name);
			snprintf(pol->alert, sizeof(pol->alert), "%s", alert);
			snprintf(pol->action, sizeof(pol->action), "%s", action);
			snprintf(pol->zone, sizeof(pol->zone), "%s", zone ? zone : "");
			pol->ttl = (int)gnum(cur, "ttl", 0);
			pol->rate_kbps = (int)gnum(cur, "rate", 0);
			pol->mode = ag_mode_parse(gstr(cur, "mode"), AG_MODE_ADVISE);
		}
	}
	if ((a = get(m, "card"))) {
		snprintf(p->card_title, sizeof(p->card_title), "%s", (s = gstr(a, "title")) ? s : "");
		snprintf(p->card_text, sizeof(p->card_text), "%s", (s = gstr(a, "text")) ? s : "");
	}
	a = get(m, "budget");
	p->ram_kb = (int)gnum(a, "ram_kb", DEFAULT_RAM_KB);
	p->cpu_pct = (int)gnum(a, "cpu_pct", DEFAULT_CPU);
	if (p->ram_kb < 512)
		p->ram_kb = 512;
	if (p->cpu_pct < 1)
		p->cpu_pct = 1;

	/* manifest config: defaults, overridden by "config plugin" sections (read later) */
	p->n_conf = 0;
	if ((a = get(m, "config")) && blobmsg_type(a) == BLOBMSG_TYPE_TABLE)
		blobmsg_for_each_attr(cur, a, rem) {
			struct kv *e;

			if (p->n_conf >= CONF_MAX)
				break;
			e = &p->conf[p->n_conf++];
			snprintf(e->k, sizeof(e->k), "%s", blobmsg_name(cur));
			if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				snprintf(e->v, sizeof(e->v), "%s", blobmsg_get_string(cur));
			else
				snprintf(e->v, sizeof(e->v), "%ld", gnum(a, blobmsg_name(cur), 0));
		}

	/* where it runs */
	p->exec[0] = '\0';
	if ((s = gstr(m, "exec"))) {
		if (strstr(s, "..") || (s[0] == '/' && !p->system)) {
			snprintf(err, n, "exec must be a file inside the plug-in folder");
			goto out;
		}
		if (s[0] == '/')
			snprintf(p->exec, sizeof(p->exec), "%s", s);
		else
			snprintf(p->exec, sizeof(p->exec), "%s/%s", p->dir, s);
		if (access(p->exec, X_OK) || safe_file(p->exec, p->system, err, n)) {
			if (!err[0] || access(p->exec, X_OK))
				snprintf(err, n, "%.150s is missing or not executable", p->exec);
			goto out;
		}
	} else if (strcmp(p->kind, "skill")) {
		snprintf(err, n, "a %s needs \"exec\" (only skills run without a program)", p->kind);
		goto out;
	}
	if ((a = get(m, "hosts"))) {
		int ok = 0;

		blobmsg_for_each_attr(cur, a, rem)
			ok |= blobmsg_type(cur) == BLOBMSG_TYPE_STRING && !strcmp(blobmsg_get_string(cur), cfg.host);
		if (!ok) {
			p->state = P_UNSUPPORTED;
			snprintf(err, n, "made for other hosts, not %s", cfg.host);
			rc = 1;
			goto out;
		}
	}
	rc = 0;
out:
	blob_buf_free(&b);
	return rc;
}

/* ---------- "config plugin" sections in /etc/config/aigw ---------- */

static void section_name(const char *plugin, char *out, size_t n)
{
	size_t i;

	snprintf(out, n, "%s", plugin);
	for (i = 0; out[i]; i++)
		if (out[i] == '-')
			out[i] = '_';
}

static void read_uci(void)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	struct uci_element *e, *oe;
	int i;

	for (i = 0; i < PLUGIN_MAX; i++)
		plugins[i].allowed = plugins[i].disabled = 0;
	if (!ctx)
		return;
	if (confdir)
		uci_set_confdir(ctx, confdir);
	if (uci_load(ctx, "aigw", &pkg) != UCI_OK || !pkg) {
		uci_free_context(ctx);
		return;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *nm = uci_lookup_option_string(ctx, s, "name");
		char sec[48];
		struct plugin *p = NULL;

		if (strcmp(s->type, "plugin"))
			continue;
		for (i = 0; i < PLUGIN_MAX && !p; i++) {
			if (!plugins[i].used)
				continue;
			section_name(plugins[i].name, sec, sizeof(sec));
			if ((nm && !strcmp(nm, plugins[i].name)) || (!s->anonymous && (!strcmp(s->e.name, sec) ||
			    (!strncmp(s->e.name, "plugin_", 7) && !strcmp(s->e.name + 7, sec)))))
				p = &plugins[i];
		}
		if (!p)
			continue;
		uci_foreach_element(&s->options, oe) {
			struct uci_option *o = uci_to_option(oe);
			struct kv *kv = NULL;

			if (o->type != UCI_TYPE_STRING)
				continue;
			if (!strcmp(oe->name, "allow")) {
				p->allowed = atoi(o->v.string) != 0;
				continue;
			}
			if (!strcmp(oe->name, "enabled")) {
				p->disabled = atoi(o->v.string) == 0;
				continue;
			}
			if (!strcmp(oe->name, "name"))
				continue;
			for (i = 0; i < p->n_conf && !kv; i++)
				if (!strcmp(p->conf[i].k, oe->name))
					kv = &p->conf[i];
			if (!kv && p->n_conf < CONF_MAX)
				kv = &p->conf[p->n_conf++];
			if (kv) {
				snprintf(kv->k, sizeof(kv->k), "%s", oe->name);
				snprintf(kv->v, sizeof(kv->v), "%s", o->v.string);
			}
		}
	}
	uci_free_context(ctx);
}

/* value "" removes the option */
static int write_uci(const char *plugin, const char *option, const char *value, char *err, size_t n)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *s;
	struct uci_ptr ptr;
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	char sec[56], tuple[160];
	int rc = -1;

	if (!ctx) {
		snprintf(err, n, "out of memory");
		return -1;
	}
	if (confdir)
		uci_set_confdir(ctx, confdir);
	section_name(plugin, sec, sizeof(sec));
	if (uci_load(ctx, "aigw", &pkg) == UCI_OK && pkg && (s = uci_lookup_section(ctx, pkg, sec)) &&
	    strcmp(s->type, "plugin")) {
		char tmp[56];

		snprintf(tmp, sizeof(tmp), "plugin_%.40s", sec);  /* the name belongs to a core section */
		snprintf(sec, sizeof(sec), "%s", tmp);
	}
	snprintf(tuple, sizeof(tuple), "aigw.%s=plugin", sec);
	if (uci_lookup_ptr(ctx, &ptr, tuple, true) != UCI_OK || uci_set(ctx, &ptr) != UCI_OK)
		goto out;
	/* set the fields directly: a value may hold any character (URLs, tokens) */
	memset(&ptr, 0, sizeof(ptr));
	ptr.package = "aigw";
	ptr.section = sec;
	ptr.option = option;
	if (uci_lookup_ptr(ctx, &ptr, NULL, true) != UCI_OK)
		goto out;
	ptr.value = value;
	if ((*value ? uci_set(ctx, &ptr) : (ptr.o ? uci_delete(ctx, &ptr) : UCI_OK)) != UCI_OK ||
	    uci_save(ctx, ptr.p) != UCI_OK || uci_commit(ctx, &ptr.p, false) != UCI_OK)
		goto out;
	rc = 0;
out:
	if (rc)
		snprintf(err, n, "could not write /etc/config/aigw");
	uci_free_context(ctx);
	return rc;
}

/* ---------- discovery ---------- */

static void evaluate(struct plugin *p)
{
	char buf[200];
	const char *miss;

	if (p->state == P_INVALID || p->state == P_UNSUPPORTED)
		return;
	if (p->disabled) {
		if (p->state != P_DISABLED)
			stop(p, P_DISABLED, "turned off (agctl plugin enable to turn it on)");
		return;
	}
	if (p->state == P_FAILED || p->stopping)
		return;
	if (p->state == P_RUNNING || (p->state == P_ACTIVE && !p->exec[0]))
		return;
	if (p->state == P_WAITING && p->restart.pending)
		return;
	if ((miss = missing(p, buf, sizeof(buf)))) {
		set_state(p, P_WAITING, "%s", miss);
		return;
	}
	if (!p->exec[0]) {
		set_state(p, P_ACTIVE, p->n_policies ? "%d polic%s in the policy table%s" : NULL, p->n_policies,
			  p->n_policies == 1 ? "y" : "ies", trusted(p) ? "" : " (advise only until you allow it)");
		return;
	}
	start(p);
}

static void forget(struct plugin *p)
{
	if (p->pid > 0) {
		/* the process may outlive the folder for a moment; reap it without callbacks */
		uloop_process_delete(&p->proc);
		kill(-p->pid, SIGKILL);
		kill(p->pid, SIGKILL);
		waitpid(p->pid, NULL, 0);
		close_fd(&p->out);
		close_fd(&p->err);
		close_fd(&p->in);
		p->pid = 0;
	}
	uloop_timeout_cancel(&p->restart);
	act_plugin_gone(p->name);
	p->state = P_DISABLED;
	snprintf(p->why, sizeof(p->why), "removed");
	lifecycle_event(p);
	AG_INFO("plug-in %s: removed", p->name);
	free(p->obuf);
	memset(p, 0, sizeof(*p));
	p->in.fd = p->out.fd = p->err.fd = -1;
}

static void scan_dir(const char *dir, int system)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char mpath[300], pdir[256], err[200] = "";
		struct stat st;
		struct plugin *p;
		int i;

		if (e->d_name[0] == '.')
			continue;
		snprintf(pdir, sizeof(pdir), "%.200s/%.40s", dir, e->d_name);
		snprintf(mpath, sizeof(mpath), "%s/manifest.json", pdir);
		if (stat(mpath, &st) != 0)
			continue;
		p = find(e->d_name);
		if (p && strcmp(p->dir, pdir)) {
			if (p->system && !system)
				continue;        /* the package's plug-in wins; reported below */
		}
		if (p && !strcmp(p->dir, pdir) && p->mtime == (int64_t)st.st_mtime) {
			p->seen = 1;
			continue;
		}
		if (p) {                      /* updated: start over with the new manifest */
			AG_INFO("plug-in %s: manifest changed, reloading", p->name);
			forget(p);
		}
		for (i = 0, p = NULL; i < PLUGIN_MAX && !p; i++)
			if (!plugins[i].used)
				p = &plugins[i];
		if (!p) {
			AG_WARN("too many plug-ins, ignoring %s", pdir);
			continue;
		}
		memset(p, 0, sizeof(*p));
		p->in.fd = p->out.fd = p->err.fd = -1;
		p->used = p->seen = 1;
		p->system = system;
		p->mtime = (int64_t)st.st_mtime;
		snprintf(p->name, sizeof(p->name), "%.31s", e->d_name);
		snprintf(p->dir, sizeof(p->dir), "%s", pdir);
		snprintf(p->manifest, sizeof(p->manifest), "%s", mpath);
		snprintf(p->kind, sizeof(p->kind), "?");
		if (!ag_plugin_name_ok(e->d_name)) {
			p->state = P_INVALID;
			snprintf(p->why, sizeof(p->why), "folder name must be 1-31 of a-z, 0-9 and -");
		} else {
			int rc = load_manifest(p, err, sizeof(err));

			if (rc < 0)
				p->state = P_INVALID;
			if (rc != 0)
				snprintf(p->why, sizeof(p->why), "%s", err);
			else
				p->state = P_WAITING;
		}
		AG_INFO("plug-in %s found in %s (%s%s%s)", p->name, dir, state_names[p->state],
			p->why[0] ? ": " : "", p->why);
		lifecycle_event(p);
	}
	closedir(d);
}

void plugin_rescan(void)
{
	int i;

	if (!cfg.plugins_enabled)
		return;
	for (i = 0; i < PLUGIN_MAX; i++)
		plugins[i].seen = 0;
	scan_dir(cfg.plugin_dir, 1);
	if (strcmp(cfg.plugin_user_dir, cfg.plugin_dir))
		scan_dir(cfg.plugin_user_dir, 0);
	for (i = 0; i < PLUGIN_MAX; i++)
		if (plugins[i].used && !plugins[i].seen)
			forget(&plugins[i]);
	read_uci();
	for (i = 0; i < PLUGIN_MAX; i++)
		if (plugins[i].used)
			evaluate(&plugins[i]);
}

/* ---------- budget ---------- */

static void measure(struct plugin *p)
{
	char path[64], buf[1024], *s;
	unsigned long long ticks = 0;
	uint64_t now = ag_mono_us();
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)p->pid);
	if ((f = fopen(path, "r"))) {
		while (fgets(buf, sizeof(buf), f))
			if (!strncmp(buf, "VmRSS:", 6))
				p->rss_kb = strtoul(buf + 6, NULL, 10);
		fclose(f);
	}
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)p->pid);
	if ((f = fopen(path, "r"))) {
		if (fgets(buf, sizeof(buf), f) && (s = strrchr(buf, ')'))) {
			unsigned long ut = 0, st = 0;

			/* after ") ": state(3) ... utime(14) stime(15) */
			if (sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2)
				ticks = (unsigned long long)ut + st;
		}
		fclose(f);
	}
	if (p->cpu_at_us && now > p->cpu_at_us && ticks >= p->cpu_ticks)
		p->cpu_now = 100.0 * (double)(ticks - p->cpu_ticks) / (double)sysconf(_SC_CLK_TCK) /
			     ((double)(now - p->cpu_at_us) / 1e6);
	p->cpu_ticks = ticks;
	p->cpu_at_us = now;
}

void plugin_tick(void)
{
	static int64_t last_scan;
	int64_t now = ag_wall_s();
	int i;

	if (!cfg.plugins_enabled)
		return;
	if (now - last_scan >= 5) {
		last_scan = now;
		plugin_rescan();
	}
	for (i = 0; i < PLUGIN_MAX; i++) {
		struct plugin *p = &plugins[i];

		if (!p->used || p->pid <= 0)
			continue;
		if (p->stopping) {
			if (now - p->stop_at >= 3) {
				kill(-p->pid, SIGKILL);
				kill(p->pid, SIGKILL);
			}
			continue;
		}
		measure(p);
		if (p->rss_kb > (unsigned long)p->ram_kb) {
			char why[160];

			snprintf(why, sizeof(why), "used %lu KB of memory, its budget is %d KB "
				 "(agctl plugin enable %s to try again)", p->rss_kb, p->ram_kb, p->name);
			stop(p, P_FAILED, why);
			continue;
		}
		p->over_cpu = p->cpu_now > p->cpu_pct ? p->over_cpu + 1 : 0;
		if (p->over_cpu >= 3) {
			char why[160];

			snprintf(why, sizeof(why), "used %.0f%% CPU for a while, its budget is %d%% "
				 "(agctl plugin enable %s to try again)", p->cpu_now, p->cpu_pct, p->name);
			stop(p, P_FAILED, why);
		}
	}
}

/* ---------- wiring ---------- */

void plugin_dispatch(const char *type, struct blob_attr *msg, const void *from)
{
	struct blob_buf b = { 0 };
	int i, j, built = 0;

	for (i = 0; i < PLUGIN_MAX; i++) {
		struct plugin *p = &plugins[i];
		int want = 0;

		if (!p->used || p->state != P_RUNNING || p == from)
			continue;
		for (j = 0; j < p->n_subs && !want; j++)
			want = type_match(p->subs[j], type) && (!priv_sub(p->subs[j]) || trusted(p)) &&
			       ((strcmp(type, AG_EV_DNS) && strcmp(type, AG_EV_FLOW)) || trusted(p));
		if (!want)
			continue;
		if (!built) {
			void *t;

			blob_buf_init(&b, 0);
			t = blobmsg_open_table(&b, "event");
			blobmsg_add_string(&b, "type", type);
			blobmsg_add_field(&b, BLOBMSG_TYPE_TABLE, "body", blob_data(msg), blob_len(msg));
			blobmsg_close_table(&b, t);
			built = 1;
		}
		send_blob(p, &b);
	}
	if (built)
		blob_buf_free(&b);
}

static struct plugin *action_owner(const char *action, int *idx)
{
	char name[48];
	const char *dot = strchr(action, '.');
	struct plugin *p;
	int i;

	if (!dot || (size_t)(dot - action) >= sizeof(name))
		return NULL;
	snprintf(name, sizeof(name), "%.*s", (int)(dot - action), action);
	if (!(p = find(name)))
		return NULL;
	for (i = 0; i < p->n_actions; i++)
		if (!strcmp(p->actions[i], dot + 1)) {
			*idx = i;
			return p;
		}
	return NULL;
}

int plugin_action(const char *action, int *device_subject, char *err, size_t errlen)
{
	struct plugin *p;
	int i;

	if (!(p = action_owner(action, &i))) {
		snprintf(err, errlen, "no plug-in offers the action '%s' (agctl plugins)", action);
		return -1;
	}
	*device_subject = p->action_device[i];
	return 0;
}

int plugin_act(const char *action, uint32_t id, const char *target, const struct device *dv, int ttl,
	       const char *reason, int undo, char *err, size_t errlen)
{
	struct blob_buf b = { 0 };
	struct plugin *p;
	void *t, *d;
	int i;

	if (!(p = action_owner(action, &i))) {
		snprintf(err, errlen, "plug-in for '%s' is gone", action);
		return -1;
	}
	if (p->state != P_RUNNING) {
		snprintf(err, errlen, "plug-in %s is %s", p->name, state_names[p->state]);
		return -1;
	}
	blob_buf_init(&b, 0);
	t = blobmsg_open_table(&b, undo ? "undo" : "act");
	blobmsg_add_u32(&b, "id", id);
	blobmsg_add_string(&b, "action", p->actions[i]);
	blobmsg_add_string(&b, "target", target);
	if (dv) {
		d = blobmsg_open_table(&b, "device");
		blobmsg_add_string(&b, "ip", dv->ip);
		blobmsg_add_string(&b, "mac", dv->mac);
		blobmsg_add_string(&b, "name", dv->hostname);
		blobmsg_add_string(&b, "zone", dv->zone);
		blobmsg_close_table(&b, d);
	}
	blobmsg_add_u32(&b, "ttl", (uint32_t)(ttl > 0 ? ttl : 0));
	blobmsg_add_string(&b, "reason", reason ? reason : "");
	blobmsg_close_table(&b, t);
	send_blob(p, &b);
	blob_buf_free(&b);
	return 0;
}

const struct ag_policy *plugin_policy_for(const char *kind, const char *zone, char *origin, size_t n, int *cap_mode)
{
	int i, j;

	for (i = 0; i < PLUGIN_MAX; i++) {
		struct plugin *p = &plugins[i];

		if (!p->used || (p->state != P_ACTIVE && p->state != P_RUNNING))
			continue;
		for (j = 0; j < p->n_policies; j++) {
			const struct ag_policy *pol = &p->policies[j];

			if (strcmp(pol->alert, kind) || (pol->zone[0] && strcmp(pol->zone, zone ? zone : "")))
				continue;
			snprintf(origin, n, "skill %s", p->name);
			/* a skill you added yourself only proposes until you allow it */
			*cap_mode = trusted(p) ? AG_MODE_ENFORCE : AG_MODE_ADVISE;
			return pol;
		}
	}
	return NULL;
}

/* ---------- API ---------- */

int plugin_count(int running)
{
	int i, n = 0;

	for (i = 0; i < PLUGIN_MAX; i++)
		if (plugins[i].used && (!running || plugins[i].state == P_RUNNING || plugins[i].state == P_ACTIVE))
			n++;
	return n;
}

static void list_blob(struct blob_buf *b, const char *name, const char *base, int count, size_t width)
{
	void *arr = blobmsg_open_array(b, name);
	int i;

	for (i = 0; i < count; i++)
		blobmsg_add_string(b, NULL, base + (size_t)i * width);
	blobmsg_close_array(b, arr);
}

void plugin_blob(struct blob_buf *b, const char *name, const char *only)
{
	void *arr = blobmsg_open_array(b, name), *t, *x;
	int i, j;

	for (i = 0; i < PLUGIN_MAX; i++) {
		struct plugin *p = &plugins[i];

		if (!p->used || (only && strcmp(only, p->name)))
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", p->name);
		blobmsg_add_string(b, "kind", p->kind);
		blobmsg_add_string(b, "version", p->version);
		blobmsg_add_string(b, "description", p->description);
		blobmsg_add_string(b, "state", state_names[p->state]);
		blobmsg_add_string(b, "why", p->why);
		blobmsg_add_string(b, "source", p->system ? "package" : "added");
		blobmsg_add_u8(b, "trusted", (uint8_t)trusted(p));
		blobmsg_add_string(b, "dir", p->dir);
		list_blob(b, "emits", &p->emits[0][0], p->n_emits, sizeof(p->emits[0]));
		list_blob(b, "subscribes", &p->subs[0][0], p->n_subs, sizeof(p->subs[0]));
		list_blob(b, "needs", &p->needs[0][0], p->n_needs, sizeof(p->needs[0]));
		list_blob(b, "alerts", &p->alerts[0][0], p->n_alerts, sizeof(p->alerts[0]));
		x = blobmsg_open_array(b, "actions");
		for (j = 0; j < p->n_actions; j++) {
			char full[64];

			snprintf(full, sizeof(full), "%s.%s", p->name, p->actions[j]);
			blobmsg_add_string(b, NULL, full);
		}
		blobmsg_close_array(b, x);
		x = blobmsg_open_array(b, "granted");
		for (j = 0; j < p->n_caps; j++)
			if (has_cap(p, p->caps[j]))
				blobmsg_add_string(b, NULL, p->caps[j]);
		blobmsg_close_array(b, x);
		x = blobmsg_open_array(b, "waiting_for_ok");
		for (j = 0; j < p->n_caps; j++)
			if (!has_cap(p, p->caps[j]))
				blobmsg_add_string(b, NULL, p->caps[j]);
		for (j = 0; j < p->n_subs; j++)
			if (priv_sub(p->subs[j]) && !trusted(p))
				blobmsg_add_string(b, NULL, p->subs[j]);
		blobmsg_close_array(b, x);
		x = blobmsg_open_array(b, "policies");
		for (j = 0; j < p->n_policies; j++) {
			void *pt = blobmsg_open_table(b, NULL);

			blobmsg_add_string(b, "alert", p->policies[j].alert);
			blobmsg_add_string(b, "action", p->policies[j].action);
			blobmsg_add_string(b, "zone", p->policies[j].zone);
			blobmsg_add_string(b, "mode", ag_mode_str(p->policies[j].mode));
			blobmsg_close_table(b, pt);
		}
		blobmsg_close_array(b, x);
		if (p->card_title[0]) {
			x = blobmsg_open_table(b, "card");
			blobmsg_add_string(b, "title", p->card_title);
			blobmsg_add_string(b, "text", p->card_text);
			blobmsg_close_table(b, x);
		}
		x = blobmsg_open_table(b, "budget");
		blobmsg_add_u32(b, "ram_kb", (uint32_t)p->ram_kb);
		blobmsg_add_u32(b, "cpu_pct", (uint32_t)p->cpu_pct);
		blobmsg_close_table(b, x);
		if (p->pid > 0) {
			x = blobmsg_open_table(b, "process");
			blobmsg_add_u32(b, "pid", (uint32_t)p->pid);
			blobmsg_add_u64(b, "started", (uint64_t)p->started);
			blobmsg_add_u32(b, "rss_kb", (uint32_t)p->rss_kb);
			blobmsg_add_double(b, "cpu_pct", p->cpu_now);
			blobmsg_close_table(b, x);
		}
		blobmsg_add_u32(b, "restarts", p->restarts);
		blobmsg_add_u64(b, "messages_in", p->events_in);
		blobmsg_add_u64(b, "messages_out", p->events_out);
		blobmsg_add_u64(b, "dropped", p->dropped);
		blobmsg_add_u64(b, "last_event", (uint64_t)p->last_event);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

/* what: "allow" (privileged capabilities) or "enabled" */
int plugin_set(const char *name, const char *what, int on, char *err, size_t errlen)
{
	struct plugin *p = find(name);

	if (!p) {
		snprintf(err, errlen, "no plug-in '%s' (agctl plugins)", name);
		return -1;
	}
	if (!strcmp(what, "allow") && p->system) {
		snprintf(err, errlen, "%s comes from a package and is already trusted", name);
		return -1;
	}
	if (write_uci(name, what, on ? "1" : "0", err, errlen))
		return -1;
	read_uci();
	if (!strcmp(what, "enabled") && on && (p->state == P_FAILED || p->state == P_DISABLED)) {
		memset(p->crashes, 0, sizeof(p->crashes));
		p->state = P_WAITING;
	}
	/* new grants reach a running plug-in through a fresh hello: restart it */
	if (!strcmp(what, "allow") && p->state == P_RUNNING)
		stop(p, P_WAITING, "restarting with new permissions");
	else if (!strcmp(what, "allow") && p->state == P_ACTIVE)
		p->state = P_WAITING;
	evaluate(p);
	return 0;
}

/* a plug-in setting ("" removes it); the plug-in restarts to see it */
int plugin_config(const char *name, const char *key, const char *value, char *err, size_t errlen)
{
	struct plugin *p = find(name);
	size_t i;
	int j;

	if (!p) {
		snprintf(err, errlen, "no plug-in '%s' (agctl plugins)", name);
		return -1;
	}
	for (i = 0; key[i]; i++)
		if (!((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= '0' && key[i] <= '9') || key[i] == '_'))
			break;
	if (!*key || key[i] || i > 31 || !strcmp(key, "allow") || !strcmp(key, "enabled") || !strcmp(key, "name")) {
		snprintf(err, errlen, "setting names are up to 31 of a-z, 0-9 and _ (allow/enabled have their own commands)");
		return -1;
	}
	if (strlen(value) >= sizeof(p->conf[0].v) || strpbrk(value, "\n\r'")) {
		snprintf(err, errlen, "value too long (max %zu) or has a quote or line break", sizeof(p->conf[0].v) - 1);
		return -1;
	}
	if (write_uci(name, key, value, err, errlen))
		return -1;
	if (!*value)                        /* removed: unset now; the manifest default returns when agd restarts */
		for (j = 0; j < p->n_conf; j++)
			if (!strcmp(p->conf[j].k, key))
				p->conf[j] = p->conf[--p->n_conf];
	read_uci();
	if (p->state == P_RUNNING)
		stop(p, P_WAITING, "restarting with new settings");
	else if (p->state == P_ACTIVE)
		p->state = P_WAITING;
	evaluate(p);
	return 0;
}

void plugin_init(void)
{
	int i;

	for (i = 0; i < PLUGIN_MAX; i++)
		plugins[i].in.fd = plugins[i].out.fd = plugins[i].err.fd = -1;
	if (!cfg.plugins_enabled) {
		AG_INFO("plug-ins turned off in config");
		return;
	}
	plugin_rescan();
	AG_INFO("plug-ins: %d found (%s, %s)", plugin_count(0), cfg.plugin_dir, cfg.plugin_user_dir);
}

void plugin_stop_all(void)
{
	int i;

	for (i = 0; i < PLUGIN_MAX; i++)
		if (plugins[i].used && plugins[i].pid > 0) {
			uloop_process_delete(&plugins[i].proc);
			kill(-plugins[i].pid, SIGTERM);
			kill(plugins[i].pid, SIGTERM);
		}
}
