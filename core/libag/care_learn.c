/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW proactive care: early warning, the home's normal learned (see ag/care.h) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/care.h"

#define HOUR_WINDOW 84          /* samples an hour's normal remembers: two weeks of one hour */
#define ALL_WINDOW  1008        /* the whole day's: a week of checks every 10 min */
#define SHRINK      12.0f       /* an hour with fewer samples than this leans on the whole day */
#define MIN_ALL     6           /* samples before there is a normal at all */

static const char *const names[CARE_METRICS] = { "NextHopLatency", "InternetLatency", "InternetLoss", "DNSLatency",
						  "DownloadSpeed" };

void care_learn_params_default(struct care_learn_params *p)
{
	/* a check counts at most 3 deviations: 12 is an hour of clearly worse, or 4 hours of a bit worse */
	static const float h[CARE_METRICS] = { 12, 12, 12, 12, 3 };
	static const float fl[CARE_METRICS] = { 2, 3, 1, 5, 0 };
	static const float ef[CARE_METRICS] = { 10, 15, 2, 40, 0 };
	static const float fa[CARE_METRICS] = { 100, 150, 5, 300, 0 };

	memset(p, 0, sizeof(*p));
	p->learn_s = 3 * 86400;
	p->k = 0.5f;
	memcpy(p->h, h, sizeof(h));
	memcpy(p->floor, fl, sizeof(fl));
	memcpy(p->effect, ef, sizeof(ef));
	memcpy(p->fault, fa, sizeof(fa));
	p->horizon_days = 14;
	p->min_days = 4;
	p->r2 = 0.6f;
}

const char *care_metric_name(int m)
{
	return m >= 0 && m < CARE_METRICS ? names[m] : "";
}

int care_metric_area(int m)
{
	return m == CM_DNS_MS ? CA_DNS : CA_INTERNET;
}

void care_metric_value(int m, float v, char *buf, size_t n)
{
	if (m == CM_LOSS)
		snprintf(buf, n, "%.1f%%", v);
	else if (m == CM_SPEED)
		snprintf(buf, n, "%.0f Mbit/s", v);
	else
		snprintf(buf, n, "%.0f ms", v);
}

/* the speed is worse when it is lower; everything else when it is higher */
static float worse_by(int m, float x, float normal)
{
	return m == CM_SPEED ? normal - x : x - normal;
}

/* the speed's floor and effect are a share of its normal */
static float floor_of(int m, float normal, const struct care_learn_params *p)
{
	return m == CM_SPEED ? 0.05f * normal + 1 : p->floor[m];
}

static float effect_of(int m, float normal, const struct care_learn_params *p)
{
	return m == CM_SPEED ? 0.2f * normal : p->effect[m];
}

int care_normal(const struct care_learn *L, int m, int hour, float *mean, float *dev)
{
	const struct care_norm *a, *b;
	float wb;

	if (m < 0 || m >= CARE_METRICS || hour < 0 || hour > 23)
		return -1;
	a = &L->m[m].all;
	b = &L->m[m].hour[hour];
	if (a->n < MIN_ALL)
		return -1;
	wb = (float)b->n;
	*mean = (wb * b->mean + SHRINK * a->mean) / (wb + SHRINK);
	*dev = (wb * b->dev + SHRINK * a->dev) / (wb + SHRINK);
	return 0;
}

int care_normal_for(const struct care_learn *L, int m, int hour, long now, const struct care_learn_params *p,
		    float *mean, float *dev, int *fleet)
{
	float om = 0, od = 0, w = 0;
	int own = care_normal(L, m, hour, &om, &od) == 0, b = hour / 6;

	*fleet = 0;
	if (!p->prior || m < 0 || m >= CARE_METRICS || !p->prior->have[m] || hour < 0 || hour > 23 ||
	    !care_learning(L, now, p)) {
		if (!own)
			return -1;
		*mean = om;
		*dev = od;
		return 0;
	}
	/* the fleet's normal hands over to the home's own as its learning goes on */
	if (own && L->started && p->learn_s > 0)
		w = (float)(now - L->started) / (float)p->learn_s;
	if (w > 1)
		w = 1;
	*mean = w * om + (1 - w) * p->prior->mean[m][b];
	*dev = w * od + (1 - w) * p->prior->dev[m][b];
	*fleet = 1;
	return 0;
}

