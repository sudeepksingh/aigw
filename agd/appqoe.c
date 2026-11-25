/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW per-app experience (the rules: core/libag/appqoe.c, ag/appqoe.h).
 *
 *   looks      every look at the connections' counters (flow.c, apps.poll s):
 *              each connection's packets and bytes each way since the look
 *              before; its kind from the rules (apps.rules, else the built-in
 *              ones), with the server's family when apps.names allows. A
 *              device's connections of one kind are one session
 *   windows    every why.interval (a minute), each session's minute is judged
 *              in its own terms (a call's mean opinion score and freezes, a
 *              stream's rebuffering, a game's lag), from its own counters and
 *              the timeline's sample of the line and the device's Wi-Fi
 *   moments    bad minutes in a row are one bad moment. When it is over, its
 *              cause from the timeline's verdict for that device then (Wi-Fi
 *              congestion, a weak signal, the line full and by whom, the ISP),
 *              else the far end; on the timeline, `agctl apps`, the ISP's data
 *              model and AppMoment! (without names, with apps.share). Then
 *              what AIGW did next, and how the same kind of session went since
 *   scores     per kind of app (calls, streaming, gaming, ...): the share of
 *              its minutes that were fine over 7 days
 *
 * Nothing but a kind and a family name ("Zoom") is kept: never a server name,
 * an address of a server, a URL or any content. Saved with agd's state.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>

#include "ag/appqoe.h"
#include "ag/diag.h"
#include "ag/sqm.h"
#include "ag/why.h"
#include "agd.h"

#define FL_MAX    160                   /* connections watched */
#define FL_HASH   256
#define SE_MAX    32                    /* sessions at once */
#define MO_MAX    32                    /* bad moments kept */
#define DONE_MAX  48                    /* sessions that ended, kept */
#define BR_MAX    32                    /* devices opening web connections */
#define DAYS      8
#define END_S     30                    /* a real-time session is over after this long without its traffic */
#define IDLE_S    300                   /* ... a stream, a download, browsing */
#define IOT_S     3600                  /* ... a smart-home device (away and back is a moment of its own) */
#define FIX_S     1800                  /* what AIGW did within this after a moment */
#define MIN_SCORE 10                    /* minutes before a score is said */

struct fl {
	uint32_t id;                    /* conntrack's; 0 unused */
	int16_t next;                   /* the hash chain */
	char ip[IP_LEN];
	uint8_t proto, kind, conf, miss, n, iot;
	int8_t fam;
	uint16_t port;
	uint32_t name_h;                /* its name's hash (to see it change; the name isn't kept) */
	struct app_iv iv[APP_HIST];
	uint64_t ub, db, up, dp, at_ms;
	long first;
	unsigned seen;
};

struct se {
	uint8_t used, kind, conf;
	int8_t fam;
	uint32_t id;
	char ip[IP_LEN], mac[18];
	long start, last;               /* last: its traffic last seen */
	/* this look (all its connections) */
	uint64_t l_db, l_ub, l_dp;
	uint32_t l_ms;
	int l_on, l_alive;              /* doing its thing; its connections still there (smart home: idle is fine) */
	int on_wifi;                    /* smart home: the device was seen on Wi-Fi (leaving it is going away) */
	/* the window being gathered */
	long w_start;
	uint64_t w_db, w_ub, w_dp;
	uint32_t w_ms, w_gap;
	int w_looks, w_active, w_offline;
	/* stalls: looks far below its usual rate, between usual ones */
	int dip_n;
	uint32_t dip_ms;
	uint64_t dip_pk;
	/* its usual */
	uint32_t ref_kbps, ref_pps;
	int windows;
	float drain;
	int counted;
	long gone_at;                   /* smart home: its connections went */
	/* how it went */
	int minutes, bad_min, moments;
	int open;                       /* the moment still going (index in M), -1 */
	struct app_q q;                 /* the last window's */
};

struct mo {
	uint32_t seq, sess;
	uint8_t kind, what, conf, cause, closed, fixed_looked;
	char app[24], mac[18], code[20];
	long t, end, fix_t;
	int seconds, mos10, bad_min;
	/* the evidence */
	int recorded, band, signal, wifi_loss, line_full, rtt, rtt_usual, loss10;
	char top[18];
	uint32_t top_kbps;
	char fix[120];
};

struct done {
	uint32_t id;
	uint8_t kind;
	char app[24], mac[18];
	long start, end;
	int minutes, bad_min, moments;
};

struct day {
	int32_t day;
	uint32_t min[AGR_N], bad[AGR_N], sess[AGR_N];
};

static struct app_rules rules;
static int rules_builtin, rules_n;
static char rules_file[256];
static struct fl F[FL_MAX];
static int16_t fh[FL_HASH];
static struct se S[SE_MAX];
static struct mo M[MO_MAX];
static unsigned m_head, m_n;
static struct done D[DONE_MAX];
static unsigned d_head, d_n;
static struct day days[DAYS];
static struct { char ip[IP_LEN]; int n; long since; } br[BR_MAX];
static uint32_t seq_mo, seq_se, total_sess, total_mo;
static unsigned look;
static uint64_t cost_us, cost_max_us;
static int inited;

static uint64_t now_ms(void)
{
	return ag_mono_us() / 1000;
}

static int win_s(void)
{
	return cfg.why_interval > 0 ? cfg.why_interval : 60;
}

/* ---------- the rules ---------- */

static void rules_load(void)
{
	FILE *f;
	char *text = NULL;
	long len = 0;

	snprintf(rules_file, sizeof(rules_file), "%s", cfg.apps_rules);
	rules_builtin = 1;
	if (rules_file[0] && (f = fopen(rules_file, "r"))) {
		if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < 256 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
		    (text = malloc((size_t)len + 1)) && fread(text, 1, (size_t)len, f) == (size_t)len) {
			text[len] = '\0';
			rules_n = app_rules_parse(&rules, text);
			if (rules_n > 0)
				rules_builtin = 0;
			else
				AG_WARN("apps: %s has no rule (%s): the built-in ones instead", rules_file,
					rules.err[0] ? rules.err : "empty");
		}
		free(text);
		fclose(f);
	}
	if (rules_builtin)
		rules_n = app_rules_parse(&rules, app_rules_default());
	if (rules.err[0])
		AG_WARN("apps: %s: %s (that line skipped)", rules_builtin ? "the built-in rules" : rules_file, rules.err);
	/* the connections are told apart again under the new rules */
	{
		int i;

		for (i = 0; i < FL_MAX; i++)
			F[i].kind = AK_NONE, F[i].name_h = 0, F[i].fam = -1;
	}
}

/* ---------- the connections ---------- */

static uint32_t hash_s(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h | 1;
}

static struct fl *fl_find(uint32_t id)
{
	int16_t k;

	for (k = fh[id % FL_HASH]; k >= 0; k = F[k].next)
		if (F[k].id == id)
			return &F[k];
	return NULL;
}

static void fl_drop(struct fl *f)
{
	int16_t *p = &fh[f->id % FL_HASH], me = (int16_t)(f - F);

	while (*p >= 0 && *p != me)
		p = &F[*p].next;
	if (*p == me)
		*p = f->next;
	memset(f, 0, sizeof(*f));
	f->next = -1;
}

static struct fl *fl_new(uint32_t id)
{
	struct fl *fr = NULL, *old = NULL;
	int i;

	for (i = 0; i < FL_MAX && !fr; i++) {
		if (!F[i].id)
			fr = &F[i];
		/* full: a connection nothing was made of, the quietest last time */
		else if (F[i].kind == AK_NONE && F[i].n && (!old || F[i].iv[F[i].n - 1].down_b + F[i].iv[F[i].n - 1].up_b <
								 old->iv[old->n - 1].down_b + old->iv[old->n - 1].up_b))
			old = &F[i];
	}
	if (!fr) {
		if (!old || old->iv[old->n - 1].down_b + old->iv[old->n - 1].up_b > 2000)
			return NULL;
		fl_drop(old);
		fr = old;
	}
	memset(fr, 0, sizeof(*fr));
	fr->id = id;
	fr->fam = -1;
	fr->next = fh[id % FL_HASH];
	fh[id % FL_HASH] = (int16_t)(fr - F);
	return fr;
}

static int iot_dev(const struct device *d)
{
	static const char *const t[] = { "IoT", "camera", "smart speaker", "smart home", "printer" };
	size_t i;

	if (!d)
		return 0;
	if (!strcmp(d->zone, "iot"))
		return 1;
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(d->type, t[i]))
			return 1;
	return 0;
}

