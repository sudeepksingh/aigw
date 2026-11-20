/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Smarter mesh Wi-Fi (v0.44.0): AIGW as the intelligence over a standard
 * Wi-Fi Alliance EasyMesh controller (prplMesh is the reference), not another
 * 1905.1 stack. The rules are pure (core/libag/mesh.c, ag/mesh.h); this reads
 * the controller, keeps each device's history and acts through the controller.
 *
 *   read     every mesh.poll seconds, the controller's Data Elements model
 *            (TR-181 Device.WiFi.DataElements.Network.) from a backend:
 *              - the mesh handler (mesh.handler; the platform's is
 *                /usr/libexec/aigw/mesh-prplmesh): "get <file>" writes the
 *                model as JSON (ubus _get, path-keyed or nested) or as
 *                "path=value" lines (ubus-cli, ba-cli, obuspa)
 *              - a snapshot file (mesh.source): tests, and controllers whose
 *                northbound is elsewhere
 *            Another vendor's controller plugs in with its own handler: the
 *            same three commands (get, steer, channel), nothing in agd changes.
 *
 *   decide   (only while no other mesh decision is live: one change at a time,
 *            so each one's effect can be measured)
 *              - steer a struggling device to an access point that hears it
 *                clearly better (ag/mesh.h for the rules); what AIGW already
 *                knows joins in: Self-Healing Wi-Fi's loss for the device, a
 *                call or game on it (never then), act.protect, the device's
 *                own steering history
 *              - the channel plan: an extender on the same air as the gateway
 *                where it is avoidable
 *            Each one is a decision through act.c as act.mode allows: enforce
 *            applies it (within the hourly limit), advise proposes it and a
 *            person says yes. Never during an area outage (area.c).
 *
 *   act      "meshsteer <station>@<BSSID>": the handler's "steer" (prplMesh:
 *            Network.ClientSteering(): a BTM request, or its blocklist for a
 *            device without 802.11v). "meshchan <RUID>:<channel>": the handler's
 *            "channel" (prplMesh: its on-demand channel selection).
 *
 *   trial    measured mesh.trial seconds after it took effect, against just
 *            before: kept when clearly better, else taken back (moved back,
 *            the channel back) and that move avoided for a day. A steer that
 *            didn't land is a miss: two in a row pause that device (doubling).
 *
 *   explain  ag.mesh events (the why timeline), MeshDecision! (the data
 *            model), Device.X_AIGW_.Mesh., the backhaul's findings for the
 *            help-desk brief and proactive care, `agctl mesh`, and counts for
 *            cortex's fleet dashboard.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

#include "agd.h"
#include "ag/brief.h"
#include "ag/mesh.h"

#define HIST_MAX   64
#define TRIED_MAX  8
#define READ_MAX   (4 << 20)            /* a snapshot this big is not a home's */
#define LAST_MAX   8

static struct mesh_net N, R;            /* the model now, and the one being read */
static int have;                        /* N holds a read */
static long read_at;
static char status[16] = "None";        /* None, NoController, Ok, Error, Disabled */
static char last_err[200];
static int fails;
static int busy;
static long busy_since;
static struct uloop_timeout tm;
static struct mesh_params P;

static struct mesh_hist hist[HIST_MAX];
static char why_not[HIST_MAX][120];     /* why it wasn't moved at the last look */

/* the change on trial: one at a time */
enum { TK_STEER, TK_CHAN };
static struct {
	int on;
	uint32_t id;
	int kind;
	char sta[18], from[18], to[18];         /* steer */
	char ruid[18];                          /* channel */
	int from_ch, to_ch;
	long started, judge_at;
	int landed;
	int no_back;                            /* ends without being taken back (it didn't happen) */
	struct mesh_sample before;
	int util_before;
} T;

/* channel changes that didn't help: not again for a day */
static struct {
	char ruid[18];
	int ch;
	long until;
} tried[TRIED_MAX];

/* backhaul findings now, with since */
static struct mesh_bh_finding BH[MESH_DEVS];
static int nbh;
static struct {
	char id[18];
	int kind;
	long since;
} bh_seen[MESH_DEVS];

static struct mesh_chan plan[4];
static int nplan;

/* counts (kept with agd's state) */
static struct {
	uint32_t steers, kept, undone, misses, proposed;
	uint32_t chans, chans_kept, chans_undone, chan_misses;
	uint32_t roams;                         /* moves the device or the controller made */
	uint32_t last_id;
	char last_kind[12], last_state[16];
	char last_summary[400];
	long last_at;
	long since;
} C;

static void schedule(long s);

static long now_s(void)
{
	return (long)ag_wall_s();
}

static void params(void)
{
	mesh_params_default(&P);
	P.weak_dbm = cfg.mesh_weak_dbm;
	P.hyst_db = cfg.mesh_hyst_db;
	P.hold_s = cfg.mesh_hold;
	P.cooldown_s = cfg.mesh_cooldown;
	P.quiet_s = cfg.mesh_quiet;
	P.legacy = cfg.mesh_legacy;
}

/* ---------- small helpers ---------- */

static int is_mac(const char *s)
{
	int i;

	if (!s || strlen(s) != 17)
		return 0;
	for (i = 0; i < 17; i++)
		if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i]))
			return 0;
	return 1;
}

static void lower(char *s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

static const char *band_words(int b)
{
	return b == 2 ? "2.4 GHz" : b == 5 ? "5 GHz" : b == 6 ? "6 GHz" : "Wi-Fi";
}

/* a station's name for people: its hostname, else its MAC */
static const char *sta_name(const char *mac)
{
	const struct device *d = dev_find_mac(mac, 0);

	return d && d->hostname[0] ? d->hostname : mac;
}

static struct mesh_hist *hist_of(const char *mac, int create, int *idx)
{
	int i, old = 0;

	for (i = 0; i < HIST_MAX; i++)
		if (hist[i].mac[0] && !strcasecmp(hist[i].mac, mac)) {
			if (idx)
				*idx = i;
			return &hist[i];
		}
	if (!create)
		return NULL;
	for (i = 0; i < HIST_MAX; i++) {
		if (!hist[i].mac[0]) {
			old = i;
			break;
		}
		if (hist[i].seen < hist[old].seen)
			old = i;
	}
	memset(&hist[old], 0, sizeof(hist[old]));
	why_not[old][0] = '\0';
	snprintf(hist[old].mac, sizeof(hist[old].mac), "%s", mac);
	lower(hist[old].mac);
	hist[old].retry_pct = -1;
	if (idx)
		*idx = old;
	return &hist[old];
}

static int protected_mac(const char *mac)
{
	struct device tmp;
	const struct device *d = dev_find_mac(mac, 0);

	if (d)
		return act_is_protected(d);
	memset(&tmp, 0, sizeof(tmp));
	snprintf(tmp.mac, sizeof(tmp.mac), "%s", mac);
	return act_is_protected(&tmp);
}

/* the gateway's own measure of a station's loss (Self-Healing Wi-Fi's probes): -1 not measured */
static int loss_of(const char *mac)
{
	const struct device *d = dev_find_mac(mac, 0);
	int loss = -1, rtt = -1, n = 0;

	if (!d)
		return -1;
	heal_device(d, &loss, &rtt, &n);
	return n > 0 ? loss : -1;
}

static void ev(const char *what, const char *mac, const char *from, const char *to, uint32_t id, const char *summary)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	if (mac && *mac) {
		blobmsg_add_string(&b, "mac", mac);
		blobmsg_add_string(&b, "hostname", sta_name(mac) != mac ? sta_name(mac) : "");
	}
	if (from)
		blobmsg_add_string(&b, "from", from);
	if (to)
		blobmsg_add_string(&b, "to", to);
	if (id)
		blobmsg_add_u32(&b, "decision", id);
	blobmsg_add_string(&b, "summary", summary ? summary : "");
	agd_emit(AG_EV_MESH, &b);
	blob_buf_free(&b);
}

static void last(uint32_t id, const char *kind, const char *state, const char *summary)
{
	C.last_id = id;
	snprintf(C.last_kind, sizeof(C.last_kind), "%s", kind);
	snprintf(C.last_state, sizeof(C.last_state), "%s", state);
	snprintf(C.last_summary, sizeof(C.last_summary), "%s", summary);
	C.last_at = now_s();
}

/* the extender names: the home's own (mesh.name "<AL MAC>=<name>"), else its DHCP hostname */
static void names(struct mesh_net *n)
{
	int i;

	for (i = 0; i < n->n_dev; i++) {
		struct mesh_device *d = &n->dev[i];
		const char *p = cfg.mesh_names;
		const struct device *dv;

		d->name[0] = '\0';
		while (p && *p) {
			size_t l = strcspn(p, ";");
			const char *eq = memchr(p, '=', l);

			if (eq && (size_t)(eq - p) == 17 && !strncasecmp(p, d->id, 17))
				snprintf(d->name, sizeof(d->name), "%.*s", (int)(l - 18), eq + 1);
			p += l + (p[l] == ';');
		}
		if (!d->name[0] && !d->gateway && (((dv = dev_find_mac(d->id, 0)) && dv->hostname[0]) ||
						    (d->bh_own[0] && (dv = dev_find_mac(d->bh_own, 0)) && dv->hostname[0])))
			snprintf(d->name, sizeof(d->name), "%.31s", dv->hostname);
	}
}

