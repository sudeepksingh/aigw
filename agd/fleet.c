/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fleet learning ("every home teaches the next"): what a fix did in other
 * homes helps this one choose, and a new gateway warns from its first day.
 * It extends federated learning (fed.c): the same bridge and cortex, the same
 * signed-update path, and only coarse, noised numbers leave the home.
 *
 *   outcome â”€â”€â”€â”€ act.c (a trial kept or tried, a person's undo), proactive.c
 *      â”‚         (fixed by AIGW, cleared by itself, raised), proven self-repair's
 *      â”‚         receipts: outcome_report()
 *      â–¼
 *   cells â”€â”€â”€â”€â”€â”€ counts per fault x action x context bucket, kept here (state)
 *      â”‚
 *   summary â”€â”€â”€â”€ fleet.share 1 only (opt-in): each count capped (fleet.cap) and
 *      â”‚         noised (fleet.noise, the same noise for the same count, so
 *      â”‚         sending it again tells nothing new), plus the learned normal's
 *      â”‚         shape per hour band; through the bridge (capability fleet.share)
 *      â–¼
 *   cortex â”€â”€â”€â”€â”€ adds up the homes (fleetlearn.py), keeps buckets of k homes or
 *      â”‚         more, signs "fleet-priors" and publishes it like a model update
 *      â–¼
 *   priors â”€â”€â”€â”€â”€ models.c checks the signature and the file, then fleet_priors_use;
 *                buckets of fewer than fleet.k homes are not used here either
 *
 * How they are used: proactive care's choice of fix (fleet_choose: the default
 * fix or waiting first, a Bayesian blend of the fleet's and the home's own
 * outcomes, local evidence dominating as it grows) and early warning's normal
 * while the gateway still learns its own (fleet_care_prior). Neither acts by
 * itself: a fix still goes through act.c, act.mode and the policies.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/types.h>
#include <time.h>

#include "agd.h"
#include "ag/care.h"
#include "ag/fleet.h"
#include "ag/sign.h"

#define CELLS     160
#define SEEN      96
#define CHOICES   8
#define WAITS     8
#define MIN_EVIDENCE 5          /* tries (fleet's and here) behind a candidate before it can change a choice */

struct cell {
	char code[FL_CODE], action[FL_ACTION], key[FL_KEY];
	struct fl_counts c;
	int64_t last;
};

static struct cell cells[CELLS];
static int n_cells;
static uint32_t n_outcomes;

/* the last outcomes by key: a later report for the same key replaces the earlier */
static struct seen {
	uint32_t key;
	int cell, result;
} seen[SEEN];
static unsigned seen_head;

static struct fl_priors *priors;
static int64_t priors_at;

static uint8_t seed[32];                /* the noise's secret: the same count gets the same noise */
static int seeded;

static struct {
	int64_t at;
	int cells, normals;
} shared;

static struct choice {
	int64_t at;
	char code[FL_CODE], dflt[FL_ACTION], chosen[FL_ACTION];
	char why[360];
} choices[CHOICES];
static uint32_t n_choices, n_changed, n_ring;      /* totals; the ring's entries */

static struct wait {
	uint32_t ticket;
	char code[FL_CODE], action[FL_ACTION];
} waits[WAITS];

static int learning_now(void)
{
#if AIGW_FEATURE_CARE
	return care_warning_learning();
#else
	return 0;
#endif
}

/* ---------- context ---------- */

static void put(struct fl_ctx *c, int d, const char *v)
{
	snprintf(c->f[d], sizeof(c->f[d]), "%s", v && *v ? v : "-");
}

static void ctx_of(const char *mac, const char *iface, struct fl_ctx *c)
{
	const struct device *d = mac && *mac ? dev_find_mac(mac, 0) : NULL;
	const struct wifi_ap *ap;
	const struct heal_radio *r;
	time_t now = time(NULL);
	struct tm tm;

	memset(c, 0, sizeof(*c));
	if ((!iface || !*iface) && d)
		iface = d->wifi_iface;
	ap = iface && *iface ? wifi_ap_find(iface) : NULL;
	put(c, 0, ap ? (ap->band == 2 ? "2.4" : ap->band == 5 ? "5" : "6") : NULL);
	put(c, 1, ap && ap->width ? (ap->width <= 20 ? "20" : ap->width <= 40 ? "40" : "80+") : NULL);
	r = ap ? heal_for(ap->iface) : NULL;
	put(c, 2, r && r->measured ? (r->loss_pct < 2 ? "low" : r->loss_pct < 10 ? "mid" : "high") : NULL);
	put(c, 3, d ? fl_device_class(d->type) : NULL);
	localtime_r(&now, &tm);
	put(c, 4, fl_hour_band_name(fl_hour_band(tm.tm_hour)));
	put(c, 5, cfg.fleet_line);
}