static void learn(struct care_norm *c, float x, unsigned window, float weight)
{
	float a;

	if (!c->n) {
		c->mean = x;
		c->dev = 0;
		c->n = 1;
		return;
	}
	if (c->n < window)
		c->n++;
	a = weight / (float)c->n;
	c->dev += a * (fabsf(x - c->mean) - c->dev);
	c->mean += a * (x - c->mean);
}

int care_learning(const struct care_learn *L, long now, const struct care_learn_params *p)
{
	return !L->started || now - L->started < p->learn_s;
}

/* the days before today with enough samples: worse by per_day a day, how well it fits */
static int trend(const struct care_mstate *M, int m, long day, float *per_day, float *r2, float *level, float *excess,
		 int *ndays)
{
	float x[CARE_TREND_DAYS], y[CARE_TREND_DAYS], mx = 0, my = 0, sxx = 0, sxy = 0, syy = 0;
	unsigned min_n = m == CM_SPEED ? 1 : 24;
	long newest = 0;
	int i, k = 0;

	for (i = 0; i < CARE_TREND_DAYS; i++) {
		const struct care_mday *d = &M->days[i];

		if (!d->n || d->n < min_n || d->day >= day || d->day < day - (CARE_TREND_DAYS - 1))
			continue;
		x[k] = (float)(d->day - day);
		/* the level, turned so that higher is worse */
		y[k] = m == CM_SPEED ? -(d->level_sum / (float)d->n) : d->level_sum / (float)d->n;
		if (d->day > newest) {
			newest = d->day;
			*level = d->level_sum / (float)d->n;
			*excess = d->excess_sum / (float)d->n;
		}
		k++;
	}
	*ndays = k;
	if (k < 2 || newest < day - 2)
		return -1;
	for (i = 0; i < k; i++) {
		mx += x[i];
		my += y[i];
	}
	mx /= (float)k;
	my /= (float)k;
	for (i = 0; i < k; i++) {
		sxx += (x[i] - mx) * (x[i] - mx);
		sxy += (x[i] - mx) * (y[i] - my);
		syy += (y[i] - my) * (y[i] - my);
	}
	if (sxx <= 0)
		return -1;
	*per_day = sxy / sxx;
	*r2 = syy > 0 ? sxy * sxy / (sxx * syy) : 0;
	return 0;
}

struct cand {
	int metric, kind, predict, days, fleet;
	float normal, current, per_day, strength;
};

/* what a metric shows now: a trend first (it says when), else a shift; 0 none */
static int candidate(const struct care_learn *L, int m, int hour, long day, long now, const struct care_learn_params *p,
		     struct cand *c)
{
	const struct care_mstate *M = &L->m[m];
	float mean, dev, per_day = 0, r2 = 0, level = 0, excess = 0, eff;
	int nd = 0, fleet;

	if (care_normal_for(L, m, hour, now, p, &mean, &dev, &fleet) < 0)
		return 0;
	eff = effect_of(m, mean, p);
	memset(c, 0, sizeof(*c));
	c->metric = m;
	c->normal = mean;
	c->fleet = fleet;
	if (fleet)
		eff *= 2;               /* another home's normal: only a clear difference */
	if (!fleet && trend(M, m, day, &per_day, &r2, &level, &excess, &nd) == 0 && nd >= p->min_days && per_day > 0 &&
	    r2 >= p->r2 && excess >= eff / 2 && per_day * (float)(nd - 1) >= eff / 2) {
		float fault = p->fault[m], left;

		c->predict = -1;
		if (fault > 0) {
			left = m == CM_SPEED ? level - fault : fault - level;
			c->predict = left <= 0 ? 0 : (int)ceilf(left / per_day);
		}
		/* a speed with nothing to compare with: a steady fall of 3% a day is worth saying */
		if ((c->predict >= 0 && c->predict <= p->horizon_days) ||
		    (c->predict < 0 && m == CM_SPEED && per_day >= 0.03f * mean)) {
			c->kind = CW_TREND;
			c->current = level;
			c->per_day = per_day;
			c->days = nd;
			c->strength = 100.0f - (float)(c->predict < 0 ? p->horizon_days : c->predict);
			return 1;
		}
	}
	if (M->cusum >= p->h[m] && M->recent >= eff) {
		c->kind = CW_SHIFT;
		c->predict = -1;
		/* the value as measured lately, not the normal less the excess: the normal has moved since */
		c->current = M->level > 0 ? M->level : 0;
		c->strength = M->cusum / p->h[m];
		return 1;
	}
	return 0;
}

