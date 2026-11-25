/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW line health: robust trend, change point and days to a fault (see ag/line.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/line.h"

/* worse when lower; fault = base * rel + add (rel 0: a fixed level); a change worth saying: floor */
static const struct {
	const char *name, *key, *unit;
	int lower;
	float rel, add, floor_abs, floor_rel;
} M[LINE_METRICS] = {
	{ "SyncDown", "sync", "kbit/s", 1, 0.7f, 0, 0, 0.05f },
	{ "NoiseMargin", "margin", "dB", 1, 0, 3, 1, 0 },
	{ "OpticalPower", "optical", "dBm", 1, 0, -27, 1, 0 },
	{ "FECErrors", "fec", "/h", 0, 10, 1000, 100, 0.5f },
	{ "CRCErrors", "crc", "/h", 0, 5, 60, 5, 0.5f },
	{ "Drops", "drops", "/day", 0, 1, 4, 2, 0 },
	{ "NextHopLatency", "latency", "ms", 0, 1, 30, 3, 0.2f },
	{ "NextHopLoss", "loss", "%", 0, 1, 2, 0.3f, 0 },
	{ "DownloadSpeed", "speed", "Mbit/s", 1, 0.7f, 0, 1, 0.05f },
};

void line_params_default(struct line_params *p)
{
	memset(p, 0, sizeof(*p));
	p->min_days = 5;
	p->window = LINE_WINDOW;
	p->cp_z = 4;
	p->tau = 0.5f;
	p->horizon = 30;
}

static int ok_m(int m)
{
	return m >= 0 && m < LINE_METRICS;
}

const char *line_metric_name(int m) { return ok_m(m) ? M[m].name : ""; }
const char *line_metric_key(int m) { return ok_m(m) ? M[m].key : ""; }
const char *line_metric_unit(int m) { return ok_m(m) ? M[m].unit : ""; }
int line_lower_worse(int m) { return ok_m(m) && M[m].lower; }

const char *line_state_name(int s)
{
	static const char *const n[] = { "", "Learning", "OK", "Stepped", "Drifting", "Fault" };

	return s >= 0 && s <= LS_FAULT ? n[s] : "";
}

const char *line_conf_name(int c)
{
	return c >= 70 ? "high" : c >= 40 ? "medium" : "low";
}

static int cmpf(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

/* the median of n values (sorts a copy) */
static float median(const float *v, int n)
{
	float t[120];
	int i;

	if (n <= 0)
		return 0;
	if (n > 120)
		n = 120;
	for (i = 0; i < n; i++)
		t[i] = v[i];
	qsort(t, (size_t)n, sizeof(float), cmpf);
	return n % 2 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
}

/* Theil-Sen slope and Kendall's tau of (x, z) */
static void theil_sen(const float *x, const float *z, int n, float *slope, float *tau)
{
	float s[120];
	int i, j, k = 0, c = 0;

	for (i = 0; i < n; i++)
		for (j = i + 1; j < n && k < 120; j++) {
			float dz = z[j] - z[i];

			s[k++] = dz / (x[j] - x[i]);
			c += dz > 0 ? 1 : dz < 0 ? -1 : 0;
		}
	*slope = median(s, k);
	*tau = k ? (float)c / (float)k : 0;
}

static int cmpday(const void *a, const void *b)
{
	const struct line_day *x = a, *y = b;

	return x->day < y->day ? -1 : x->day > y->day;
}

void line_analyse(const struct line_day *d, int n, long today, int m, const struct line_params *p,
		  struct line_trend *o)
{
	struct line_day w[LINE_DAYS];
	float x[LINE_DAYS], z[LINE_DAYS], r[LINE_DAYS], sg = line_lower_worse(m) ? -1.0f : 1.0f;
	float best = 0, step = 0, slope = 0, tau = 0, base, floor_, fault, lz, fz, cs = 0, cd = 0, sig;
	int i, k = 0, cp = 0, from = 0, seg, nseg, drift = 0, win = p->window < LINE_DAYS ? p->window : LINE_DAYS;

	memset(o, 0, sizeof(*o));
	o->days_left = -1;
	o->fault = NAN;
	if (!ok_m(m))
		return;
	for (i = 0; i < n && k < LINE_DAYS; i++)
		if (d[i].day && d[i].n && d[i].day < today && d[i].day >= today - win && isfinite(d[i].v))
			w[k++] = d[i];
	o->ndays = k;
	if (!k)
		return;
	qsort(w, (size_t)k, sizeof(w[0]), cmpday);
	for (i = 0; i < k; i++) {
		x[i] = (float)(w[i].day - today);
		z[i] = sg * w[i].v;
	}
	if (k < p->min_days) {
		o->state = LS_LEARNING;
		o->level = w[k - 1].v;
		o->base = sg * median(z, k);
		return;
	}
	/* the base, roughly, for the floor: the first third */
	base = sg * median(z, k / 3 > 3 ? k / 3 : 3);
	floor_ = fmaxf(M[m].floor_abs, M[m].floor_rel * fabsf(base));
	/* the day-to-day noise: from the differences of neighbouring days (a step is one of them, a trend a little) */
	for (i = 1; i < k; i++)
		r[i - 1] = fabsf(z[i] - z[i - 1]);
	sig = fmaxf(1.4826f * median(r, k - 1) / 1.4142f, 0.25f * floor_);
	/* the last 7 days on their own, proven or not: weak for one home, telling when many share it (cortex) */
	if (k >= 7) {
		theil_sen(x + k - 7, z + k - 7, 7, &o->slope7, &o->tau7);
		o->slope7 *= sg;
	}
	/* the change point: the strongest split, each side 3 days or more, and most days of each side on its side
	   (two bad days at the end are not a step) */
	for (i = 3; i <= k - 3; i++) {
		float a = median(z, i), b = median(z + i, k - i), mid = (a + b) / 2, st;
		int j, lo = 0, hi = 0;

		for (j = 0; j < k; j++) {
			lo += j < i && z[j] < mid;
			hi += j >= i && z[j] > mid;
		}
		if (4 * lo < 3 * i || 4 * hi < 3 * (k - i))
			continue;
		st = (b - a) / sig * sqrtf((float)i * (float)(k - i) / (float)k);
		if (st > best) {
			best = st;
			step = b - a;
			from = i;
		}
	}
	cp = best >= p->cp_z && step >= floor_;
	if (cp)
		base = sg * median(z, from);
	else
		from = 0;
	seg = from;
	/* the trend: after the change point; a ramp's median split comes late, so if that is too short to prove, a
	   day or more earlier (to day 3) */
	for (i = from; !drift && i >= (cp ? 2 : 0); i--) {
		nseg = k - i;
		if (nseg < 4)
			continue;
		theil_sen(x + i, z + i, nseg, &slope, &tau);
		/* steady enough (tau), and not by chance: Kendall's test, one-sided p < 0.01 */
		drift = slope > 0 && tau >= p->tau && slope * (x[k - 1] - x[i]) >= floor_ &&
			3 * tau * sqrtf((float)nseg * (float)(nseg - 1)) / sqrtf(2.0f * (float)(2 * nseg + 5)) >= 2.33f;
		if (drift)
			seg = i;
		if (!cp)
			break;
	}
	nseg = k - seg;
	if (drift) {
		for (i = seg; i < k; i++)
			r[i - seg] = z[i] - slope * x[i];
		lz = median(r, nseg) + slope * x[k - 1];
	} else {
		slope = 0;
		lz = median(z + from, nseg);
	}
	/* the fault level, unless its normal is past it already (a long line: judged by its own normal) */
	fault = p->fault[m] != 0 ? p->fault[m] : M[m].rel ? base * M[m].rel + M[m].add : M[m].add;
	fz = sg * fault;
	if (sg * base >= fz)
		fault = NAN;
	o->level = sg * lz;
	o->base = base;
	o->slope = sg * slope;
	o->fault = fault;
	/* when it began: a drift, where its line leaves the base (a ramp's median split comes late); a step, its day */
	o->onset = cp ? w[from].day : 0;
	if (drift) {
		float at = x[k - 1] - (lz - sg * base) / slope;

		if (at < x[0])
			at = x[0];
		if (at > x[k - 1])
			at = x[k - 1];
		o->onset = (int32_t)(today + lroundf(at));
	}
	if (drift)
		cd = 100 * tau * fminf(1, (float)nseg / 10);
	if (cp)
		cs = 100 * fminf(1, best / (2 * p->cp_z)) * fminf(1, (float)k / 10);
	if (isfinite(fault) && lz >= fz) {
		o->state = LS_FAULT;
		o->days_left = 0;
		o->conf = (int)lroundf(fmaxf(60, fmaxf(cd, cs)));
		if (!o->onset)
			o->onset = w[k - 1].day;
		return;
	}
	if (drift && isfinite(fault)) {
		float left = ceilf((fz - lz) / slope);

		o->days_left = left <= (float)p->horizon ? (int)left : -1;
	}
	o->state = drift ? LS_DRIFT : cp ? LS_STEP : LS_OK;
	o->conf = drift ? (int)lroundf(fmaxf(cd, cs)) : cp ? (int)lroundf(cs) : 0;
}

static void val(int m, float v, char *buf, size_t n)
{
	float a = fabsf(v);

	snprintf(buf, n, a >= 100 || m == LM_SYNC || m == LM_DROPS ? "%.0f %s" : a >= 10 ? "%.1f %s" : "%.2g %s", v,
		 line_metric_unit(m));
}

void line_words(int m, const struct line_trend *t, char *buf, size_t n)
{
	static const char *const what[LINE_METRICS] = { "The sync rate", "The noise margin", "The optical level",
		"FEC errors", "CRC errors", "Retrains and drops", "The next hop's delay", "Loss to the next hop",
		"The speed test" };
	char a[32], b[32], s[32], f[32];
	size_t l;
	int down = line_lower_worse(m);

	if (!ok_m(m) || t->state < LS_OK) {
		snprintf(buf, n, "%s: not enough days yet (%d of 5).", ok_m(m) ? what[m] : "The line", t->ndays);
		return;
	}
	val(m, t->base, a, sizeof(a));
	val(m, t->level, b, sizeof(b));
	val(m, fabsf(t->slope), s, sizeof(s));
	if (isfinite(t->fault))
		val(m, t->fault, f, sizeof(f));
	if (t->state == LS_OK) {
		snprintf(buf, n, "%s is steady (%s).", what[m], b);
		return;
	}
	l = (size_t)snprintf(buf, n, "%s %s from %s to %s", what[m],
			     t->state == LS_STEP ? (down ? "stepped down" : "stepped up") : down ? "fell" : "rose", a, b);
	if (t->state == LS_DRIFT && l < n)
		l += (size_t)snprintf(buf + l, n - l, ", %s a day", s);
	if (l >= n)
		return;
	if (t->days_left == 0)
		snprintf(buf + l, n - l, ": a fault now (%s).", f);
	else if (t->days_left > 0)
		snprintf(buf + l, n - l, ": at this rate a fault (%s) in about %d day%s.", f, t->days_left,
			 t->days_left == 1 ? "" : "s");
	else if (t->state == LS_STEP)
		snprintf(buf + l, n - l, ", and stays there.");
	else
		snprintf(buf + l, n - l, ".");
}
