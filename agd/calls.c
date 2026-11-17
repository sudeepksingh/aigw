/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW calls and games first (the rules: core/libag/calls.c).
 *
 *   noticed   every look at the connections' counters (flow.c, calls.poll
 *             seconds): a UDP connection from a device, packets both ways at
 *             a steady rate, small and slow enough, for calls.c's rules; a
 *             known port or the name the device looked up says which (Zoom,
 *             Xbox Live, ...). A device's real-time connections are one call
 *   first     while AIGW's own smart queueing shapes the line (agd/sqm.c),
 *             the calls' far ends go first in it (scripts/aigw-sqm prio:
 *             cake's voice tin, or htb's priority class). Without smart
 *             queueing nothing is shaped for them: calls first never turns it
 *             on (bufferbloat does, by its own policy)
 *   proved    when a call ends (no packets that look so for 30 s): from the
 *             timeline (agd/why.c), the internet's delay, jitter and loss
 *             while it lasted, and whether the line was full meanwhile.
 *             Smooth or rough, in words: on the timeline and agctl with the
 *             device's name, for the ISP (Device.X_AIGW_.CallsFirst.,
 *             CallReport!) without names, and only with calls.share
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>

#include "ag/calls.h"
#include "ag/sqm.h"
#include "ag/why.h"
#include "agd.h"

#define CAND_MAX  64                    /* UDP connections watched */
#define LIVE_MAX  16                    /* calls and games at once */
#define EP_MAX    16                    /* far ends given priority */
#define END_S     30                    /* a call is over when nothing looked so for this long */
#define REPRIO_S  60                    /* the priority put back this often while calls last */

struct cand {
	uint32_t id;                    /* conntrack's; 0 unused */
	char client[IP_LEN], dst[IP_LEN];
	char name[NAME_LEN];
	uint8_t proto;
	uint16_t dport;
	uint64_t up_b, down_b, up_pk, down_pk;  /* the counters at the last look */
	uint64_t at_ms;
	long first;                     /* first seen, wall s */
	unsigned seen;                  /* the look it was last in */
	struct call_iv iv[CALL_HIST];
	int n;
	int kind, by;                   /* while it is a call or game */
	const char *app;
};

struct live {
	int used;
	char client[IP_LEN], mac[18];
	int kind;
	const char *app;
	long start, last;
	int prioritized, sqm_on;
};

static struct cand cands[CAND_MAX];
static struct live lives[LIVE_MAX];
static unsigned look;
static char applied[EP_MAX * 64], applied_if[32], prio_err[300];
static long applied_at;
static int prio_now;                    /* the priority is in place for the calls now */

/* the reports (kept with agd's state) */
static struct {
	uint32_t total, calls, games, on_full, smooth_full, prioritized;
	struct {
		int kind, verdict, prioritized, sqm_on;
		long start, end, full_s;
		struct call_quality q;
		char mac[18], app[32];
		char home[600], isp[400];
	} last;
} R;

static struct call_params params(void)
{
	struct call_params p;

	call_params_default(&p);
	return p;
}

static uint64_t now_ms(void)
{
	return ag_mono_us() / 1000;
}

/* ---------- the counters ---------- */

static struct cand *cand_of(uint32_t id, int create)
{
	struct cand *free_c = NULL, *old = NULL;
	int i;

	for (i = 0; i < CAND_MAX; i++) {
		struct cand *c = &cands[i];

		if (c->id == id)
			return c;
		if (!c->id && !free_c)
			free_c = c;
		/* full: the one longest unseen goes, unless it is a call */
		if (c->id && c->kind == CK_NONE && (!old || c->seen < old->seen))
			old = c;
	}
	if (!create)
		return NULL;
	if (!free_c)
		free_c = old;
	if (free_c) {
		memset(free_c, 0, sizeof(*free_c));
		free_c->id = id;
	}
	return free_c;
}