/* is the open warning still so (with some slack, so it doesn't flap) */
static int still(const struct care_learn *L, int hour, long day, long now, const struct care_learn_params *p)
{
	const struct care_warning *w = &L->w;
	const struct care_mstate *M = &L->m[w->metric];
	float mean, dev;
	struct cand c;
	int fleet;

	if (candidate(L, w->metric, hour, day, now, p, &c))
		return 1;
	if (care_normal_for(L, w->metric, hour, now, p, &mean, &dev, &fleet) < 0)
		return 0;
	if (w->kind == CW_SHIFT)
		return M->cusum > p->h[w->metric] / 4 && M->recent >= effect_of(w->metric, mean, p) / 2;
	return 0;
}

static void take(struct care_warning *w, const struct cand *c)
{
	w->metric = c->metric;
	w->kind = c->kind;
	w->normal = c->normal;
	w->current = c->current;
	w->per_day = c->per_day;
	w->days = c->days;
	w->predict = c->predict;
	w->fleet = c->fleet;
}

int care_learn_step(struct care_learn *L, const struct care_sample *s, int hour, long day, long now, unsigned quiet_areas,
		    const struct care_learn_params *p)
{
	struct cand best, c;
	int m, ev = 0, have = 0, learning;

	memset(&best, 0, sizeof(best));
	if (hour < 0 || hour > 23)
		return 0;
	if (!L->started)
		L->started = now;
	learning = care_learning(L, now, p);
	for (m = 0; m < CARE_METRICS; m++) {
		struct care_mstate *M = &L->m[m];
		float x = s->v[m], mean, dev, scale, excess, z;
		int quiet = (quiet_areas >> care_metric_area(m)) & 1, slot, fleet;

		if (!s->have[m] || !isfinite(x) || x < 0)
			continue;
		if (care_normal_for(L, m, hour, now, p, &mean, &dev, &fleet) == 0) {
			scale = 1.25f * dev;
			if (scale < floor_of(m, mean, p))
				scale = floor_of(m, mean, p);
			excess = worse_by(m, x, mean);
			z = excess / scale;
			if (z > 3)
				z = 3;          /* one far-off check is a blip, not hours */
			if ((!learning || fleet) && !quiet) {
				M->cusum += z - p->k;
				if (M->cusum < 0)
					M->cusum = 0;
				if (M->cusum > 3 * p->h[m])
					M->cusum = 3 * p->h[m];
				/* a check every 10 min is smoothed; the speed, tested once a night, is the last test's */
				if (M->level == 0 && M->recent == 0)
					M->level = x;
				else
					M->level += (m == CM_SPEED ? 1.0f : 0.25f) * (x - M->level);
				M->recent += (m == CM_SPEED ? 1.0f : 0.25f) * (excess - M->recent);
			}
			slot = (int)(day % CARE_TREND_DAYS);
			if (M->days[slot].day != day) {
				memset(&M->days[slot], 0, sizeof(M->days[slot]));
				M->days[slot].day = day;
			}
			if (!quiet) {
				M->days[slot].level_sum += x;
				M->days[slot].excess_sum += excess;
				M->days[slot].n++;
			}
			/* a far-off value counts as at most 6 deviations from the normal (the home's own:
			   another home's normal must not pull this one's towards it) */
			if (!fleet && x > mean + 6 * scale)
				x = mean + 6 * scale;
			if (!fleet && x < mean - 6 * scale)
				x = mean - 6 * scale;
		}
		if (quiet)
			continue;       /* a fault is never normal */
		learn(&M->hour[hour], x, HOUR_WINDOW, L->w.active && L->w.metric == m ? 0.1f : 1.0f);
		learn(&M->all, x, ALL_WINDOW, L->w.active && L->w.metric == m ? 0.1f : 1.0f);
	}
	if (learning && !p->prior)
		return 0;
	for (m = 0; m < CARE_METRICS; m++) {
		if ((quiet_areas >> care_metric_area(m)) & 1)
			continue;
		if (candidate(L, m, hour, day, now, p, &c) && (!have || c.strength > best.strength)) {
			best = c;
			have = 1;
		}
	}
	if (!L->w.active) {
		if (have) {
			memset(&L->w, 0, sizeof(L->w));
			L->w.active = 1;
			L->w.since = now;
			take(&L->w, &best);
			ev |= CW_OPENED;
		}
		return ev;
	}
	if (still(L, hour, day, now, p) && !((quiet_areas >> care_metric_area(L->w.metric)) & 1)) {
		if (have && best.metric == L->w.metric)
			take(&L->w, &best);     /* the same metric: its newest numbers */
		return ev;
	}
	if (have && best.metric != L->w.metric) {
		take(&L->w, &best);
		L->w.since = now;
		return ev | CW_CHANGED;
	}
	L->w.duration = now - L->w.since;
	L->w.active = 0;
	L->last_area = care_metric_area(L->w.metric);
	L->last_since = L->w.since;
	L->last_end = now;
	return ev | CW_CLEARED;
}