void appqoe_counters(uint32_t id, const char *client, const char *domain, uint8_t proto, uint16_t dport, uint64_t up_b,
		     uint64_t down_b, uint64_t up_pk, uint64_t down_pk)
{
	struct fl *f;
	uint64_t now = now_ms();

	if (!cfg.apps || !inited || !id || (proto != 6 && proto != 17))
		return;
	if (!(f = fl_find(id)) && !(f = fl_new(id)))
		return;
	if (!f->at_ms || up_pk < f->up || down_pk < f->dp || up_b < f->ub || down_b < f->db) {
		snprintf(f->ip, sizeof(f->ip), "%s", client);
		f->proto = proto;
		f->port = dport;
		f->first = (long)ag_wall_s();
		f->n = 0;
		f->iot = (uint8_t)iot_dev(dev_find_ip(client, 0));
	} else {
		struct app_iv *v;

		if (f->n == APP_HIST) {
			memmove(&f->iv[0], &f->iv[1], sizeof(f->iv[0]) * (APP_HIST - 1));
			f->n--;
		}
		v = &f->iv[f->n++];
		v->ms = (uint32_t)(now - f->at_ms);
		v->up_pk = (uint32_t)(up_pk - f->up);
		v->down_pk = (uint32_t)(down_pk - f->dp);
		v->up_b = (uint32_t)(up_b - f->ub > 0xffffffffu ? 0xffffffffu : up_b - f->ub);
		v->down_b = (uint32_t)(down_b - f->db > 0xffffffffu ? 0xffffffffu : down_b - f->db);
	}
	/* the family only (never the name itself) */
	if (cfg.apps_names && domain && *domain) {
		uint32_t h = hash_s(domain);

		if (h != f->name_h) {
			f->name_h = h;
			f->fam = (int8_t)app_family_of(&rules, domain);
		}
	} else
		f->fam = -1;
	f->ub = up_b, f->db = down_b, f->up = up_pk, f->dp = down_pk;
	f->at_ms = now;
	f->seen = look;
}

void appqoe_flow_end(uint32_t id)
{
	struct fl *f = inited ? fl_find(id) : NULL;

	if (f)
		fl_drop(f);
}

/* conntrack saw a connection start: web ones, counted for browsing */
void appqoe_flow_new(const char *client, uint8_t proto, uint16_t dport)
{
	int i, fr = -1;
	long now = (long)ag_wall_s();

	if (!cfg.apps || !inited || !((proto == 6 && (dport == 443 || dport == 80)) || (proto == 17 && dport == 443)))
		return;
	for (i = 0; i < BR_MAX; i++) {
		if (br[i].ip[0] && !strcmp(br[i].ip, client)) {
			if (now - br[i].since >= win_s()) {
				br[i].since = now;
				br[i].n = 0;
			}
			br[i].n++;
			return;
		}
		if (fr < 0 && (!br[i].ip[0] || now - br[i].since >= 2L * win_s()))
			fr = i;
	}
	if (fr >= 0) {
		snprintf(br[fr].ip, sizeof(br[fr].ip), "%s", client);
		br[fr].since = now;
		br[fr].n = 1;
	}
}

/* ---------- sessions ---------- */

static const char *who(const char *mac, const char *ip)
{
	struct device *d = mac && mac[0] ? dev_find_mac(mac, 0) : NULL;

	if (!d && ip && ip[0])
		d = dev_find_ip(ip, 0);
	return d && d->hostname[0] ? d->hostname : mac && mac[0] ? mac : ip ? ip : "";
}

static const char *fam_name(int fam)
{
	return fam >= 0 && fam < rules.nfam ? rules.fam[fam].name : "";
}

static struct se *se_of(const char *ip, int kind, int create)
{
	struct se *fr = NULL;
	int i;

	for (i = 0; i < SE_MAX; i++) {
		if (S[i].used && S[i].kind == kind && !strcmp(S[i].ip, ip))
			return &S[i];
		if (!S[i].used && !fr)
			fr = &S[i];
	}
	if (!create || !fr)
		return NULL;
	memset(fr, 0, sizeof(*fr));
	fr->used = 1;
	fr->kind = (uint8_t)kind;
	fr->fam = -1;
	fr->id = ++seq_se;
	fr->open = -1;
	fr->start = fr->last = fr->w_start = (long)ag_wall_s();
	snprintf(fr->ip, sizeof(fr->ip), "%s", ip);
	{
		struct device *d = dev_find_ip(ip, 0);

		if (d)
			snprintf(fr->mac, sizeof(fr->mac), "%s", d->mac);
	}
	return fr;
}

static int realtime(int kind)
{
	return kind == AK_VOICE || kind == AK_VIDEO || kind == AK_SCREEN || kind == AK_GAME || kind == AK_CLOUDGAME;
}

/* a connection doing its kind's thing in its last look */
static int on(const struct fl *f)
{
	const struct app_iv *v = f->n ? &f->iv[f->n - 1] : NULL;

	if (!v || !v->ms)
		return 0;
	if (realtime(f->kind))
		return v->up_pk && v->down_pk && ((uint64_t)v->down_pk + v->up_pk) * 1000 / v->ms >= 2;
	return v->down_b + (uint64_t)v->up_b > 0;
}

static struct day *today(long now)
{
	int32_t d = (int32_t)(now / 86400);
	int i, oldest = 0;

	for (i = 0; i < DAYS; i++) {
		if (days[i].day == d)
			return &days[i];
		if (days[i].day < days[oldest].day)
			oldest = i;
	}
	memset(&days[oldest], 0, sizeof(days[oldest]));
	days[oldest].day = d;
	return &days[oldest];
}

/* a look's packets against the session's usual: a stall when it falls far below it, between usual looks */
static void look_rate(struct se *s, uint32_t pps, uint32_t ms, uint64_t pk)
{
	if (!s->ref_pps) {
		if (pps >= 4)
			s->ref_pps = pps;
		return;
	}
	if ((uint64_t)pps * 2 < s->ref_pps) {
		s->dip_n++;
		s->dip_ms += ms;
		s->dip_pk += pk;
		if (s->dip_ms > 30000) {
			/* not a stall: it changed (the camera off, on hold); its new usual */
			if (pps >= 4)
				s->ref_pps = pps;
			s->dip_n = 0, s->dip_ms = 0, s->dip_pk = 0;
		}
		return;
	}
	if (s->dip_n && (uint64_t)pps * 10 >= (uint64_t)s->ref_pps * 7) {
		uint64_t want = (uint64_t)s->ref_pps * s->dip_ms / 1000;

		if (want > s->dip_pk)
			s->w_gap += (uint32_t)((want - s->dip_pk) * 1000 / s->ref_pps);
	}
	if (!s->dip_n || (uint64_t)pps * 10 >= (uint64_t)s->ref_pps * 7)
		s->dip_n = 0, s->dip_ms = 0, s->dip_pk = 0;
	if ((uint64_t)pps * 10 >= (uint64_t)s->ref_pps * 7)
		s->ref_pps = (s->ref_pps * 4 + pps) / 5;
}

/* ---------- the network meanwhile (one sample of the timeline for all sessions judged at once) ---------- */

struct net {
	long t;                         /* the sample's start; 0 none */
	int rtt, usual, jitter, loss10, dns_ms, dns_fail, full;
	uint8_t top[6];
	uint32_t top_kbps;
};

static void net_now(long from, struct net *o)
{
	struct why_min m[4];
	struct sqm_line line;
	int n;

	memset(o, 0, sizeof(*o));
	o->rtt = o->usual = o->jitter = o->loss10 = o->dns_ms = o->dns_fail = -1;
	if (!cfg.why)
		return;
	n = why_samples(from, (long)ag_wall_s() + 1, m, 4);
	if (n <= 0)
		return;
	why_line_now(&line);
	if (line.have_normal)
		o->usual = (int)(line.normal_ms + 0.5f);
	o->t = (long)m[n - 1].t;
	if (m[n - 1].in_sent) {
		o->loss10 = m[n - 1].in_lost * 1000 / m[n - 1].in_sent;
		if (m[n - 1].in_ms != WHY_NONE)
			o->rtt = m[n - 1].in_ms;
	}
	if (n >= 2 && o->rtt >= 0 && m[n - 2].in_ms != WHY_NONE)
		o->jitter = o->rtt > m[n - 2].in_ms ? o->rtt - m[n - 2].in_ms : m[n - 2].in_ms - o->rtt;
	if (m[n - 1].dns_ms != WHY_NONE)
		o->dns_ms = m[n - 1].dns_ms;
	if (m[n - 1].dns_q)
		o->dns_fail = m[n - 1].dns_fail * 100 / m[n - 1].dns_q;
	o->full = m[n - 1].route && why_full(&m[n - 1], line.down_mbps, line.peak_down_mbps, line.peak_up_mbps);
	memcpy(o->top, m[n - 1].top[0].mac, 6);
	o->top_kbps = m[n - 1].top[0].kbps;
}