void calls_counters(uint32_t id, const char *client, const char *dst, const char *domain, uint8_t proto,
		    uint16_t dport, uint64_t up_b, uint64_t down_b, uint64_t up_pk, uint64_t down_pk)
{
	struct cand *c;
	uint64_t now = now_ms();

	if (!id || !(c = cand_of(id, 1)))
		return;
	snprintf(c->name, sizeof(c->name), "%s", domain ? domain : "");
	if (!c->at_ms || up_pk < c->up_pk || down_pk < c->down_pk || up_b < c->up_b || down_b < c->down_b) {
		snprintf(c->client, sizeof(c->client), "%s", client);
		snprintf(c->dst, sizeof(c->dst), "%s", dst);
		c->proto = proto;
		c->dport = dport;
		c->first = (long)ag_wall_s();
		c->n = 0;
	} else {
		struct call_iv *v;

		if (c->n == CALL_HIST) {
			memmove(&c->iv[0], &c->iv[1], sizeof(c->iv[0]) * (CALL_HIST - 1));
			c->n--;
		}
		v = &c->iv[c->n++];
		v->ms = (uint32_t)(now - c->at_ms);
		v->up_pk = (uint32_t)(up_pk - c->up_pk);
		v->down_pk = (uint32_t)(down_pk - c->down_pk);
		v->up_b = (uint32_t)(up_b - c->up_b > 0xffffffffu ? 0xffffffffu : up_b - c->up_b);
		v->down_b = (uint32_t)(down_b - c->down_b > 0xffffffffu ? 0xffffffffu : down_b - c->down_b);
	}
	c->up_b = up_b, c->down_b = down_b, c->up_pk = up_pk, c->down_pk = down_pk;
	c->at_ms = now;
	c->seen = look;
}

void calls_flow_end(uint32_t id)
{
	struct cand *c = cand_of(id, 0);

	if (c)
		memset(c, 0, sizeof(*c));
}

/* ---------- calls ---------- */

static struct live *live_of(const char *client, int create)
{
	int i;
	struct live *fr = NULL;

	for (i = 0; i < LIVE_MAX; i++) {
		if (lives[i].used && !strcmp(lives[i].client, client))
			return &lives[i];
		if (!lives[i].used && !fr)
			fr = &lives[i];
	}
	if (!create || !fr)
		return NULL;
	memset(fr, 0, sizeof(*fr));
	fr->used = 1;
	snprintf(fr->client, sizeof(fr->client), "%s", client);
	return fr;
}

static const char *who(const char *client, const char *mac)
{
	struct device *d = mac[0] ? dev_find_mac(mac, 0) : NULL;

	if (!d)
		d = dev_find_ip(client, 0);
	return d && d->hostname[0] ? d->hostname : client;
}

static void ev(const char *what, const struct live *l, const char *words)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "client", l->client);
	blobmsg_add_string(&b, "mac", l->mac);
	blobmsg_add_string(&b, "device", who(l->client, l->mac));
	blobmsg_add_string(&b, "kind", call_kind_name(l->kind));
	if (l->app)
		blobmsg_add_string(&b, "app", l->app);
	if (words)
		blobmsg_add_string(&b, "summary", words);
	agd_emit(AG_EV_CALL, &b);
	blob_buf_free(&b);
}

