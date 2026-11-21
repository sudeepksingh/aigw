/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW coverage placement advice: a device's history, its weak spots, the
 * home's spots, and what to do (see ag/coverage.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/coverage.h"

void cov_params_default(struct cov_params *p)
{
	p->weak_dbm = -75;
	p->loss_pct = 10;
	p->learn = 60;
	p->spot_min = 10;
	p->same_db = 6;
	p->steady_db = 8;
	p->good_dbm = -68;
	p->decay = 10080;
}

const char *cov_kind_name(int k)
{
	static const char *const n[CK_N] = { "Learning", "Fine", "Sometimes", "Always", "WeakTransmitter" };

	return k >= 0 && k < CK_N ? n[k] : "Learning";
}

const char *cov_verdict_name(int v)
{
	static const char *const n[CV_N] = { "Learning", "Fine", "WeakDevice", "MoveDevice", "Extender", "UseExtender" };

	return v >= 0 && v < CV_N ? n[v] : "Learning";
}

/* ---------- the history ---------- */

static void halve16(uint16_t *v, int n)
{
	while (n--)
		v[n] /= 2;
}

/* the last week or two weigh most: everything halves now and then */
static void halve(struct cov_dev *d)
{
	int j;

	d->n /= 2;
	d->weak /= 2;
	halve16(d->bins, COV_BINS);
	halve16(d->hn, 24);
	halve16(d->hw, 24);
	d->loss_w /= 2, d->loss_o /= 2, d->lw /= 2, d->lo /= 2;
	d->rate_w /= 2, d->retry_w /= 2, d->rw /= 2, d->tw /= 2;
	d->on24 /= 2, d->drops /= 2, d->roams /= 2;
	for (j = 0; j < COV_SPOTS; j++) {
		struct cov_spot *s = &d->spot[j];

		s->n /= 2, s->measured /= 2, s->lossy /= 2;
		halve16(s->when, 4);
		if (!s->n)
			memset(s, 0, sizeof(*s));
	}
}

static float absf(float x)
{
	return x < 0 ? -x : x;
}

/* mean difference over the access points both heard; 99 when none */
static float dist(const float *a, const float *b)
{
	float sum = 0;
	int e, k = 0;

	for (e = 0; e < COV_EARS; e++)
		if (a[e] && b[e]) {
			sum += absf(a[e] - b[e]);
			k++;
		}
	return k ? sum / (float)k : 99;
}

static int part_of(int hour)
{
	return hour < 6 ? 0 : hour < 12 ? 1 : hour < 18 ? 2 : 3;
}

void cov_add(struct cov_dev *d, const struct cov_sample *s, const struct cov_params *p)
{
	int weak = s->dbm <= p->weak_dbm, h = s->hour >= 0 && s->hour < 24 ? s->hour : 0, b, e, j, best = -1, part;
	float v[COV_EARS] = { 0 }, bd = 99;
	struct cov_spot *sp;

	if (!s->dbm)
		return;
	if (d->n >= p->decay || d->n >= 60000)
		halve(d);
	if (s->day != d->day0) {
		long k = s->day - d->day0;

		d->days = k > 0 && k < 8 ? (uint8_t)(d->days << k) : 0;
		d->day0 = s->day;
	}
	if (!d->first)
		d->first = s->t;
	d->last = s->t;
	d->n++;
	d->hn[h]++;
	b = (s->dbm - COV_BIN0) / 2;
	d->bins[b < 0 ? 0 : b >= COV_BINS ? COV_BINS - 1 : b]++;
	d->on24 += s->band == 2;
	if (s->loss_pct >= 0) {
		if (weak)
			d->loss_w += (uint32_t)s->loss_pct, d->lw++;
		else
			d->loss_o += (uint32_t)s->loss_pct, d->lo++;
	}
	if (!weak)
		return;
	d->weak++;
	d->hw[h]++;
	d->days |= 1;
	if (s->rate_mbps > 0)
		d->rate_w += (uint32_t)s->rate_mbps, d->rw++;
	if (s->retry_pct >= 0)
		d->retry_w += (uint32_t)s->retry_pct, d->tw++;

	/* its signature: where it is, and what the other access points hear */
	for (e = 0; e < COV_EARS; e++)
		if (s->heard[e] && e != s->ear)
			v[e] = (float)s->heard[e];
	if (s->ear >= 0 && s->ear < COV_EARS)
		v[s->ear] = (float)(s->band == 2 ? s->dbm - COV_OFF24 : s->dbm);
	part = part_of(h);
	for (j = 0; j < COV_SPOTS; j++) {
		float x;

		if (!d->spot[j].n)
			continue;
		/* the time of day counts a little: a spot never seen at this time is a bit further */
		x = dist(v, d->spot[j].sig) + (d->spot[j].n >= 10 && !d->spot[j].when[part] ? 3.0f : 0.0f);
		if (x < bd)
			bd = x, best = j;
	}
	if (best < 0 || bd > (float)p->same_db) {
		int k = -1;

		for (j = 0; j < COV_SPOTS; j++)
			if (!d->spot[j].n && k < 0)
				k = j;
			else if (d->spot[j].n && d->spot[j].n < 3 && k < 0)
				k = j;          /* a spot seen only once or twice gives way */
		if (k >= 0 || best < 0) {
			best = k >= 0 ? k : 0;
			memset(&d->spot[best], 0, sizeof(d->spot[best]));
			d->spot[best].first = s->t;
		}
	}
	sp = &d->spot[best];
	for (e = 0; e < COV_EARS; e++)
		if (v[e])
			sp->sig[e] = sp->sig[e] ? sp->sig[e] + (v[e] - sp->sig[e]) / (float)(sp->n < 31 ? sp->n + 1 : 32) : v[e];
	sp->n++;
	sp->when[part]++;
	sp->last = s->t;
	if (s->loss_pct >= 0) {
		sp->measured++;
		sp->lossy += s->loss_pct >= p->loss_pct;
	}
}