static void ctx_of_key(const char *key, struct fl_ctx *c)
{
	const char *p = key;
	int d;

	memset(c, 0, sizeof(*c));
	for (d = 0; d < FL_DIMS && p; d++) {
		size_t w = strcspn(p, "/");

		snprintf(c->f[d], sizeof(c->f[d]), "%.*s", (int)w, p);
		p = p[w] ? p + w + 1 : NULL;
	}
}

/* ---------- outcomes ---------- */

static int cell_get(const char *code, const char *action, const char *key)
{
	int i, old = 0;

	for (i = 0; i < n_cells; i++)
		if (!strcmp(cells[i].code, code) && !strcmp(cells[i].action, action) && !strcmp(cells[i].key, key))
			return i;
	if (n_cells < CELLS) {
		i = n_cells++;
	} else {                                /* the one not seen longest goes */
		for (i = 1; i < CELLS; i++)
			if (cells[i].last < cells[old].last)
				old = i;
		i = old;
		for (old = 0; old < SEEN; old++)
			if (seen[old].key && seen[old].cell == i)
				seen[old].key = 0;
	}
	memset(&cells[i], 0, sizeof(cells[i]));
	snprintf(cells[i].code, sizeof(cells[i].code), "%s", code);
	snprintf(cells[i].action, sizeof(cells[i].action), "%s", action);
	snprintf(cells[i].key, sizeof(cells[i].key), "%s", key);
	return i;
}

static void wait_drop(uint32_t ticket)
{
	int i;

	for (i = 0; i < WAITS; i++)
		if (waits[i].ticket == ticket)
			waits[i].ticket = 0;
}

void outcome_report(const char *code, const char *action, int result, uint32_t key, const char *mac, const char *iface)
{
	struct fl_ctx c;
	char k[FL_KEY];
	int i, ci;

	if (!cfg.fleet || !fl_word_ok(code, FL_CODE) || !fl_word_ok(action, FL_ACTION) || result < 0 || result >= FL_RESULTS)
		return;
	ctx_of(mac, iface, &c);
	fl_key(&c, 0, k, sizeof(k));
	if (key & OUTCOME_TICKET)
		wait_drop(key & ~OUTCOME_TICKET);
	for (i = 0; key && i < SEEN; i++)
		if (seen[i].key == key) {
			/* the same fix again (a receipt after the trial's own verdict): it replaces that one */
			fl_count(&cells[seen[i].cell].c, seen[i].result, -1);
			seen[i].key = 0;
		}
	ci = cell_get(code, action, k);
	fl_count(&cells[ci].c, result, 1);
	cells[ci].last = ag_wall_s();
	n_outcomes++;
	if (key) {
		seen[seen_head].key = key;
		seen[seen_head].cell = ci;
		seen[seen_head].result = result;
		seen_head = (seen_head + 1) % SEEN;
	}
	AG_INFO("fleet: %s for %s: %s (%s)", action, code, fl_result_name(result), k);
}

/* the home's own counts for this context: the most specific level it has any for */
static void local_counts(const char *code, const char *action, const struct fl_ctx *c, struct fl_counts *out)
{
	char want[FL_KEY], have[FL_KEY];
	struct fl_ctx x;
	int l, i;

	for (l = 0; l < FL_LEVELS; l++) {
		memset(out, 0, sizeof(*out));
		fl_key(c, l, want, sizeof(want));
		for (i = 0; i < n_cells; i++) {
			if (strcmp(cells[i].code, code) || strcmp(cells[i].action, action))
				continue;
			ctx_of_key(cells[i].key, &x);
			fl_key(&x, l, have, sizeof(have));
			if (!strcmp(have, want)) {
				out->tried += cells[i].c.tried;
				out->helped += cells[i].c.helped;
				out->worse += cells[i].c.worse;
				out->undone += cells[i].c.undone;
			}
		}
		if (out->tried)
			return;
	}
}

static void blend_of(const char *code, const char *action, const struct fl_ctx *c, struct fl_blend *b)
{
	struct fl_counts local;
	int level;
	const struct fl_prior *p = fl_prior_find(priors, code, action, c, cfg.fleet_k, &level);

	local_counts(code, action, c, &local);
	fl_blend(p ? &p->c : NULL, p ? p->homes : 0, level, &local, cfg.fleet_strength, b);
}

/* ---------- choosing ---------- */