/* the device that downloaded most while the line was full meanwhile (not the call's own); NULL */
static const char *top_of(const struct why_min *m, int n, const struct sqm_line *l, const char *own)
{
	static char name[64];
	char mac[18];
	uint32_t best = 0;
	int i, k = -1;

	for (i = 0; i < n; i++) {
		if (!m[i].top[0].kbps || !why_full(&m[i], l->down_mbps, l->peak_down_mbps, l->peak_up_mbps))
			continue;
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[i].top[0].mac[0], m[i].top[0].mac[1],
			 m[i].top[0].mac[2], m[i].top[0].mac[3], m[i].top[0].mac[4], m[i].top[0].mac[5]);
		if (own[0] && !strcasecmp(mac, own))
			continue;
		if (m[i].top[0].kbps > best) {
			best = m[i].top[0].kbps;
			k = i;
		}
	}
	if (k < 0)
		return NULL;
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[k].top[0].mac[0], m[k].top[0].mac[1],
		 m[k].top[0].mac[2], m[k].top[0].mac[3], m[k].top[0].mac[4], m[k].top[0].mac[5]);
	{
		struct device *d = dev_find_mac(mac, 0);

		snprintf(name, sizeof(name), "%s", d && d->hostname[0] ? d->hostname : mac);
	}
	return name;
}

static void ended(struct live *l)
{
	struct call_params p = params();
	struct call_report r;
	struct sqm_line line;
	struct why_min *m;
	long len = l->last - l->start;
	int n, max, step = cfg.why_interval > 0 ? cfg.why_interval : 60;

	if (len < cfg.calls_min_s) {
		AG_DEBUG("calls: %s on %s over after %ld s: too short to report", call_kind_words(l->kind), l->client, len);
		ev("end", l, NULL);
		return;
	}
	max = (int)(len / step) + 4;
	if (max > WHY_MAX_SAMPLES)
		max = WHY_MAX_SAMPLES;
	m = malloc(sizeof(*m) * (size_t)max);
	memset(&r, 0, sizeof(r));
	why_line_now(&line);
	n = m ? why_samples(l->start, l->last + 1, m, max) : 0;
	call_quality_of(m, n, l->start, l->last + 1, &line, &r.q);
	r.kind = l->kind;
	r.start = l->start;
	r.end = l->last;
	r.verdict = call_verdict(&r.q, &p);
	r.prioritized = l->prioritized;
	r.sqm_on = l->sqm_on;
	/* for the ISP: no device, app or other device */
	call_words(&r, R.last.isp, sizeof(R.last.isp));
	r.device = who(l->client, l->mac);
	r.app = l->app;
	r.top = m ? top_of(m, n, &line, l->mac) : NULL;
	call_words(&r, R.last.home, sizeof(R.last.home));
	free(m);

	R.total++;
	if (l->kind == CK_GAME)
		R.games++;
	else
		R.calls++;
	if (r.q.full) {
		R.on_full++;
		if (r.verdict == CV_SMOOTH)
			R.smooth_full++;
	}
	if (l->prioritized)
		R.prioritized++;
	R.last.kind = l->kind;
	R.last.verdict = r.verdict;
	R.last.prioritized = l->prioritized;
	R.last.sqm_on = l->sqm_on;
	R.last.start = l->start;
	R.last.end = l->last;
	R.last.full_s = r.q.samples ? len * r.q.full / r.q.samples : 0;
	R.last.q = r.q;
	snprintf(R.last.mac, sizeof(R.last.mac), "%s", l->mac);
	snprintf(R.last.app, sizeof(R.last.app), "%s", l->app ? l->app : "");

	AG_INFO("calls: %s", R.last.home);
	{
		/* the timeline's line is short: "Call on laptop: smooth, 12 min (line full 9 min): 24 ms, jitter 8 ms,
		   0.0% lost; went first". No app: the ISP's WhySlow() tells the story (with the timeline's names) */
		char tl[WHY_TEXT + 96], d[24], fd[24];      /* the timeline keeps WHY_TEXT of it */
		const char *k = call_kind_name(l->kind), *dev = who(l->client, l->mac);

		call_duration(len, d, sizeof(d));
		call_duration(R.last.full_s, fd, sizeof(fd));
		snprintf(tl, sizeof(tl), "%s on %.40s: %s, %s%s%s%s: %d ms, jitter %d ms, %d.%d%% lost%s",
			 l->kind == CK_RT ? "Call or game" : k, dev,
			 r.verdict == CV_SMOOTH ? "smooth" : r.verdict == CV_ROUGH ? "rough" : "not measured", d,
			 r.q.full ? " (line full " : "", r.q.full ? fd : "", r.q.full ? ")" : "", r.q.ms < 0 ? 0 : r.q.ms,
			 r.q.jitter_ms < 0 ? 0 : r.q.jitter_ms, r.q.loss10 / 10, r.q.loss10 % 10,
			 l->prioritized ? "; went first" : "");
		why_note_call(l->mac, r.verdict, tl);
	}
	ev("end", l, R.last.home);
	/* the ISP hears of the ones that met a full line, and the rough ones */
	if (cfg.calls_share && (r.q.full || r.verdict == CV_ROUGH))
		dm_on_call(call_kind_name(l->kind), call_verdict_name(r.verdict), len, R.last.full_s, r.q.line_kbps, r.q.ms,
			   r.q.jitter_ms, r.q.loss10, l->prioritized, R.last.isp);
}