/* ---------- reading ---------- */

static void walk(struct mesh_net *n, struct blob_attr *head, size_t len, const char *prefix)
{
	struct blob_attr *cur;
	size_t rem = len;
	int idx = 0;

	__blob_for_each_attr(cur, head, rem) {
		const char *name = blobmsg_name(cur);
		char path[320], v[512];
		int t = blobmsg_type(cur);

		idx++;
		if (!strncmp(name, "Device.", 7) || !strncmp(name, "Network.", 8))
			snprintf(path, sizeof(path), "%s", name);
		else if (!*name)
			snprintf(path, sizeof(path), "%s%d", prefix, idx);
		else
			snprintf(path, sizeof(path), "%s%s", prefix, name);
		if (t == BLOBMSG_TYPE_TABLE || t == BLOBMSG_TYPE_ARRAY) {
			size_t l = strlen(path);

			if (l && path[l - 1] != '.' && l + 1 < sizeof(path))
				strcat(path, ".");
			walk(n, blobmsg_data(cur), blobmsg_data_len(cur), path);
			continue;
		}
		switch (t) {
		case BLOBMSG_TYPE_STRING: snprintf(v, sizeof(v), "%s", blobmsg_get_string(cur)); break;
		case BLOBMSG_TYPE_BOOL: snprintf(v, sizeof(v), "%s", blobmsg_get_bool(cur) ? "true" : "false"); break;
		case BLOBMSG_TYPE_INT64: snprintf(v, sizeof(v), "%lld", (long long)blobmsg_get_u64(cur)); break;
		case BLOBMSG_TYPE_INT32: snprintf(v, sizeof(v), "%d", (int32_t)blobmsg_get_u32(cur)); break;
		case BLOBMSG_TYPE_INT16: snprintf(v, sizeof(v), "%d", (int16_t)blobmsg_get_u16(cur)); break;
		case BLOBMSG_TYPE_DOUBLE: snprintf(v, sizeof(v), "%.0f", blobmsg_get_double(cur)); break;
		default: continue;
		}
		if (strstr(path, "DataElements.") || !strncmp(path, "Network.", 8))
			mesh_set(n, path, v);
	}
}

/* "Device.WiFi.DataElements.Network.ID=\"x\"" lines (ubus-cli, ba-cli; obuspa's "=>") */
static void lines(struct mesh_net *n, char *buf)
{
	char *line, *save = NULL;

	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *p = line, *eq, *v, *e;

		/* colours from a terminal tool */
		for (e = line; *e; e++)
			if (*e == '\033') {
				char *m = e;

				while (*m && !isalpha((unsigned char)*m))
					m++;
				memmove(e, *m ? m + 1 : m, strlen(*m ? m + 1 : m) + 1);
				e--;
			}
		while (isspace((unsigned char)*p) || *p == '>')
			p++;
		if (strncmp(p, "Device.", 7) && strncmp(p, "Network.", 8))
			continue;
		if (!(eq = strchr(p, '=')))
			continue;
		v = eq + 1 + (eq[1] == '>');
		*eq = '\0';
		for (e = eq - 1; e > p && isspace((unsigned char)*e); e--)
			*e = '\0';
		while (isspace((unsigned char)*v))
			v++;
		e = v + strlen(v);
		while (e > v && (isspace((unsigned char)e[-1]) || e[-1] == '\r'))
			*--e = '\0';
		if (*v == '"' && e > v + 1 && e[-1] == '"') {
			e[-1] = '\0';
			v++;
		}
		mesh_set(n, p, v);
	}
}

static int parse_file(const char *file, struct mesh_net *n, char *err, size_t en)
{
	FILE *f = fopen(file, "r");
	struct stat st;
	char *buf, *p;
	size_t got;

	mesh_reset(n);
	if (!f) {
		snprintf(err, en, "%s: %s", file, strerror(errno));
		return -1;
	}
	if (fstat(fileno(f), &st) < 0 || st.st_size > READ_MAX || !(buf = malloc((size_t)st.st_size + 1))) {
		fclose(f);
		snprintf(err, en, "%s: too big or unreadable", file);
		return -1;
	}
	got = fread(buf, 1, (size_t)st.st_size, f);
	fclose(f);
	buf[got] = '\0';
	for (p = buf; isspace((unsigned char)*p); p++)
		;
	if (*p == '{') {
		struct blob_buf b = { 0 };

		blob_buf_init(&b, 0);
		if (!blobmsg_add_json_from_string(&b, p)) {
			snprintf(err, en, "%s: not valid JSON", file);
			blob_buf_free(&b);
			free(buf);
			return -1;
		}
		walk(n, blob_data(b.head), blob_len(b.head), "");
		blob_buf_free(&b);
	} else {
		lines(n, p);
	}
	free(buf);
	if (!n->n_dev) {
		snprintf(err, en, "no Device.WiFi.DataElements.Network.Device in what the controller gave");
		return -1;
	}
	return 0;
}

static void on_read(void);

static void read_failed(int no_controller, const char *msg)
{
	int was_ok = !strcmp(status, "Ok");

	fails++;
	snprintf(status, sizeof(status), "%s", no_controller ? "NoController" : "Error");
	snprintf(last_err, sizeof(last_err), "%s", msg && *msg ? msg : "no answer");
	if (was_ok || fails == 1)
		AG_INFO("mesh: %s", last_err);
	if (was_ok)
		ev("controller-lost", NULL, NULL, NULL, 0, last_err);
}

struct getjob {
	char file[64];
};

static void got(void *ctx, int code, const char *msg)
{
	struct getjob *j = ctx;
	char err[200];

	busy = 0;
	if (code != 0) {
		read_failed(code == 3, msg);
	} else if (parse_file(j->file, &R, err, sizeof(err)) < 0) {
		read_failed(0, err);
	} else {
		on_read();
	}
	unlink(j->file);
	free(j);
}

int mesh_run(void)
{
	const char *h;
	char err[200];

	if (!cfg.mesh) {
		snprintf(status, sizeof(status), "Disabled");
		return -1;
	}
	params();
	if (cfg.mesh_source[0]) {
		if (parse_file(cfg.mesh_source, &R, err, sizeof(err)) < 0)
			read_failed(access(cfg.mesh_source, R_OK) != 0, err);
		else
			on_read();
		return 0;
	}
	if (busy && now_s() - busy_since < 120)
		return 0;               /* the last read is still going */
	h = handler_path(cfg.mesh_handler, "mesh-prplmesh");
	if (!h) {
		read_failed(1, cfg.mesh_handler[0] ? "mesh.handler is not an executable program" :
			    "no mesh handler (mesh-prplmesh): no EasyMesh controller on this platform");
		return 0;
	}
	{
		struct getjob *j = calloc(1, sizeof(*j));
		char *argv[4];
		int fd;

		if (!j)
			return 0;
		snprintf(j->file, sizeof(j->file), "/tmp/aigw-mesh.XXXXXX");
		if ((fd = mkstemp(j->file)) < 0) {
			free(j);
			return 0;
		}
		close(fd);
		argv[0] = (char *)h;
		argv[1] = "get";
		argv[2] = j->file;
		argv[3] = NULL;
		if (handler_start(h, argv, NULL, got, j) < 0) {
			unlink(j->file);
			free(j);
			read_failed(0, "the mesh handler could not be started");
			return 0;
		}
		busy = 1;
		busy_since = now_s();
	}
	return 0;
}

static void tick(struct uloop_timeout *t)
{
	long next = cfg.mesh_poll;
	int k;

	(void)t;
	mesh_run();
	if (!cfg.mesh)
		return;
	/* no controller here: look now and then; errors: back off, doubling */
	if (!strcmp(status, "NoController"))
		next = next > 300 ? next : 300;
	else if (fails)
		for (k = 0; k < fails && k < 5 && next < 900; k++)
			next *= 2;
	schedule(next);
}

static void schedule(long s)
{
	tm.cb = tick;
	uloop_timeout_set(&tm, (int)(s > 0 ? s : 1) * 1000);
}

/* ---------- the controller's commands ---------- */