static const char *action_words(const char *a)
{
	if (!strcmp(a, "wait")) return "waiting (it often clears by itself)";
	if (!strcmp(a, "stay24")) return "keeping the device on 2.4 GHz";
	if (!strcmp(a, "sqm")) return "smart queueing";
	if (!strcmp(a, "channel")) return "another channel";
	return a;
}

static void tell_choice(const struct choice *ch, int changed)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", "choice");
	blobmsg_add_string(&b, "code", ch->code);
	blobmsg_add_string(&b, "default", ch->dflt);
	blobmsg_add_string(&b, "chosen", ch->chosen);
	blobmsg_add_u8(&b, "changed", (uint8_t)changed);
	blobmsg_add_string(&b, "why", ch->why);
	agd_emit(AG_EV_FLEET, &b);
	blob_buf_free(&b);
}

int fleet_choose(const char *code, const char *dflt, const char *mac, uint32_t ticket, char *why, size_t wn)
{
	struct fl_cand cand[2];
	struct choice *ch;
	struct fl_ctx c;
	char a[160], w[160];
	int pick, i;

	if (why && wn)
		why[0] = '\0';
	if (!cfg.fleet || !cfg.fleet_choose || !priors || !dflt || !strcmp(dflt, "wait"))
		return 0;
	memset(cand, 0, sizeof(cand));
	cand[0].action = dflt;
	cand[1].action = "wait";
	ctx_of(mac, NULL, &c);
	for (i = 0; i < 2; i++)
		blend_of(code, cand[i].action, &c, &cand[i].b);
	if (cand[0].b.level < 0 && cand[1].b.level < 0)
		return 0;                       /* the fleet knows nothing of it: as before */
	pick = fl_choose(cand, 2, cfg.fleet_margin, MIN_EVIDENCE);
	fl_blend_words(&cand[0].b, a, sizeof(a));
	fl_blend_words(&cand[1].b, w, sizeof(w));
	ch = &choices[n_ring++ % CHOICES];
	memset(ch, 0, sizeof(*ch));
	ch->at = ag_wall_s();
	snprintf(ch->code, sizeof(ch->code), "%s", code);
	snprintf(ch->dflt, sizeof(ch->dflt), "%s", dflt);
	snprintf(ch->chosen, sizeof(ch->chosen), "%s", cand[pick].action);
	snprintf(ch->why, sizeof(ch->why), "%s for %s: %s %s; %s %s.%s", pick ? "Waited first" : "The usual fix", code,
		 action_words(dflt), a, action_words("wait"), w,
		 pick ? " The fix is tried if it hasn't cleared within the grace time." : "");
	n_choices++;
	if (why && wn)
		snprintf(why, wn, "Fleet learning: %s", ch->why);
	AG_INFO("fleet: %s", ch->why);
	tell_choice(ch, pick != 0);
	if (!pick)
		return 0;
	n_changed++;
	for (i = 0; i < WAITS && waits[i].ticket; i++)
		;
	if (i == WAITS)
		i = (int)(n_changed % WAITS);
	waits[i].ticket = ticket;
	snprintf(waits[i].code, sizeof(waits[i].code), "%s", code);
	snprintf(waits[i].action, sizeof(waits[i].action), "%s", dflt);
	dm_on_fleet_choice(code, dflt, "wait", ch->why);
	return 1;
}

int fleet_wait_over(uint32_t ticket, char *action, size_t an)
{
	int i;

	for (i = 0; ticket && i < WAITS; i++)
		if (waits[i].ticket == ticket) {
			snprintf(action, an, "%s", waits[i].action);
			waits[i].ticket = 0;
			return 1;
		}
	return 0;
}