static void mac_str(const uint8_t m[6], char *s, size_t n)
{
	snprintf(s, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---------- bad moments ---------- */

static struct mo *mo_at(int k)
{
	return k >= 0 && k < MO_MAX ? &M[k] : NULL;
}

static int mo_new(const struct se *s, long t)
{
	int k = (int)m_head;
	struct mo *m = &M[k];

	m_head = (m_head + 1) % MO_MAX;
	if (m_n < MO_MAX)
		m_n++;
	/* a session's open moment may be the one overwritten (32 later): it is closed with it */
	{
		int i;

		for (i = 0; i < SE_MAX; i++)
			if (S[i].used && S[i].open == k)
				S[i].open = -1;
	}
	memset(m, 0, sizeof(*m));
	m->seq = ++seq_mo;
	m->sess = s->id;
	m->kind = s->kind;
	m->t = t;
	m->end = t;
	snprintf(m->app, sizeof(m->app), "%s", fam_name(s->fam));
	snprintf(m->mac, sizeof(m->mac), "%s", s->mac);
	m->rtt = m->rtt_usual = m->loss10 = m->wifi_loss = -1;
	return k;
}

static void evidence_of(const struct mo *m, struct app_evidence *e, char *top, size_t tn)
{
	memset(e, 0, sizeof(*e));
	e->code = m->code[0] ? m->code : NULL;
	e->recorded = m->recorded;
	e->band = m->band;
	e->signal_dbm = m->signal;
	e->wifi_loss_pct = m->wifi_loss;
	e->line_full = m->line_full;
	e->top_kbps = m->top_kbps;
	e->rtt_ms = m->rtt;
	e->rtt_usual_ms = m->rtt_usual;
	e->loss10 = m->loss10;
	if (m->top[0] && top) {
		snprintf(top, tn, "%s", who(m->top, NULL));
		e->top = top;
	}
}

/* sessions of its group on its device since (the fix, else the moment): fine, with trouble */
static void since_of(const struct mo *m, int *ok, int *bad)
{
	long from = m->fix_t ? m->fix_t : m->end;
	int g = app_group_of(m->kind);
	unsigned i;

	*ok = *bad = 0;
	for (i = 0; i < d_n; i++) {
		const struct done *d = &D[(d_head + DONE_MAX - 1 - i) % DONE_MAX];

		if (d->id == m->sess || d->start < from || app_group_of(d->kind) != g || strcasecmp(d->mac, m->mac))
			continue;
		if (d->moments)
			(*bad)++;
		else
			(*ok)++;
	}
}

/* in words: home (names) or for the ISP (none; the app only with brief.names) */
static void mo_text(const struct mo *m, int home, char *buf, size_t n)
{
	struct app_moment_words w;
	struct app_evidence e;
	char top[64];

	evidence_of(m, &e, home || cfg.brief_names ? top : NULL, sizeof(top));
	memset(&w, 0, sizeof(w));
	w.kind = m->kind;
	w.device = home || cfg.brief_names ? who(m->mac, NULL) : NULL;
	w.app = m->app[0] && (home || cfg.brief_names) ? m->app : NULL;
	w.t = m->t;
	w.now = (long)ag_wall_s();
	w.what = m->what;
	w.seconds = m->seconds;
	w.conf = m->conf;
	w.cause = m->cause;
	w.ev = &e;
	w.fix = m->fix[0] ? m->fix : NULL;
	w.fix_t = m->fix_t;
	since_of(m, &w.since_ok, &w.since_bad);
	app_moment_text(&w, buf, n);
	if (!home && !cfg.brief_names)
		brief_scrub_isp(buf, n);
}

static void undo_bad(const struct mo *m)
{
	struct day *d = today(m->t);
	int g = app_group_of(m->kind), i;

	if (g >= 0)
		d->bad[g] = d->bad[g] > (uint32_t)m->bad_min ? d->bad[g] - (uint32_t)m->bad_min : 0;
	for (i = 0; i < SE_MAX; i++)
		if (S[i].used && S[i].id == m->sess) {
			S[i].bad_min = S[i].bad_min > m->bad_min ? S[i].bad_min - m->bad_min : 0;
			S[i].moments--;
		}
}

/* it is over: why, and say so */
static void mo_close(int k)
{
	struct mo *m = mo_at(k);
	struct app_evidence e;
	char home[600], isp[600], err[120], top[64];

	if (!m || m->closed)
		return;
	m->closed = 1;
	{
		static struct why_result r;
		struct why_in in;
		long w = win_s();

		if (cfg.why && why_ask(m->t - w, m->end + 1, m->mac[0] ? m->mac : "", &r, &in, err, sizeof(err)) == 0) {
			m->recorded = r.recorded > 0;
			if (r.n && strcmp(r.f[0].code, "NOT_RECORDED") && strcmp(r.f[0].code, "NO_DATA"))
				snprintf(m->code, sizeof(m->code), "%s", r.f[0].code);
		}
	}
	evidence_of(m, &e, top, sizeof(top));
	m->cause = (uint8_t)app_cause_of(&e);
	/* a download is slow for many reasons beyond anyone's reach (the server): a moment only for the home's or the line's */
	if (m->kind == AK_DOWNLOAD && !app_cause_home(m->cause) && m->cause != AC_LINE && m->cause != AC_ISP) {
		AG_DEBUG("apps: a slow download on %s, not the home's or the line's (%s): not a moment", who(m->mac, NULL),
			 app_cause_name(m->cause));
		undo_bad(m);
		m->seq = 0;             /* not listed */
		return;
	}
	total_mo++;
	mo_text(m, 1, home, sizeof(home));
	mo_text(m, 0, isp, sizeof(isp));
	AG_INFO("apps: %s", home);
	{
		/* the timeline's line (no app: the ISP's WhySlow() tells the story) */
		char tl[WHY_TEXT + 64], wh[96];

		app_what_words(m->kind, m->what, m->seconds, wh, sizeof(wh));
		snprintf(tl, sizeof(tl), "%c%s on %.40s %s (%s)", app_kind_words(m->kind)[0] - 32, app_kind_words(m->kind) + 1,
			 who(m->mac, NULL), wh, app_cause_name(m->cause));
		why_note_app(m->mac, m->kind, tl);
	}
	{
		struct blob_buf b = { 0 };

		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "event", "moment");
		blobmsg_add_u32(&b, "moment", m->seq);
		blobmsg_add_string(&b, "kind", app_kind_name(m->kind));
		if (m->app[0])
			blobmsg_add_string(&b, "app", m->app);
		blobmsg_add_string(&b, "mac", m->mac);
		blobmsg_add_string(&b, "device", who(m->mac, NULL));
		blobmsg_add_string(&b, "what", app_what_name(m->what));
		blobmsg_add_u32(&b, "seconds", (uint32_t)m->seconds);
		blobmsg_add_u32(&b, "confidence", m->conf);
		blobmsg_add_string(&b, "cause", app_cause_name(m->cause));
		blobmsg_add_string(&b, "summary", home);
		agd_emit(AG_EV_APP, &b);
		blob_buf_free(&b);
	}
	if (cfg.apps_share)
		dm_on_app(m->seq, app_kind_name(m->kind), app_what_name(m->what), m->seconds, m->conf, app_cause_name(m->cause),
			  m->code, m->t, isp);
	dev_state_save();
}

/* what happened next: a channel moved, a device steered, smart queueing on (only what can help with its cause) */
static void mo_follow(struct mo *m, long now)
{
	static struct why_ev ev[64];
	int n, i, wifi = m->cause == AC_WIFI_BUSY || m->cause == AC_WIFI_WEAK || m->cause == AC_WIFI_DROPS ||
		       m->cause == AC_WIFI;
	uint8_t mac[6] = { 0 };
	unsigned v[6];

	if (m->fixed_looked || !m->closed || !m->seq)
		return;
	if (!wifi && m->cause != AC_LINE_FULL) {
		m->fixed_looked = 1;
		return;
	}
	if (sscanf(m->mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
		for (i = 0; i < 6; i++)
			mac[i] = (uint8_t)v[i];
	n = why_events(m->t, now + 1, ev, 64);
	for (i = 0; i < n && !m->fix[0]; i++) {
		const struct why_ev *e = &ev[i];
		const char *p;

		if ((long)e->t > m->end + FIX_S)
			break;
		if (wifi && e->kind == WE_CHANNEL && (p = strstr(e->text, " moved from channel "))) {
			int k, aigw = 0;

			/* AIGW's own decision just before: AIGW moved it */
			for (k = i; k >= 0 && (long)ev[k].t >= (long)e->t - 300; k--)
				if (ev[k].kind == WE_DECISION && !(ev[k].flag & WF_ENDED) && strstr(ev[k].text, "acted: channel"))
					aigw = 1;
			if (aigw)
				snprintf(m->fix, sizeof(m->fix), "AIGW moved %.*s from channel%.80s", (int)(p - e->text), e->text,
					 p + strlen(" moved from channel"));
			else
				snprintf(m->fix, sizeof(m->fix), "%.118s", e->text);
		} else if (wifi && e->kind == WE_STEER && !memcmp(e->mac, mac, 6))
			snprintf(m->fix, sizeof(m->fix), "%.118s", e->text);
		else if (e->kind == WE_DECISION && !(e->flag & WF_ENDED) && (p = strstr(e->text, "acted: "))) {
			p += strlen("acted: ");
			if (m->cause == AC_LINE_FULL && !strncmp(p, "sqm", 3))
				snprintf(m->fix, sizeof(m->fix), "AIGW turned on smart queueing");
			else if (wifi && !strncmp(p, "stay24", 6) && !memcmp(e->mac, mac, 6))
				snprintf(m->fix, sizeof(m->fix), "AIGW kept it on 2.4 GHz");
			else if (wifi && !strncmp(p, "meshsteer", 9) && !memcmp(e->mac, mac, 6))
				snprintf(m->fix, sizeof(m->fix), "AIGW moved it to a better access point");
		}
		if (m->fix[0])
			m->fix_t = (long)e->t;
	}
	if (m->fix[0] || now > m->end + FIX_S)
		m->fixed_looked = 1;
}

/* ---------- a session's window ---------- */

static void judge(struct se *s, long now, const struct net *nt)
{
	struct app_win w;
	struct device *d = s->mac[0] ? dev_find_mac(s->mac, 0) : dev_find_ip(s->ip, 0);
	int g = app_group_of(s->kind), loss = -1, rtt = -1, samples = 0, scored;
	struct day *dy;

	memset(&w, 0, sizeof(w));
	w.kind = s->kind;
	w.ms = s->w_ms ? s->w_ms : (uint32_t)(now - s->w_start) * 1000;
	w.looks = s->w_looks;
	w.active = s->w_active;
	if (w.ms) {
		w.down_kbps = (uint32_t)(s->w_db * 8 / w.ms);
		w.up_kbps = (uint32_t)(s->w_ub * 8 / w.ms);
		w.down_pps = (uint32_t)(s->w_dp * 1000 / w.ms);
	}
	w.ref_kbps = s->ref_kbps;
	w.ref_pps = s->ref_pps;
	w.gap_ms = s->w_gap;
	w.offline_s = s->w_offline;
	w.named = s->fam >= 0;
	w.class_conf = s->conf;
	w.rtt_ms = nt->rtt;
	w.rtt_usual_ms = nt->usual;
	w.jitter_ms = nt->jitter;
	w.loss10 = nt->loss10;
	w.dns_ms = nt->dns_ms;
	w.dns_fail_pct = nt->dns_fail;
	w.wifi_rtt_ms = w.wifi_loss_pct = -1;
	if (d && d->wifi_connected) {
		heal_device(d, &loss, &rtt, &samples);
		if (samples > 0) {
			w.wifi_loss_pct = loss;
			w.wifi_rtt_ms = rtt;
		}
	}
	w.drain_s = s->drain;
	w.counted_s = s->counted;
	app_quality(&w, &s->q);
	s->drain = w.drain_s;
	s->counted = w.counted_s;
	s->windows++;
	/* its usual: a stream's bitrate from the windows it wasn't fetching all the time (the mean then is what it plays),
	   a download's best rate, a cloud game's rate when it was fine */
	if (s->kind == AK_STREAM && s->windows > 1 && w.active * 100 < w.looks * 90)
		s->ref_kbps = s->ref_kbps ? (s->ref_kbps * 3 + w.down_kbps) / 4 : w.down_kbps;
	else if (s->kind == AK_DOWNLOAD && w.active >= w.looks && w.down_kbps > s->ref_kbps)
		s->ref_kbps = w.down_kbps;
	else if (s->kind == AK_CLOUDGAME && s->q.state == AQ_GOOD && w.active >= w.looks)
		s->ref_kbps = s->ref_kbps ? (s->ref_kbps * 3 + w.down_kbps) / 4 : w.down_kbps;
	/* scored once it has lasted apps.min_s, while it did something */
	scored = (w.active > 0 || w.offline_s > 0) && now - s->start >= cfg.apps_min_s && g >= 0;
	if (scored) {
		dy = today(now);
		s->minutes++;
		dy->min[g]++;
		if (s->q.state == AQ_BAD) {
			s->bad_min++;
			dy->bad[g]++;
		}
	}
	if (scored && s->q.state == AQ_BAD) {
		struct mo *m = mo_at(s->open);

		if (!m || m->closed) {
			s->open = mo_new(s, s->w_start);
			m = mo_at(s->open);
			m->what = (uint8_t)s->q.what;
			m->mos10 = s->q.mos10;
			s->moments++;
			if (d) {
				m->band = d->wifi_connected ? d->wifi_band : 0;
				m->signal = d->wifi_connected ? d->wifi_signal : 0;
			}
			m->wifi_loss = w.wifi_loss_pct;
			m->rtt = nt->rtt >= 0 ? nt->rtt + (w.wifi_rtt_ms > 0 ? w.wifi_rtt_ms : 0) : -1;
			m->rtt_usual = nt->usual;
			m->loss10 = nt->loss10;
			m->line_full = nt->full;
			if (nt->full && nt->top_kbps) {
				mac_str(nt->top, m->top, sizeof(m->top));
				if (!strcasecmp(m->top, s->mac))
					m->top[0] = '\0';       /* the session's own device filled the line */
				m->top_kbps = nt->top_kbps;
			}
		}
		/* the worst of it */
		if (s->q.what == AW_FREEZE || s->q.what == AW_REBUFFER || (m->what != AW_FREEZE && m->what != AW_REBUFFER))
			m->what = (uint8_t)s->q.what;
		if (s->q.mos10 && (!m->mos10 || s->q.mos10 < m->mos10))
			m->mos10 = s->q.mos10;
		m->seconds += s->q.seconds;
		if (s->q.conf > m->conf)
			m->conf = (uint8_t)s->q.conf;
		m->bad_min++;
		m->end = now;
		if (d && d->wifi_connected && d->wifi_signal && d->wifi_signal < m->signal)
			m->signal = d->wifi_signal;
		if (w.wifi_loss_pct > m->wifi_loss)
			m->wifi_loss = w.wifi_loss_pct;
		if (nt->full && !m->line_full) {
			m->line_full = 1;
			mac_str(nt->top, m->top, sizeof(m->top));
			if (!strcasecmp(m->top, s->mac))
				m->top[0] = '\0';
			m->top_kbps = nt->top_kbps;
		}
	} else if (s->open >= 0 && s->q.state != AQ_BAD) {
		mo_close(s->open);
		s->open = -1;
	}
	s->w_start = now;
	s->w_db = s->w_ub = s->w_dp = 0;
	s->w_ms = s->w_gap = 0;
	s->w_looks = s->w_active = s->w_offline = 0;
}

static void se_end(struct se *s)
{
	struct done *d;
	int g = app_group_of(s->kind);

	if (s->open >= 0) {
		mo_close(s->open);
		s->open = -1;
	}
	if (s->last - s->start >= cfg.apps_min_s && s->minutes) {
		d = &D[d_head];
		d_head = (d_head + 1) % DONE_MAX;
		if (d_n < DONE_MAX)
			d_n++;
		memset(d, 0, sizeof(*d));
		d->id = s->id;
		d->kind = s->kind;
		snprintf(d->app, sizeof(d->app), "%s", fam_name(s->fam));
		snprintf(d->mac, sizeof(d->mac), "%s", s->mac);
		d->start = s->start;
		d->end = s->last;
		d->minutes = s->minutes;
		d->bad_min = s->bad_min;
		d->moments = s->moments;
		total_sess++;
		if (g >= 0)
			today(s->last)->sess[g]++;
		AG_INFO("apps: %s on %s%s%s%s over: %d min, %d bad", app_kind_words(s->kind), who(s->mac, s->ip),
			s->fam >= 0 ? " (" : "", fam_name(s->fam), s->fam >= 0 ? ")" : "", s->minutes, s->bad_min);
	}
	s->used = 0;
}

/* every connection's counters seen: kinds, sessions, windows */
void appqoe_dump_done(void)
{
	uint64_t t0 = ag_mono_us();
	long now = (long)ag_wall_s();
	int i, w = win_s(), judged = 0;
	struct net nt;

	if (!cfg.apps || !inited)
		return;
	for (i = 0; i < SE_MAX; i++)
		S[i].l_db = S[i].l_ub = S[i].l_dp = 0, S[i].l_ms = 0, S[i].l_on = S[i].l_alive = 0;
	for (i = 0; i < FL_MAX; i++) {
		struct fl *f = &F[i];
		struct app_flow af;
		struct se *s;
		int k, c = 0;

		if (!f->id)
			continue;
		if (f->seen != look) {          /* gone from conntrack */
			fl_drop(f);
			continue;
		}
		if (!f->n)
			continue;
		memset(&af, 0, sizeof(af));
		af.proto = f->proto;
		af.port = f->port;
		af.age_s = (uint32_t)(now - f->first);
		af.fam = f->fam;
		af.iot = f->iot;
		af.n = f->n;
		memcpy(af.iv, f->iv, sizeof(af.iv));
		k = app_classify(&rules, &af, &c);
		if (f->kind == AK_NONE || k == f->kind) {
			if (k != AK_NONE)
				f->kind = (uint8_t)k, f->conf = (uint8_t)(c > f->conf ? c : f->conf);
			f->miss = 0;
		} else if (k != AK_NONE && (c > f->conf + 10 || ++f->miss >= 3)) {
			f->kind = (uint8_t)k;   /* it is something else now */
			f->conf = (uint8_t)c;
			f->miss = 0;
		} else if (k == AK_NONE && realtime(f->kind) && !on(f) && ++f->miss * cfg.apps_poll >= END_S)
			f->kind = AK_NONE;      /* a call that ended keeps its connection open for a while (a stall is shorter) */
		if (f->kind == AK_NONE || !(s = se_of(f->ip, f->kind, on(f))))
			continue;
		s->l_alive = 1;
		if (f->iv[f->n - 1].ms > s->l_ms)
			s->l_ms = f->iv[f->n - 1].ms;  /* a real-time one that went silent: a look of nothing (a stall?) */
		if (!on(f))
			continue;
		if (f->fam >= 0 && s->fam < 0 && app_family_has(&rules, f->fam, s->kind))
			s->fam = f->fam;
		if (f->conf > s->conf)
			s->conf = f->conf;
		if (f->first < s->start && f->kind == AK_STREAM)
			s->start = f->first;
		s->l_db += f->iv[f->n - 1].down_b;
		s->l_ub += f->iv[f->n - 1].up_b;
		s->l_dp += f->iv[f->n - 1].down_pk;
		s->l_on = 1;
	}
	/* browsing: a device opening web connections */
	for (i = 0; i < BR_MAX; i++) {
		struct se *s;

		if (!br[i].ip[0] || br[i].n < rules.browse_new || now - br[i].since >= w)
			continue;
		if ((s = se_of(br[i].ip, AK_BROWSE, 1)) && !s->l_on) {
			s->l_on = 1;
			s->l_ms = (uint32_t)cfg.apps_poll * 1000;
		}
	}
	nt.t = -1;
	for (i = 0; i < SE_MAX; i++) {
		struct se *s = &S[i];
		long idle = s->kind == AK_IOT ? IOT_S : realtime(s->kind) ? END_S : IDLE_S;

		if (!s->used)
			continue;
		if (s->kind == AK_IOT) {
			/* away: its connections gone, or off Wi-Fi; back: offline that long (a moment) */
			struct device *d = s->mac[0] ? dev_find_mac(s->mac, 0) : dev_find_ip(s->ip, 0);
			int alive = s->l_on || s->l_alive;

			if (d && d->wifi_connected)
				s->on_wifi = 1;
			if (s->on_wifi && d && !d->wifi_connected)
				alive = 0;
			if (alive) {
				if (s->gone_at)
					s->w_offline += (int)(now - s->gone_at);
				s->gone_at = 0;
				s->last = now;
			} else if (!s->gone_at)
				s->gone_at = now;
		}
		if (s->l_on) {
			s->last = now;
			s->w_looks++;
			s->w_active++;
			s->w_db += s->l_db;
			s->w_ub += s->l_ub;
			s->w_dp += s->l_dp;
			s->w_ms += s->l_ms;
			if (realtime(s->kind) && s->l_ms)
				look_rate(s, (uint32_t)(s->l_dp * 1000 / s->l_ms), s->l_ms, s->l_dp);
		} else {
			s->w_looks++;
			s->w_ms += s->l_ms ? s->l_ms : (uint32_t)cfg.apps_poll * 1000;
			if (realtime(s->kind) && s->l_alive && s->l_ms)
				look_rate(s, 0, s->l_ms, 0);    /* its connection still there, nothing on it */
		}
		if (now - s->last >= idle) {
			se_end(s);
			continue;
		}
		if (now - s->w_start >= w) {
			if (nt.t < 0)
				net_now(now - 2L * w, &nt);
			judge(s, now, &nt);
			judged++;
		}
	}
	if (judged)
		for (i = 0; i < (int)m_n; i++)
			mo_follow(&M[i], now);
	look++;
	cost_us = ag_mono_us() - t0;
	if (cost_us > cost_max_us)
		cost_max_us = cost_us;
}

/* ---------- what it says ---------- */

static int live_n(void)
{
	int i, n = 0;

	for (i = 0; i < SE_MAX; i++)
		n += S[i].used && S[i].minutes > 0;
	return n;
}

static void week(uint32_t *min, uint32_t *bad, uint32_t *sess)
{
	int32_t d = (int32_t)(ag_wall_s() / 86400);
	int i, g;

	for (g = 0; g < AGR_N; g++)
		min[g] = bad[g] = sess[g] = 0;
	for (i = 0; i < DAYS; i++)
		if (days[i].day && days[i].day > d - 7)
			for (g = 0; g < AGR_N; g++) {
				min[g] += days[i].min[g];
				bad[g] += days[i].bad[g];
				sess[g] += days[i].sess[g];
			}
}

int appqoe_score(int group)
{
	uint32_t min[AGR_N], bad[AGR_N], sess[AGR_N], m = 0, b = 0;
	int g;

	week(min, bad, sess);
	if (group >= 0 && group < AGR_N)
		return app_score(min[group], bad[group], MIN_SCORE);
	for (g = 0; g < AGR_N; g++)
		m += min[g], b += bad[g];
	return app_score(m, b, MIN_SCORE);
}

/* "calls 98%, streaming 100%, gaming 91%": the groups with a score; 0 none */
int appqoe_scores_words(char *buf, size_t n)
{
	size_t l = 0;
	int g, k = 0;

	buf[0] = '\0';
	for (g = 0; g < AGR_N; g++) {
		int s = appqoe_score(g);

		if (s < 0 || l >= n)
			continue;
		l += (size_t)snprintf(buf + l, n - l, "%s%s %d%%", k++ ? ", " : "", app_group_words(g), s);
	}
	return k;
}

static const char *status(void)
{
	if (!cfg.apps || !flow_active())
		return "Disabled";
	return total_sess || live_n() || total_mo ? "Active" : "Learning";
}

static void mo_blob(struct blob_buf *b, const struct mo *m, int names)
{
	char text[600];
	int ok, bad;

	blobmsg_add_u32(b, "moment", m->seq);
	blobmsg_add_u64(b, "time", (uint64_t)m->t);
	blobmsg_add_u64(b, "end", (uint64_t)m->end);
	blobmsg_add_string(b, "kind", app_kind_name(m->kind));
	blobmsg_add_string(b, "group", app_group_name(app_group_of(m->kind)));
	if (m->app[0] && names)
		blobmsg_add_string(b, "app", m->app);
	if (names) {
		blobmsg_add_string(b, "mac", m->mac);
		blobmsg_add_string(b, "device", who(m->mac, NULL));
	}
	blobmsg_add_string(b, "what", app_what_name(m->what));
	blobmsg_add_u32(b, "seconds", (uint32_t)m->seconds);
	blobmsg_add_u32(b, "confidence", m->conf);
	if (m->mos10)
		blobmsg_add_string(b, "mos", (snprintf(text, sizeof(text), "%d.%d", m->mos10 / 10, m->mos10 % 10), text));
	blobmsg_add_string(b, "cause", app_cause_name(m->cause));
	if (m->code[0])
		blobmsg_add_string(b, "code", m->code);
	if (m->fix[0]) {
		blobmsg_add_string(b, "fix", m->fix);
		blobmsg_add_u64(b, "fix_time", (uint64_t)m->fix_t);
	}
	since_of(m, &ok, &bad);
	blobmsg_add_u32(b, "since_fine", (uint32_t)ok);
	blobmsg_add_u32(b, "since_trouble", (uint32_t)bad);
	mo_text(m, names, text, sizeof(text));
	blobmsg_add_string(b, "summary", text);
}

static int mo_match(const struct mo *m, const char *mac, int group)
{
	return m->seq && m->closed && (!mac || !*mac || !strcasecmp(m->mac, mac)) &&
	       (group < 0 || app_group_of(m->kind) == group);
}

void appqoe_blob(struct blob_buf *b, const char *host, int group, int limit)
{
	uint32_t min[AGR_N], bad[AGR_N], sess[AGR_N];
	const struct device *hd = host && *host ? act_find_device(host) : NULL;
	const char *mac = hd ? hd->mac : NULL;
	char words[200];
	void *a, *t;
	int i, g, k;
	unsigned u;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.apps);
	blobmsg_add_u8(b, "names", (uint8_t)cfg.apps_names);
	blobmsg_add_u8(b, "shared", (uint8_t)cfg.apps_share);
	blobmsg_add_u8(b, "flows", (uint8_t)flow_active());
	blobmsg_add_string(b, "status", status());
	if (host && *host && !hd)
		blobmsg_add_string(b, "error", "no such device");
	t = blobmsg_open_table(b, "rules");
	blobmsg_add_string(b, "file", rules_builtin ? "" : rules_file);
	blobmsg_add_u32(b, "kinds", (uint32_t)(rules_n > 0 ? rules_n : 0));
	blobmsg_add_u32(b, "families", (uint32_t)rules.nfam);
	blobmsg_add_u32(b, "browse_new", (uint32_t)rules.browse_new);
	if (rules.err[0])
		blobmsg_add_string(b, "error", rules.err);
	blobmsg_close_table(b, t);
	t = blobmsg_open_table(b, "cost");
	for (i = 0, k = 0; i < FL_MAX; i++)
		k += F[i].id != 0;
	blobmsg_add_u32(b, "connections", (uint32_t)k);
	blobmsg_add_u32(b, "connections_max", FL_MAX);
	for (i = 0, k = 0; i < SE_MAX; i++)
		k += S[i].used;
	blobmsg_add_u32(b, "sessions", (uint32_t)k);
	blobmsg_add_u32(b, "ram_bytes", (uint32_t)(sizeof(F) + sizeof(fh) + sizeof(S) + sizeof(M) + sizeof(D) + sizeof(days) +
						    sizeof(br) + sizeof(rules)));
	blobmsg_add_u64(b, "look_us", cost_us);
	blobmsg_add_u64(b, "look_us_max", cost_max_us);
	blobmsg_close_table(b, t);
	week(min, bad, sess);
	a = blobmsg_open_array(b, "scores");
	for (g = 0; g < AGR_N; g++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "group", app_group_name(g));
		blobmsg_add_string(b, "words", app_group_words(g));
		blobmsg_add_u32(b, "score", (uint32_t)app_score(min[g], bad[g], MIN_SCORE));     /* -1: not yet */
		blobmsg_add_u32(b, "minutes", min[g]);
		blobmsg_add_u32(b, "bad_minutes", bad[g]);
		blobmsg_add_u32(b, "sessions", sess[g]);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	blobmsg_add_u32(b, "score", (uint32_t)appqoe_score(-1));
	if (appqoe_scores_words(words, sizeof(words)))
		blobmsg_add_string(b, "scores_words", words);
	a = blobmsg_open_array(b, "live");
	for (i = 0; i < SE_MAX; i++) {
		const struct se *s = &S[i];

		if (!s->used || (mac && strcasecmp(s->mac, mac)) || (group >= 0 && app_group_of(s->kind) != group))
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "kind", app_kind_name(s->kind));
		blobmsg_add_string(b, "words", app_kind_words(s->kind));
		if (s->fam >= 0)
			blobmsg_add_string(b, "app", fam_name(s->fam));
		blobmsg_add_string(b, "device", who(s->mac, s->ip));
		blobmsg_add_string(b, "mac", s->mac);
		blobmsg_add_u64(b, "since", (uint64_t)s->start);
		blobmsg_add_u32(b, "confidence", s->conf);
		blobmsg_add_u32(b, "minutes", (uint32_t)s->minutes);
		blobmsg_add_u32(b, "bad_minutes", (uint32_t)s->bad_min);
		if (s->windows) {
			blobmsg_add_string(b, "now", app_state_name(s->q.state));
			if (s->q.what)
				blobmsg_add_string(b, "what", app_what_name(s->q.what));
			if (s->q.mos10)
				blobmsg_add_string(b, "mos", (snprintf(words, sizeof(words), "%d.%d", s->q.mos10 / 10, s->q.mos10 % 10),
							     words));
			blobmsg_add_u32(b, "now_confidence", (uint32_t)s->q.conf);
		}
		if (s->ref_kbps)
			blobmsg_add_u32(b, "usual_kbps", s->ref_kbps);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "moments");
	for (u = 0, k = 0; u < m_n && k < limit; u++) {
		const struct mo *m = &M[(m_head + MO_MAX - 1 - u) % MO_MAX];

		if (!mo_match(m, mac, group))
			continue;
		t = blobmsg_open_table(b, NULL);
		mo_blob(b, m, 1);
		{
			char isp[600];

			mo_text(m, 0, isp, sizeof(isp));
			blobmsg_add_string(b, "isp_summary", isp);
		}
		blobmsg_close_table(b, t);
		k++;
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "sessions");
	for (u = 0, k = 0; u < d_n && k < limit; u++) {
		const struct done *d = &D[(d_head + DONE_MAX - 1 - u) % DONE_MAX];

		if ((mac && strcasecmp(d->mac, mac)) || (group >= 0 && app_group_of(d->kind) != group))
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "kind", app_kind_name(d->kind));
		blobmsg_add_string(b, "words", app_kind_words(d->kind));
		if (d->app[0])
			blobmsg_add_string(b, "app", d->app);
		blobmsg_add_string(b, "device", who(d->mac, NULL));
		blobmsg_add_string(b, "mac", d->mac);
		blobmsg_add_u64(b, "start", (uint64_t)d->start);
		blobmsg_add_u64(b, "end", (uint64_t)d->end);
		blobmsg_add_u32(b, "minutes", (uint32_t)d->minutes);
		blobmsg_add_u32(b, "bad_minutes", (uint32_t)d->bad_min);
		blobmsg_add_u32(b, "moments", (uint32_t)d->moments);
		blobmsg_close_table(b, t);
		k++;
	}
	blobmsg_close_array(b, a);
	blobmsg_add_u32(b, "sessions_total", total_sess);
	blobmsg_add_u32(b, "moments_total", total_mo);
}