static int handler_cmd(char *const argv[], char *err, size_t en)
{
	const char *h = handler_path(cfg.mesh_handler, "mesh-prplmesh");
	char out[600], *l, *lastl = NULL, *save = NULL, *av[10];
	int rc, i;

	if (!h) {
		snprintf(err, en, "no mesh handler to act through (mesh.handler)");
		return -1;
	}
	av[0] = (char *)h;
	for (i = 0; argv[i] && i < 8; i++)
		av[i + 1] = argv[i];
	av[i + 1] = NULL;
	rc = handler_run(h, av, out, sizeof(out), 15000);
	for (l = strtok_r(out, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
		if (*l)
			lastl = l;
	if (rc != 0) {
		snprintf(err, en, "the controller refused: %.150s", lastl ? lastl : rc < 0 ? "no answer" : "failed");
		return -1;
	}
	return 0;
}

/* ---------- checks (act.c's guard) ---------- */

static int split(const char *what, int sep, char *a, size_t an, char *b, size_t bn)
{
	const char *s = sep == ':' ? strrchr(what, ':') : strchr(what, sep);

	if (!s || (size_t)(s - what) >= an || strlen(s + 1) >= bn)
		return -1;
	snprintf(a, an, "%.*s", (int)(s - what), what);
	snprintf(b, bn, "%s", s + 1);
	lower(a);
	lower(b);
	return 0;
}

int mesh_target_ok(const char *action, const char *target)
{
	char a[64], b[64];

	if (!strcmp(action, "meshsteer"))
		return split(target, '@', a, sizeof(a), b, sizeof(b)) == 0 && is_mac(a) && is_mac(b);
	if (!strcmp(action, "meshchan"))
		return split(target, ':', a, sizeof(a), b, sizeof(b)) == 0 && is_mac(a) && atoi(b) > 0 && atoi(b) < 200;
	return 0;
}

static int find_radio(const char *ruid, int *d, int *r)
{
	int i, j;

	for (i = 0; i < N.n_dev; i++)
		for (j = 0; j < N.dev[i].n_radio; j++)
			if (!strcasecmp(N.dev[i].radio[j].id, ruid)) {
				*d = i;
				*r = j;
				return 0;
			}
	return -1;
}

static int chan_ok(int band, int ch)
{
	if (band == 2)
		return ch >= 1 && ch <= 13;
	if (band == 5)
		return chan_valid(5, ch);       /* the channels AIGW uses (no DFS) */
	return 0;
}

int mesh_guard(const char *action, const char *what, char *target, size_t tn, char *scope, size_t sn, char *device,
	       size_t dn, char *err, size_t en)
{
	char a[64], b[64], fw[64], tw[64];
	int d, r, k, s;

	if (!have) {
		snprintf(err, en, "the mesh hasn't been read yet (agctl mesh)");
		return -1;
	}
	if (!strcmp(action, "meshsteer")) {
		if (split(what, '@', a, sizeof(a), b, sizeof(b)) == 0 && !is_mac(a)) {
			const struct device *dv = act_find_device(a);   /* a name or an IP address, as people type it */

			if (dv && dv->mac[0]) {
				snprintf(a, sizeof(a), "%s", dv->mac);
				lower(a);
			}
		}
		if (split(what, '@', fw, sizeof(fw), tw, sizeof(tw)) < 0 || !is_mac(a) || !is_mac(b)) {
			snprintf(err, en, "'%.40s' is not <device MAC>@<access point BSSID>", what);
			return -1;
		}
		if ((s = mesh_find_sta(&N, a)) < 0) {
			snprintf(err, en, "%s is not on the mesh Wi-Fi now", a);
			return -1;
		}
		if (protected_mac(a)) {
			snprintf(err, en, "%s is protected (act.protect); AIGW never acts on it", sta_name(a));
			return -1;
		}
		if (mesh_find_bss(&N, b, &d, &r, &k) < 0 || !N.dev[d].radio[r].bss[k].fronthaul) {
			snprintf(err, en, "%s is not an access point of the mesh devices join", b);
			return -1;
		}
		if (N.sta[s].dev == d && N.sta[s].radio == r && N.sta[s].bss == k) {
			snprintf(err, en, "%s is already on %s", sta_name(a), b);
			return -1;
		}
		snprintf(target, tn, "%s@%s", a, b);
		snprintf(scope, sn, "%s", N.dev[N.sta[s].dev].radio[N.sta[s].radio].bss[N.sta[s].bss].bssid);
		mesh_dev_words(&N, N.sta[s].dev, fw, sizeof(fw));
		mesh_dev_words(&N, d, tw, sizeof(tw));
		snprintf(device, dn, "%.20s: %s %s -> %s %s", sta_name(a),
			 N.sta[s].dev == d ? "" : fw + 4, band_words(N.dev[N.sta[s].dev].radio[N.sta[s].radio].band),
			 N.sta[s].dev == d ? "" : tw + 4, band_words(N.dev[d].radio[r].band));
		return 0;
	}
	if (!strcmp(action, "meshchan")) {
		int ch;

		if (split(what, ':', a, sizeof(a), b, sizeof(b)) < 0 || !is_mac(a) || (ch = atoi(b)) <= 0) {
			snprintf(err, en, "'%.40s' is not <radio id>:<channel>", what);
			return -1;
		}
		if (find_radio(a, &d, &r) < 0) {
			snprintf(err, en, "no radio %s in the mesh (agctl mesh)", a);
			return -1;
		}
		if (!chan_ok(N.dev[d].radio[r].band, ch)) {
			snprintf(err, en, "channel %d is not one AIGW uses on %s (2.4 GHz 1-13; 5 GHz 36-48, 149-165)", ch,
				 band_words(N.dev[d].radio[r].band));
			return -1;
		}
		if (N.dev[d].radio[r].channel == ch) {
			snprintf(err, en, "that radio is already on channel %d", ch);
			return -1;
		}
		mesh_dev_words(&N, d, fw, sizeof(fw));
		snprintf(target, tn, "%s:%d", a, ch);
		snprintf(scope, sn, "%d", N.dev[d].radio[r].channel);
		snprintf(device, dn, "%.32s %s, channel %d -> %d", fw + 4, band_words(N.dev[d].radio[r].band),
			 N.dev[d].radio[r].channel, ch);
		return 0;
	}
	snprintf(err, en, "not a mesh action");
	return -1;
}

/* ---------- trials ---------- */

/* the stations on one radio, together: their mean rate and resends */
static void radio_sample(int d, int r, struct mesh_sample *s, int *util)
{
	int i, n = 0, nr = 0;
	uint64_t kb = 0;
	long rt = 0;

	memset(s, 0, sizeof(*s));
	s->retry_pct = -1;
	s->loss_pct = -1;
	*util = d >= 0 && r >= 0 ? N.dev[d].radio[r].util : -1;
	for (i = 0; d >= 0 && i < N.n_sta; i++) {
		struct mesh_hist *h;

		if (N.sta[i].dev != d || N.sta[i].radio != r)
			continue;
		kb += N.sta[i].down_kbps ? N.sta[i].down_kbps : N.sta[i].est_mbps * 1000;
		n++;
		if ((h = hist_of(N.sta[i].mac, 0, NULL)) && h->retry_pct >= 0) {
			rt += h->retry_pct;
			nr++;
		}
	}
	if (n)
		s->kbps = (uint32_t)(kb / (uint64_t)n);
	if (nr)
		s->retry_pct = (int)(rt / nr);
}

static void sample_words(const struct mesh_sample *s, char *buf, size_t n)
{
	size_t l = 0;

	buf[0] = '\0';
	if (s->dbm)
		l += (size_t)snprintf(buf + l, n - l, "%d dBm", s->dbm);
	if (s->kbps && l < n)
		l += (size_t)snprintf(buf + l, n - l, "%s%u Mbit/s", l ? ", " : "", s->kbps / 1000);
	if (s->retry_pct >= 0 && l < n)
		l += (size_t)snprintf(buf + l, n - l, "%s%d%% sent again", l ? ", " : "", s->retry_pct);
	if (s->loss_pct >= 0 && l < n)
		snprintf(buf + l, n - l, "%s%d%% lost", l ? ", " : "", s->loss_pct);
}

static void trial_end(void)
{
	memset(&T, 0, sizeof(T));
}

static void trial_step(long now)
{
	char state[16] = "", note[160], why[240], bw[96], aw[96], summary[400];
	struct mesh_sample after;
	int idx, d, r, util;

	if (!T.on)
		return;
	if (act_field(T.id, "state", state, sizeof(state)) < 0 || strcmp(state, "active")) {
		trial_end();                    /* undone by a person, or ended otherwise: nothing to judge */
		return;
	}
	if (T.kind == TK_STEER) {
		struct mesh_hist *h = hist_of(T.sta, 1, NULL);
		const char *who = sta_name(T.sta);

		idx = mesh_find_sta(&N, T.sta);
		if (idx < 0 || (T.landed && !mesh_steer_landed(&N, T.sta, T.to))) {
			T.no_back = 1;
			snprintf(note, sizeof(note), "not judged: %s %s during the trial", who,
				 idx < 0 ? "left the Wi-Fi" : "moved again by itself");
			act_undo_because(T.id, note);
			ev("trial-ended", T.sta, T.from, T.to, T.id, note);
			last(T.id, "Steer", "Ended", note);
			dm_on_mesh(T.id, "Steer", "Ended", T.sta, T.from, T.to, note, "", "");
			trial_end();
			return;
		}
		if (!T.landed) {
			if (mesh_steer_landed(&N, T.sta, T.to)) {
				T.landed = 1;
				T.judge_at = now + cfg.mesh_trial;
				snprintf(summary, sizeof(summary), "%s moved to %s; measured for %d s", who, T.to, cfg.mesh_trial);
				ev("moved", T.sta, T.from, T.to, T.id, summary);
				AG_INFO("mesh: %s", summary);
			} else if (now - T.started >= (cfg.mesh_poll * 3 > 10 ? cfg.mesh_poll * 3 : 10)) {
				mesh_steer_failed(h, &P, now, 0, T.from, T.to);
				C.misses++;
				T.no_back = 1;
				snprintf(note, sizeof(note), "missed: %s didn't move (the controller's request didn't land)%s", who,
					 h->pause_until > now ? "; left alone for a while" : "");
				act_undo_because(T.id, note);
				ev("missed", T.sta, T.from, T.to, T.id, note);
				last(T.id, "Steer", "Missed", note);
				dm_on_mesh(T.id, "Steer", "Missed", T.sta, T.from, T.to, note, "", "");
				AG_INFO("mesh: decision %u: %s", T.id, note);
				trial_end();
			}
			return;
		}
		if (now < T.judge_at)
			return;
		mesh_sample_of(&N, idx, h, loss_of(T.sta), &after);
		sample_words(&T.before, bw, sizeof(bw));
		sample_words(&after, aw, sizeof(aw));
		if (mesh_trial_judge(&T.before, &after, &P, why, sizeof(why)) == MESH_KEEP) {
			h->kept++;
			C.kept++;
			snprintf(note, sizeof(note), "kept: %.150s", why);
			act_keep(T.id, note);
			snprintf(summary, sizeof(summary), "%s stays on %s: %s", who, T.to, why);
			ev("kept", T.sta, T.from, T.to, T.id, summary);
			last(T.id, "Steer", "Kept", summary);
			dm_on_mesh(T.id, "Steer", "Kept", T.sta, T.from, T.to, why, bw, aw);
		} else {
			mesh_steer_failed(h, &P, now, 1, T.from, T.to);
			C.undone++;
			snprintf(note, sizeof(note), "moved back: %.140s", why);
			act_undo_because(T.id, note);      /* steers it back (mesh_apply undo) */
			snprintf(summary, sizeof(summary), "%s moved back to %s: %s; that move avoided for a day", who, T.from, why);
			ev("undone", T.sta, T.from, T.to, T.id, summary);
			last(T.id, "Steer", "Undone", summary);
			dm_on_mesh(T.id, "Steer", "Undone", T.sta, T.from, T.to, why, bw, aw);
		}
		AG_INFO("mesh: decision %u: %s", T.id, summary);
		trial_end();
		return;
	}
	/* a channel change */
	if (find_radio(T.ruid, &d, &r) < 0) {
		T.no_back = 1;
		act_undo_because(T.id, "not judged: the radio left the mesh during the trial");
		trial_end();
		return;
	}
	if (!T.landed) {
		if (N.dev[d].radio[r].channel == T.to_ch) {
			T.landed = 1;
			T.judge_at = now + cfg.mesh_trial;
			ev("channel-moved", NULL, NULL, NULL, T.id, "the radio is on its new channel; measured now");
		} else if (now - T.started >= (cfg.mesh_poll * 3 > 20 ? cfg.mesh_poll * 3 : 20)) {
			C.chan_misses++;
			T.no_back = 1;
			snprintf(note, sizeof(note), "missed: the controller didn't change the channel (still %d)",
				 N.dev[d].radio[r].channel);
			act_undo_because(T.id, note);
			last(T.id, "Channel", "Missed", note);
			dm_on_mesh(T.id, "Channel", "Missed", T.ruid, "", "", note, "", "");
			trial_end();
		}
		return;
	}
	if (now < T.judge_at)
		return;
	radio_sample(d, r, &after, &util);
	sample_words(&T.before, bw, sizeof(bw));
	sample_words(&after, aw, sizeof(aw));
	{
		char fr[8], to[8];
		int keep;

		snprintf(fr, sizeof(fr), "%d", T.from_ch);
		snprintf(to, sizeof(to), "%d", T.to_ch);
		if (!T.before.kbps && T.before.retry_pct < 0 && !after.kbps && after.retry_pct < 0) {
			keep = T.util_before < 0 || util < 0 || util <= T.util_before + 10;
			snprintf(why, sizeof(why), "%s", keep ? "no device on it to measure; the plan itself removes the clash" :
				 "the air got busier on the new channel");
		} else
			keep = mesh_trial_judge(&T.before, &after, &P, why, sizeof(why)) == MESH_KEEP;
		if (keep) {
			C.chans_kept++;
			snprintf(note, sizeof(note), "kept: %.150s", why);
			act_keep(T.id, note);
		} else {
			int k, o = 0;

			C.chans_undone++;
			for (k = 0; k < TRIED_MAX; k++)
				if (tried[k].until < tried[o].until)
					o = k;
			snprintf(tried[o].ruid, sizeof(tried[o].ruid), "%s", T.ruid);
			tried[o].ch = T.to_ch;
			tried[o].until = now + 86400;
			snprintf(note, sizeof(note), "switched back: %.140s", why);
			act_undo_because(T.id, note);
		}
		snprintf(summary, sizeof(summary), "channel %d -> %d %s: %s", T.from_ch, T.to_ch, keep ? "kept" : "switched back",
			 why);
		ev(keep ? "channel-kept" : "channel-undone", NULL, fr, to, T.id, summary);
		last(T.id, "Channel", keep ? "Kept" : "Undone", summary);
		dm_on_mesh(T.id, "Channel", keep ? "Kept" : "Undone", T.ruid, fr, to, why, bw, aw);
		AG_INFO("mesh: decision %u: %s", T.id, summary);
	}
	trial_end();
}

/* ---------- carrying it out (act.c) ---------- */

int mesh_apply(uint32_t id, const char *action, const char *target, const char *scope, int undo, char *err, size_t en)
{
	char a[64], b[64], c1[16], c2[16], c3[16], c4[16], summary[400];
	char *argv[8];
	int d, r, s;

	err[0] = '\0';
	if (!strcmp(action, "meshsteer")) {
		if (split(target, '@', a, sizeof(a), b, sizeof(b)) < 0) {
			snprintf(err, en, "malformed target");
			return -1;
		}
		if (undo) {
			int back = !(T.on && T.id == id && T.no_back) && is_mac(scope) && mesh_steer_landed(&N, a, b);

			if (T.on && T.id == id)
				trial_end();
			if (!back)
				return 0;
			argv[0] = "steer";
			argv[1] = a;
			argv[2] = (char *)scope;
			argv[3] = "btm";
			argv[4] = NULL;
			if (handler_cmd(argv, err, en) < 0) {
				AG_WARN("mesh: moving %s back: %s", a, err);
				return -1;
			}
			return 0;
		}
		if (T.on && T.id != id) {
			snprintf(err, en, "another mesh change (decision %u) is on trial; one at a time", T.id);
			return -1;
		}
		if ((s = mesh_find_sta(&N, a)) < 0) {
			snprintf(err, en, "%s is not on the mesh Wi-Fi now", a);
			return -1;
		}
		if (T.on && T.id == id)
			return 0;               /* asked again while on trial: it is in hand */
		argv[0] = "steer";
		argv[1] = a;
		argv[2] = b;
		argv[3] = N.sta[s].btm == MESH_CAP_NO || (N.sta[s].btm == MESH_CAP_UNKNOWN && N.sta[s].btm_tries <= N.sta[s].btm_fails) ?
			  "mandate" : "btm";
		argv[4] = NULL;
		if (handler_cmd(argv, err, en) < 0)
			return -1;
		{
			struct mesh_hist *h = hist_of(a, 1, NULL);

			memset(&T, 0, sizeof(T));
			T.on = 1;
			T.id = id;
			T.kind = TK_STEER;
			snprintf(T.sta, sizeof(T.sta), "%.17s", a);
			snprintf(T.to, sizeof(T.to), "%.17s", b);
			snprintf(T.from, sizeof(T.from), "%.17s", scope);
			T.started = now_s();
			mesh_sample_of(&N, s, h, loss_of(a), &T.before);
			h->last_steer = T.started;
			h->steers++;
			C.steers++;
		}
		snprintf(summary, sizeof(summary), "asked the controller to move %s to %s (%s)", sta_name(a), b, argv[3]);
		ev("steered", a, scope, b, id, summary);
		last(id, "Steer", "Applied", summary);
		dm_on_mesh(id, "Steer", "Applied", a, scope, b, summary, "", "");
		AG_INFO("mesh: decision %u: %s", id, summary);
		return 0;
	}
	if (!strcmp(action, "meshchan")) {
		const struct mesh_radio *rad;
		int ch;

		if (split(target, ':', a, sizeof(a), b, sizeof(b)) < 0 || find_radio(a, &d, &r) < 0) {
			snprintf(err, en, "no radio %.40s in the mesh", target);
			return -1;
		}
		rad = &N.dev[d].radio[r];
		ch = undo ? atoi(scope) : atoi(b);
		if (undo) {
			int back = !(T.on && T.id == id && T.no_back) && ch > 0 && rad->channel != ch;

			if (T.on && T.id == id)
				trial_end();
			if (!back)
				return 0;
		} else {
			if (T.on && T.id != id) {
				snprintf(err, en, "another mesh change (decision %u) is on trial; one at a time", T.id);
				return -1;
			}
			if (T.on && T.id == id)
				return 0;
		}
		snprintf(c1, sizeof(c1), "%d", ch);
		snprintf(c2, sizeof(c2), "%d", mesh_opclass(rad->band, ch, rad->width));
		snprintf(c3, sizeof(c3), "%d", mesh_center(rad->band, ch, rad->width));
		snprintf(c4, sizeof(c4), "%d", rad->width);
		argv[0] = "channel";
		argv[1] = a;
		argv[2] = c1;
		argv[3] = c2;
		argv[4] = c3;
		argv[5] = c4;
		argv[6] = NULL;
		if (handler_cmd(argv, err, en) < 0)
			return -1;
		if (undo)
			return 0;
		memset(&T, 0, sizeof(T));
		T.on = 1;
		T.id = id;
		T.kind = TK_CHAN;
		snprintf(T.ruid, sizeof(T.ruid), "%.17s", a);
		T.from_ch = rad->channel;
		T.to_ch = ch;
		T.started = now_s();
		radio_sample(d, r, &T.before, &T.util_before);
		C.chans++;
		snprintf(summary, sizeof(summary), "asked the controller to move radio %s from channel %d to %d", a, rad->channel, ch);
		ev("channel", NULL, NULL, NULL, id, summary);
		last(id, "Channel", "Applied", summary);
		dm_on_mesh(id, "Channel", "Applied", a, c1, c1, summary, "", "");
		AG_INFO("mesh: decision %u: %s", id, summary);
		return 0;
	}
	snprintf(err, en, "not a mesh action");
	return -1;
}

/* ---------- deciding ---------- */

/* a mesh decision waiting for a yes or in effect */
static uint32_t live_decision(void)
{
	uint32_t ids[256];
	char a[16], st[16];
	int i, n = act_ids(ids, 256);

	for (i = 0; i < n; i++)
		if (act_field(ids[i], "action", a, sizeof(a)) == 0 && (!strcmp(a, "meshsteer") || !strcmp(a, "meshchan")) &&
		    act_field(ids[i], "state", st, sizeof(st)) == 0 && (!strcmp(st, "proposed") || !strcmp(st, "active")))
			return ids[i];
	return 0;
}

static int was_tried(const char *ruid, int ch, long now)
{
	int k;

	for (k = 0; k < TRIED_MAX; k++)
		if (tried[k].until > now && tried[k].ch == ch && !strcasecmp(tried[k].ruid, ruid))
			return 1;
	return 0;
}

static void decide(long now)
{
	char target[64], reason[400], err[200], summary[400];
	uint32_t id = 0;
	int i, applied = 0, made = 0;

	nplan = mesh_chan_plan(&N, plan, 4);
	if (cfg.act_mode == AG_MODE_OFF || T.on || live_decision())
		return;
	if (cfg.mesh_steer && !area_hold(AH_STEER)) {
		for (i = 0; i < N.n_sta && !made; i++) {
			const struct mesh_sta *s = &N.sta[i];
			struct mesh_extra x;
			struct mesh_steer m;
			int hi;
			struct mesh_hist *h = hist_of(s->mac, 1, &hi);

			x.loss_pct = loss_of(s->mac);
			x.in_call = calls_live_on(s->mac);
			x.protected_dev = protected_mac(s->mac);
			if (!mesh_steer_pick(&N, i, h, &x, &P, now, &m, why_not[hi], sizeof(why_not[hi])))
				continue;
			snprintf(why_not[hi], sizeof(why_not[hi]), "moving it: %s", m.to);
			snprintf(target, sizeof(target), "%s@%s", s->mac, m.to);
			snprintf(reason, sizeof(reason), "%s is %s", sta_name(s->mac), m.why);
			made = 1;               /* one attempt a read, whatever comes of it */
			if (act_auto("meshsteer", target, cfg.mesh_trial * 3 + 600, "mesh", reason, &id, &applied, err,
				     sizeof(err)) < 0) {
				AG_INFO("mesh: not moving %s: %s", sta_name(s->mac), err);
				snprintf(why_not[hi], sizeof(why_not[hi]), "%.110s", err);
				continue;
			}
			if (!applied) {
				C.proposed++;
				snprintf(summary, sizeof(summary), "proposed (decision %u): %.360s", id, reason);
				ev("proposed", s->mac, m.from, m.to, id, summary);
				last(id, "Steer", "Proposed", reason);
				dm_on_mesh(id, "Steer", "Proposed", s->mac, m.from, m.to, reason, "", "");
				AG_INFO("mesh: %s", summary);
			}
		}
	}
	if (made || !cfg.mesh_channel || area_hold(AH_CHANNEL))
		return;
	for (i = 0; i < nplan; i++) {
		const struct mesh_radio *rad = &N.dev[plan[i].dev].radio[plan[i].radio];

		if (was_tried(rad->id, plan[i].to, now))
			continue;
		snprintf(target, sizeof(target), "%s:%d", rad->id, plan[i].to);
		if (act_auto("meshchan", target, cfg.mesh_trial * 3 + 600, "mesh", plan[i].why, &id, &applied, err,
			     sizeof(err)) < 0) {
			AG_INFO("mesh: not changing channel %s: %s", target, err);
			break;
		}
		if (!applied) {
			char fr[8], to[8];

			C.proposed++;
			snprintf(fr, sizeof(fr), "%d", plan[i].from);
			snprintf(to, sizeof(to), "%d", plan[i].to);
			last(id, "Channel", "Proposed", plan[i].why);
			ev("proposed", NULL, fr, to, id, plan[i].why);
			dm_on_mesh(id, "Channel", "Proposed", rad->id, fr, to, plan[i].why, "", "");
		}
		break;
	}
}

/* ---------- each read ---------- */

static void backhaul_step(long now)
{
	struct mesh_bh_finding f[MESH_DEVS];
	int n = mesh_backhaul(&N, &P, f, MESH_DEVS), i, k;

	/* cleared: an extender with a finding before and none now (or gone) */
	for (k = 0; k < MESH_DEVS; k++) {
		int still = 0;

		if (!bh_seen[k].id[0])
			continue;
		for (i = 0; i < n; i++)
			if (!strcasecmp(N.dev[f[i].dev].id, bh_seen[k].id))
				still = 1;
		if (!still) {
			char w[96], s[160];
			int d = mesh_find_dev(&N, bh_seen[k].id);

			if (d >= 0)
				mesh_dev_words(&N, d, w, sizeof(w));
			else
				snprintf(w, sizeof(w), "the extender %.17s", bh_seen[k].id);
			snprintf(s, sizeof(s), "%s: its backhaul is fine now%s", w, d < 0 ? " (it left the mesh)" : "");
			ev("backhaul-ok", NULL, NULL, NULL, 0, s);
			AG_INFO("mesh: %s", s);
			memset(&bh_seen[k], 0, sizeof(bh_seen[k]));
		}
	}
	for (i = 0; i < n; i++) {
		const char *id = N.dev[f[i].dev].id;
		int slot = -1, free_slot = -1;

		for (k = 0; k < MESH_DEVS; k++) {
			if (bh_seen[k].id[0] && !strcasecmp(bh_seen[k].id, id))
				slot = k;
			if (!bh_seen[k].id[0] && free_slot < 0)
				free_slot = k;
		}
		if (slot >= 0 && bh_seen[slot].kind == f[i].kind)
			continue;
		if (slot < 0 && (slot = free_slot) < 0)
			continue;
		snprintf(bh_seen[slot].id, sizeof(bh_seen[slot].id), "%s", id);
		if (!bh_seen[slot].since || bh_seen[slot].kind != f[i].kind)
			bh_seen[slot].since = now;
		bh_seen[slot].kind = f[i].kind;
		{
			char s[500];

			snprintf(s, sizeof(s), "%s %s", f[i].words, f[i].advice);
			ev("backhaul", NULL, NULL, NULL, 0, s);
			AG_INFO("mesh: %s", f[i].words);
		}
	}
	memcpy(BH, f, sizeof(f[0]) * (size_t)n);
	nbh = n;
}

/* a mesh decision live when agd stopped: its trial is gone with it, so it ends (taken back) */
static void stale_decisions(void)
{
	uint32_t ids[256];
	char a[16], st[16];
	int i, n = act_ids(ids, 256);

	for (i = 0; i < n; i++)
		if (act_field(ids[i], "action", a, sizeof(a)) == 0 && (!strcmp(a, "meshsteer") || !strcmp(a, "meshchan")) &&
		    act_field(ids[i], "state", st, sizeof(st)) == 0 && !strcmp(st, "active") && !(T.on && T.id == ids[i]))
			act_undo_because(ids[i], "taken back: agd restarted before its trial was judged");
}

static void on_read(void)
{
	long now = now_s();
	int i, first = !have;

	mesh_finish(&R);
	names(&R);
	/* moves made by the device or the controller (not ours on trial) */
	for (i = 0; have && i < R.n_sta; i++) {
		int o = mesh_find_sta(&N, R.sta[i].mac);
		char fw[64], tw[64], s[240];

		if (o < 0 || N.sta[o].dev == R.sta[i].dev || (T.on && T.kind == TK_STEER && !strcasecmp(T.sta, R.sta[i].mac)))
			continue;
		mesh_dev_words(&N, N.sta[o].dev, fw, sizeof(fw));
		mesh_dev_words(&R, R.sta[i].dev, tw, sizeof(tw));
		C.roams++;
		snprintf(s, sizeof(s), "%s moved from %s to %s %s", sta_name(R.sta[i].mac), fw, tw,
			 band_words(R.dev[R.sta[i].dev].radio[R.sta[i].radio].band));
		ev("roamed", R.sta[i].mac, fw, tw, 0, s);
	}
	memcpy(&N, &R, sizeof(N));
	have = 1;
	read_at = now;
	if (strcmp(status, "Ok")) {
		AG_INFO("mesh: reading the EasyMesh controller%s%s: %d access points, %d devices", N.controller[0] ? " " : "",
			N.controller, N.n_dev, N.n_sta);
		ev("controller", NULL, NULL, NULL, 0, "the EasyMesh controller answers");
	}
	snprintf(status, sizeof(status), "Ok");
	last_err[0] = '\0';
	fails = 0;
	for (i = 0; i < N.n_sta; i++)
		mesh_hist_update(hist_of(N.sta[i].mac, 1, NULL), &N, i, now);
	if (first)
		stale_decisions();
	trial_step(now);
	backhaul_step(now);
	decide(now);
}

/* ---------- for others ---------- */

int mesh_issues(struct mesh_issue *out, int max)
{
	int i, k, n = 0;

	for (i = 0; have && i < nbh && n < max; i++) {
		struct mesh_issue *o = &out[n++];

		memset(o, 0, sizeof(*o));
		snprintf(o->code, sizeof(o->code), "MESH_BACKHAUL");
		snprintf(o->target, sizeof(o->target), "%s", N.dev[BH[i].dev].id);
		snprintf(o->kind, sizeof(o->kind), "%s", mesh_bh_kind_name(BH[i].kind));
		snprintf(o->text, sizeof(o->text), "%.239s %.159s", BH[i].words, BH[i].advice);
		o->stations = BH[i].stations;
		o->hurts = BH[i].hurts;
		for (k = 0; k < MESH_DEVS; k++)
			if (!strcasecmp(bh_seen[k].id, o->target))
				o->since = bh_seen[k].since;
	}
	return n;
}

static int count_wireless(int weak_only, int hurt_only)
{
	int i, k = 0;

	if (!weak_only && !hurt_only) {
		for (i = 0; i < N.n_dev; i++)
			k += !N.dev[i].gateway && N.dev[i].bh == MESH_BH_WIFI;
		return k;
	}
	for (i = 0; i < nbh; i++)
		k += hurt_only ? BH[i].hurts : (BH[i].kind == MBH_WEAK || BH[i].kind == MBH_SLOW);
	return k;
}

static void summary_words(char *buf, size_t n)
{
	int ext = have ? mesh_extenders(&N) : 0, wl = count_wireless(0, 0), weak = count_wireless(1, 0);

	if (!have || strcmp(status, "Ok")) {
		snprintf(buf, n, "%s", !strcmp(status, "Disabled") ? "Mesh Wi-Fi is off (mesh.enabled)." :
			 !strcmp(status, "NoController") ? "No EasyMesh controller here." :
			 last_err[0] ? last_err : "Not read yet.");
		return;
	}
	snprintf(buf, n, "%d access point%s (%d extender%s%s%s), %d device%s on the mesh; AIGW moved %u, kept %u, moved back %u; "
		 "%u channel change%s.", N.n_dev, N.n_dev == 1 ? "" : "s", ext, ext == 1 ? "" : "s",
		 wl ? ", wireless backhaul" : "", weak ? (weak == 1 ? ", one weak" : ", some weak") : "", N.n_sta,
		 N.n_sta == 1 ? "" : "s", C.steers, C.kept, C.undone, C.chans, C.chans == 1 ? "" : "s");
}

int mesh_field(const char *f, char *buf, size_t n)
{
	if (!strcmp(f, "Enable")) snprintf(buf, n, "%s", cfg.mesh ? "true" : "false");
	else if (!strcmp(f, "Status")) snprintf(buf, n, "%s", status);
	else if (!strcmp(f, "ControllerID")) snprintf(buf, n, "%s", have ? N.controller : "");
	else if (!strcmp(f, "Agents")) snprintf(buf, n, "%d", have ? N.n_dev : 0);
	else if (!strcmp(f, "Extenders")) snprintf(buf, n, "%d", have ? mesh_extenders(&N) : 0);
	else if (!strcmp(f, "Stations")) snprintf(buf, n, "%d", have ? N.n_sta : 0);
	else if (!strcmp(f, "WirelessBackhauls")) snprintf(buf, n, "%d", have ? count_wireless(0, 0) : 0);
	else if (!strcmp(f, "WeakBackhauls")) snprintf(buf, n, "%d", have ? count_wireless(1, 0) : 0);
	else if (!strcmp(f, "HurtingExtenders")) snprintf(buf, n, "%d", have ? count_wireless(0, 1) : 0);
	else if (!strcmp(f, "Steers")) snprintf(buf, n, "%u", C.steers);
	else if (!strcmp(f, "SteersKept")) snprintf(buf, n, "%u", C.kept);
	else if (!strcmp(f, "SteersUndone")) snprintf(buf, n, "%u", C.undone);
	else if (!strcmp(f, "SteerMisses")) snprintf(buf, n, "%u", C.misses);
	else if (!strcmp(f, "ChannelChanges")) snprintf(buf, n, "%u", C.chans);
	else if (!strcmp(f, "ChannelsKept")) snprintf(buf, n, "%u", C.chans_kept);
	else if (!strcmp(f, "ChannelsUndone")) snprintf(buf, n, "%u", C.chans_undone);
	else if (!strcmp(f, "Proposed")) snprintf(buf, n, "%u", C.proposed);
	else if (!strcmp(f, "LastDecision")) snprintf(buf, n, "%u", C.last_id);
	else if (!strcmp(f, "LastKind")) snprintf(buf, n, "%s", C.last_kind);
	else if (!strcmp(f, "LastState")) snprintf(buf, n, "%s", C.last_state);
	else if (!strcmp(f, "LastSummary")) {
		snprintf(buf, n, "%s", C.last_summary);
		if (!cfg.brief_names)
			brief_scrub_isp(buf, n);
	} else if (!strcmp(f, "LastTime")) snprintf(buf, n, "%ld", C.last_at);
	else if (!strcmp(f, "ReadTime")) snprintf(buf, n, "%ld", read_at);
	else if (!strcmp(f, "Summary")) summary_words(buf, n);
	else return -1;
	return 0;
}

static void sta_blob(struct blob_buf *b, int i, int names_ok)
{
	const struct mesh_sta *s = &N.sta[i];
	const struct device *dv = dev_find_mac(s->mac, 0);
	struct mesh_hist *h;
	char w[64], k[40];
	int hi;
	void *t = blobmsg_open_table(b, NULL);

	if (names_ok) {
		blobmsg_add_string(b, "mac", s->mac);
		blobmsg_add_string(b, "name", dv && dv->hostname[0] ? dv->hostname : "");
	} else {
		brief_kind(dv ? dv->type : "", dv ? dv->hostname : "", k, sizeof(k));
		blobmsg_add_string(b, "kind", k);
	}
	mesh_dev_words(&N, s->dev, w, sizeof(w));
	blobmsg_add_string(b, "on", w);
	blobmsg_add_string(b, "agent", N.dev[s->dev].id);
	blobmsg_add_string(b, "bssid", N.dev[s->dev].radio[s->radio].bss[s->bss].bssid);
	blobmsg_add_u32(b, "band", (uint32_t)N.dev[s->dev].radio[s->radio].band);
	if (s->dbm)
		blobmsg_add_u32(b, "signal", (uint32_t)s->dbm);
	if (s->down_kbps)
		blobmsg_add_u32(b, "rate_kbps", s->down_kbps);
	blobmsg_add_string(b, "btm", s->btm == MESH_CAP_YES ? "yes" : s->btm == MESH_CAP_NO ? "no" : "unknown");
	blobmsg_add_string(b, "rrm", s->rrm == MESH_CAP_YES ? "yes" : s->rrm == MESH_CAP_NO ? "no" : "unknown");
	if ((h = hist_of(s->mac, 0, &hi))) {
		if (h->retry_pct >= 0)
			blobmsg_add_u32(b, "retry_pct", (uint32_t)h->retry_pct);
		blobmsg_add_u32(b, "steers", h->steers);
		blobmsg_add_u32(b, "kept", h->kept);
		blobmsg_add_u32(b, "undone", h->undone);
		blobmsg_add_u32(b, "misses", h->misses);
		if (h->pause_until > now_s())
			blobmsg_add_u64(b, "paused_until", (uint64_t)h->pause_until);
		if (names_ok && why_not[hi][0])
			blobmsg_add_string(b, "now", why_not[hi]);
	}
	blobmsg_close_table(b, t);
}

static void counts_blob(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "counts");

	blobmsg_add_u32(b, "steers", C.steers);
	blobmsg_add_u32(b, "kept", C.kept);
	blobmsg_add_u32(b, "undone", C.undone);
	blobmsg_add_u32(b, "misses", C.misses);
	blobmsg_add_u32(b, "proposed", C.proposed);
	blobmsg_add_u32(b, "channel_changes", C.chans);
	blobmsg_add_u32(b, "channels_kept", C.chans_kept);
	blobmsg_add_u32(b, "channels_undone", C.chans_undone);
	blobmsg_add_u32(b, "channel_misses", C.chan_misses);
	blobmsg_add_u32(b, "roams", C.roams);
	blobmsg_add_u64(b, "since", (uint64_t)C.since);
	blobmsg_close_table(b, t);
}

void mesh_blob(struct blob_buf *b, int names_ok)
{
	char w[400];
	void *t, *a, *x;
	int i, j, k;

	t = blobmsg_open_table(b, "mesh");
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.mesh);
	blobmsg_add_string(b, "status", status);
	if (last_err[0])
		blobmsg_add_string(b, "error", last_err);
	blobmsg_add_string(b, "source", cfg.mesh_source[0] ? "snapshot" : "handler");
	blobmsg_add_string(b, "mode", ag_mode_str(cfg.act_mode));
	blobmsg_add_u8(b, "steer", (uint8_t)cfg.mesh_steer);
	blobmsg_add_u8(b, "channel", (uint8_t)cfg.mesh_channel);
	summary_words(w, sizeof(w));
	blobmsg_add_string(b, "summary", w);
	if (have) {
		blobmsg_add_string(b, "network", N.id);
		blobmsg_add_string(b, "controller", N.controller);
		blobmsg_add_u64(b, "read", (uint64_t)read_at);
		a = blobmsg_open_array(b, "agents");
		for (i = 0; i < N.n_dev; i++) {
			const struct mesh_device *d = &N.dev[i];

			x = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "id", d->id);
			mesh_dev_words(&N, i, w, sizeof(w));
			blobmsg_add_string(b, "words", w);
			blobmsg_add_string(b, "role", d->gateway ? "gateway" : "extender");
			if (names_ok && d->name[0])
				blobmsg_add_string(b, "name", d->name);
			if (d->maker[0] || d->model[0])
				blobmsg_add_string(b, "model", (snprintf(w, sizeof(w), "%s %s", d->maker, d->model), w));
			blobmsg_add_u32(b, "stations", (uint32_t)mesh_stas_on(&N, i));
			if (!d->gateway) {
				void *bh = blobmsg_open_table(b, "backhaul");

				blobmsg_add_string(b, "type", mesh_bh_name(d->bh));
				blobmsg_add_string(b, "parent", d->bh_parent);
				if (d->bh_band)
					blobmsg_add_u32(b, "band", (uint32_t)d->bh_band);
				if (d->bh_dbm)
					blobmsg_add_u32(b, "signal", (uint32_t)d->bh_dbm);
				if (d->bh_down_kbps)
					blobmsg_add_u32(b, "rate_kbps", d->bh_down_kbps);
				if (d->bh_util >= 0)
					blobmsg_add_u32(b, "utilization", (uint32_t)d->bh_util);
				blobmsg_close_table(b, bh);
			}
			{
				void *ra = blobmsg_open_array(b, "radios");

				for (j = 0; j < d->n_radio; j++) {
					const struct mesh_radio *r = &d->radio[j];
					void *rt = blobmsg_open_table(b, NULL), *ba;

					blobmsg_add_string(b, "id", r->id);
					blobmsg_add_u32(b, "band", (uint32_t)r->band);
					blobmsg_add_u32(b, "channel", (uint32_t)r->channel);
					blobmsg_add_u32(b, "width", (uint32_t)r->width);
					blobmsg_add_u32(b, "opclass", (uint32_t)r->opclass);
					if (r->util >= 0)
						blobmsg_add_u32(b, "utilization", (uint32_t)r->util);
					ba = blobmsg_open_array(b, "bss");
					for (k = 0; k < r->n_bss; k++) {
						void *bt = blobmsg_open_table(b, NULL);

						blobmsg_add_string(b, "bssid", r->bss[k].bssid);
						if (names_ok)
							blobmsg_add_string(b, "ssid", r->bss[k].ssid);
						blobmsg_add_u8(b, "backhaul", (uint8_t)r->bss[k].backhaul);
						blobmsg_add_u8(b, "fronthaul", (uint8_t)r->bss[k].fronthaul);
						blobmsg_close_table(b, bt);
					}
					blobmsg_close_array(b, ba);
					blobmsg_close_table(b, rt);
				}
				blobmsg_close_array(b, ra);
			}
			blobmsg_close_table(b, x);
		}
		blobmsg_close_array(b, a);
		a = blobmsg_open_array(b, "stations");
		for (i = 0; i < N.n_sta; i++)
			sta_blob(b, i, names_ok);
		blobmsg_close_array(b, a);
		a = blobmsg_open_array(b, "backhaul");
		for (i = 0; i < nbh; i++) {
			x = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "agent", N.dev[BH[i].dev].id);
			blobmsg_add_string(b, "kind", mesh_bh_kind_name(BH[i].kind));
			blobmsg_add_u32(b, "stations", (uint32_t)BH[i].stations);
			blobmsg_add_u8(b, "hurts", (uint8_t)BH[i].hurts);
			blobmsg_add_string(b, "words", BH[i].words);
			blobmsg_add_string(b, "advice", BH[i].advice);
			for (k = 0; k < MESH_DEVS; k++)
				if (!strcasecmp(bh_seen[k].id, N.dev[BH[i].dev].id))
					blobmsg_add_u64(b, "since", (uint64_t)bh_seen[k].since);
			blobmsg_close_table(b, x);
		}
		blobmsg_close_array(b, a);
		a = blobmsg_open_array(b, "plan");
		for (i = 0; i < nplan; i++) {
			x = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "radio", N.dev[plan[i].dev].radio[plan[i].radio].id);
			blobmsg_add_u32(b, "from", (uint32_t)plan[i].from);
			blobmsg_add_u32(b, "to", (uint32_t)plan[i].to);
			blobmsg_add_u32(b, "opclass", (uint32_t)plan[i].opclass);
			blobmsg_add_u8(b, "tried", (uint8_t)was_tried(N.dev[plan[i].dev].radio[plan[i].radio].id, plan[i].to, now_s()));
			blobmsg_add_string(b, "why", plan[i].why);
			blobmsg_close_table(b, x);
		}
		blobmsg_close_array(b, a);
	}
	if (T.on) {
		x = blobmsg_open_table(b, "trial");
		blobmsg_add_u32(b, "decision", T.id);
		blobmsg_add_string(b, "kind", T.kind == TK_STEER ? "steer" : "channel");
		if (T.kind == TK_STEER) {
			if (names_ok)
				blobmsg_add_string(b, "station", T.sta);
			blobmsg_add_string(b, "from", T.from);
			blobmsg_add_string(b, "to", T.to);
		} else {
			blobmsg_add_string(b, "radio", T.ruid);
			blobmsg_add_u32(b, "from", (uint32_t)T.from_ch);
			blobmsg_add_u32(b, "to", (uint32_t)T.to_ch);
		}
		blobmsg_add_u8(b, "landed", (uint8_t)T.landed);
		blobmsg_add_u64(b, "started", (uint64_t)T.started);
		if (T.judge_at)
			blobmsg_add_u64(b, "judged_at", (uint64_t)T.judge_at);
		sample_words(&T.before, w, sizeof(w));
		blobmsg_add_string(b, "before", w);
		blobmsg_close_table(b, x);
	}
	if (C.last_id) {
		x = blobmsg_open_table(b, "last");
		blobmsg_add_u32(b, "decision", C.last_id);
		blobmsg_add_string(b, "kind", C.last_kind);
		blobmsg_add_string(b, "state", C.last_state);
		snprintf(w, sizeof(w), "%s", C.last_summary);
		if (!names_ok)
			brief_scrub_isp(w, sizeof(w));
		blobmsg_add_string(b, "summary", w);
		blobmsg_add_u64(b, "at", (uint64_t)C.last_at);
		blobmsg_close_table(b, x);
	}
	counts_blob(b);
	blobmsg_close_table(b, t);
}