void fleet_rank_blob(struct blob_buf *b, const char *code, const char *mac)
{
	static const char *const acts[] = { "wait", "stay24", "sqm", "channel", "meshsteer", "meshchan" };
	struct fl_ctx c;
	char key[FL_KEY], words[200];
	unsigned i;
	void *arr;

	ctx_of(mac, NULL, &c);
	fl_key(&c, 0, key, sizeof(key));
	blobmsg_add_string(b, "code", code);
	blobmsg_add_string(b, "context", key);
	arr = blobmsg_open_array(b, "actions");
	for (i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
		struct fl_blend r;
		void *t;

		blend_of(code, acts[i], &c, &r);
		if (r.level < 0 && !r.local_tried)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "action", acts[i]);
		blobmsg_add_double(b, "helped", r.helped);
		blobmsg_add_double(b, "worse", r.bad);
		blobmsg_add_double(b, "score", r.score);
		blobmsg_add_double(b, "fleet_share", r.fleet_share);
		blobmsg_add_u32(b, "fleet_tried", r.fleet_tried);
		blobmsg_add_u32(b, "fleet_homes", r.fleet_homes);
		blobmsg_add_u32(b, "level", (uint32_t)(r.level < 0 ? 0 : r.level));
		blobmsg_add_u8(b, "from_fleet", r.level >= 0);
		blobmsg_add_u32(b, "local_tried", r.local_tried);
		blobmsg_add_u32(b, "local_helped", r.local_helped);
		fl_blend_words(&r, words, sizeof(words));
		blobmsg_add_string(b, "words", words);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

/* ---------- cold start: the fleet's normal ---------- */

const struct care_prior *fleet_care_prior(void)
{
	static struct care_prior cp;
	int m, band, any = 0;

	if (!cfg.fleet || !priors || !priors->nn)
		return NULL;
	memset(&cp, 0, sizeof(cp));
	for (m = 0; m < CARE_METRICS; m++) {
		int have = 0;
		float sm = 0, sd = 0;

		for (band = 0; band < 4; band++) {
			const struct fl_norm *q = fl_norm_find(priors, cfg.fleet_line, m, band, cfg.fleet_k);

			if (!q)
				continue;
			cp.mean[m][band] = q->mean;
			cp.dev[m][band] = q->dev;
			sm += q->mean;
			sd += q->dev;
			have |= 1 << band;
		}
		if (!have)
			continue;
		/* an hour band the fleet has nothing for: the others' average */
		for (band = 0; band < 4; band++)
			if (!(have & (1 << band))) {
				int k = __builtin_popcount((unsigned)have);

				cp.mean[m][band] = sm / (float)k;
				cp.dev[m][band] = sd / (float)k;
			}
		cp.have[m] = 1;
		any = 1;
	}
	return any ? &cp : NULL;
}

/* ---------- the priors (models.c slot "fleet-priors") ---------- */

void *fleet_priors_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en)
{
	struct fl_priors *P = fl_priors_parse((const char *)data, len, err, en);

	if (P)
		snprintf(name, nn, "fleet-priors");
	return P;
}

int fleet_priors_check(const void *o, char *err, size_t n)
{
	const struct fl_priors *P = o;
	int i;

	/* what the aggregator must never publish: a bucket of fewer homes than its own k */
	for (i = 0; i < P->n; i++)
		if (P->p[i].homes < (uint32_t)P->k) {
			snprintf(err, n, "has a bucket of %u home%s, fewer than its k (%d): refused", P->p[i].homes,
				 P->p[i].homes == 1 ? "" : "s", P->k);
			return -1;
		}
	for (i = 0; i < P->nn; i++)
		if (P->nm[i].homes < (uint32_t)P->k) {
			snprintf(err, n, "has a normal of fewer homes than its k (%d): refused", P->k);
			return -1;
		}
	return 0;
}

void fleet_priors_free(void *o) { fl_priors_free(o); }
int fleet_priors_size(const void *o) { const struct fl_priors *P = o; return P->n + P->nn; }
int fleet_priors_version(const void *o, char *date, size_t n)
{
	const struct fl_priors *P = o;

	if (date && n)
		snprintf(date, n, "%s", P->published);
	return P->version;
}

void fleet_priors_use(void *o)
{
	struct fl_priors *old = priors;
	struct blob_buf b = { 0 };

	priors = o;
	priors_at = ag_wall_s();
	if (old && old != o)
		fl_priors_free(old);
	if (!priors)
		return;
	AG_INFO("fleet: priors version %d in use: %d bucket%s, %d normal%s, from %u homes (k %d; used here at %d or more)",
		priors->version, priors->n, priors->n == 1 ? "" : "s", priors->nn, priors->nn == 1 ? "" : "s", priors->homes,
		priors->k, cfg.fleet_k);
	if (!agd_connected())
		return;                 /* loaded at start (models_init): nobody to tell yet */
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", "priors");
	blobmsg_add_u32(&b, "version", (uint32_t)priors->version);
	blobmsg_add_u32(&b, "buckets", (uint32_t)priors->n);
	blobmsg_add_u32(&b, "normals", (uint32_t)priors->nn);
	blobmsg_add_u32(&b, "homes", priors->homes);
	agd_emit(AG_EV_FLEET, &b);
	blob_buf_free(&b);
	dm_on_fleet_priors(priors->version, priors->n, priors->nn, priors->homes);
}

/* ---------- the summary: what leaves the home ---------- */

/* the same noise for the same count of the same bucket: sending it again (cumulative counts, every
   interval) tells the aggregator nothing new, so averaging many summaries doesn't remove it */