/* ---------- judging ---------- */

/* the dBm at a percentile of the history */
static int pct(const struct cov_dev *d, int q)
{
	uint32_t want = d->n * (uint32_t)q / 100, k = 0;
	int b;

	for (b = 0; b < COV_BINS; b++)
		if ((k += d->bins[b]) > want)
			break;
	return COV_BIN0 + 2 * (b < COV_BINS ? b : COV_BINS - 1) + 1;
}

static int bits7(uint8_t v)
{
	int k = 0;

	for (v &= 0x7f; v; v &= (uint8_t)(v - 1))
		k++;
	return k;
}

/* the longest run of hours (round the clock) where it is weak most of the time */
static void hours(const struct cov_dev *d, int *from, int *to)
{
	int h, best = 0, start = -1, run = 0, bs = -1;

	*from = *to = -1;
	for (h = 0; h < 48; h++) {
		int x = h % 24, weak = d->hn[x] >= 2 && d->hw[x] * 2 >= d->hn[x];

		if (weak) {
			if (!run)
				start = x;
			if (++run > best && run <= 24)
				best = run, bs = start;
		} else
			run = 0;
	}
	if (best && best < 24) {
		*from = bs;
		*to = (bs + best) % 24;
	}
}

static void judge_dev(const struct cov_dev *d, const struct cov_params *p, struct cov_sum *s)
{
	memset(s, 0, sizeof(*s));
	s->spot = s->from_h = s->to_h = -1;
	s->loss_weak = d->lw ? (int)(d->loss_w / d->lw) : -1;
	s->loss_ok = d->lo ? (int)(d->loss_o / d->lo) : -1;
	if (!d->n)
		return;
	s->usual = pct(d, 50);
	s->low = pct(d, 10);
	s->high = pct(d, 90);
	s->weak_pct = (int)(d->weak * 100 / d->n);
	s->on24_pct = (int)(d->on24 * 100u / d->n);
	s->days = bits7(d->days);
	s->suffers = s->loss_weak >= 0 ? s->loss_weak >= p->loss_pct : d->tw ? (int)(d->retry_w / d->tw) >= 30 : 1;
	if ((int)d->n < p->learn) {
		s->kind = CK_LEARNING;
		return;
	}
	if (s->weak_pct < 5 && (int)d->weak < p->spot_min)
		s->kind = CK_FINE;
	else if (s->weak_pct >= 70)
		s->kind = CK_ALWAYS;
	else {
		s->kind = CK_SOMETIMES;
		hours(d, &s->from_h, &s->to_h);
	}
}

void cov_judge(const struct cov_dev *d, int n, const struct cov_params *p, int (*iot)(int i, void *ctx), void *ctx,
	       struct cov_result *r)
{
	int i, j, k, others_loss = -1, others_usual = -100, learning = 0;
	long lsum = 0, lk = 0, usum = 0, uk = 0;
	uint8_t used[COV_DEVS][COV_SPOTS];