/* ---------- first in the queue ---------- */

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void prioritize(long now)
{
	static char eps_s[EP_MAX][IP_LEN + 16];
	const char *eps[EP_MAX];
	char ifc[32], sig[sizeof(applied)], out[300];
	int i, n = 0, shaping;
	size_t l = 0;

	shaping = sqm_shaping(ifc, sizeof(ifc)) == 0;
	for (i = 0; i < LIVE_MAX; i++)
		if (lives[i].used && shaping)
			lives[i].sqm_on = 1;
	if (!shaping) {
		applied[0] = applied_if[0] = '\0';      /* smart queueing gone: its filters with it */
		prio_now = 0;
		return;
	}
	for (i = 0; i < CAND_MAX && n < EP_MAX && cfg.calls_prio; i++) {
		struct cand *c = &cands[i];
		int k;

		if (!c->id || c->kind == CK_NONE)
			continue;
		snprintf(eps_s[n], sizeof(eps_s[n]), "%s,%s,%u", c->proto == 6 ? "tcp" : "udp", c->dst, c->dport);
		for (k = 0; k < n && strcmp(eps[k], eps_s[n]); k++)
			;
		if (k == n) {
			eps[n] = eps_s[n];
			n++;
		}
	}
	qsort(eps, (size_t)n, sizeof(eps[0]), cmp_str);
	sig[0] = '\0';
	for (i = 0; i < n && l < sizeof(sig); i++)
		l += (size_t)snprintf(sig + l, sizeof(sig) - l, "%s ", eps[i]);
	if (!strcmp(sig, applied) && !strcmp(ifc, applied_if) && (!n || now - applied_at < REPRIO_S))
		goto mark;
	if (sqm_prio(ifc, eps, n, out, sizeof(out)) != 0) {
		if (strcmp(prio_err, out))
			AG_WARN("calls: no priority on %s: %s", ifc, out);
		snprintf(prio_err, sizeof(prio_err), "%s", out);
		applied[0] = '\0';
		prio_now = 0;
		return;
	}
	if (strcmp(sig, applied))
		AG_INFO("calls: %s", out);
	prio_err[0] = '\0';
	snprintf(applied, sizeof(applied), "%s", sig);
	snprintf(applied_if, sizeof(applied_if), "%s", ifc);
	applied_at = now;
mark:
	prio_now = n > 0;
	if (prio_now)
		for (i = 0; i < CAND_MAX; i++) {
			struct live *lv;

			if (cands[i].id && cands[i].kind != CK_NONE && (lv = live_of(cands[i].client, 0)))
				lv->prioritized = 1;
		}
}

void calls_sqm_changed(void)
{
	applied[0] = '\0';
}