/* the bridge to cortex (with apps.share): scores and counts, no names, no apps */
void appqoe_report_blob(struct blob_buf *b)
{
	uint32_t min[AGR_N], bad[AGR_N], sess[AGR_N];
	void *t, *s;
	int g;

	if (!cfg.apps || !cfg.apps_share)
		return;
	week(min, bad, sess);
	t = blobmsg_open_table(b, "apps");
	blobmsg_add_u32(b, "score", (uint32_t)appqoe_score(-1));
	blobmsg_add_u32(b, "moments_total", total_mo);
	s = blobmsg_open_table(b, "groups");
	for (g = 0; g < AGR_N; g++) {
		void *x = blobmsg_open_table(b, app_group_name(g));

		blobmsg_add_u32(b, "score", (uint32_t)app_score(min[g], bad[g], MIN_SCORE));
		blobmsg_add_u32(b, "minutes", min[g]);
		blobmsg_add_u32(b, "bad_minutes", bad[g]);
		blobmsg_close_table(b, x);
	}
	blobmsg_close_table(b, s);
	blobmsg_close_table(b, t);
}

/* ---------- the help desk ---------- */

int appqoe_brief(long from, long to, const char *mac, int group, int names, struct appqoe_brief *o)
{
	unsigned u;
	int g;

	memset(o, 0, sizeof(*o));
	if (!cfg.apps)
		return -1;
	o->group = group;
	for (u = 0; u < d_n; u++) {
		const struct done *d = &D[(d_head + DONE_MAX - 1 - u) % DONE_MAX];

		if (d->end < from || d->start >= to || (mac && *mac && strcasecmp(d->mac, mac)) ||
		    (group >= 0 && app_group_of(d->kind) != group))
			continue;
		o->sessions++;
		o->troubled += d->moments > 0;
	}
	for (u = 0; u < SE_MAX; u++) {
		const struct se *s = &S[u];

		if (!s->used || !s->minutes || s->last < from || s->start >= to || (mac && *mac && strcasecmp(s->mac, mac)) ||
		    (group >= 0 && app_group_of(s->kind) != group))
			continue;
		o->sessions++;
		o->troubled += s->moments > 0;
	}
	for (u = 0; u < m_n; u++) {
		const struct mo *m = &M[(m_head + MO_MAX - 1 - u) % MO_MAX];

		if (!mo_match(m, mac, group) || m->end < from || m->t >= to)
			continue;
		o->moments++;
		o->far_end += m->cause == AC_FAR_END;
		o->home += app_cause_home(m->cause);
		if (o->nline < APPQOE_BRIEF_LINES) {
			mo_text(m, names, o->line[o->nline], sizeof(o->line[0]));
			o->t[o->nline] = m->t;
			if (!o->worst_code[0] && m->code[0] && m->cause != AC_FAR_END && m->cause != AC_UNKNOWN) {
				snprintf(o->worst_code, sizeof(o->worst_code), "%s", m->code);
				o->worst_since = m->t;
				o->worst_until = m->end;
				o->worst = o->nline;
			}
			o->nline++;
		}
	}
	g = appqoe_scores_words(o->scores, sizeof(o->scores));
	(void)g;
	return 0;
}