static double noise_for(const struct cell *c, const char *field, uint32_t v)
{
	uint8_t msg[160], h[64];
	uint64_t a, b;
	int n;
	double u1, u2;

	if (cfg.fleet_noise <= 0)
		return 0;
	if (!seeded) {
		if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed))
			return 0;
		seeded = 1;
	}
	memcpy(msg, seed, sizeof(seed));
	n = snprintf((char *)msg + sizeof(seed), sizeof(msg) - sizeof(seed), "%s|%s|%s|%s|%u", c->code, c->action, c->key,
		     field, v);
	ag_sha512(msg, sizeof(seed) + (size_t)(n < 0 ? 0 : n), h);
	memcpy(&a, h, 8);
	memcpy(&b, h + 8, 8);
	u1 = ((double)(a >> 11) + 1) / 9007199254740993.0;
	u2 = (double)(b >> 11) / 9007199254740992.0;
	return cfg.fleet_noise * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static uint32_t shared_count(const struct cell *c, const char *field, uint32_t v, uint32_t max)
{
	double x = (v > (uint32_t)cfg.fleet_cap ? (double)cfg.fleet_cap : (double)v) + noise_for(c, field, v);
	long r = lround(x);

	if (r < 0)
		r = 0;
	return (uint32_t)r > max ? max : (uint32_t)r;
}

int fleet_summary(struct blob_buf *b, char *err, size_t n)
{
	int64_t now = ag_wall_s();
	int i, m, band, nc = 0, nn = 0;
	void *arr, *row;

	if (!cfg.fleet || !cfg.fleet_share) {
		snprintf(err, n, "%s", !cfg.fleet ? "fleet learning is off here (fleet.enabled)" :
			 "this home doesn't share (fleet.share 0, the default: sharing is opt-in)");
		return -1;
	}
	if (shared.at && now - shared.at < cfg.fleet_interval && now >= shared.at) {
		snprintf(err, n, "next summary in %lld s (fleet.interval)", (long long)(cfg.fleet_interval - (now - shared.at)));
		return -1;
	}
	blobmsg_add_u32(b, "v", 1);
	blobmsg_add_string(b, "line", cfg.fleet_line[0] ? cfg.fleet_line : "-");
	/* [code, action, bucket, tried, helped, worse, undone]: nothing else about the home */
	arr = blobmsg_open_array(b, "outcomes");
	for (i = 0; i < n_cells; i++) {
		const struct cell *c = &cells[i];
		uint32_t t = shared_count(c, "tried", c->c.tried, (uint32_t)cfg.fleet_cap), h, w, u;

		if (!c->c.tried || !t)
			continue;
		h = shared_count(c, "helped", c->c.helped, t);
		w = shared_count(c, "worse", c->c.worse, t - h);
		u = shared_count(c, "undone", c->c.undone, t - h - w);
		row = blobmsg_open_array(b, NULL);
		blobmsg_add_string(b, NULL, c->code);
		blobmsg_add_string(b, NULL, c->action);
		blobmsg_add_string(b, NULL, c->key);
		blobmsg_add_u32(b, NULL, t);
		blobmsg_add_u32(b, NULL, h);
		blobmsg_add_u32(b, NULL, w);
		blobmsg_add_u32(b, NULL, u);
		blobmsg_close_array(b, row);
		nc++;
	}
	blobmsg_close_array(b, arr);
	/* [metric, hour band, normal, typical deviation], rounded: only once this home's own is learned */
	arr = blobmsg_open_array(b, "normals");
	for (m = 0; m < CARE_METRICS; m++)
		for (band = 0; band < 4; band++) {
			float mean, dev;
			double q = m == CM_LOSS ? 10 : 1;

			if (care_normal_band(m, band, &mean, &dev) < 0)
				continue;
			if (m == CM_SPEED)          /* two significant figures: a plan's speed, not the line's own */
				q = mean >= 100 ? 0.1 : mean >= 10 ? 1 : 10;
			row = blobmsg_open_array(b, NULL);
			blobmsg_add_u32(b, NULL, (uint32_t)m);
			blobmsg_add_u32(b, NULL, (uint32_t)band);
			blobmsg_add_double(b, NULL, round(mean * q) / q);
			blobmsg_add_double(b, NULL, round(dev * q) / q);
			blobmsg_close_array(b, row);
			nn++;
		}
	blobmsg_close_array(b, arr);
	shared.at = now;
	shared.cells = nc;
	shared.normals = nn;
	AG_INFO("fleet: summary shared: %d bucket%s of outcomes, %d normal%s (capped at %d, noise %.2f)", nc,
		nc == 1 ? "" : "s", nn, nn == 1 ? "" : "s", cfg.fleet_cap, cfg.fleet_noise);
	return 0;
}