/* every connection's counters seen: which are calls or games, which calls are over */
void calls_dump_done(void)
{
	struct call_params p = params();
	long now = (long)ag_wall_s();
	int i;

	for (i = 0; i < CAND_MAX; i++) {
		struct cand *c = &cands[i];
		struct call_flow f;
		struct call_class cl;
		struct live *l;

		if (!c->id)
			continue;
		if (c->seen != look) {          /* gone from conntrack */
			memset(c, 0, sizeof(*c));
			continue;
		}
		memset(&f, 0, sizeof(f));
		f.proto = c->proto;
		f.rport = c->dport;
		f.name = c->name;
		f.n = c->n;
		memcpy(f.iv, c->iv, sizeof(f.iv));
		if (call_classify(&f, &p, &cl)) {
			if (c->kind == CK_NONE) {
				/* it started with the intervals that showed it (an app's connection may be older) */
				uint64_t ms = 0;
				int k;

				for (k = c->n - p.steady; k < c->n; k++)
					ms += c->iv[k].ms;
				c->first = now - (long)(ms / 1000);
			}
			if (c->kind == CK_NONE || (c->kind == CK_RT && cl.kind != CK_RT)) {
				c->kind = cl.kind;
				c->by = cl.by;
				c->app = cl.app;
			}
		} else if (c->kind != CK_NONE && c->n &&
			   !(c->iv[c->n - 1].up_pk && c->iv[c->n - 1].down_pk && c->iv[c->n - 1].ms)) {
			c->kind = CK_NONE;      /* a call muted, or on hold, still sends some packets each way */
		}
		if (c->kind == CK_NONE)
			continue;
		l = live_of(c->client, 0);
		if (!l && (l = live_of(c->client, 1))) {
			struct device *d = dev_find_ip(c->client, 0);

			l->kind = c->kind;
			l->app = c->app;
			l->start = c->first;
			if (d)
				snprintf(l->mac, sizeof(l->mac), "%s", d->mac);
			AG_INFO("calls: a %s on %s%s%s%s (%s, port %u)", call_kind_words(l->kind), who(l->client, l->mac),
				l->app ? " (" : "", l->app ? l->app : "", l->app ? ")" : "", c->dst, c->dport);
			ev("start", l, NULL);
		}
		if (!l)
			continue;
		if ((l->kind == CK_RT && c->kind != CK_RT) || (!l->app && c->app)) {
			l->kind = c->kind;
			l->app = c->app;
		}
		if (c->first < l->start)
			l->start = c->first;
		l->last = now;
	}
	look++;
	prioritize(now);
	for (i = 0; i < LIVE_MAX; i++)
		if (lives[i].used && now - lives[i].last >= END_S) {
			ended(&lives[i]);
			lives[i].used = 0;
			dev_state_save();
		}
}

/* ---------- what it is doing ---------- */

static int live_n(void)
{
	int i, n = 0;

	for (i = 0; i < LIVE_MAX; i++)
		n += lives[i].used;
	return n;
}

static const char *status(void)
{
	if (!cfg.calls || !flow_active())
		return "Disabled";
	if (!live_n())
		return "Idle";
	return prio_now ? "Prioritized" : "Active";
}

static void quality_blob(struct blob_buf *b, const struct call_quality *q)
{
	blobmsg_add_u32(b, "samples", (uint32_t)q->samples);
	blobmsg_add_u32(b, "full_samples", (uint32_t)q->full);
	blobmsg_add_u32(b, "latency_ms", (uint32_t)(q->ms < 0 ? 0 : q->ms));
	blobmsg_add_u32(b, "jitter_ms", (uint32_t)(q->jitter_ms < 0 ? 0 : q->jitter_ms));
	blobmsg_add_u32(b, "loss_permille", (uint32_t)q->loss10);
	blobmsg_add_u32(b, "line_kbps", q->line_kbps);
}

