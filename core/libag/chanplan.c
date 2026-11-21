/* SPDX-License-Identifier: Apache-2.0 */
/* Neighbour-aware channel choice: the rules (ag/chanplan.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/chanplan.h"

void cp_params_default(struct cp_params *p)
{
	p->days = 3;
	p->better = 30;
	p->min_scans = 3;
	p->worse_hour = 10;
}

void cp_init(struct cp_table *t, int band, const int *ch, int n)
{
	memset(t, 0, sizeof(*t));
	t->band = band;
	t->nch = n < CP_CH ? n : CP_CH;
	memcpy(t->ch, ch, sizeof(int) * (size_t)t->nch);
}

int cp_slot(const struct cp_table *t, int ch)
{
	int i;

	for (i = 0; i < t->nch; i++)
		if (t->ch[i] == ch)
			return i;
	return -1;
}

void cp_add_n(float *avg, uint16_t *n, float v, int window)
{
	if (*n < window)
		*avg = (*avg * *n + v) / (float)(*n + 1);
	else
		*avg += (v - *avg) / (float)window;
	if (*n < 65535)
		(*n)++;
}

void cp_add(float *avg, uint16_t *n, float v)
{
	cp_add_n(avg, n, v, 8);
}

static int block(int ch, int width)
{
	int base = ch >= 149 ? 149 : 36, span = width >= 80 ? 16 : width == 40 ? 8 : 4;

	return base * 100 + (ch - base) / span;
}

float cp_nb_weight(int band, int ch, int width, int bss_ch, int signal, int util)
{
	float ov = 1, sw = signal >= -60 ? 3 : signal >= -75 ? 2 : 1;

	if (band == 2) {
		int d = abs(ch - bss_ch);

		if (d >= 5)
			return 0;
		ov = (float)(5 - d) / 5.0f;     /* overlapping, less the farther away */
	} else if (block(ch, width) != block(bss_ch, width)) {
		return 0;
	}
	return ov * sw * (util >= 0 ? 0.5f + 2.0f * (float)util / 255.0f : 1.0f);
}

int cp_busy_hours(const struct cp_table *t, float w[CP_HOURS])
{
	float max = 0;
	int h, n = 0;

	for (h = 0; h < CP_HOURS; h++)
		if (t->n_use[h] >= 2 && t->use[h] > max)
			max = t->use[h];
	for (h = 0; h < CP_HOURS; h++) {
		w[h] = 1;
		if (max < 1.0f)
			continue;               /* the home hardly uses its Wi-Fi yet: every hour alike */
		w[h] = t->n_use[h] >= 2 && t->use[h] >= max / 2 ? 1.0f : 0.1f;
		n += w[h] >= 1;
	}
	return n;
}

void cp_hours_words(const float w[CP_HOURS], char *buf, size_t n)
{
	int h, len = 0, start = -1, busy = 0;

	buf[0] = '\0';
	for (h = 0; h < CP_HOURS; h++)
		busy += w[h] >= 1;
	if (busy == CP_HOURS) {
		snprintf(buf, n, "all day");
		return;
	}
	for (h = 0; h <= CP_HOURS; h++) {
		int on = h < CP_HOURS && w[h] >= 1;

		if (on && start < 0)
			start = h;
		if (!on && start >= 0) {
			if (len < (int)n)
				len += snprintf(buf + len, n - (size_t)len, "%s%02d:00-%02d:00", len ? ", " : "", start, h);
			start = -1;
		}
	}
}

float cp_load(const struct cp_table *t, int slot, int hour, int metric)
{
	const struct cp_cell *c;
	float sum = 0;
	long n = 0;
	int h;

	if (slot < 0 || slot >= t->nch || hour < 0 || hour >= CP_HOURS)
		return -1;
	c = &t->c[slot][hour];
	if (metric == CP_AIR)
		return c->n_air ? c->air : -1;
	if (c->n_nb)
		return c->nb;
	for (h = 0; h < CP_HOURS; h++)
		if (t->c[slot][h].n_nb) {
			sum += t->c[slot][h].nb * t->c[slot][h].n_nb;
			n += t->c[slot][h].n_nb;
		}
	return n ? sum / (float)n : -1;
}