/* ---------- status, the data model, settings ---------- */

void fleet_blob(struct blob_buf *b, int full)
{
	void *t, *arr;
	int i;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.fleet);
	blobmsg_add_u8(b, "share", (uint8_t)cfg.fleet_share);
	blobmsg_add_u32(b, "k", (uint32_t)cfg.fleet_k);
	blobmsg_add_u32(b, "interval", (uint32_t)cfg.fleet_interval);
	blobmsg_add_string(b, "line", cfg.fleet_line);
	blobmsg_add_double(b, "noise", cfg.fleet_noise);
	blobmsg_add_u32(b, "cap", (uint32_t)cfg.fleet_cap);
	blobmsg_add_double(b, "strength", cfg.fleet_strength);
	blobmsg_add_u8(b, "choose", (uint8_t)cfg.fleet_choose);
	blobmsg_add_u32(b, "outcomes", n_outcomes);
	blobmsg_add_u32(b, "cells", (uint32_t)n_cells);
	t = blobmsg_open_table(b, "shared");
	blobmsg_add_u64(b, "at", (uint64_t)shared.at);
	blobmsg_add_u32(b, "buckets", (uint32_t)shared.cells);
	blobmsg_add_u32(b, "normals", (uint32_t)shared.normals);
	blobmsg_close_table(b, t);
	t = blobmsg_open_table(b, "received");
	blobmsg_add_u8(b, "loaded", priors != NULL);
	if (priors) {
		blobmsg_add_u32(b, "version", (uint32_t)priors->version);
		blobmsg_add_string(b, "published", priors->published);
		blobmsg_add_u32(b, "homes", priors->homes);
		blobmsg_add_u32(b, "k", (uint32_t)priors->k);
		blobmsg_add_u32(b, "buckets", (uint32_t)priors->n);
		blobmsg_add_u32(b, "normals", (uint32_t)priors->nn);
		blobmsg_add_u64(b, "at", (uint64_t)priors_at);
	}
	blobmsg_add_u8(b, "cold_start", fleet_care_prior() != NULL && learning_now());
	blobmsg_close_table(b, t);
	blobmsg_add_u32(b, "choices", n_choices);
	blobmsg_add_u32(b, "changed", n_changed);
	arr = blobmsg_open_array(b, "last_choices");
	for (i = 0; i < CHOICES && (uint32_t)i < n_ring; i++) {
		const struct choice *ch = &choices[(n_ring - 1 - (uint32_t)i) % CHOICES];

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_u64(b, "at", (uint64_t)ch->at);
		blobmsg_add_string(b, "code", ch->code);
		blobmsg_add_string(b, "default", ch->dflt);
		blobmsg_add_string(b, "chosen", ch->chosen);
		blobmsg_add_u8(b, "changed", (uint8_t)(strcmp(ch->dflt, ch->chosen) != 0));
		blobmsg_add_string(b, "why", ch->why);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
	if (!full)
		return;
	arr = blobmsg_open_array(b, "local");
	for (i = 0; i < n_cells; i++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "code", cells[i].code);
		blobmsg_add_string(b, "action", cells[i].action);
		blobmsg_add_string(b, "bucket", cells[i].key);
		blobmsg_add_u32(b, "tried", cells[i].c.tried);
		blobmsg_add_u32(b, "helped", cells[i].c.helped);
		blobmsg_add_u32(b, "worse", cells[i].c.worse);
		blobmsg_add_u32(b, "undone", cells[i].c.undone);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

int fleet_field(const char *f, char *buf, size_t n)
{
	const struct choice *ch = n_ring ? &choices[(n_ring - 1) % CHOICES] : NULL;

#define B(x) snprintf(buf, n, "%s", (x) ? "true" : "false")
	if (!strcmp(f, "Enable")) B(cfg.fleet);
	else if (!strcmp(f, "Share")) B(cfg.fleet_share);
	else if (!strcmp(f, "K")) snprintf(buf, n, "%d", cfg.fleet_k);
	else if (!strcmp(f, "UploadInterval")) snprintf(buf, n, "%d", cfg.fleet_interval);
	else if (!strcmp(f, "LineType")) snprintf(buf, n, "%s", cfg.fleet_line);
	else if (!strcmp(f, "OutcomesRecorded")) snprintf(buf, n, "%u", n_outcomes);
	else if (!strcmp(f, "LastShared")) snprintf(buf, n, "%lld", (long long)shared.at);
	else if (!strcmp(f, "PriorsVersion")) snprintf(buf, n, "%d", priors ? priors->version : 0);
	else if (!strcmp(f, "PriorsHomes")) snprintf(buf, n, "%u", priors ? priors->homes : 0);
	else if (!strcmp(f, "PriorsBuckets")) snprintf(buf, n, "%d", priors ? priors->n : 0);
	else if (!strcmp(f, "PriorsReceived")) snprintf(buf, n, "%lld", (long long)(priors ? priors_at : 0));
	else if (!strcmp(f, "ColdStart")) B(fleet_care_prior() != NULL && learning_now());
	else if (!strcmp(f, "ChoicesMade")) snprintf(buf, n, "%u", n_choices);
	else if (!strcmp(f, "ChoicesChanged")) snprintf(buf, n, "%u", n_changed);
	else if (!strcmp(f, "LastChoice")) snprintf(buf, n, "%s", ch ? ch->why : "");
	else return -1;
#undef B
	return 0;
}

static int boolv(const char *v)
{
	return !strcmp(v, "1") || !strcmp(v, "true") ? 1 : !strcmp(v, "0") || !strcmp(v, "false") ? 0 : -1;
}

int fleet_set(const char *f, const char *v, int apply, char *err, size_t n)
{
	const char *opt = NULL;
	char val[16];
	long x = strtol(v, NULL, 10);

	snprintf(val, sizeof(val), "%s", v);
	if (!strcmp(f, "Enable") || !strcmp(f, "Share")) {
		if (boolv(v) < 0) {
			snprintf(err, n, "%s is true or false", f);
			return -1;
		}
		snprintf(val, sizeof(val), "%d", boolv(v));
		opt = f[0] == 'E' ? "enabled" : "share";
	} else if (!strcmp(f, "K")) {
		if (x < 2 || x > 10000) {
			snprintf(err, n, "K is 2 .. 10000 (one home's counts are never a prior)");
			return -1;
		}
		opt = "k";
	} else if (!strcmp(f, "UploadInterval")) {
		if (x < 60 || x > 30 * 86400) {
			snprintf(err, n, "UploadInterval is 60 .. 2592000 s");
			return -1;
		}
		opt = "interval";
	} else if (!strcmp(f, "LineType")) {
		if (!ag_fleet_line_ok(v)) {
			snprintf(err, n, "LineType is fibre, dsl, cable, fwa, satellite, other or empty");
			return -1;
		}
		opt = "line";
	} else {
		snprintf(err, n, "%s is not writable", f);
		return -1;
	}
	if (!apply)
		return 0;
	if (!strcmp(opt, "enabled")) cfg.fleet = atoi(val);
	else if (!strcmp(opt, "share")) cfg.fleet_share = atoi(val);
	else if (!strcmp(opt, "k")) cfg.fleet_k = (int)x;
	else if (!strcmp(opt, "interval")) cfg.fleet_interval = (int)x;
	else snprintf(cfg.fleet_line, sizeof(cfg.fleet_line), "%s", v);
	if (ag_config_ensure("fleet") < 0 || ag_config_set("fleet", opt, val) < 0)
		AG_WARN("fleet: %s set to %s, but not saved in the config", opt, val);
	return 0;
}

/* tests only (AIGW_TEST_OUTCOMES): an outcome as if a fix had been judged */
int fleet_test_outcome(struct blob_attr *msg, char *err, size_t n)
{
	const char *code = NULL, *action = NULL, *mac = "", *iface = "";
	struct blob_attr *cur;
	int result = -1, times = 1;
	uint32_t key = 0;
	size_t rem;

	if (!getenv("AIGW_TEST_OUTCOMES")) {
		snprintf(err, n, "only in tests (AIGW_TEST_OUTCOMES)");
		return -1;
	}
	blobmsg_for_each_attr(cur, msg, rem) {
		const char *k = blobmsg_name(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			const char *s = blobmsg_get_string(cur);

			if (!strcmp(k, "code")) code = s;
			else if (!strcmp(k, "action")) action = s;
			else if (!strcmp(k, "mac")) mac = s;
			else if (!strcmp(k, "iface")) iface = s;
			else if (!strcmp(k, "result"))
				for (result = 0; result < FL_RESULTS && strcmp(fl_result_name(result), s); result++)
					;
		} else if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32) {
			if (!strcmp(k, "times")) times = (int)blobmsg_get_u32(cur);
			else if (!strcmp(k, "key")) key = blobmsg_get_u32(cur);
		}
	}
	if (!code || !action || result < 0 || result >= FL_RESULTS || times < 1 || times > 1000) {
		snprintf(err, n, "needs code, action, result (helped, no-change, worse, undone)");
		return -1;
	}
	while (times--)
		outcome_report(code, action, result, key, mac, iface);
	return 0;
}