void calls_blob(struct blob_buf *b)
{
	char ifc[32] = "";
	void *a, *t;
	int i;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.calls);
	blobmsg_add_u8(b, "priority", (uint8_t)cfg.calls_prio);
	blobmsg_add_u8(b, "shared", (uint8_t)cfg.calls_share);
	blobmsg_add_u8(b, "flows", (uint8_t)flow_active());
	blobmsg_add_string(b, "status", status());
	if (sqm_shaping(ifc, sizeof(ifc)) == 0)
		blobmsg_add_string(b, "sqm", ifc);
	if (prio_err[0])
		blobmsg_add_string(b, "priority_error", prio_err);
	a = blobmsg_open_array(b, "live");
	for (i = 0; i < LIVE_MAX; i++) {
		const struct live *l = &lives[i];
		void *e;
		int k;

		if (!l->used)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "kind", call_kind_name(l->kind));
		blobmsg_add_string(b, "device", who(l->client, l->mac));
		blobmsg_add_string(b, "client", l->client);
		blobmsg_add_string(b, "mac", l->mac);
		if (l->app)
			blobmsg_add_string(b, "app", l->app);
		blobmsg_add_u64(b, "since", (uint64_t)l->start);
		blobmsg_add_u8(b, "first", (uint8_t)(prio_now && l->prioritized));
		e = blobmsg_open_array(b, "far_ends");
		for (k = 0; k < CAND_MAX; k++)
			if (cands[k].id && cands[k].kind != CK_NONE && !strcmp(cands[k].client, l->client)) {
				char ep[IP_LEN + 16];

				snprintf(ep, sizeof(ep), "%s:%u", cands[k].dst, cands[k].dport);
				blobmsg_add_string(b, NULL, ep);
			}
		blobmsg_close_array(b, e);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	t = blobmsg_open_table(b, "stats");
	blobmsg_add_u32(b, "total", R.total);
	blobmsg_add_u32(b, "calls", R.calls);
	blobmsg_add_u32(b, "games", R.games);
	blobmsg_add_u32(b, "on_full_line", R.on_full);
	blobmsg_add_u32(b, "smooth_on_full_line", R.smooth_full);
	blobmsg_add_u32(b, "prioritized", R.prioritized);
	blobmsg_close_table(b, t);
	if (R.last.end) {
		t = blobmsg_open_table(b, "last");
		blobmsg_add_string(b, "kind", call_kind_name(R.last.kind));
		if (R.last.app[0])
			blobmsg_add_string(b, "app", R.last.app);
		blobmsg_add_string(b, "mac", R.last.mac);
		blobmsg_add_u64(b, "start", (uint64_t)R.last.start);
		blobmsg_add_u64(b, "end", (uint64_t)R.last.end);
		blobmsg_add_u32(b, "full_s", (uint32_t)R.last.full_s);
		blobmsg_add_string(b, "verdict", call_verdict_name(R.last.verdict));
		blobmsg_add_u8(b, "prioritized", (uint8_t)R.last.prioritized);
		blobmsg_add_u8(b, "sqm", (uint8_t)R.last.sqm_on);
		quality_blob(b, &R.last.q);
		blobmsg_add_string(b, "summary", R.last.home);
		blobmsg_add_string(b, "isp_summary", R.last.isp);
		blobmsg_close_table(b, t);
	}
}