/* for cortex: counts and findings, no names, no addresses */
void mesh_report_blob(struct blob_buf *b)
{
	void *t, *a;
	int i;

	if (!cfg.mesh || !have || strcmp(status, "Ok"))
		return;
	t = blobmsg_open_table(b, "mesh");
	blobmsg_add_u32(b, "agents", (uint32_t)N.n_dev);
	blobmsg_add_u32(b, "extenders", (uint32_t)mesh_extenders(&N));
	blobmsg_add_u32(b, "stations", (uint32_t)N.n_sta);
	blobmsg_add_u32(b, "wireless_backhauls", (uint32_t)count_wireless(0, 0));
	blobmsg_add_u32(b, "weak_backhauls", (uint32_t)count_wireless(1, 0));
	blobmsg_add_u32(b, "hurting", (uint32_t)count_wireless(0, 1));
	a = blobmsg_open_array(b, "findings");
	for (i = 0; i < nbh; i++) {
		void *x = blobmsg_open_table(b, NULL);

		blobmsg_add_string(b, "kind", mesh_bh_kind_name(BH[i].kind));
		blobmsg_add_u32(b, "stations", (uint32_t)BH[i].stations);
		blobmsg_add_u8(b, "hurts", (uint8_t)BH[i].hurts);
		if (N.dev[BH[i].dev].bh_dbm)
			blobmsg_add_u32(b, "signal", (uint32_t)N.dev[BH[i].dev].bh_dbm);
		if (N.dev[BH[i].dev].bh_down_kbps)
			blobmsg_add_u32(b, "rate_kbps", N.dev[BH[i].dev].bh_down_kbps);
		blobmsg_close_table(b, x);
	}
	blobmsg_close_array(b, a);
	counts_blob(b);
	blobmsg_close_table(b, t);
}