/* ---------- state ---------- */

void fleet_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "fleet"), *arr, *row;
	char hex[2 * sizeof(seed) + 1];
	int i;

	if (seeded) {
		for (i = 0; i < (int)sizeof(seed); i++)
			snprintf(hex + 2 * i, 3, "%02x", seed[i]);
		blobmsg_add_string(b, "seed", hex);
	}
	blobmsg_add_u32(b, "outcomes", n_outcomes);
	blobmsg_add_u64(b, "shared_at", (uint64_t)shared.at);
	blobmsg_add_u32(b, "choices", n_choices);
	blobmsg_add_u32(b, "changed", n_changed);
	arr = blobmsg_open_array(b, "cells");
	for (i = 0; i < n_cells; i++) {
		row = blobmsg_open_array(b, NULL);
		blobmsg_add_string(b, NULL, cells[i].code);
		blobmsg_add_string(b, NULL, cells[i].action);
		blobmsg_add_string(b, NULL, cells[i].key);
		blobmsg_add_u32(b, NULL, cells[i].c.tried);
		blobmsg_add_u32(b, NULL, cells[i].c.helped);
		blobmsg_add_u32(b, NULL, cells[i].c.worse);
		blobmsg_add_u32(b, NULL, cells[i].c.undone);
		blobmsg_add_u64(b, NULL, (uint64_t)cells[i].last);
		blobmsg_close_array(b, row);
	}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