/* Device.X_AIGW_.CallsFirst.: what the ISP may see (nothing but Enable and Shared without calls.share) */
int calls_field(const char *f, char *buf, size_t n)
{
	int sh = cfg.calls && cfg.calls_share, has = sh && R.last.end;
	const struct call_quality *q = &R.last.q;

	if (!strcmp(f, "Enable"))
		snprintf(buf, n, "%s", cfg.calls ? "true" : "false");
	else if (!strcmp(f, "Shared"))
		snprintf(buf, n, "%s", cfg.calls_share ? "true" : "false");
	else if (!strcmp(f, "Priority"))
		snprintf(buf, n, "%s", cfg.calls && cfg.calls_prio ? "true" : "false");
	else if (!strcmp(f, "Status"))
		snprintf(buf, n, "%s", !cfg.calls || !flow_active() ? "Disabled" : sh ? status() : "NotShared");
	else if (!strcmp(f, "ActiveCalls"))
		snprintf(buf, n, "%d", sh ? live_n() : 0);
	else if (!strcmp(f, "Total"))
		snprintf(buf, n, "%u", sh ? R.total : 0);
	else if (!strcmp(f, "OnFullLine"))
		snprintf(buf, n, "%u", sh ? R.on_full : 0);
	else if (!strcmp(f, "SmoothOnFullLine"))
		snprintf(buf, n, "%u", sh ? R.smooth_full : 0);
	else if (!strcmp(f, "Prioritized"))
		snprintf(buf, n, "%u", sh ? R.prioritized : 0);
	else if (!strcmp(f, "Last.Kind"))
		snprintf(buf, n, "%s", has ? call_kind_name(R.last.kind) : "");
	else if (!strcmp(f, "Last.Start"))
		snprintf(buf, n, "%ld", has ? R.last.start : 0L);
	else if (!strcmp(f, "Last.DurationSeconds"))
		snprintf(buf, n, "%ld", has ? R.last.end - R.last.start : 0L);
	else if (!strcmp(f, "Last.FullLineSeconds"))
		snprintf(buf, n, "%ld", has ? R.last.full_s : 0L);
	else if (!strcmp(f, "Last.LineKbps"))
		snprintf(buf, n, "%u", has ? q->line_kbps : 0);
	else if (!strcmp(f, "Last.LatencyMs"))
		snprintf(buf, n, "%d", has && q->ms > 0 ? q->ms : 0);
	else if (!strcmp(f, "Last.JitterMs"))
		snprintf(buf, n, "%d", has && q->jitter_ms > 0 ? q->jitter_ms : 0);
	else if (!strcmp(f, "Last.LossPercent"))
		snprintf(buf, n, "%d.%d", has ? q->loss10 / 10 : 0, has ? q->loss10 % 10 : 0);
	else if (!strcmp(f, "Last.Prioritized"))
		snprintf(buf, n, "%s", has && R.last.prioritized ? "true" : "false");
	else if (!strcmp(f, "Last.Verdict"))
		snprintf(buf, n, "%s", has ? call_verdict_name(R.last.verdict) : "");
	else if (!strcmp(f, "Last.Summary"))
		snprintf(buf, n, "%s", has ? R.last.isp : "");
	else
		return -1;
	return 0;
}

/* ---------- saved with agd's state ---------- */

void calls_save(struct blob_buf *b)
{
	void *t;

	if (!R.total)
		return;
	t = blobmsg_open_table(b, "calls");
	blobmsg_add_u32(b, "total", R.total);
	blobmsg_add_u32(b, "calls", R.calls);
	blobmsg_add_u32(b, "games", R.games);
	blobmsg_add_u32(b, "on_full", R.on_full);
	blobmsg_add_u32(b, "smooth_full", R.smooth_full);
	blobmsg_add_u32(b, "prioritized", R.prioritized);
	blobmsg_add_u32(b, "kind", (uint32_t)R.last.kind);
	blobmsg_add_u32(b, "verdict", (uint32_t)R.last.verdict);
	blobmsg_add_u32(b, "last_prio", (uint32_t)R.last.prioritized);
	blobmsg_add_u32(b, "last_sqm", (uint32_t)R.last.sqm_on);
	blobmsg_add_u64(b, "start", (uint64_t)R.last.start);
	blobmsg_add_u64(b, "end", (uint64_t)R.last.end);
	blobmsg_add_u64(b, "full_s", (uint64_t)R.last.full_s);
	blobmsg_add_u32(b, "samples", (uint32_t)R.last.q.samples);
	blobmsg_add_u32(b, "full", (uint32_t)R.last.q.full);
	blobmsg_add_u32(b, "judged", (uint32_t)R.last.q.judged);
	blobmsg_add_u32(b, "ms", (uint32_t)(R.last.q.ms + 1));
	blobmsg_add_u32(b, "jitter", (uint32_t)(R.last.q.jitter_ms + 1));
	blobmsg_add_u32(b, "loss10", (uint32_t)R.last.q.loss10);
	blobmsg_add_u32(b, "line_kbps", R.last.q.line_kbps);
	blobmsg_add_string(b, "mac", R.last.mac);
	blobmsg_add_string(b, "app", R.last.app);
	blobmsg_add_string(b, "home", R.last.home);
	blobmsg_add_string(b, "isp", R.last.isp);
	blobmsg_close_table(b, t);
}