/* ---------- state ---------- */

void mesh_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "mesh"), *a, *x;
	long now = now_s();
	int i;

	blobmsg_add_u32(b, "steers", C.steers);
	blobmsg_add_u32(b, "kept", C.kept);
	blobmsg_add_u32(b, "undone", C.undone);
	blobmsg_add_u32(b, "misses", C.misses);
	blobmsg_add_u32(b, "proposed", C.proposed);
	blobmsg_add_u32(b, "chans", C.chans);
	blobmsg_add_u32(b, "chans_kept", C.chans_kept);
	blobmsg_add_u32(b, "chans_undone", C.chans_undone);
	blobmsg_add_u32(b, "chan_misses", C.chan_misses);
	blobmsg_add_u32(b, "roams", C.roams);
	blobmsg_add_u32(b, "last_id", C.last_id);
	blobmsg_add_string(b, "last_kind", C.last_kind);
	blobmsg_add_string(b, "last_state", C.last_state);
	blobmsg_add_string(b, "last_summary", C.last_summary);
	blobmsg_add_u64(b, "last_at", (uint64_t)C.last_at);
	blobmsg_add_u64(b, "since", (uint64_t)C.since);
	a = blobmsg_open_array(b, "devices");
	for (i = 0; i < HIST_MAX; i++) {
		const struct mesh_hist *h = &hist[i];

		/* only what matters later: a history of steers, a pause, a move to avoid; seen in the last week */
		if (!h->mac[0] || now - h->seen > 7 * 86400 || (!h->steers && !h->misses && h->pause_until <= now &&
								 h->bad_until <= now))
			continue;
		x = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "mac", h->mac);
		blobmsg_add_u64(b, "seen", (uint64_t)h->seen);
		blobmsg_add_u64(b, "last_steer", (uint64_t)h->last_steer);
		blobmsg_add_u64(b, "pause_until", (uint64_t)h->pause_until);
		blobmsg_add_u32(b, "fails", (uint32_t)h->fails);
		blobmsg_add_u32(b, "steers", h->steers);
		blobmsg_add_u32(b, "kept", h->kept);
		blobmsg_add_u32(b, "undone", h->undone);
		blobmsg_add_u32(b, "misses", h->misses);
		blobmsg_add_string(b, "bad_from", h->bad_from);
		blobmsg_add_string(b, "bad_to", h->bad_to);
		blobmsg_add_u64(b, "bad_until", (uint64_t)h->bad_until);
		blobmsg_close_table(b, x);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "tried");
	for (i = 0; i < TRIED_MAX; i++)
		if (tried[i].until > now) {
			x = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "radio", tried[i].ruid);
			blobmsg_add_u32(b, "channel", (uint32_t)tried[i].ch);
			blobmsg_add_u64(b, "until", (uint64_t)tried[i].until);
			blobmsg_close_table(b, x);
		}
	blobmsg_close_array(b, a);
	blobmsg_close_table(b, t);
}