/* ---------- the data model (Device.X_AIGW_.AppExperience.) ---------- */

static int shown(void)
{
	return cfg.apps && cfg.apps_share;
}

int appqoe_field(const char *f, char *buf, size_t n)
{
	int sh = shown();

	if (!strcmp(f, "Enable"))
		snprintf(buf, n, "%s", cfg.apps ? "true" : "false");
	else if (!strcmp(f, "ServerNames"))
		snprintf(buf, n, "%s", cfg.apps && cfg.apps_names ? "true" : "false");
	else if (!strcmp(f, "Shared"))
		snprintf(buf, n, "%s", cfg.apps_share ? "true" : "false");
	else if (!strcmp(f, "Status"))
		snprintf(buf, n, "%s", !cfg.apps || !flow_active() ? "Disabled" : sh ? status() : "NotShared");
	else if (!strcmp(f, "ActiveSessions"))
		snprintf(buf, n, "%d", sh ? live_n() : 0);
	else if (!strcmp(f, "Sessions"))
		snprintf(buf, n, "%u", sh ? total_sess : 0);
	else if (!strcmp(f, "BadMoments"))
		snprintf(buf, n, "%u", sh ? total_mo : 0);
	else if (!strcmp(f, "Score"))
		snprintf(buf, n, "%d", sh ? appqoe_score(-1) : -1);
	else if (!strcmp(f, "LastMoment")) {
		long t = 0;
		unsigned u;

		for (u = 0; sh && u < m_n && !t; u++) {
			const struct mo *m = &M[(m_head + MO_MAX - 1 - u) % MO_MAX];

			if (m->seq && m->closed)
				t = m->t;
		}
		snprintf(buf, n, "%ld", t);
	} else
		return -1;
	return 0;
}

