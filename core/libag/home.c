/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW home Wi-Fi model: places, path loss, experience, problems, and what-if
 * predictions (see ag/home.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag/home.h"

#define NO_PROOF 127

void hm_params_default(struct hm_params *p)
{
	p->weak_dbm = -75;
	p->loss_pct = 10;
	p->same_db = 6;
	p->margin_db = 3;
	p->backhaul_dbm = -65;
	p->strong_dbm = -65;
	p->tx24 = 20;
	p->tx5 = 23;
	p->path_exp = 3.0f;
	p->up_db = 4;
	p->learn = 120;
	p->min_problem = 5;
	p->days = 30;
	p->interval_s = 60;
	p->decay = 43200;
}

void hm_init(struct hm_model *m)
{
	int i;

	memset(m, 0, sizeof(*m));
	m->ears = 1;
	m->chk_place = m->ext_place = -1;
	m->chk_measured = NO_PROOF;
	for (i = 0; i < HM_DEVS; i++)
		m->d[i].place = m->d[i].ep_place = -1;
}

const char *hm_kind_name(int k)
{
	static const char *const n[HO_N] = { "Extender", "GatewayUp", "GatewayCentral", "Band24", "Band5" };

	return k >= 0 && k < HO_N ? n[k] : "";
}

const char *hm_status_name(int s)
{
	return s == HS_READY ? "Ready" : "Learning";
}

const char *hm_confidence_name(int c)
{
	return c >= 70 ? "High" : c >= 45 ? "Medium" : "Low";
}

/* ---------- signatures ---------- */

static float absf(float x)
{
	return x < 0 ? -x : x;
}

/* mean difference where both are heard, and a penalty for an access point that hears one well and not the other */
static float dist(const float *a, const float *b, const struct hm_params *p)
{
	float sum = 0, pen = 0;
	int e, k = 0;

	for (e = 0; e < HM_EARS; e++)
		if (a[e] && b[e]) {
			sum += absf(a[e] - b[e]);
			k++;
		} else if ((a[e] && a[e] > (float)(p->weak_dbm + 5)) || (b[e] && b[e] > (float)(p->weak_dbm + 5)))
			pen += 4;
	return k ? sum / (float)k + pen : 99;
}

static void signature(const struct cov_sample *s, float *v)
{
	int e;

	memset(v, 0, sizeof(float) * HM_EARS);
	for (e = 0; e < HM_EARS; e++)
		if (s->heard[e] && e != s->ear)
			v[e] = (float)s->heard[e];
	if (s->ear >= 0 && s->ear < HM_EARS)
		v[s->ear] = (float)(s->band == 2 ? s->dbm - COV_OFF24 : s->dbm);
}

static int band_ix(int band)
{
	return band == 2 ? 0 : 1;
}

/* ---------- places ---------- */

static uint16_t sat16(uint32_t v)
{
	return v > 65535 ? 65535 : (uint16_t)v;
}

/* place j into place i (j is free afterwards) */
static void merge(struct hm_model *m, int i, int j)
{
	struct hm_place *a = &m->p[i], *b = &m->p[j];
	uint32_t n = a->n + b->n;
	int e, k, x;

	for (e = 0; e < HM_EARS; e++)
		if (a->sig[e] && b->sig[e])
			a->sig[e] = (a->sig[e] * (float)a->n + b->sig[e] * (float)b->n) / (float)(n ? n : 1);
		else if (!a->sig[e] && b->sig[e] && b->n > a->n)
			a->sig[e] = b->sig[e];
	a->spread = n ? (a->spread * (float)a->n + b->spread * (float)b->n) / (float)n : 0;
	a->n = n;
	for (k = 0; k < HM_DEVS; k++)
		a->dn[k] = sat16((uint32_t)a->dn[k] + b->dn[k]);
	for (k = 0; k < HM_PLACES; k++) {
		a->adj[k] = sat16((uint32_t)a->adj[k] + b->adj[k]);
		m->p[k].adj[i] = sat16((uint32_t)m->p[k].adj[i] + m->p[k].adj[j]);
		m->p[k].adj[j] = 0;
	}
	a->adj[i] = a->adj[j] = 0;
	for (x = 0; x < 2; x++)
		for (e = 0; e < HM_EARS; e++) {
			struct hm_cell *c = &a->c[x][e];
			const struct hm_cell *d = &b->c[x][e];

			c->n = sat16((uint32_t)c->n + d->n), c->lm = sat16((uint32_t)c->lm + d->lm);
			c->prob = sat16((uint32_t)c->prob + d->prob), c->rn = sat16((uint32_t)c->rn + d->rn);
			c->dbm += d->dbm, c->loss += d->loss, c->rate += d->rate;
		}
	if (!a->name[0])
		memcpy(a->name, b->name, sizeof(a->name));
	if (b->first && (!a->first || b->first < a->first))
		a->first = b->first;
	if (b->last > a->last)
		a->last = b->last;
	for (k = 0; k < HM_DEVS; k++) {
		if (m->d[k].place == j)
			m->d[k].place = (int8_t)i;
		if (m->d[k].ep_place == j)
			m->d[k].ep_place = (int8_t)i;
	}
	for (k = 0; k < HM_PROBS; k++)
		if (m->pr[k].t && m->pr[k].place == j)
			m->pr[k].place = (int8_t)i;
	if (m->chk_place == j)
		m->chk_place = (int8_t)i;
	if (m->ext_place == j)
		m->ext_place = (int8_t)i;
	memset(b, 0, sizeof(*b));
}

/* a free place: a new one, else the two closest merged (an unnamed one goes into the other) */
static int new_place(struct hm_model *m, const struct hm_params *p)
{
	float bd = 1e9f;
	int i, j, bi = -1, bj = -1;

	if (m->np < HM_PLACES)
		return m->np++;
	for (i = 0; i < m->np; i++)
		for (j = i + 1; j < m->np; j++) {
			float d = dist(m->p[i].sig, m->p[j].sig, p);

			if (m->p[i].name[0] && m->p[j].name[0])
				d += 50;        /* two places a person named stay apart, if anything else can merge */
			if (d < bd)
				bd = d, bi = i, bj = j;
		}
	if (m->p[bj].name[0] && !m->p[bi].name[0]) {
		int t = bi;

		bi = bj, bj = t;
	}
	merge(m, bi, bj);
	return bj;
}

static int nearest(const struct hm_model *m, const float *v, const struct hm_params *p, float *bd)
{
	int k, best = -1;

	*bd = 1e9f;
	for (k = 0; k < m->np; k++) {
		float d;

		if (!m->p[k].n)
			continue;
		d = dist(v, m->p[k].sig, p);
		if (d < *bd)
			*bd = d, best = k;
	}
	return best;
}

/* ---------- the history ---------- */

static void halve(struct hm_model *m)
{
	int k, x, e, j;

	m->looks /= 2;
	for (k = 0; k < m->np; k++) {
		struct hm_place *pl = &m->p[k];

		pl->n /= 2;
		if (!pl->n && pl->name[0])
			pl->n = 1;              /* a named place stays */
		for (j = 0; j < HM_DEVS; j++)
			pl->dn[j] /= 2;
		for (j = 0; j < HM_PLACES; j++)
			pl->adj[j] /= 2;
		for (x = 0; x < 2; x++)
			for (e = 0; e < HM_EARS; e++) {
				struct hm_cell *c = &pl->c[x][e];

				c->n /= 2, c->lm /= 2, c->prob /= 2, c->rn /= 2;
				c->dbm /= 2, c->loss /= 2, c->rate /= 2;
			}
	}
}

static void end_problem(struct hm_model *m, int di, const struct hm_params *p)
{
	struct hm_dev *d = &m->d[di];
	struct hm_prob *slot = NULL;
	int k;

	if (d->ep_n >= p->min_problem && d->ep_place >= 0) {
		for (k = 0; k < HM_PROBS; k++)
			if (!slot || !m->pr[k].t || (slot->t && m->pr[k].t < slot->t))
				slot = &m->pr[k];
		slot->t = d->ep_start;
		k = (int)d->ep_n * (p->interval_s > 0 ? p->interval_s : 60) / 60;
		slot->minutes = (uint16_t)(k < 1 ? 1 : k > 65535 ? 65535 : k);
		slot->place = d->ep_place;
		slot->dev = (int8_t)di;
		slot->band = d->ep_band;
		slot->ear = d->ep_ear;
		slot->kind = d->ep_kind;
		slot->dbm = (int8_t)(d->ep_dbm / (int32_t)d->ep_n);
	}
	d->ep_n = d->ep_miss = 0;
	d->ep_dbm = 0;
	d->ep_place = -1;
}

static int dev_slot(struct hm_model *m, const char *mac, const struct hm_params *p)
{
	int k, free_k = -1, old = -1;

	for (k = 0; k < HM_DEVS; k++) {
		if (!strcasecmp(m->d[k].mac, mac))
			return k;
		if (!m->d[k].mac[0] && free_k < 0)
			free_k = k;
		if (old < 0 || m->d[k].last < m->d[old].last)
			old = k;
	}
	if (free_k < 0) {
		int j;

		free_k = old;               /* the device not seen for longest gives way */
		end_problem(m, free_k, p);
		for (j = 0; j < HM_PLACES; j++)
			m->p[j].dn[free_k] = 0;
		for (j = 0; j < HM_PROBS; j++)
			if (m->pr[j].t && m->pr[j].dev == free_k)
				m->pr[j].dev = -1;
	}
	memset(&m->d[free_k], 0, sizeof(m->d[free_k]));
	snprintf(m->d[free_k].mac, sizeof(m->d[free_k].mac), "%s", mac);
	m->d[free_k].place = m->d[free_k].ep_place = -1;
	return free_k;
}

void hm_add(struct hm_model *m, const struct hm_look *l, const struct hm_params *p)
{
	const struct cov_sample *s = l->s;
	float v[HM_EARS], bd, best_heard = -127;
	struct hm_place *pl;
	struct hm_cell *c;
	struct hm_dev *d;
	int k, e, di, ears = 1, weak, problem, kind = 0, suffers;

	if (!s || !s->dbm || !l->mac || !*l->mac || s->ear < 0 || s->ear >= HM_EARS)
		return;
	if (m->looks >= p->decay)
		halve(m);
	m->looks++;

	/* the access points: a new one after an Extender prediction starts the proof */
	for (e = 0; e < HM_EARS; e++)
		if (e == s->ear || s->heard[e])
			ears = e + 1 > ears ? e + 1 : ears;
	if (ears > m->ears) {
		if (m->ext_place >= 0 && m->chk_place < 0 && m->p[m->ext_place].sig[0]) {
			m->chk_place = m->ext_place;
			m->chk_pred = m->ext_gain;
			m->chk_before = (int8_t)(m->p[m->ext_place].sig[0] - 0.5f);
			m->chk_measured = NO_PROOF;
			m->chk_at = s->t;
			m->p[m->ext_place].chk_sum = 0;
			m->p[m->ext_place].chk_n = 0;
		}
		m->ears = ears;
	}

	di = dev_slot(m, l->mac, p);
	d = &m->d[di];
	signature(s, v);
	k = nearest(m, v, p, &bd);
	if (k < 0 || bd > (float)p->same_db) {
		k = new_place(m, p);
		pl = &m->p[k];
		memset(pl, 0, sizeof(*pl));
		memcpy(pl->sig, v, sizeof(v));
		pl->first = s->t;
	}
	pl = &m->p[k];
	for (e = 0; e < HM_EARS; e++)
		if (v[e]) {
			float a = 1.0f / (float)(pl->n < 63 ? pl->n + 1 : 64);

			pl->sig[e] = pl->sig[e] ? pl->sig[e] + (v[e] - pl->sig[e]) * a : v[e];
		}
	if (pl->n) {
		float x = dist(v, pl->sig, p);

		pl->spread += ((x < 20 ? x : 20) - pl->spread) / (float)(pl->n < 63 ? pl->n + 1 : 64);
	}
	pl->n++;
	pl->last = s->t;
	pl->dn[di] = sat16((uint32_t)pl->dn[di] + 1);
	c = &pl->c[band_ix(s->band)][s->ear];
	c->n = sat16((uint32_t)c->n + 1);
	c->dbm += s->dbm;
	if (s->loss_pct >= 0)
		c->lm = sat16((uint32_t)c->lm + 1), c->loss += (uint32_t)s->loss_pct;
	if (s->rate_mbps > 0)
		c->rn = sat16((uint32_t)c->rn + 1), c->rate += (uint32_t)s->rate_mbps;

	/* neighbours: it went from there to here between two looks */
	if (d->place >= 0 && d->place != k && d->place < HM_PLACES && s->t - d->last <= 3L * (p->interval_s > 0 ? p->interval_s : 60)) {
		m->p[d->place].adj[k] = sat16((uint32_t)m->p[d->place].adj[k] + 1);
		pl->adj[d->place] = sat16((uint32_t)pl->adj[d->place] + 1);
	}
	d->place = (int8_t)k;
	d->last = s->t;
	d->dual |= l->dual || s->band == 5;

	/* the proof: the best signal at the place the extender was for */
	if (m->chk_place == k && m->chk_measured == NO_PROOF) {
		for (e = 0; e < HM_EARS; e++)
			if (v[e] && v[e] > best_heard)
				best_heard = v[e];
		pl->chk_sum += (int32_t)best_heard;
		if (++pl->chk_n >= 30) {
			int g = (int)(pl->chk_sum / pl->chk_n) - m->chk_before;

			m->chk_measured = (int8_t)(g > 100 ? 100 : g < -100 ? -100 : g);
		}
	}

	/* a problem: it suffers where it is weak, or loses a lot where the signal is strong */
	weak = s->dbm <= p->weak_dbm;
	suffers = s->loss_pct >= 0 ? s->loss_pct >= p->loss_pct : s->retry_pct >= 0 ? s->retry_pct >= 30 : 1;
	problem = !l->weak_tx && weak && suffers;
	if (!problem && !l->weak_tx && s->dbm >= p->strong_dbm && s->loss_pct >= p->loss_pct)
		problem = 1, kind = 1;
	if (problem) {
		c->prob = sat16((uint32_t)c->prob + 1);
		if (d->ep_n && (d->ep_kind != kind || d->ep_band != s->band))
			end_problem(m, di, p);
		if (!d->ep_n) {
			d->ep_place = (int8_t)k;
			d->ep_band = (int8_t)s->band;
			d->ep_ear = (int8_t)s->ear;
			d->ep_kind = (int8_t)kind;
			d->ep_start = s->t;
		}
		d->ep_n++;
		d->ep_dbm += s->dbm;
		d->ep_miss = 0;
	} else if (d->ep_n && ++d->ep_miss >= 2)
		end_problem(m, di, p);
}

void hm_expire(struct hm_model *m, long now, const struct hm_params *p)
{
	long gap = 3L * (p->interval_s > 0 ? p->interval_s : 60);
	int k;

	for (k = 0; k < HM_DEVS; k++)
		if (m->d[k].mac[0] && m->d[k].ep_n && now - m->d[k].last > gap)
			end_problem(m, k, p);
	for (k = 0; k < HM_PROBS; k++)
		if (m->pr[k].t && m->pr[k].t < now - (long)p->days * 86400)
			memset(&m->pr[k], 0, sizeof(m->pr[k]));
}

int hm_place_of(const struct hm_model *m, const char *mac)
{
	int k;

	for (k = 0; mac && k < HM_DEVS; k++)
		if (m->d[k].mac[0] && !strcasecmp(m->d[k].mac, mac))
			return m->d[k].place;
	return -1;
}

int hm_name(struct hm_model *m, int place, const char *name)
{
	int k;

	if (place < 0 || place >= m->np || !m->p[place].n)
		return -1;
	if (name && *name)
		for (k = 0; k < m->np; k++)     /* one place per name */
			if (k != place && !strcasecmp(m->p[k].name, name))
				m->p[k].name[0] = '\0';
	snprintf(m->p[place].name, sizeof(m->p[place].name), "%s", name ? name : "");
	return 0;
}

/* ---------- what is known of a place ---------- */

int hm_place_dbm(const struct hm_place *pl, int band, int ear)
{
	const struct hm_cell *c = &pl->c[band_ix(band)][ear >= 0 && ear < HM_EARS ? ear : 0];

	return c->n ? (int)(c->dbm / (int32_t)c->n) : 0;
}

int hm_offset24(const struct hm_place *pl, int *learned)
{
	const struct hm_cell *a = &pl->c[0][0], *b = &pl->c[1][0];
	int off;

	if (learned)
		*learned = 0;
	if (a->n < 10 || b->n < 10)
		return COV_OFF24;
	off = (int)(a->dbm / (int32_t)a->n) - (int)(b->dbm / (int32_t)b->n);
	if (learned)
		*learned = 1;
	return off < 0 ? 0 : off > 15 ? 15 : off;
}

/* the gateway's signal there in 5 GHz terms (else the best access point's); 0 not heard */
static float sig5(const struct hm_place *pl)
{
	return pl->sig[0];
}

int hm_path_loss(const struct hm_place *pl, int band, const struct hm_params *p)
{
	int dbm = hm_place_dbm(pl, band, 0), off = hm_offset24(pl, NULL);

	if (!dbm && sig5(pl))
		dbm = (int)(sig5(pl) - 0.5f) + (band == 2 ? off : 0);
	return dbm ? (band == 2 ? p->tx24 : p->tx5) - dbm : 0;
}

/* free-space loss at 1 m on 5 GHz (5.2 GHz: 46.8 dB) */
#define PL0_5 46.8f

float hm_distance(const struct hm_place *pl, const struct hm_params *p)
{
	float pl5 = sig5(pl) ? (float)p->tx5 - sig5(pl) : 0, d;

	if (!pl5)
		return 0;
	d = powf(10.0f, (pl5 - PL0_5) / (10.0f * p->path_exp));
	return d < 1.0f ? 1.0f : d;
}

/* what a device at distance-like d gains from an access point at distance-like a instead */
static float gain_at(float d, float a, const struct hm_params *p)
{
	return 10.0f * p->path_exp * log10f(d / (a < 1.5f ? 1.5f : a));
}

int hm_best_link(const struct hm_place *pl, int *ear)
{
	int x, e, bx = -1, be = -1;
	float best = 1e9f;

	for (x = 0; x < 2; x++)
		for (e = 0; e < HM_EARS; e++) {
			const struct hm_cell *c = &pl->c[x][e];
			float score;

			if (c->n < 5)
				continue;
			/* the share of looks with a problem first, then the loss, then the signal */
			score = (float)c->prob * 100.0f / (float)c->n + (c->lm ? (float)c->loss / (float)c->lm : 0) -
				(float)(c->dbm / (int32_t)c->n) / 100.0f;
			if (score < best)
				best = score, bx = x, be = e;
		}
	if (ear)
		*ear = be;
	return bx < 0 ? -1 : bx ? 5 : 2;
}

int hm_place_problems(const struct hm_model *m, int place, long now, const struct hm_params *p)
{
	int k, n = 0;

	for (k = 0; k < HM_PROBS; k++)
		n += m->pr[k].t && m->pr[k].t >= now - (long)p->days * 86400 && (place < 0 || m->pr[k].place == place);
	return n;
}

/* ---------- what if ---------- */

static int live(const struct hm_prob *q, long now, const struct hm_params *p)
{
	return q->t && q->t >= now - (long)p->days * 86400 && q->place >= 0;
}

static int fixed_by(int dbm, float gain, const struct hm_params *p)
{
	return (float)dbm + gain >= (float)(p->weak_dbm + p->margin_db);
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* how sure: the looks and problems at the place, how far its looks spread, a mesh */
static int confidence(const struct hm_model *m, int place, int base, int cap, long now, const struct hm_params *p)
{
	const struct hm_place *pl = place >= 0 ? &m->p[place] : NULL;
	int c = base, n = pl ? (int)pl->n : (int)m->looks, k = hm_place_problems(m, place, now, p);

	c += n >= p->learn ? 15 : n * 15 / (p->learn > 0 ? p->learn : 1);
	c += k >= 3 ? 10 : k * 3;
	if (pl)
		c -= clamp((int)(pl->spread * 2), 0, 15);
	return clamp(c, 5, cap);
}

/* an extender near place `at` (-1: about halfway to `to`): its gain at each place (0 none) */
static void extender_gains(const struct hm_model *m, int at, int to, const struct hm_params *p, float *g)
{
	float dto = hm_distance(&m->p[to], p), dx = at >= 0 ? hm_distance(&m->p[at], p) : dto / 2;
	int k;

	for (k = 0; k < m->np; k++) {
		float dk = hm_distance(&m->p[k], p), a;

		g[k] = 0;
		if (!dk || !m->p[k].n)
			continue;
		if (k == to || k == at || (at >= 0 && (m->p[at].adj[k] || m->p[k].adj[at])))
			a = k == at ? 1.5f : absf(dk - dx);
		else
			continue;       /* another direction, as far as AIGW can tell: no gain counted */
		if (dk > dx || k == at) {
			float x = gain_at(dk, a, p);

			g[k] = x > 0 ? x : 0;
		}
	}
}

static void count(const struct hm_model *m, const float *g, int kind_weak, long now, const struct hm_params *p,
		  int *fixed)
{
	int k;

	*fixed = 0;
	for (k = 0; k < HM_PROBS; k++) {
		const struct hm_prob *q = &m->pr[k];

		if (live(q, now, p) && (!kind_weak || q->kind == 0) && g[q->place] > 0 && fixed_by(q->dbm, g[q->place], p))
			(*fixed)++;
	}
}

static void add_option(struct hm_result *r, const struct hm_option *o)
{
	int k, w = -1;

	if (o->fixed <= 0)
		return;
	if (r->nopt < HM_OPTS) {
		r->o[r->nopt++] = *o;
		return;
	}
	for (k = 0; k < HM_OPTS; k++)
		if (w < 0 || r->o[k].fixed < r->o[w].fixed)
			w = k;
	if (r->o[w].fixed < o->fixed)
		r->o[w] = *o;
}

static int better(const struct hm_option *a, const struct hm_option *b)
{
	if (a->fixed != b->fixed)
		return a->fixed > b->fixed;
	if (a->broken != b->broken)
		return a->broken < b->broken;
	/* before you buy: the free one first, when AIGW is fairly sure of both */
	if (a->buy != b->buy && a->confidence >= 45 && b->confidence >= 45)
		return a->buy < b->buy;
	return a->confidence > b->confidence;
}

static void opt_extender(const struct hm_model *m, long now, const struct hm_params *p, struct hm_result *r)
{
	struct hm_option best = { 0 }, o;
	float g[HM_PLACES];
	int to, at;

	best.fixed = -1;
	for (to = 0; to < m->np; to++) {
		float dto = hm_distance(&m->p[to], p);

		if (!dto || !hm_place_problems(m, to, now, p))
			continue;
		for (at = -1; at < m->np; at++) {
			int bh;

			if (at == to)
				continue;
			if (at >= 0) {
				const struct hm_place *x = &m->p[at];

				if (x->n < 10 || !sig5(x) || sig5(x) < (float)p->backhaul_dbm || hm_distance(x, p) >= dto)
					continue;
				bh = (int)(sig5(x) - 0.5f);
			} else
				bh = (int)(sig5(&m->p[to]) + gain_at(dto, dto / 2, p) - 0.5f);
			extender_gains(m, at, to, p, g);
			memset(&o, 0, sizeof(o));
			o.kind = HO_EXTENDER;
			o.place = to;
			o.at = at;
			o.buy = 1;
			o.gain_db = (int)(g[to] + 0.5f);
			o.plus_minus = 3 + (int)(m->p[to].spread + 0.5f);
			o.backhaul_dbm = bh;
			count(m, g, 1, now, p, &o.fixed);
			o.confidence = confidence(m, to, at >= 0 ? 35 : 30, 90, now, p) +
				       (at >= 0 && (m->p[at].adj[to] || m->p[to].adj[at]) ? 15 : 0) + (m->ears > 1 ? 10 : 0) -
				       (bh < p->backhaul_dbm ? 15 : 0);
			o.confidence = clamp(o.confidence, 5, 90);
			if (o.fixed > 0 && (best.fixed < 0 || better(&o, &best) ||
					    (o.fixed == best.fixed && o.gain_db > best.gain_db)))
				best = o;
		}
	}
	if (best.fixed > 0)
		add_option(r, &best);
}

static void opt_gateway(const struct hm_model *m, long now, const struct hm_params *p, struct hm_result *r)
{
	struct hm_option o;
	float g[HM_PLACES], far = 0, near = 1e9f;
	int k, top = -1, n = 0;

	/* higher, in the open: a few dB everywhere */
	memset(&o, 0, sizeof(o));
	memset(g, 0, sizeof(g));
	for (k = 0; k < m->np; k++)
		g[k] = m->p[k].n ? (float)p->up_db : 0;
	o.kind = HO_GATEWAY_UP;
	o.place = r->target;
	o.at = -1;
	o.gain_db = p->up_db;
	o.plus_minus = 2;
	count(m, g, 1, now, p, &o.fixed);
	o.confidence = confidence(m, -1, 15, 35, now, p);
	add_option(r, &o);

	/* toward the middle: the far end gains, places near it now lose */
	for (k = 0; k < m->np; k++) {
		float d = hm_distance(&m->p[k], p);

		if (!d || m->p[k].n < 10)
			continue;
		n++;
		if (d > far)
			far = d;
		if (d < near)
			near = d;
	}
	if (n < 2 || far < 3 * near)
		return;                 /* not lopsided: the gateway is not at one end */
	memset(&o, 0, sizeof(o));
	o.kind = HO_GATEWAY_CENTRAL;
	o.at = -1;
	for (k = 0; k < m->np; k++) {
		float d = hm_distance(&m->p[k], p), now5 = sig5(&m->p[k]);

		g[k] = 0;
		if (!d)
			continue;
		g[k] = gain_at(d, absf(d - far / 2), p);
		/* fine now, weak after: devices that live there would suffer */
		if (g[k] < 0 && m->p[k].n >= 10 && now5 > (float)p->weak_dbm && now5 + g[k] <= (float)p->weak_dbm)
			o.broken++;
		if (top < 0 || hm_place_problems(m, k, now, p) > hm_place_problems(m, top, now, p))
			top = k;
	}
	o.place = top;
	o.gain_db = top >= 0 ? (int)(g[top] + 0.5f) : 0;
	o.plus_minus = 3 + (top >= 0 ? (int)(m->p[top].spread + 0.5f) : 0);
	count(m, g, 1, now, p, &o.fixed);
	o.confidence = confidence(m, top, 10, 35, now, p);
	add_option(r, &o);
}

static void opt_bands(const struct hm_model *m, long now, const struct hm_params *p, struct hm_result *r)
{
	int fx24[HM_PLACES] = { 0 }, fx5[HM_PLACES] = { 0 }, k, b24 = -1, b5 = -1, n24 = 0, n5 = 0;
	struct hm_option o;

	for (k = 0; k < HM_PROBS; k++) {
		const struct hm_prob *q = &m->pr[k];
		int off;

		if (!live(q, now, p))
			continue;
		off = hm_offset24(&m->p[q->place], NULL);
		if (q->band == 5 && q->kind == 0 && fixed_by(q->dbm, (float)off, p))
			fx24[q->place]++, n24++;
		/* a crowded 2.4 GHz: 5 GHz must be heard well, and the device able to use it */
		if (q->band == 2 && q->kind == 1 && q->dev >= 0 && m->d[q->dev].dual &&
		    q->dbm - off >= p->weak_dbm + p->margin_db + 5)
			fx5[q->place]++, n5++;
	}
	for (k = 0; k < m->np; k++) {
		if (fx24[k] && (b24 < 0 || fx24[k] > fx24[b24]))
			b24 = k;
		if (fx5[k] && (b5 < 0 || fx5[k] > fx5[b5]))
			b5 = k;
	}
	if (b24 >= 0) {
		int learned;

		memset(&o, 0, sizeof(o));
		o.kind = HO_BAND24;
		o.place = b24;
		o.at = -1;
		o.gain_db = hm_offset24(&m->p[b24], &learned);
		o.plus_minus = learned ? 2 : 4;
		o.fixed = n24;
		o.confidence = confidence(m, b24, learned ? 55 : 35, 90, now, p);
		add_option(r, &o);
	}
	if (b5 >= 0) {
		int learned;

		memset(&o, 0, sizeof(o));
		o.kind = HO_BAND5;
		o.place = b5;
		o.at = -1;
		o.gain_db = -hm_offset24(&m->p[b5], &learned);
		o.plus_minus = learned ? 2 : 4;
		o.fixed = n5;
		o.confidence = confidence(m, b5, learned ? 50 : 30, 85, now, p);
		add_option(r, &o);
	}
}

void hm_judge(const struct hm_model *m, long now, const struct hm_params *p, struct hm_result *r)
{
	int k, j, most = 0, sure_free = -1;

	memset(r, 0, sizeof(*r));
	r->best = r->best_free = r->best_buy = r->target = -1;
	r->extender_helps = -1;
	for (k = 0; k < m->np; k++) {
		int n = hm_place_problems(m, k, now, p);

		r->places += m->p[k].n > 0;
		if (n > most)
			most = n, r->target = k;
	}
	for (k = 0; k < HM_DEVS; k++)
		r->devices += m->d[k].mac[0] != '\0';
	r->problems = hm_place_problems(m, -1, now, p);
	r->status = (int)m->looks >= p->learn && r->places ? HS_READY : HS_LEARNING;
	if (r->status != HS_READY)
		return;
	opt_extender(m, now, p, r);
	opt_gateway(m, now, p, r);
	opt_bands(m, now, p, r);
	for (k = 0; k < r->nopt; k++)           /* best first */
		for (j = k + 1; j < r->nopt; j++)
			if (better(&r->o[j], &r->o[k])) {
				struct hm_option t = r->o[k];

				r->o[k] = r->o[j];
				r->o[j] = t;
			}
	for (k = 0; k < r->nopt; k++) {
		if (r->o[k].buy && r->best_buy < 0)
			r->best_buy = k;
		if (!r->o[k].buy && r->best_free < 0)
			r->best_free = k;
		if (!r->o[k].buy && r->o[k].confidence >= 45 && sure_free < 0)
			sure_free = k;
	}
	r->best = r->nopt ? 0 : -1;
	/* an extender is worth buying only when it fixes more than a free change AIGW is fairly sure of */
	r->extender_helps = r->best_buy >= 0 && (sure_free < 0 || r->o[r->best_buy].fixed > r->o[sure_free].fixed);
}

/* ---------- words ---------- */

void hm_place_words(const struct hm_model *m, int place, const struct hm_names *nm, int prep, char *buf, size_t n)
{
	const struct hm_place *pl;
	const char *a = NULL, *b = NULL;
	int k, t0 = -1, t1 = -1;

	buf[0] = '\0';
	if (place < 0 || place >= m->np)
		return;
	pl = &m->p[place];
	if (nm && nm->zones && pl->name[0]) {
		const char *s;

		/* "bedroom": the bedroom; "Sam's room": as it is */
		for (s = pl->name; *s && ((*s >= 'a' && *s <= 'z') || *s == ' '); s++)
			;
		snprintf(buf, n, "%s%s%s", prep ? "in " : "", *s ? "" : "the ", pl->name);
		return;
	}
	for (k = 0; nm && nm->dev && k < HM_DEVS; k++) {
		if (!pl->dn[k] || !m->d[k].mac[0])
			continue;
		if (t0 < 0 || pl->dn[k] > pl->dn[t0])
			t1 = t0, t0 = k;
		else if (t1 < 0 || pl->dn[k] > pl->dn[t1])
			t1 = k;
	}
	if (t0 >= 0)
		a = nm->dev(m->d[t0].mac, nm->ctx);
	if (t1 >= 0)
		b = nm->dev(m->d[t1].mac, nm->ctx);
	if (a && b && *a && *b)
		snprintf(buf, n, "%swhere %s and %s are", prep ? "" : "the spot ", a, b);
	else if (a && *a)
		snprintf(buf, n, "%swhere %s is", prep ? "" : "the spot ", a);
	else
		snprintf(buf, n, "%splace %d", prep ? "at " : "", place + 1);
}

static void limits(const struct hm_model *m, const struct hm_option *o, char *buf, size_t n)
{
	const char *c = hm_confidence_name(o->confidence);

	buf[0] = '\0';
	switch (o->kind) {
	case HO_EXTENDER:
		if (m->ears <= 1)
			snprintf(buf, n, " Confidence %s: with one access point AIGW knows how far that place is, not in which "
				 "direction: put the extender on the way there.", c);
		else
			snprintf(buf, n, " Confidence %s.", c);
		break;
	case HO_GATEWAY_UP:
		snprintf(buf, n, " Confidence %s: AIGW can't see where the gateway sits; on the floor, in a cabinet or "
			 "behind a TV it gains most, already high and in the open nothing.", c);
		break;
	case HO_GATEWAY_CENTRAL:
		snprintf(buf, n, " Confidence %s: this assumes the far places are on one side of the gateway.", c);
		break;
	default:
		snprintf(buf, n, " Confidence %s.", c);
	}
}

void hm_option_words(const struct hm_model *m, const struct hm_result *r, int i, const struct hm_names *nm, char *buf,
		     size_t n)
{
	const struct hm_option *o;
	char to[96], noun[96], at[96], lim[240], fix[96];
	int learned = 0;

	buf[0] = '\0';
	if (i < 0 || i >= r->nopt)
		return;
	o = &r->o[i];
	hm_place_words(m, o->place, nm, 1, to, sizeof(to));
	hm_place_words(m, o->place, nm, 0, noun, sizeof(noun));
	if (!to[0])
		snprintf(to, sizeof(to), "in the far places");
	if (!noun[0])
		snprintf(noun, sizeof(noun), "the far places");
	hm_place_words(m, o->at, nm, 0, at, sizeof(at));
	limits(m, o, lim, sizeof(lim));
	snprintf(fix, sizeof(fix), "fix %d of the %d coverage problem%s of the last 30 days", o->fixed, r->problems,
		 r->problems == 1 ? "" : "s");
	switch (o->kind) {
	case HO_EXTENDER:
		if (o->at >= 0)
			snprintf(buf, n, "An extender near %s (its own link there about %d dBm) would raise the signal %s by "
				 "about %d dB (give or take %d) and %s.%s", at, o->backhaul_dbm, to, o->gain_db, o->plus_minus,
				 fix, lim);
		else
			snprintf(buf, n, "An extender about halfway between the gateway and %s would raise the signal there by "
				 "about %d dB (give or take %d) and %s.%s%s", noun, o->gain_db, o->plus_minus, fix, lim,
				 o->backhaul_dbm < -70 ? " Its own link would be weak there: use one with a wired link "
				 "(Ethernet or powerline)." : "");
		break;
	case HO_GATEWAY_UP:
		snprintf(buf, n, "Putting the gateway higher, in the open, usually adds about %d dB everywhere: it would %s.%s",
			 o->gain_db, fix, lim);
		break;
	case HO_GATEWAY_CENTRAL:
		snprintf(buf, n, "Moving the gateway toward the middle of the home would raise the signal %s by about %d dB "
			 "and %s; %d place%s near the gateway now may turn weak.%s", to, o->gain_db, fix, o->broken,
			 o->broken == 1 ? "" : "s", lim);
		break;
	case HO_BAND24:
		hm_offset24(&m->p[o->place], &learned);
		snprintf(buf, n, "Keeping devices %s on 2.4 GHz would raise their signal by about %d dB (it reaches further, "
			 "but is slower) and %s.%s%s", to, o->gain_db, fix, lim,
			 learned ? "" : " (A typical difference: only one band was seen there.)");
		break;
	case HO_BAND5:
		snprintf(buf, n, "Moving the devices %s that can use 5 GHz to it would %s: 2.4 GHz loses traffic there "
			 "despite a strong signal (a crowded band).%s", to, fix, lim);
		break;
	}
}

void hm_advice(const struct hm_model *m, const struct hm_result *r, const struct hm_names *nm, char *buf, size_t n)
{
	static const char *const short_words[HO_N] = { "an extender", "the gateway higher", "the gateway nearer the middle",
							 "2.4 GHz there", "5 GHz there" };
	size_t l;

	buf[0] = '\0';
	if (r->status != HS_READY)
		return;
	if (!r->problems) {
		snprintf(buf, n, "No coverage problems in the last 30 days: nothing to buy or move.");
		return;
	}
	if (r->best < 0) {
		snprintf(buf, n, "None of the changes AIGW can predict would fix the last 30 days' %d coverage problem%s: "
			 "they are losses where the signal is fine (see the channels and Self-Healing Wi-Fi).", r->problems,
			 r->problems == 1 ? "" : "s");
		return;
	}
	hm_option_words(m, r, r->best, nm, buf, n);
	l = strlen(buf);
	if (l >= n)
		return;
	if (r->o[r->best].buy && r->best_free >= 0)
		snprintf(buf + l, n - l, r->o[r->best_free].fixed >= r->o[r->best].fixed ?
			 " A free change, %s, may fix as many (%d), but AIGW is less sure of it." :
			 " Free changes fix fewer: %s fixes %d.", short_words[r->o[r->best_free].kind],
			 r->o[r->best_free].fixed);
	else if (!r->o[r->best].buy && r->best_buy >= 0)
		snprintf(buf + l, n - l, " Try this before buying an extender: an extender would fix %d.",
			 r->o[r->best_buy].fixed);
	else if (!r->o[r->best].buy)
		snprintf(buf + l, n - l, " It costs nothing.");
}

void hm_proof_words(const struct hm_model *m, const struct hm_names *nm, char *buf, size_t n)
{
	char where[80];

	buf[0] = '\0';
	if (m->chk_place < 0)
		return;
	hm_place_words(m, m->chk_place, nm, 1, where, sizeof(where));
	if (m->chk_measured == NO_PROOF)
		snprintf(buf, n, "An extender appeared: AIGW is measuring %s again (it predicted about %+d dB).", where,
			 m->chk_pred);
	else
		snprintf(buf, n, "AIGW predicted an extender would add about %+d dB %s; measured since it appeared: %+d dB.",
			 m->chk_pred, where, m->chk_measured);
}