static void text_of(const struct care_warning *w, const struct care_learn_params *p, char *buf, size_t n);

void care_warning_text(const struct care_warning *w, const struct care_learn_params *p, char *buf, size_t n)
{
	size_t len;

	text_of(w, p, buf, n);
	len = strlen(buf);
	if (w->fleet && len < n)
		snprintf(buf + len, n - len, " The normal is from the fleet (homes like this one): this gateway is still "
			 "learning its own.");
}

static void text_of(const struct care_warning *w, const struct care_learn_params *p, char *buf, size_t n)
{
	static const char *const what[CARE_METRICS] = { "The next router's delay", "The internet's delay",
		"Packet loss to the internet", "The time names take to resolve (DNS)", "The download speed" };
	char now[24], normal[24], step[24], fault[24];
	const char *wh = w->metric >= 0 && w->metric < CARE_METRICS ? what[w->metric] : "The line";

	care_metric_value(w->metric, w->current, now, sizeof(now));
	care_metric_value(w->metric, w->normal, normal, sizeof(normal));
	if (w->kind == CW_SHIFT) {
		if (w->metric == CM_SPEED)
			snprintf(buf, n, "%s is %s, against a normal %s: slower than usual in the last tests, though not a "
				 "fault yet.", wh, now, normal);
		else
			snprintf(buf, n, "%s is %s, against a normal %s at this hour: worse than usual for hours, though not "
				 "a fault yet.", wh, now, normal);
		return;
	}
	care_metric_value(w->metric, w->per_day, step, sizeof(step));
	care_metric_value(w->metric, p->fault[w->metric], fault, sizeof(fault));
	if (w->predict > 0)
		snprintf(buf, n, "%s has got worse for %d days (now %s, normal %s; %s a day): at this rate it is a fault "
			 "(%s) in about %d day%s.", wh, w->days, now, normal, step, fault, w->predict, w->predict == 1 ? "" : "s");
	else if (w->predict == 0)
		snprintf(buf, n, "%s has got worse for %d days (now %s, normal %s; %s a day), and is at the fault level "
			 "(%s).", wh, w->days, now, normal, step, fault);
	else
		snprintf(buf, n, "%s has got worse for %d days (now %s, normal %s; %s a day).", wh, w->days, now, normal,
			 step);
}

const char *care_warning_advice(int metric)
{
	switch (metric) {
	case CM_GW_MS: return "Check the line to the home (modem, cable, fibre) before it fails: its delay is rising.";
	case CM_INET_MS: return "Check the ISP's network beyond the home: congestion is growing at these hours.";
	case CM_LOSS: return "The line starts to lose packets: check it (signal levels, connectors) before it fails.";
	case CM_DNS_MS: return "The DNS servers answer slower than usual: check their load or use closer ones.";
	case CM_SPEED: return "The line's speed is falling: check its sync rate and the access network.";
	}
	return "";
}