int appqoe_kind_ids(uint32_t *out, int max)
{
	int g, n = 0;

	for (g = 0; shown() && g < AGR_N && n < max; g++)
		out[n++] = (uint32_t)g + 1;
	return n;
}

int appqoe_kind_field(uint32_t inst, const char *f, char *buf, size_t n)
{
	uint32_t min[AGR_N], bad[AGR_N], sess[AGR_N];
	int g = (int)inst - 1;

	if (!shown() || g < 0 || g >= AGR_N)
		return -1;
	week(min, bad, sess);
	if (!strcmp(f, "Name"))
		snprintf(buf, n, "%s", app_group_name(g));
	else if (!strcmp(f, "Score"))
		snprintf(buf, n, "%d", app_score(min[g], bad[g], MIN_SCORE));
	else if (!strcmp(f, "Minutes"))
		snprintf(buf, n, "%u", min[g]);
	else if (!strcmp(f, "BadMinutes"))
		snprintf(buf, n, "%u", bad[g]);
	else if (!strcmp(f, "Sessions"))
		snprintf(buf, n, "%u", sess[g]);
	else
		return -1;
	return 0;
}

int appqoe_moment_ids(uint32_t *out, int max)
{
	unsigned u;
	int n = 0;

	for (u = 0; shown() && u < m_n && n < max; u++) {
		const struct mo *m = &M[(m_head + MO_MAX - m_n + u) % MO_MAX];   /* oldest first */

		if (m->seq && m->closed)
			out[n++] = m->seq;
	}
	return n;
}

