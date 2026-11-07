/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ag-bridge: links this gateway to cortex, AIGW's optional services outside.
 *
 *   gateway (agd)                     bridge                    cortex (cloud, PC, NAS)
 *   status, devices, alerts,  ──────► report ── HTTPS POST ───► fleet history, people's
 *   decisions, models                          /v1/report       answers become labels
 *   signed model update       ◄────── offer ◄─ GET /v1/models ─ retrained, signed models
 *     (agd checks the signature and hot-loads it; the bridge can't install anything itself)
 *   proposal, waits for you   ◄────── propose ◄ GET /v1/proposals  suggestions (e.g. from
 *                                     result ─► POST /v1/proposals/<id>   the MCP assistant)
 *
 * Everything outward goes through curl (TLS, certificate checks), run as the
 * unprivileged plug-in user. What leaves the home: the gateway's counters,
 * the device list (settings: devices = names | anonymous | none), alerts and
 * decisions. Never packets, DNS logs or traffic contents. A missed report is
 * covered by the next one, which carries the same recent history.
 *
 * Settings (agctl plugin config bridge <key> <value>):
 *   url        cortex base URL, e.g. https://cortex.example.net:8790
 *   token      this gateway's token from `aigw_cortex enroll`
 *   interval   seconds between check-ins (default 300)
 *   devices    names | anonymous | none
 *   models     1 = fetch model updates;  proposals  1 = fetch suggestions
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>

#include "ag/sign.h"

#define CURL_TIMEOUT "30"
#define MAX_DOWNLOAD "8388608"
#define SEEN_MAX     8

static struct {
	char url[128], token[128], devices[16], state[256], host[16], aigw[16];
	int interval, models, proposals;
	int can_read, can_propose, can_model;
} c = { .devices = "names", .interval = 300, .models = 1, .proposals = 1 };

static char *ibuf;
static size_t ilen, icap;
static struct blob_buf got;          /* the reply we wait for */
static int want_id, have_reply, next_id = 1;
static char reply_err[256];
static int configured;
/* versions agd turned down: not fetched again for an hour (e.g. until you trust the key) */
static struct { char name[32]; int version; time_t at; } refused[SEEN_MAX];

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------- JSON helpers ---------- */

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

static const char *fstr(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : NULL;
}

static long fnum(struct blob_attr *tbl, const char *name, long dflt)
{
	struct blob_attr *a = field(tbl, name);

	if (!a)
		return dflt;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (long)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);     /* also BOOL */
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	case BLOBMSG_TYPE_STRING: return atol(blobmsg_get_string(a));
	default: return dflt;
	}
}

static void copy_attr(struct blob_buf *b, const char *name, struct blob_attr *a)
{
	if (a)
		blobmsg_add_field(b, blobmsg_type(a), name, blobmsg_data(a), blobmsg_data_len(a));
}

static int write_json(const char *path, struct blob_buf *b)
{
	char *json = blobmsg_format_json(b->head, true);
	FILE *f;
	int ok;

	if (!json)
		return -1;
	f = fopen(path, "w");
	ok = f && fputs(json, f) >= 0;
	if (f && fclose(f))
		ok = 0;
	free(json);
	return ok ? 0 : -1;
}

static int read_json(const char *path, struct blob_buf *b)
{
	char *text;
	long n;
	FILE *f = fopen(path, "r");
	int ok = 0;

	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	if (n > 0 && n < 4 * 1024 * 1024 && (text = malloc((size_t)n + 1))) {
		if (fread(text, 1, (size_t)n, f) == (size_t)n) {
			text[n] = '\0';
			blob_buf_init(b, 0);
			ok = blobmsg_add_json_from_string(b, text);
		}
		free(text);
	}
	fclose(f);
	return ok ? 0 : -1;
}

/* ---------- talking to agd ---------- */

static void to_agd(struct blob_buf *b)
{
	char *json = blobmsg_format_json(b->head, true);

	if (json) {
		fputs(json, stdout);
		fputc('\n', stdout);
		fflush(stdout);
		free(json);
	}
}