	memset(r, 0, sizeof(*r));
	r->main = r->worst = -1;
	if (n > COV_DEVS)
		n = COV_DEVS;
	for (i = 0; i < n; i++) {
		if (!d[i].mac[0] || !d[i].n) {
			r->d[i].kind = CK_LEARNING;
			r->d[i].spot = -1;
			continue;
		}
		r->devices++;
		judge_dev(&d[i], p, &r->d[i]);
		learning += r->d[i].kind == CK_LEARNING;
	}

	/* a weak transmitter: compared with the others (their loss when weak, their usual signal, their rate) */
	for (i = 0; i < n; i++) {
		struct cov_sum *s = &r->d[i];
		long rs = 0, rk = 0;
		int steady, low_loss, cmp, rate;

		if (s->kind != CK_ALWAYS && !(s->kind == CK_SOMETIMES && s->weak_pct >= 60))
			continue;
		lsum = lk = usum = uk = 0;
		for (j = 0; j < n; j++) {
			if (j == i || r->d[j].kind == CK_LEARNING || !d[j].n)
				continue;
			if (d[j].lw >= 3)
				lsum += (long)(d[j].loss_w / d[j].lw), lk++;
			if (d[j].rw)
				rs += (long)(d[j].rate_w / d[j].rw), rk++;
			usum += r->d[j].usual, uk++;
		}
		others_loss = lk ? (int)(lsum / lk) : -1;
		others_usual = uk ? (int)(usum / uk) : -100;
		steady = s->high - s->low <= p->steady_db;
		low_loss = d[i].lw >= 3 && s->loss_weak * 2 < p->loss_pct;
		/* the rate to it stays high where the others' falls: it hears the gateway well, only its own radio is weak */
		rate = d[i].rw && rk && rs > 0 && (long)(d[i].rate_w / d[i].rw) >= 2 * (rs / rk);
		cmp = others_loss >= p->loss_pct || others_usual > p->weak_dbm + 10 || rate;
		if (steady && (d[i].lw >= 3 ? low_loss && cmp :
			       rate || (iot && iot(i, ctx) && others_usual > p->weak_dbm + 10))) {
			s->kind = CK_WEAK_TX;
			s->suffers = 0;
			s->from_h = s->to_h = -1;
		}
	}

	/* the home's spots: the devices' spots with the same signature, the biggest placed first */
	memset(used, 0, sizeof(used));
	for (;;) {
		int bi = -1, bj = -1, bn = 0;
		const struct cov_spot *sp;
		struct cov_home_spot *h = NULL;
		int suffers;

		for (i = 0; i < n; i++) {
			if (r->d[i].kind != CK_SOMETIMES && r->d[i].kind != CK_ALWAYS)
				continue;
			for (j = 0; j < COV_SPOTS; j++)
				if (!used[i][j] && d[i].spot[j].n >= p->spot_min && d[i].spot[j].n > bn)
					bi = i, bj = j, bn = d[i].spot[j].n;
		}
		if (bi < 0)
			break;
		used[bi][bj] = 1;
		sp = &d[bi].spot[bj];
		for (k = 0; k < r->nspot; k++)
			if (dist(r->spot[k].sig, sp->sig) <= (float)p->same_db) {
				h = &r->spot[k];
				break;
			}
		if (!h) {
			if (r->nspot >= COV_HOME)
				continue;
			h = &r->spot[r->nspot++];
			memcpy(h->sig, sp->sig, sizeof(h->sig));
		}
		suffers = sp->measured >= 3 ? sp->lossy * 3 >= sp->measured : r->d[bi].suffers;
		for (j = 0; j < h->ndev && h->dev[j] != bi; j++)
			;
		if (j == h->ndev) {
			if (h->ndev < COV_HOME_DEVS)
				h->dev[h->ndev++] = bi;
			h->suffering += suffers;
		}
		h->n += sp->n;
		for (k = 0; k < 4; k++)
			h->when[k] += sp->when[k];
		if (r->d[bi].spot < 0)
			r->d[bi].spot = (int)(h - r->spot);
	}
	r->shared = 0;
	for (k = 0; k < r->nspot; k++) {
		struct cov_home_spot *h = &r->spot[k];
		int e;

		h->shared = h->suffering >= 2;
		r->shared += h->shared;
		h->best_ear = -1;
		for (e = 0; e < COV_EARS; e++)
			if (h->sig[e] && (h->best_ear < 0 || h->sig[e] > h->sig[h->best_ear]))
				h->best_ear = e;
		h->best_dbm = h->best_ear >= 0 ? (int)(h->sig[h->best_ear] - 0.5f) : 0;
		h->dbm = h->sig[0] ? (int)(h->sig[0] - 0.5f) : h->best_dbm;
		h->covered = h->best_ear > 0 && h->best_dbm >= p->good_dbm;
		if (h->shared && (r->main < 0 || h->suffering > r->spot[r->main].suffering ||
				  (h->suffering == r->spot[r->main].suffering && h->n > r->spot[r->main].n)))
			r->main = k;
	}

