/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW Self-Healing Wi-Fi: the diagnosis (see include/ag/heal.h) */
#include <string.h>

#include "ag/heal.h"

void heal_params_default(struct heal_params *p)
{
	p->min_samples = 15;
	p->loss_pct = 5;
	p->delay_ms = 100;
	p->delay_factor = 3;
	p->weak_signal = -75;
	p->hops = 4;
	p->poor_loss_pct = 20;
	p->poor_delay_ms = 300;
}

int heal_weak_step(int prev, int signal, int weak_signal, int margin)
{
	if (!signal)
		return prev;                    /* nothing measured: as it was */
	if (signal <= weak_signal)
		return 1;
	if (signal > weak_signal + margin)
		return 2;
	return prev ? prev : 2;                 /* in the margin: unchanged */
}

static int is_weak(const struct heal_sta *s, const struct heal_params *p)
{
	if (s->weak)
		return s->weak == 1;
	return s->signal && s->signal <= p->weak_signal;
}

int heal_sta_loss_pct(const struct heal_sta *s)
{
	return s->samples > 0 ? (s->lost * 100 + s->samples / 2) / s->samples : 0;
}

int heal_sta_degraded(const struct heal_sta *s, const struct heal_params *p)
{
	if (s->samples < p->min_samples)
		return 0;
	if (heal_sta_loss_pct(s) >= p->loss_pct)
		return 1;
	/* slow: past the line and, once its usual delay is known, well past that too */
	return s->rtt_ms >= p->delay_ms && (s->base_rtt_ms <= 0 || s->rtt_ms >= p->delay_factor * s->base_rtt_ms);
}

static int poor(const struct heal_sta *s, const struct heal_params *p)
{
	return heal_sta_loss_pct(s) >= p->poor_loss_pct || s->rtt_ms >= p->poor_delay_ms;
}

void heal_diagnose(const struct heal_sta *s, int n, const struct heal_params *p, struct heal_result *r)
{
	int i, measured = 0, nd = 0, nweak = 0, all_lost = 1, any_poor = 0, hopping = -1;
	long samples = 0, lost = 0;
	int worst_rtt = -1, rtt_all = -1;

	r->status = HEAL_UNKNOWN;
	r->cause = HC_NONE;
	r->affected = 0;
	if (n > 32)
		n = 32;
	for (i = 0; i < n; i++) {
		if (s[i].roams >= p->hops && (hopping < 0 || s[i].roams > s[hopping].roams))
			hopping = i;
		if (s[i].samples < p->min_samples)
			continue;
		measured++;
		samples += s[i].samples;
		lost += s[i].lost;
		if (s[i].lost < s[i].samples)
			all_lost = 0;
		if (s[i].rtt_ms > rtt_all)
			rtt_all = s[i].rtt_ms;
		if (!heal_sta_degraded(&s[i], p))
			continue;
		nd++;
		r->affected |= 1u << i;
		if (is_weak(&s[i], p))
			nweak++;
		if (poor(&s[i], p))
			any_poor = 1;
		if (s[i].rtt_ms > worst_rtt)
			worst_rtt = s[i].rtt_ms;
	}
	r->measured = measured;
	r->loss_pct = samples ? (int)((lost * 100 + samples / 2) / samples) : 0;
	r->rtt_ms = nd ? worst_rtt : rtt_all;

	/* no device answers at all, though several are awake: the radio itself */
	if (measured >= 2 && all_lost) {
		r->status = HEAL_POOR;
		r->cause = HC_RADIO;
		return;
	}
	/* a device going back and forth between the bands: fixable, so named first */
	if (hopping >= 0) {
		r->status = nd && any_poor ? HEAL_POOR : HEAL_DEGRADED;
		r->cause = HC_HOPPING;
		r->affected |= 1u << hopping;
		return;
	}
	if (!measured)
		return;                 /* unknown: nobody awake to measure */
	if (!nd) {
		r->status = HEAL_GOOD;
		return;
	}
	r->status = any_poor ? HEAL_POOR : HEAL_DEGRADED;
	if (nweak * 2 > nd)
		r->cause = HC_COVERAGE;         /* most of the affected are far */
	else if (nd == 1 && measured >= 2)
		r->cause = HC_DEVICE;           /* the others are fine */
	else
		r->cause = HC_CONGESTION;       /* fine signal, still losing: the air is busy */
}

const char *heal_status_name(enum heal_status st)
{
	static const char *const n[] = { "Unknown", "Good", "Degraded", "Poor" };

	return st <= HEAL_POOR ? n[st] : "Unknown";
}

const char *heal_cause_name(enum heal_cause c)
{
	static const char *const n[] = { "None", "Congestion", "WeakCoverage", "BandHopping", "Device", "Radio" };

	return c <= HC_RADIO ? n[c] : "None";
}

/* ---------- the healing report ---------- */

static long day_of(long wall_s)
{
	return wall_s > 0 ? wall_s / 86400 : 0;
}

struct heal_day *heal_report_day(struct heal_report *r, long wall_s)
{
	long day = day_of(wall_s);
	struct heal_day *d = &r->d[day % HEAL_DAYS];

	if (d->day != day) {
		memset(d, 0, sizeof(*d));
		d->day = day;
	}
	return d;
}

void heal_report_sum(const struct heal_report *r, long wall_s, int days, struct heal_day *out)
{
	long today = day_of(wall_s);
	int i, k;

	memset(out, 0, sizeof(*out));
	out->day = today;
	if (days > HEAL_DAYS)
		days = HEAL_DAYS;
	for (i = 0; i < HEAL_DAYS; i++) {
		const struct heal_day *d = &r->d[i];

		if (!d->day || d->day > today || d->day <= today - days)
			continue;
		for (k = 0; k < 4; k++)
			out->secs[k] += d->secs[k];
		for (k = 0; k < HEAL_CAUSES; k++)
			out->found[k] += d->found[k];
		out->kept += d->kept;
		out->tried += d->tried;
		out->band_rules += d->band_rules;
		out->fixes += d->fixes;
		out->fix_secs += d->fix_secs;
		out->loss_before += d->loss_before;
		out->loss_after += d->loss_after;
		out->rtt_before += d->rtt_before;
		out->rtt_after += d->rtt_after;
	}
}

unsigned heal_day_problems(const struct heal_day *d)
{
	unsigned n = 0;
	int k;

	for (k = 1; k < HEAL_CAUSES; k++)
		n += d->found[k];
	return n;
}

int heal_day_good_pct(const struct heal_day *d)
{
	unsigned long m = (unsigned long)d->secs[HEAL_GOOD] + d->secs[HEAL_DEGRADED] + d->secs[HEAL_POOR];

	return m ? (int)((d->secs[HEAL_GOOD] * 100UL + m / 2) / m) : -1;
}