static void on_hello(struct blob_attr *h)
{
	struct blob_attr *conf = field(h, "config"), *g, *cur;
	const char *v;
	size_t rem;

	if ((v = fstr(h, "host")))
		snprintf(c.host, sizeof(c.host), "%s", v);
	if ((v = fstr(h, "aigw")))
		snprintf(c.aigw, sizeof(c.aigw), "%s", v);
	if ((v = fstr(conf, "url")))
		snprintf(c.url, sizeof(c.url), "%s", v);
	if ((v = fstr(conf, "token")))
		snprintf(c.token, sizeof(c.token), "%s", v);
	if ((v = fstr(conf, "devices")))
		snprintf(c.devices, sizeof(c.devices), "%s", v);
	c.interval = (int)fnum(conf, "interval", 300);
	if (c.interval < 2)
		c.interval = 2;
	c.models = (int)fnum(conf, "models", 1);
	c.proposals = (int)fnum(conf, "proposals", 1);
	if ((g = field(h, "granted")))
		blobmsg_for_each_attr(cur, g, rem) {
			const char *cap = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : "";

			c.can_read |= !strcmp(cap, "devices.read");
			c.can_propose |= !strcmp(cap, "decision.propose");
			c.can_model |= !strcmp(cap, "model.offer");
		}
	configured = 1;
}

static void handle_line(char *line)
{
	struct blob_buf b = { 0 };
	struct blob_attr *a;

	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, line)) {
		blob_buf_free(&b);
		return;
	}
	if ((a = field(b.head, "hello")) && blobmsg_type(a) == BLOBMSG_TYPE_TABLE) {
		on_hello(a);
	} else if (field(b.head, "reply") && fnum(b.head, "reply", -1) == want_id) {
		const char *e = fstr(b.head, "error");

		blob_buf_init(&got, 0);
		if ((a = field(b.head, "data")) && blobmsg_type(a) == BLOBMSG_TYPE_TABLE)
			blobmsg_add_field(&got, BLOBMSG_TYPE_TABLE, "data", blobmsg_data(a), blobmsg_data_len(a));
		snprintf(reply_err, sizeof(reply_err), "%s", e ? e : "");
		have_reply = 1;
	}
	blob_buf_free(&b);
}

/* read and handle agd's lines for up to timeout_ms; -1 when agd went away */
static int pump(int timeout_ms)
{
	struct pollfd pfd = { .fd = 0, .events = POLLIN };
	char *nl;
	ssize_t n;
	int r = poll(&pfd, 1, timeout_ms);

	if (r < 0)
		return errno == EINTR ? 0 : -1;
	if (r == 0)
		return 0;
	if (ilen + 65536 > icap) {
		char *nb = realloc(ibuf, icap ? icap * 2 : 131072);

		if (!nb)
			return -1;
		ibuf = nb;
		icap = icap ? icap * 2 : 131072;
	}
	n = read(0, ibuf + ilen, icap - ilen - 1);
	if (n <= 0)
		return n < 0 && errno == EINTR ? 0 : -1;
	ilen += (size_t)n;
	while ((nl = memchr(ibuf, '\n', ilen))) {
		size_t len = (size_t)(nl - ibuf);

		*nl = '\0';
		handle_line(ibuf);
		memmove(ibuf, nl + 1, ilen - len - 1);
		ilen -= len + 1;
	}
	return 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* send b (with "id" filled in by the caller) and wait for agd's reply: the data table, or NULL */
static struct blob_attr *ask(struct blob_buf *b, int id)
{
	uint64_t until = now_ms() + 10000;

	want_id = id;
	have_reply = 0;
	reply_err[0] = '\0';
	to_agd(b);
	while (!have_reply && now_ms() < until)
		if (pump((int)(until - now_ms())) < 0)
			exit(0);
	want_id = 0;
	if (!have_reply) {
		snprintf(reply_err, sizeof(reply_err), "no answer from agd");
		return NULL;
	}
	return field(got.head, "data");
}

static struct blob_attr *query(const char *what)
{
	struct blob_buf b = { 0 };
	struct blob_attr *d;
	int id = next_id++;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "query", what);
	blobmsg_add_u32(&b, "id", (uint32_t)id);
	d = ask(&b, id);
	blob_buf_free(&b);
	return d;
}

/* ---------- talking to cortex (curl) ---------- */

static int safe_token(const char *s)
{
	for (; *s; s++)
		if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
		      *s == '-' || *s == '_' || *s == '.' || *s == '~' || *s == '+' || *s == '/' || *s == '='))
			return 0;
	return 1;
}

static int safe_url(const char *s)
{
	if (strncmp(s, "https://", 8) && strncmp(s, "http://", 7))
		return 0;
	return !strpbrk(s, " \t\"'\\<>`");
}

/* the token goes to curl in a file only this plug-in can read, never on the command line */
static int write_curl_conf(void)
{
	char path[300];
	FILE *f;
	int fd;

	snprintf(path, sizeof(path), "%s/curl.conf", c.state);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	fprintf(f, "header = \"Authorization: Bearer %s\"\nheader = \"User-Agent: aigw-bridge/%s\"\n", c.token, c.aigw);
	return fclose(f) ? -1 : 0;
}