static uint64_t unum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (uint64_t)blobmsg_get_double(a);
	default: return 0;
	}
}

void fleet_load(struct blob_attr *tbl)
{
	struct blob_attr *cur, *row, *v;
	size_t r1, r2, r3;

	blobmsg_for_each_attr(cur, tbl, r1) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "seed") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING &&
		    strlen(blobmsg_get_string(cur)) == 2 * sizeof(seed)) {
			const char *s = blobmsg_get_string(cur);
			unsigned i, x;

			for (i = 0; i < sizeof(seed) && sscanf(s + 2 * i, "%2x", &x) == 1; i++)
				seed[i] = (uint8_t)x;
			seeded = i == sizeof(seed);
		} else if (!strcmp(k, "outcomes")) {
			n_outcomes = (uint32_t)unum(cur);
		} else if (!strcmp(k, "shared_at")) {
			shared.at = (int64_t)unum(cur);
		} else if (!strcmp(k, "choices")) {
			n_choices = (uint32_t)unum(cur);
		} else if (!strcmp(k, "changed")) {
			n_changed = (uint32_t)unum(cur);
		} else if (!strcmp(k, "cells") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(row, cur, r2) {
				const char *s[3] = { 0 };
				uint64_t x[5] = { 0 };
				int j = 0;
				struct cell *c;

				blobmsg_for_each_attr(v, row, r3) {
					if (j < 3 && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
						s[j] = blobmsg_get_string(v);
					else if (j >= 3 && j < 8)
						x[j - 3] = unum(v);
					j++;
				}
				if (j != 8 || n_cells >= CELLS || !fl_word_ok(s[0], FL_CODE) || !fl_word_ok(s[1], FL_ACTION) ||
				    fl_key_level(s[2]) != 0)
					continue;
				c = &cells[n_cells];
				snprintf(c->code, sizeof(c->code), "%s", s[0]);
				snprintf(c->action, sizeof(c->action), "%s", s[1]);
				snprintf(c->key, sizeof(c->key), "%s", s[2]);
				c->c.tried = (uint32_t)x[0];
				c->c.helped = (uint32_t)x[1];
				c->c.worse = (uint32_t)x[2];
				c->c.undone = (uint32_t)x[3];
				c->last = (int64_t)x[4];
				if (fl_counts_ok(&c->c))
					n_cells++;
			}
		}
	}
	if (n_choices < n_changed)
		n_changed = n_choices;
	AG_INFO("fleet: restored %d bucket%s of outcomes (%u outcomes)", n_cells, n_cells == 1 ? "" : "s", n_outcomes);
}

void fleet_init(void)
{
	AG_INFO("fleet learning %s: %s; sharing %s (k %d, every %d s)", cfg.fleet ? "on" : "OFF (fleet.enabled)",
		"outcomes kept here, the fleet's priors used to choose fixes and for warnings while learning",
		cfg.fleet_share ? "on" : "off (fleet.share)", cfg.fleet_k, cfg.fleet_interval);
}