int appqoe_moment_field(uint32_t inst, const char *f, char *buf, size_t n)
{
	const struct mo *m = NULL;
	unsigned u;

	for (u = 0; shown() && u < m_n && !m; u++)
		if (M[u].seq == inst && M[u].closed)
			m = &M[u];
	if (!m)
		return -1;
	if (!strcmp(f, "Time"))
		snprintf(buf, n, "%ld", m->t);
	else if (!strcmp(f, "Kind"))
		snprintf(buf, n, "%s", app_kind_name(m->kind));
	else if (!strcmp(f, "What"))
		snprintf(buf, n, "%s", app_what_name(m->what));
	else if (!strcmp(f, "Seconds"))
		snprintf(buf, n, "%d", m->seconds);
	else if (!strcmp(f, "Confidence"))
		snprintf(buf, n, "%u", m->conf);
	else if (!strcmp(f, "Cause"))
		snprintf(buf, n, "%s", app_cause_name(m->cause));
	else if (!strcmp(f, "CauseCode"))
		snprintf(buf, n, "%s", m->code);
	else if (!strcmp(f, "Fixed"))
		snprintf(buf, n, "%s", m->fix[0] ? "true" : "false");
	else if (!strcmp(f, "FixTime"))
		snprintf(buf, n, "%ld", m->fix_t);
	else if (!strcmp(f, "Summary"))
		mo_text(m, 0, buf, n);
	else
		return -1;
	return 0;
}

/* ---------- saved with agd's state ---------- */

void appqoe_save(struct blob_buf *b)
{
	void *t, *a, *e;
	unsigned u;
	int i, g;

	if (!total_sess && !total_mo && !m_n && !d_n)
		return;
	t = blobmsg_open_table(b, "apps");
	blobmsg_add_u32(b, "seq_mo", seq_mo);
	blobmsg_add_u32(b, "seq_se", seq_se);
	blobmsg_add_u32(b, "sessions", total_sess);
	blobmsg_add_u32(b, "moments_total", total_mo);
	a = blobmsg_open_array(b, "days");
	for (i = 0; i < DAYS; i++) {
		if (!days[i].day)
			continue;
		e = blobmsg_open_array(b, NULL);
		blobmsg_add_u32(b, NULL, (uint32_t)days[i].day);
		for (g = 0; g < AGR_N; g++) {
			blobmsg_add_u32(b, NULL, days[i].min[g]);
			blobmsg_add_u32(b, NULL, days[i].bad[g]);
			blobmsg_add_u32(b, NULL, days[i].sess[g]);
		}
		blobmsg_close_array(b, e);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "moments");
	for (u = 0; u < m_n; u++) {
		const struct mo *m = &M[(m_head + MO_MAX - m_n + u) % MO_MAX];

		if (!m->seq || !m->closed)
			continue;
		e = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "seq", m->seq);
		blobmsg_add_u32(b, "sess", m->sess);
		blobmsg_add_u32(b, "kind", m->kind);
		blobmsg_add_u32(b, "what", m->what);
		blobmsg_add_u32(b, "conf", m->conf);
		blobmsg_add_u32(b, "cause", m->cause);
		blobmsg_add_u32(b, "looked", m->fixed_looked);
		blobmsg_add_string(b, "app", m->app);
		blobmsg_add_string(b, "mac", m->mac);
		blobmsg_add_string(b, "code", m->code);
		blobmsg_add_string(b, "top", m->top);
		blobmsg_add_string(b, "fix", m->fix);
		blobmsg_add_u64(b, "t", (uint64_t)m->t);
		blobmsg_add_u64(b, "end", (uint64_t)m->end);
		blobmsg_add_u64(b, "fix_t", (uint64_t)m->fix_t);
		blobmsg_add_u32(b, "seconds", (uint32_t)m->seconds);
		blobmsg_add_u32(b, "mos", (uint32_t)m->mos10);
		blobmsg_add_u32(b, "bad_min", (uint32_t)m->bad_min);
		blobmsg_add_u32(b, "recorded", (uint32_t)m->recorded);
		blobmsg_add_u32(b, "band", (uint32_t)m->band);
		blobmsg_add_u32(b, "signal", (uint32_t)-m->signal);
		blobmsg_add_u32(b, "wifi_loss", (uint32_t)(m->wifi_loss + 1));
		blobmsg_add_u32(b, "full", (uint32_t)m->line_full);
		blobmsg_add_u32(b, "rtt", (uint32_t)(m->rtt + 1));
		blobmsg_add_u32(b, "usual", (uint32_t)(m->rtt_usual + 1));
		blobmsg_add_u32(b, "loss10", (uint32_t)(m->loss10 + 1));
		blobmsg_add_u32(b, "top_kbps", m->top_kbps);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "done");
	for (u = 0; u < d_n; u++) {
		const struct done *d = &D[(d_head + DONE_MAX - d_n + u) % DONE_MAX];

		e = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "id", d->id);
		blobmsg_add_u32(b, "kind", d->kind);
		blobmsg_add_string(b, "app", d->app);
		blobmsg_add_string(b, "mac", d->mac);
		blobmsg_add_u64(b, "start", (uint64_t)d->start);
		blobmsg_add_u64(b, "end", (uint64_t)d->end);
		blobmsg_add_u32(b, "minutes", (uint32_t)d->minutes);
		blobmsg_add_u32(b, "bad_min", (uint32_t)d->bad_min);
		blobmsg_add_u32(b, "moments", (uint32_t)d->moments);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, a);
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