/* 0 on HTTP 2xx; the response body lands in out */
static int http(const char *method, const char *path, const char *body, const char *out)
{
	char url[400], conf[300];
	const char *argv[24];
	int n = 0, status;
	pid_t pid;

	snprintf(url, sizeof(url), "%s%s", c.url, path);
	snprintf(conf, sizeof(conf), "%s/curl.conf", c.state);
	argv[n++] = "curl";
	argv[n++] = "-sS";
	argv[n++] = "--fail";
	argv[n++] = "--max-time";
	argv[n++] = CURL_TIMEOUT;
	argv[n++] = "--max-filesize";
	argv[n++] = MAX_DOWNLOAD;
	argv[n++] = "--proto";
	argv[n++] = "=http,https";
	argv[n++] = "-K";
	argv[n++] = conf;
	argv[n++] = "-o";
	argv[n++] = out;
	if (!strcmp(method, "POST")) {
		static char data[320];

		snprintf(data, sizeof(data), "@%s", body);
		argv[n++] = "-H";
		argv[n++] = "Content-Type: application/json";
		argv[n++] = "--data-binary";
		argv[n++] = data;
	}
	argv[n++] = url;
	argv[n] = NULL;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0)
			dup2(fd, 0), dup2(fd, 1);       /* stdout is agd's channel: keep curl off it */
		execvp("curl", (char *const *)argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		logf_("%s %s: curl failed (exit %d)", method, path, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return -1;
	}
	return 0;
}

/* ---------- report ---------- */

static void anon_id(const char *mac, char out[13])
{
	uint8_t h[64];
	char buf[256], hex[129];
	int i;

	snprintf(buf, sizeof(buf), "%s|%s", c.token, mac);
	ag_sha512((const uint8_t *)buf, strlen(buf), h);
	for (i = 0; i < 6; i++)
		sprintf(hex + 2 * i, "%02x", h[i]);
	memcpy(out, hex, 12);
	out[12] = '\0';
}

static const char *const dev_fields[] = {
	"ip", "mac", "hostname", "type", "zone", "wifi_key", "first_seen", "last_seen", "queries", "flows",
	"flows_no_dns", "bytes_up", "bytes_down", NULL
};

static void add_devices(struct blob_buf *r, struct blob_attr *devs)
{
	struct blob_attr *d;
	void *arr, *t;
	size_t rem;
	int i, anon = !strcmp(c.devices, "anonymous");

	if (!devs || !strcmp(c.devices, "none"))
		return;
	arr = blobmsg_open_array(r, "devices");
	blobmsg_for_each_attr(d, devs, rem) {
		char id[13];
		const char *mac = fstr(d, "mac");
		struct blob_attr *w = field(d, "wifi");

		t = blobmsg_open_table(r, NULL);
		for (i = 0; dev_fields[i]; i++) {
			const char *k = dev_fields[i];

			if (anon && (!strcmp(k, "ip") || !strcmp(k, "mac") || !strcmp(k, "hostname") || !strcmp(k, "wifi_key")))
				continue;
			copy_attr(r, k, field(d, k));
		}
		if (anon) {
			anon_id(mac ? mac : "", id);
			blobmsg_add_string(r, "id", id);
		}
		if (w && field(w, "connected"))
			blobmsg_add_u8(r, "online", (uint8_t)fnum(w, "connected", 0));
		blobmsg_close_table(r, t);
	}
	blobmsg_close_array(r, arr);
}

static void add_list(struct blob_buf *r, const char *name, struct blob_attr *items, const char *const *keep)
{
	struct blob_attr *e;
	void *arr, *t;
	size_t rem;
	int i, anon = !strcmp(c.devices, "anonymous") || !strcmp(c.devices, "none");

	if (!items)
		return;
	arr = blobmsg_open_array(r, name);
	blobmsg_for_each_attr(e, items, rem) {
		t = blobmsg_open_table(r, NULL);
		for (i = 0; keep[i]; i++) {
			/* reasons and device names describe people's devices: only with devices = names */
			if (anon && (!strcmp(keep[i], "reason") || !strcmp(keep[i], "device") || !strcmp(keep[i], "ip") ||
				     !strcmp(keep[i], "mac")))
				continue;
			copy_attr(r, keep[i], field(e, keep[i]));
		}
		blobmsg_close_table(r, t);
	}
	blobmsg_close_array(r, arr);
}