/* the two compared on one measure over the busy hours: 0 when it covers enough of them */
static int compare(const struct cp_table *t, int cs, int s, int m, const float *w, const struct cp_params *p,
		   float *lc, float *ls, int *worse)
{
	float sc = 0, ss = 0, cov = 0, all = 0;
	int h;

	*worse = 0;
	if (m == CP_NB && t->scans < (uint32_t)p->min_scans)
		return -1;
	for (h = 0; h < CP_HOURS; h++) {
		float a = cp_load(t, cs, h, m), b = cp_load(t, s, h, m);

		all += w[h];
		if (a < 0 || b < 0)
			continue;
		cov += w[h];
		sc += w[h] * a;
		ss += w[h] * b;
		if (m == CP_AIR && w[h] >= 1 && b > a + (float)p->worse_hour)
			*worse = 1;             /* worse in one of the hours that matter */
	}
	if (all <= 0 || cov < 0.6f * all)
		return -1;
	*lc = sc / cov;
	*ls = ss / cov;
	return 0;
}

int cp_choose(const struct cp_table *t, int cur, const uint8_t *avoid, long now, const struct cp_params *p,
	      struct cp_choice *out)
{
	float w[CP_HOURS];
	char hours[64];
	int cs = cp_slot(t, cur), s, compared = 0, busy;

	memset(out, 0, sizeof(*out));
	out->cur = cur;
	out->days = t->first && now > t->first ? (int)((now - t->first) / 86400) : 0;
	if (cs < 0) {
		snprintf(out->words, sizeof(out->words), "Channel %d is not one AIGW plans with.", cur);
		return 0;
	}
	if (!t->first || now - t->first < (long)p->days * 86400) {
		snprintf(out->words, sizeof(out->words), "Learning the channels: %d of %d days so far (%u scans).",
			 out->days, p->days, t->scans);
		return 0;
	}
	busy = cp_busy_hours(t, w);
	cp_hours_words(w, hours, sizeof(hours));
	out->status = CP_STAY;
	for (s = 0; s < t->nch; s++) {
		float lc = 0, ls = 0;
		int m, worse = 0, gain, clearly;

		if (s == cs || (avoid && avoid[s]))
			continue;
		for (m = CP_AIR; m <= CP_NB; m++)
			if (!compare(t, cs, s, m, w, p, &lc, &ls, &worse))
				break;
		if (m > CP_NB)
			continue;
		compared++;
		gain = lc > 0 ? (int)((lc - ls) * 100.0f / lc) : 0;
		clearly = gain >= p->better && lc - ls >= (m == CP_AIR ? 10.0f : 2.0f) && !worse;
		if (clearly && (!out->best || gain > out->gain_pct)) {
			out->status = CP_BETTER;
			out->best = t->ch[s];
			out->metric = m;
			out->cur_load = lc;
			out->best_load = ls;
			out->gain_pct = gain;
		}
	}
	if (out->status == CP_BETTER && out->metric == CP_AIR)
		snprintf(out->words, sizeof(out->words),
			 "Channel %d is clearly quieter in your busy hours (%s%s): others use %.0f%% of its air, "
			 "%.0f%% of channel %d's (%d days of history).", out->best, hours, busy ? "" : ", not known yet",
			 out->best_load, out->cur_load, cur, out->days);
	else if (out->status == CP_BETTER)
		snprintf(out->words, sizeof(out->words),
			 "Channel %d has clearly fewer and weaker neighbouring networks in your busy hours (%s%s): "
			 "they weigh %.1f on it, %.1f on channel %d (%u scans, %d days).", out->best, hours,
			 busy ? "" : ", not known yet", out->best_load, out->cur_load, cur, t->scans, out->days);
	else if (compared)
		snprintf(out->words, sizeof(out->words),
			 "Channel %d is as good as any here in your busy hours (%s%s): no other is clearly better.", cur,
			 hours, busy ? "" : ", not known yet");
	else
		snprintf(out->words, sizeof(out->words),
			 "Not enough known of the other channels yet to compare them (%u scans).", t->scans);
	return 0;
}