static void str_of(struct blob_attr *c, char *dst, size_t n)
{
	snprintf(dst, n, "%s", blobmsg_type(c) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(c) : "");
}

void appqoe_load(struct blob_attr *t)
{
	struct blob_attr *c, *x, *y;
	size_t rem, r2, r3;

	memset(days, 0, sizeof(days));
	memset(M, 0, sizeof(M));
	memset(D, 0, sizeof(D));
	m_head = m_n = d_head = d_n = 0;
	blobmsg_for_each_attr(c, t, rem) {
		const char *k = blobmsg_name(c);

		if (!strcmp(k, "seq_mo")) seq_mo = (uint32_t)jnum(c);
		else if (!strcmp(k, "seq_se")) seq_se = (uint32_t)jnum(c);
		else if (!strcmp(k, "sessions")) total_sess = (uint32_t)jnum(c);
		else if (!strcmp(k, "moments_total")) total_mo = (uint32_t)jnum(c);
		else if (!strcmp(k, "days") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY) {
			int i = 0;

			blobmsg_for_each_attr(x, c, r2) {
				uint32_t v[1 + 3 * AGR_N];
				int j = 0, g;

				if (i >= DAYS || blobmsg_type(x) != BLOBMSG_TYPE_ARRAY)
					continue;
				memset(v, 0, sizeof(v));
				blobmsg_for_each_attr(y, x, r3)
					if (j < (int)(sizeof(v) / sizeof(v[0])))
						v[j++] = (uint32_t)jnum(y);
				days[i].day = (int32_t)v[0];
				for (g = 0; g < AGR_N; g++) {
					days[i].min[g] = v[1 + 3 * g];
					days[i].bad[g] = v[2 + 3 * g];
					days[i].sess[g] = v[3 + 3 * g];
				}
				i++;
			}
		} else if (!strcmp(k, "moments") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(x, c, r2) {
				struct mo *m = &M[m_head];

				memset(m, 0, sizeof(*m));
				blobmsg_for_each_attr(y, x, r3) {
					const char *n = blobmsg_name(y);
					long v = jnum(y);

					if (!strcmp(n, "seq")) m->seq = (uint32_t)v;
					else if (!strcmp(n, "sess")) m->sess = (uint32_t)v;
					else if (!strcmp(n, "kind")) m->kind = (uint8_t)v;
					else if (!strcmp(n, "what")) m->what = (uint8_t)v;
					else if (!strcmp(n, "conf")) m->conf = (uint8_t)v;
					else if (!strcmp(n, "cause")) m->cause = (uint8_t)v;
					else if (!strcmp(n, "looked")) m->fixed_looked = (uint8_t)v;
					else if (!strcmp(n, "app")) str_of(y, m->app, sizeof(m->app));
					else if (!strcmp(n, "mac")) str_of(y, m->mac, sizeof(m->mac));
					else if (!strcmp(n, "code")) str_of(y, m->code, sizeof(m->code));
					else if (!strcmp(n, "top")) str_of(y, m->top, sizeof(m->top));
					else if (!strcmp(n, "fix")) str_of(y, m->fix, sizeof(m->fix));
					else if (!strcmp(n, "t")) m->t = v;
					else if (!strcmp(n, "end")) m->end = v;
					else if (!strcmp(n, "fix_t")) m->fix_t = v;
					else if (!strcmp(n, "seconds")) m->seconds = (int)v;
					else if (!strcmp(n, "mos")) m->mos10 = (int)v;
					else if (!strcmp(n, "bad_min")) m->bad_min = (int)v;
					else if (!strcmp(n, "recorded")) m->recorded = (int)v;
					else if (!strcmp(n, "band")) m->band = (int)v;
					else if (!strcmp(n, "signal")) m->signal = -(int)v;
					else if (!strcmp(n, "wifi_loss")) m->wifi_loss = (int)v - 1;
					else if (!strcmp(n, "full")) m->line_full = (int)v;
					else if (!strcmp(n, "rtt")) m->rtt = (int)v - 1;
					else if (!strcmp(n, "usual")) m->rtt_usual = (int)v - 1;
					else if (!strcmp(n, "loss10")) m->loss10 = (int)v - 1;
					else if (!strcmp(n, "top_kbps")) m->top_kbps = (uint32_t)v;
				}
				if (!m->seq || m->kind <= AK_NONE || m->kind >= AK_N)
					continue;
				m->closed = 1;
				m_head = (m_head + 1) % MO_MAX;
				if (m_n < MO_MAX)
					m_n++;
			}
		} else if (!strcmp(k, "done") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(x, c, r2) {
				struct done *d = &D[d_head];

				memset(d, 0, sizeof(*d));
				blobmsg_for_each_attr(y, x, r3) {
					const char *n = blobmsg_name(y);
					long v = jnum(y);

					if (!strcmp(n, "id")) d->id = (uint32_t)v;
					else if (!strcmp(n, "kind")) d->kind = (uint8_t)v;
					else if (!strcmp(n, "app")) str_of(y, d->app, sizeof(d->app));
					else if (!strcmp(n, "mac")) str_of(y, d->mac, sizeof(d->mac));
					else if (!strcmp(n, "start")) d->start = v;
					else if (!strcmp(n, "end")) d->end = v;
					else if (!strcmp(n, "minutes")) d->minutes = (int)v;
					else if (!strcmp(n, "bad_min")) d->bad_min = (int)v;
					else if (!strcmp(n, "moments")) d->moments = (int)v;
				}
				if (d->kind <= AK_NONE || d->kind >= AK_N)
					continue;
				d_head = (d_head + 1) % DONE_MAX;
				if (d_n < DONE_MAX)
					d_n++;
			}
		}
	}
}

void appqoe_reload(void)
{
	rules_load();
	AG_INFO("apps: %d rules, %d families (%s)", rules_n, rules.nfam, rules_builtin ? "built in" : rules_file);
}

void appqoe_init(void)
{
	int i;

	for (i = 0; i < FL_HASH; i++)
		fh[i] = -1;
	for (i = 0; i < FL_MAX; i++)
		F[i].next = -1;
	inited = 1;
	if (!cfg.apps)
		return;
	rules_load();
	AG_INFO("apps: per-app experience (a look every %d s; %d rules, %d families from %s%s%s)", cfg.apps_poll, rules_n,
		rules.nfam, rules_builtin ? "the built-in rules" : rules_file,
		cfg.apps_names ? "" : "; server names not used: apps.names 0", cfg.apps_share ? "" : "; kept home: apps.share 0");
	if (!flow_active())
		AG_INFO("apps: the flow sensor is off: no sessions noticed");
}