static const char *const alert_fields[] = {
	"id", "kind", "severity", "first", "last", "count", "device", "subject", "score", "reason", NULL
};
static const char *const decision_fields[] = {
	"id", "state", "action", "target", "device", "origin", "alert", "ttl", "automatic", "created", "decided",
	"expires", "reason", "note", NULL
};
static const char *const model_fields[] = {
	"name", "loaded", "version", "trees", "source", "signer", "origin", "trained", "installed", "held_back",
	"refused", NULL
};

static struct blob_buf models_now;

static void report(void)
{
	struct blob_buf r = { 0 };
	struct blob_attr *d;
	char path[300], out[300];

	blob_buf_init(&r, 0);
	blobmsg_add_string(&r, "bridge", c.aigw);
	if ((d = query("status")))
		copy_attr(&r, "status", d);
	blob_buf_init(&models_now, 0);
	if ((d = query("models")) && (d = field(d, "models"))) {
		blobmsg_add_field(&models_now, BLOBMSG_TYPE_ARRAY, "models", blobmsg_data(d), blobmsg_data_len(d));
		add_list(&r, "models", field(models_now.head, "models"), model_fields);
	}
	if (c.can_read) {
		/* each answer replaces the last one: use it before asking the next question */
		if ((d = query("alerts")) && (d = field(d, "alerts")))
			add_list(&r, "alerts", d, alert_fields);
		if ((d = query("decisions")) && (d = field(d, "decisions")))
			add_list(&r, "decisions", d, decision_fields);
		if ((d = query("devices")) && (d = field(d, "devices")))
			add_devices(&r, d);
	}
	snprintf(path, sizeof(path), "%s/report.json", c.state);
	snprintf(out, sizeof(out), "%s/report.out", c.state);
	if (write_json(path, &r) == 0 && http("POST", "/v1/report", path, out) == 0)
		logf_("checked in with cortex");
	blob_buf_free(&r);
}

/* ---------- model updates ---------- */

static int local_version(const char *name)
{
	struct blob_attr *m, *arr = field(models_now.head, "models");
	size_t rem;

	if (!arr)
		return -1;
	blobmsg_for_each_attr(m, arr, rem) {
		const char *n = fstr(m, "name");

		if (n && !strcmp(n, name)) {
			int v = (int)fnum(m, "version", 0), hold = (int)fnum(m, "held_back", 0);

			return v > hold ? v : hold;
		}
	}
	return -2;                           /* not a model this gateway uses */
}

static int safe_file(const char *s)
{
	if (!*s || *s == '.' || strlen(s) > 90)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'))
			return 0;
	return 1;
}

static int was_refused(const char *name, int version)
{
	int i;

	for (i = 0; i < SEEN_MAX; i++)
		if (refused[i].version == version && !strcmp(refused[i].name, name) && time(NULL) - refused[i].at < 3600)
			return 1;
	return 0;
}

static void remember_refused(const char *name, int version)
{
	static int k;

	snprintf(refused[k].name, sizeof(refused[k].name), "%s", name);
	refused[k].version = version;
	refused[k].at = time(NULL);
	k = (k + 1) % SEEN_MAX;
}

static void fetch_models(void)
{
	struct blob_buf list = { 0 }, b = { 0 };
	struct blob_attr *m, *arr;
	char out[300];
	size_t rem;

	snprintf(out, sizeof(out), "%s/models.json", c.state);
	if (http("GET", "/v1/models", NULL, out) || read_json(out, &list) || !(arr = field(list.head, "models"))) {
		blob_buf_free(&list);
		return;
	}
	blobmsg_for_each_attr(m, arr, rem) {
		const char *name = fstr(m, "name"), *file = fstr(m, "file"), *sig = fstr(m, "sig");
		int version = (int)fnum(m, "version", 0), have;
		char fpath[300], spath[300], url[200];
		struct blob_attr *d;
		void *t;
		int id;

		if (!name || !file || !sig || !safe_file(file) || !safe_file(sig))
			continue;
		have = local_version(name);
		if (have == -2 || version <= have || was_refused(name, version))
			continue;
		snprintf(fpath, sizeof(fpath), "%s/%s", c.state, file);
		snprintf(spath, sizeof(spath), "%s/%s", c.state, sig);
		snprintf(url, sizeof(url), "/v1/models/%s", file);
		if (http("GET", url, NULL, fpath))
			continue;
		snprintf(url, sizeof(url), "/v1/models/%s", sig);
		if (http("GET", url, NULL, spath)) {
			unlink(fpath);
			continue;
		}
		id = next_id++;
		blob_buf_init(&b, 0);
		t = blobmsg_open_table(&b, "model");
		blobmsg_add_string(&b, "file", fpath);
		blobmsg_add_string(&b, "sig", spath);
		blobmsg_add_u32(&b, "id", (uint32_t)id);
		blobmsg_close_table(&b, t);
		d = ask(&b, id);
		if (d && fnum(d, "ok", 0))
			logf_("model %s version %d handed to agd: in use", name, version);
		else {
			logf_("model %s version %d refused by agd: %s", name, version, reply_err);
			remember_refused(name, version);
		}
		unlink(fpath);
		unlink(spath);
	}
	blob_buf_free(&b);
	blob_buf_free(&list);
}