void cp_before(const struct cp_table *t, int slot, struct cp_feel *f)
{
	float w[CP_HOURS], wd = 0, wr = 0, wa = 0;
	int h;

	memset(f, 0, sizeof(*f));
	if (slot < 0 || slot >= t->nch)
		return;
	cp_busy_hours(t, w);
	for (h = 0; h < CP_HOURS; h++) {
		const struct cp_cell *c = &t->c[slot][h];

		if (c->n_dev) {
			f->loss += w[h] * c->loss;
			f->rtt += w[h] * c->rtt;
			wd += w[h];
			f->n_dev += c->n_dev;
		}
		if (c->n_rate) {
			f->rate += w[h] * c->rate;
			wr += w[h];
			f->n_rate += c->n_rate;
		}
		if (c->n_air) {
			f->air += w[h] * c->air;
			wa += w[h];
			f->n_air += c->n_air;
		}
	}
	if (wd > 0)
		f->loss /= wd, f->rtt /= wd;
	if (wr > 0)
		f->rate /= wr;
	if (wa > 0)
		f->air /= wa;
}

void cp_feel_words(const struct cp_feel *f, char *buf, size_t n)
{
	int len = 0;

	buf[0] = '\0';
	if (f->n_dev)
		len += snprintf(buf, n, "%.0f%% lost, %.0f ms", f->loss, f->rtt);
	if (f->n_rate && len < (int)n)
		len += snprintf(buf + len, n - (size_t)len, "%s%.0f Mbit/s", len ? ", " : "", f->rate);
	if (f->n_air && len < (int)n)
		len += snprintf(buf + len, n - (size_t)len, "%sothers use %.0f%% of the air", len ? ", " : "", f->air);
	if (!len)
		snprintf(buf, n, "not measured");
}

int cp_verdict(const struct cp_feel *b, const struct cp_feel *a, int better, char *why, size_t n)
{
	int dev = b->n_dev && a->n_dev, rate = b->n_rate && a->n_rate, air = b->n_air && a->n_air;
	int worse = 0, gain = 0;
	char bw[120], aw[120];
	const char *what = "nothing clearly better";

	cp_feel_words(b, bw, sizeof(bw));
	cp_feel_words(a, aw, sizeof(aw));
	if (!a->n_dev && !a->n_rate) {
		snprintf(why, n, "no device could be measured on it (before: %s)", bw);
		return 0;
	}
	if (dev && a->loss > b->loss + 2 && a->loss > b->loss * 1.5f)
		worse = 1, what = "devices lose more";
	else if (dev && a->rtt > b->rtt * 1.3f + 5)
		worse = 1, what = "replies are slower";
	else if (rate && a->rate < b->rate * 0.8f)
		worse = 1, what = "link rates are lower";
	else if (dev && b->loss >= 2 && a->loss * 2 <= b->loss)
		gain = 1, what = "devices lose half as much or less";
	else if (dev && b->rtt > 0 && a->rtt <= b->rtt * 0.7f)
		gain = 1, what = "replies are clearly faster";
	else if (rate && a->rate >= b->rate * 1.2f)
		gain = 1, what = "link rates are clearly higher";
	else if (air && b->air - a->air >= 10 && a->air <= b->air * (float)(100 - better) / 100.0f)
		gain = 1, what = "others use clearly less of the air";
	snprintf(why, n, "%s: now %s; before %s", what, aw, bw);
	return !worse && gain;
}