	/* the verdict, and the device that matters most */
	for (i = 0; i < n; i++) {
		const struct cov_sum *s = &r->d[i];
		int moves = (s->kind == CK_SOMETIMES || s->kind == CK_ALWAYS) && s->suffers;

		r->judged += s->kind != CK_LEARNING && d[i].n;
		r->weak_devices += s->kind == CK_SOMETIMES || s->kind == CK_ALWAYS || s->kind == CK_WEAK_TX;
		r->movers += moves;
		r->weak_tx += s->kind == CK_WEAK_TX;
		if (r->main >= 0 ? s->spot == r->main && d[i].weak > (r->worst >= 0 ? d[r->worst].weak : 0) :
		    moves && d[i].weak > (r->worst >= 0 ? d[r->worst].weak : 0))
			r->worst = i;
	}
	if (r->main >= 0)
		r->verdict = r->spot[r->main].covered ? CV_USE_EXTENDER : CV_EXTENDER;
	else if (r->movers)
		r->verdict = CV_MOVE_DEVICE;
	else if (r->weak_tx) {
		r->verdict = CV_WEAK_DEVICE;
		for (i = 0; i < n; i++)
			if (r->d[i].kind == CK_WEAK_TX && r->worst < 0)
				r->worst = i;
	} else
		r->verdict = r->judged ? CV_FINE : CV_LEARNING;
	if (r->verdict == CV_FINE && learning && !r->weak_devices && r->judged < learning)
		r->verdict = CV_LEARNING;
}

/* ---------- words ---------- */

const char *cov_part_words(const uint32_t when[4])
{
	static const char *const one[4] = { "at night", "in the morning", "in the afternoon", "in the evening" };
	static char two[48];
	uint32_t t = when[0] + when[1] + when[2] + when[3];
	int k, a = -1, b = -1;

	if (!t)
		return "";
	if (when[0] * 5 >= t && when[1] * 5 >= t && when[2] * 5 >= t && when[3] * 5 >= t)
		return "at any time of day";
	for (k = 0; k < 4; k++)
		if (when[k] * 10 >= t * 3) {
			if (a < 0 || when[k] > when[a])
				b = a, a = k;
			else if (b < 0 || when[k] > when[b])
				b = k;
		}
	if (a < 0)
		return "at any time of day";
	if (b < 0)
		return one[a];
	if (a > b)
		k = a, a = b, b = k;
	if (a == 0 && b == 3)           /* the evening into the night */
		k = a, a = b, b = k;
	snprintf(two, sizeof(two), "%s and %s", one[a], one[b]);
	return two;
}

static const char *dev_name(const struct cov_names *nm, int i)
{
	const char *s = nm && nm->dev ? nm->dev(i, nm->ctx) : NULL;

	return s && *s ? s : "a device";
}

static const char *ear_name(const struct cov_names *nm, int e)
{
	const char *s = nm && nm->ear ? nm->ear(e, nm->ctx) : NULL;

	return s && *s ? s : e ? "an extender" : "the gateway";
}

void cov_spot_words(const struct cov_result *r, int spot, const struct cov_names *nm, char *buf, size_t n)
{
	const struct cov_home_spot *h;
	const char *w;
	size_t l;

	buf[0] = '\0';
	if (spot < 0 || spot >= r->nspot)
		return;
	h = &r->spot[spot];
	if (h->ndev == 1)
		l = (size_t)snprintf(buf, n, "where %s is", dev_name(nm, h->dev[0]));
	else if (h->ndev == 2)
		l = (size_t)snprintf(buf, n, "where %s and %s are", dev_name(nm, h->dev[0]), dev_name(nm, h->dev[1]));
	else
		l = (size_t)snprintf(buf, n, "where %s, %s and %d more are", dev_name(nm, h->dev[0]), dev_name(nm, h->dev[1]),
				     h->ndev - 2);
	w = cov_part_words(h->when);
	if (l < n)
		snprintf(buf + l, n - l, "%s%s (about %d dBm at the gateway)", *w ? ", " : "", w, h->dbm);
}