/* ---------- proposals ---------- */

static void fetch_proposals(void)
{
	struct blob_buf list = { 0 }, b = { 0 };
	struct blob_attr *p, *arr;
	char out[300], body[300], rout[300];
	size_t rem;

	snprintf(out, sizeof(out), "%s/proposals.json", c.state);
	if (http("GET", "/v1/proposals", NULL, out) || read_json(out, &list) || !(arr = field(list.head, "proposals"))) {
		blob_buf_free(&list);
		return;
	}
	snprintf(body, sizeof(body), "%s/result.json", c.state);
	snprintf(rout, sizeof(rout), "%s/result.out", c.state);
	blobmsg_for_each_attr(p, arr, rem) {
		long pid = fnum(p, "id", 0);
		const char *action = fstr(p, "action"), *target = fstr(p, "target"), *reason = fstr(p, "reason");
		const char *origin = fstr(p, "origin");
		struct blob_attr *d = NULL;
		char why[400], path[64];
		long decision = 0;
		int ok = 0, id = next_id++;
		void *t;

		if (!pid)
			continue;
		if (action && target) {
			snprintf(why, sizeof(why), "from cortex (%s): %.300s", origin ? origin : "?", reason ? reason : "");
			blob_buf_init(&b, 0);
			t = blobmsg_open_table(&b, "propose");
			blobmsg_add_string(&b, "action", action);
			blobmsg_add_string(&b, "target", target);
			blobmsg_add_u32(&b, "ttl", (uint32_t)fnum(p, "ttl", 0));
			blobmsg_add_string(&b, "reason", why);
			blobmsg_add_u32(&b, "id", (uint32_t)id);
			blobmsg_close_table(&b, t);
			d = ask(&b, id);
			ok = d && fnum(d, "ok", 0);
			decision = d ? fnum(d, "decision", 0) : 0;
		} else {
			snprintf(reply_err, sizeof(reply_err), "needs action and target");
		}
		logf_("proposal %ld (%s %s): %s", pid, action ? action : "?", target ? target : "?",
		      ok ? "waiting for approval on the gateway" : reply_err);
		blob_buf_init(&b, 0);
		blobmsg_add_u8(&b, "ok", (uint8_t)ok);
		blobmsg_add_u32(&b, "decision", (uint32_t)decision);
		blobmsg_add_string(&b, "error", ok ? "" : reply_err);
		snprintf(path, sizeof(path), "/v1/proposals/%ld", pid);
		if (write_json(body, &b) == 0)
			http("POST", path, body, rout);
	}
	blob_buf_free(&b);
	blob_buf_free(&list);
}

/* ---------- main ---------- */

int main(void)
{
	const char *st = getenv("AIGW_STATE");
	uint64_t next = 0;

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!st || !*st) {
		logf_("no AIGW_STATE: run me from agd");
		return 1;
	}
	snprintf(c.state, sizeof(c.state), "%s", st);
	umask(077);
	while (!configured)
		if (pump(1000) < 0)
			return 0;
	if (!safe_url(c.url)) {
		logf_("setting 'url' must be an http:// or https:// address (agctl bridge <url>)");
		return 1;
	}
	if (!safe_token(c.token) || write_curl_conf()) {
		logf_("setting 'token' has odd characters (agctl bridge token)");
		return 1;
	}
	logf_("linking %s to %s every %d s (devices: %s%s%s)", c.host, c.url, c.interval, c.devices,
	      c.models && c.can_model ? ", model updates" : "", c.proposals && c.can_propose ? ", proposals" : "");
	next = now_ms() + 1000;
	for (;;) {
		uint64_t t = now_ms();

		if (t >= next) {
			report();
			if (c.models && c.can_model)
				fetch_models();
			if (c.proposals && c.can_propose)
				fetch_proposals();
			next = now_ms() + (uint64_t)c.interval * 1000;
			continue;
		}
		if (pump((int)(next - t)) < 0)
			return 0;
	}
}