static long long jnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_STRING: return strtoll(blobmsg_get_string(a), NULL, 10);
	default: return 0;
	}
}

static const char *jstr(struct blob_attr *a)
{
	return blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "";
}

void mesh_load(struct blob_attr *t)
{
	struct blob_attr *cur, *e, *f;
	size_t rem, r2, r3;
	int nh = 0, nt = 0;

	blobmsg_for_each_attr(cur, t, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "steers")) C.steers = (uint32_t)jnum(cur);
		else if (!strcmp(k, "kept")) C.kept = (uint32_t)jnum(cur);
		else if (!strcmp(k, "undone")) C.undone = (uint32_t)jnum(cur);
		else if (!strcmp(k, "misses")) C.misses = (uint32_t)jnum(cur);
		else if (!strcmp(k, "proposed")) C.proposed = (uint32_t)jnum(cur);
		else if (!strcmp(k, "chans")) C.chans = (uint32_t)jnum(cur);
		else if (!strcmp(k, "chans_kept")) C.chans_kept = (uint32_t)jnum(cur);
		else if (!strcmp(k, "chans_undone")) C.chans_undone = (uint32_t)jnum(cur);
		else if (!strcmp(k, "chan_misses")) C.chan_misses = (uint32_t)jnum(cur);
		else if (!strcmp(k, "roams")) C.roams = (uint32_t)jnum(cur);
		else if (!strcmp(k, "last_id")) C.last_id = (uint32_t)jnum(cur);
		else if (!strcmp(k, "last_kind")) snprintf(C.last_kind, sizeof(C.last_kind), "%s", jstr(cur));
		else if (!strcmp(k, "last_state")) snprintf(C.last_state, sizeof(C.last_state), "%s", jstr(cur));
		else if (!strcmp(k, "last_summary")) snprintf(C.last_summary, sizeof(C.last_summary), "%s", jstr(cur));
		else if (!strcmp(k, "last_at")) C.last_at = (long)jnum(cur);
		else if (!strcmp(k, "since")) C.since = (long)jnum(cur);
		else if (!strcmp(k, "devices") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(e, cur, r2) {
				struct mesh_hist *h;

				if (nh >= HIST_MAX)
					break;
				h = &hist[nh];
				memset(h, 0, sizeof(*h));
				h->retry_pct = -1;
				blobmsg_for_each_attr(f, e, r3) {
					const char *fk = blobmsg_name(f);

					if (!strcmp(fk, "mac")) snprintf(h->mac, sizeof(h->mac), "%s", jstr(f));
					else if (!strcmp(fk, "seen")) h->seen = (long)jnum(f);
					else if (!strcmp(fk, "last_steer")) h->last_steer = (long)jnum(f);
					else if (!strcmp(fk, "pause_until")) h->pause_until = (long)jnum(f);
					else if (!strcmp(fk, "fails")) h->fails = (int)jnum(f);
					else if (!strcmp(fk, "steers")) h->steers = (uint32_t)jnum(f);
					else if (!strcmp(fk, "kept")) h->kept = (uint32_t)jnum(f);
					else if (!strcmp(fk, "undone")) h->undone = (uint32_t)jnum(f);
					else if (!strcmp(fk, "misses")) h->misses = (uint32_t)jnum(f);
					else if (!strcmp(fk, "bad_from")) snprintf(h->bad_from, sizeof(h->bad_from), "%s", jstr(f));
					else if (!strcmp(fk, "bad_to")) snprintf(h->bad_to, sizeof(h->bad_to), "%s", jstr(f));
					else if (!strcmp(fk, "bad_until")) h->bad_until = (long)jnum(f);
				}
				if (is_mac(h->mac))
					nh++;
				else
					memset(h, 0, sizeof(*h));
			}
		} else if (!strcmp(k, "tried") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(e, cur, r2) {
				if (nt >= TRIED_MAX)
					break;
				blobmsg_for_each_attr(f, e, r3) {
					const char *fk = blobmsg_name(f);

					if (!strcmp(fk, "radio")) snprintf(tried[nt].ruid, sizeof(tried[nt].ruid), "%s", jstr(f));
					else if (!strcmp(fk, "channel")) tried[nt].ch = (int)jnum(f);
					else if (!strcmp(fk, "until")) tried[nt].until = (long)jnum(f);
				}
				nt++;
			}
		}
	}
}

void mesh_init(void)
{
	if (!C.since)
		C.since = now_s();
	params();
	if (!cfg.mesh) {
		snprintf(status, sizeof(status), "Disabled");
		AG_INFO("mesh Wi-Fi: off (mesh.enabled)");
		return;
	}
	snprintf(status, sizeof(status), "None");
	AG_INFO("mesh Wi-Fi: reading %s every %d s; steering %s, channel plan %s (act.mode %s)",
		cfg.mesh_source[0] ? cfg.mesh_source : "the EasyMesh controller (mesh handler)", cfg.mesh_poll,
		cfg.mesh_steer ? "on" : "off", cfg.mesh_channel ? "on" : "off", ag_mode_str(cfg.act_mode));
	schedule(2);
}