/* a sentence that starts with a name ("the extender ...") */
static size_t sentence(char *buf, size_t n, const char *fmt, const char *name, int dbm)
{
	size_t l = (size_t)snprintf(buf, n, fmt, name, dbm);

	if (n && buf[0] == ' ' && buf[1] >= 'a' && buf[1] <= 'z')
		buf[1] = (char)(buf[1] - 'a' + 'A');
	return l;
}

static void when_words(const struct cov_sum *s, char *buf, size_t n)
{
	size_t l = 0;

	buf[0] = '\0';
	if (s->from_h >= 0)
		l = (size_t)snprintf(buf, n, "mostly %02d:00-%02d:00", s->from_h, s->to_h);
	if (s->days && l < n)
		snprintf(buf + l, n - l, "%son %d of the last 7 days", l ? ", " : "", s->days);
}

void cov_dev_words(const struct cov_result *r, int i, const struct cov_names *nm, char *buf, size_t n)
{
	const struct cov_sum *s = &r->d[i];
	char when[64], loss[64] = "";
	static const char *const what[CK_N] = {
		"still being learned", "fine", "weak some of the time (it moves into a far spot)",
		"weak all the time (it stays in a far spot)", "weak, but by its own radio (it loses little)",
	};

	when_words(s, when, sizeof(when));
	if (s->loss_weak >= 0 && s->loss_ok >= 0)
		snprintf(loss, sizeof(loss), "; loses %d%% when weak, %d%% otherwise", s->loss_weak, s->loss_ok);
	else if (s->loss_weak >= 0)
		snprintf(loss, sizeof(loss), "; loses %d%% when weak", s->loss_weak);
	snprintf(buf, n, "%s is %s; usually %d dBm (%d to %d), weak %d%% of the time%s%s%s%s", dev_name(nm, i),
		 what[s->kind < CK_N ? s->kind : 0], s->usual, s->low, s->high, s->weak_pct, *when ? ", " : "", when, loss,
		 s->on24_pct >= 50 ? "; mostly on 2.4 GHz" : "");
}

/* which access point hears the spot best (a mesh only) */
static size_t mesh_words(const struct cov_result *r, int spot, const struct cov_names *nm, const struct cov_params *p,
			 char *buf, size_t n)
{
	const struct cov_home_spot *h = &r->spot[spot];

	if (!nm || nm->ears <= 1 || h->best_ear < 0 || !n)
		return 0;
	if (h->covered)
		return sentence(buf, n, " %s hears that spot best (about %d dBm): devices there should use it.",
				ear_name(nm, h->best_ear), h->best_dbm);
	if (h->best_ear)
		return sentence(buf, n, " %s hears it best, but only at about %d dBm: move it about halfway toward that "
				"spot.", ear_name(nm, h->best_ear), h->best_dbm);
	return (size_t)snprintf(buf, n, " No extender hears it better than the gateway: one more, about halfway "
				"between the gateway and that spot, would.");
}