static long jnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (long)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (long)blobmsg_get_u8(a);
	}
	return 0;
}

void calls_load(struct blob_attr *t)
{
	struct blob_attr *c;
	size_t rem;

	memset(&R, 0, sizeof(R));
	R.last.q.ms = R.last.q.jitter_ms = -1;
	blobmsg_for_each_attr(c, t, rem) {
		const char *k = blobmsg_name(c);
		long v = jnum(c);

		if (blobmsg_type(c) == BLOBMSG_TYPE_STRING) {
			const char *s = blobmsg_get_string(c);

			if (!strcmp(k, "mac")) snprintf(R.last.mac, sizeof(R.last.mac), "%s", s);
			else if (!strcmp(k, "app")) snprintf(R.last.app, sizeof(R.last.app), "%s", s);
			else if (!strcmp(k, "home")) snprintf(R.last.home, sizeof(R.last.home), "%s", s);
			else if (!strcmp(k, "isp")) snprintf(R.last.isp, sizeof(R.last.isp), "%s", s);
			continue;
		}
		if (!strcmp(k, "total")) R.total = (uint32_t)v;
		else if (!strcmp(k, "calls")) R.calls = (uint32_t)v;
		else if (!strcmp(k, "games")) R.games = (uint32_t)v;
		else if (!strcmp(k, "on_full")) R.on_full = (uint32_t)v;
		else if (!strcmp(k, "smooth_full")) R.smooth_full = (uint32_t)v;
		else if (!strcmp(k, "prioritized")) R.prioritized = (uint32_t)v;
		else if (!strcmp(k, "kind")) R.last.kind = (int)v;
		else if (!strcmp(k, "verdict")) R.last.verdict = (int)v;
		else if (!strcmp(k, "last_prio")) R.last.prioritized = (int)v;
		else if (!strcmp(k, "last_sqm")) R.last.sqm_on = (int)v;
		else if (!strcmp(k, "start")) R.last.start = v;
		else if (!strcmp(k, "end")) R.last.end = v;
		else if (!strcmp(k, "full_s")) R.last.full_s = v;
		else if (!strcmp(k, "samples")) R.last.q.samples = (int)v;
		else if (!strcmp(k, "full")) R.last.q.full = (int)v;
		else if (!strcmp(k, "judged")) R.last.q.judged = (int)v;
		else if (!strcmp(k, "ms")) R.last.q.ms = (int)v - 1;
		else if (!strcmp(k, "jitter")) R.last.q.jitter_ms = (int)v - 1;
		else if (!strcmp(k, "loss10")) R.last.q.loss10 = (int)v;
		else if (!strcmp(k, "line_kbps")) R.last.q.line_kbps = (uint32_t)v;
	}
}

void calls_init(void)
{
	if (!cfg.calls)
		return;
	AG_INFO("calls: calls and games first (a look every %d s%s%s)", cfg.calls_poll,
		cfg.calls_prio ? ", first inside AIGW's smart queueing when it is on" : ", reports only",
		cfg.calls_share ? "" : "; kept home: calls.share 0");
	if (!flow_active())
		AG_INFO("calls: the flow sensor is off: no calls or games noticed");
}