void cov_advice(const struct cov_result *r, int i, const struct cov_names *nm, char *buf, size_t n)
{
	struct cov_params p;
	const struct cov_sum *s;
	char where[200], when[64];
	size_t l;

	buf[0] = '\0';
	cov_params_default(&p);
	if (i < 0) {
		if (r->verdict == CV_FINE) {
			snprintf(buf, n, "No device is weak for long: nothing to move.");
			return;
		}
		i = r->worst;
		if (r->verdict == CV_USE_EXTENDER && r->main >= 0) {
			const struct cov_home_spot *h = &r->spot[r->main];

			cov_spot_words(r, r->main, nm, where, sizeof(where));
			snprintf(buf, n, "%d devices are weak in the same spot (%s), but %s hears it well (about %d dBm): they "
				 "belong on it. Mesh steering moves the ones that can be moved; turning Wi-Fi off and on near "
				 "the extender moves the others.", h->suffering, where, ear_name(nm, h->best_ear), h->best_dbm);
			return;
		}
		if (r->verdict == CV_EXTENDER && r->main >= 0) {
			const struct cov_home_spot *h = &r->spot[r->main];

			cov_spot_words(r, r->main, nm, where, sizeof(where));
			l = (size_t)snprintf(buf, n, "%d devices are weak in the same spot (%s): the gateway is too far from it. "
					     "Move the gateway toward that spot (higher, in the open), or add a Wi-Fi extender "
					     "about halfway between the gateway and it.", h->suffering, where);
			if (l < n)
				mesh_words(r, r->main, nm, &p, buf + l, n - l);
			return;
		}
	}
	if (i < 0 || i >= COV_DEVS)
		return;
	s = &r->d[i];
	cov_spot_words(r, s->spot, nm, where, sizeof(where));
	when_words(s, when, sizeof(when));
	if (s->spot >= 0 && r->spot[s->spot].shared) {
		const struct cov_home_spot *h = &r->spot[s->spot];

		l = (size_t)snprintf(buf, n, "%s is weak in a spot %d other device%s share (%s): the gateway is too far "
				     "from it. Move the gateway toward that spot, or add a Wi-Fi extender about halfway "
				     "between the gateway and it.", dev_name(nm, i), h->suffering - 1,
				     h->suffering - 1 == 1 ? "" : "s", where);
		if (l < n)
			mesh_words(r, s->spot, nm, &p, buf + l, n - l);
		return;
	}
	switch (s->kind) {
	case CK_SOMETIMES:
		l = (size_t)snprintf(buf, n, "%s is weak only some of the time%s%s%s: it is fine elsewhere, so the trouble "
				     "is one spot%s%s%s. Use it nearer the gateway there; if it is used there a lot, a Wi-Fi "
				     "extender about halfway between the gateway and that spot would fix it.",
				     dev_name(nm, i), *when ? " (" : "", when, *when ? ")" : "", *where ? " (" : "", where,
				     *where ? ")" : "");
		break;
	case CK_ALWAYS:
		l = (size_t)snprintf(buf, n, "%s is weak all the time (usually %d dBm): it stays in a far spot. Move it, or "
				     "the gateway, a little closer; if neither can move, a Wi-Fi extender about halfway between "
				     "them would fix it.", dev_name(nm, i), s->usual);
		break;
	case CK_WEAK_TX:
		snprintf(buf, n, "%s's own radio is weak (usually %d dBm) but it loses little, unlike other devices that "
			 "weak: a small or cheap device, not a coverage problem. Nothing to fix; move it a little closer "
			 "only if it misbehaves.", dev_name(nm, i), s->usual);
		return;
	case CK_FINE:
		snprintf(buf, n, "%s's Wi-Fi is fine most of the time (usually %d dBm).", dev_name(nm, i), s->usual);
		return;
	default:
		return;
	}
	if (l < n && s->spot >= 0)
		mesh_words(r, s->spot, nm, &p, buf + l, n - l);
}

void cov_note(const struct cov_result *r, int i, char *buf, size_t n)
{
	const struct cov_sum *s;
	const char *w = "";

	buf[0] = '\0';
	if (i < 0)
		i = r->worst;
	if (r->verdict == CV_USE_EXTENDER && r->main >= 0 && (i < 0 || r->d[i].spot == r->main)) {
		snprintf(buf, n, "We noticed some of your devices have weak Wi-Fi in one part of your home, although your Wi-Fi "
			 "extender reaches it well. Your gateway is moving them to the extender; turning their Wi-Fi off and "
			 "on near it also helps.");
		return;
	}
	if (r->verdict == CV_EXTENDER && r->main >= 0 && (i < 0 || r->d[i].spot == r->main)) {
		w = cov_part_words(r->spot[r->main].when);
		snprintf(buf, n, "We noticed the Wi-Fi is weak in one part of your home, where several of your devices are%s%s. "
			 "A Wi-Fi extender placed about halfway between your gateway and that spot would fix it; we will be in "
			 "touch about it.", *w ? " " : "", w);
		return;
	}
	if (i < 0 || i >= COV_DEVS)
		return;
	s = &r->d[i];
	if (s->spot >= 0)
		w = cov_part_words(r->spot[s->spot].when);
	if (s->kind == CK_SOMETIMES)
		snprintf(buf, n, "We noticed one of your devices loses its Wi-Fi in one part of your home%s%s. It works well "
			 "elsewhere: using it a little closer to the gateway there helps, and we can suggest a Wi-Fi extender "
			 "if you use it there a lot.", *w ? ", mostly " : "", w);
	else if (s->kind == CK_ALWAYS)
		snprintf(buf, n, "We noticed one of your devices has weak Wi-Fi where it stays. Moving it, or your gateway, "
			 "a little closer usually fixes it; if neither can move, a Wi-Fi extender would.");
	else if (s->kind == CK_WEAK_TX)
		snprintf(buf, n, "We noticed one of your devices has a weak Wi-Fi signal of its own. Your Wi-Fi is fine; if "
			 "that device misbehaves, moving it a little closer to your gateway helps.");
}
