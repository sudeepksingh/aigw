/* SPDX-License-Identifier: Apache-2.0 */
/* Proven self-repair: predictions, verdicts, the evidence by day (ag/repair.h). Pure. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/repair.h"

static const struct repair_mdef mdefs[RM_N] = {
	[RM_NONE]       = { "", "", "", 0, 0, REPAIR_NONE },
	[RM_DEV_LOSS]   = { "DeviceLoss", "loss", "%", 0, 3, 5 },
	[RM_RADIO_LOSS] = { "RadioLoss", "devices' loss", "%", 0, 3, 5 },
	[RM_AIR]        = { "AirBusy", "air used by others", "%", 0, 5, REPAIR_NONE },
	[RM_FULL_MS]    = { "FullLineDelay", "delay while the line is full", "ms", 0, 15, REPAIR_NONE },
	[RM_SIGNAL]     = { "Signal", "signal", "dBm", 1, 4, REPAIR_NONE },
	[RM_RETRY]      = { "Retries", "sent again", "%", 0, 5, REPAIR_NONE },
	[RM_HOPS]       = { "BandChanges", "band changes in 10 min", "", 0, 2, 1 },
};

const struct repair_mdef *repair_metric(int m)
{
	return m > RM_NONE && m < RM_N ? &mdefs[m] : NULL;
}

const char *repair_metric_name(int m)
{
	return m > RM_NONE && m < RM_N ? mdefs[m].name : "";
}

int repair_metric_by_name(const char *name)
{
	int m;

	for (m = RM_NONE + 1; name && m < RM_N; m++)
		if (!strcmp(mdefs[m].name, name))
			return m;
	return RM_NONE;
}

void repair_value(int m, float v, char *buf, size_t n)
{
	const struct repair_mdef *d = repair_metric(m);

	if (!n)
		return;
	if (!d || v <= REPAIR_NONE + 1) {
		snprintf(buf, n, "-");
		return;
	}
	if (!strcmp(d->unit, "%"))
		snprintf(buf, n, v < 10 && fabsf(v - roundf(v)) >= 0.05f ? "%.1f%%" : "%.0f%%", v);
	else if (!d->unit[0])
		snprintf(buf, n, "%.0f", v);
	else
		snprintf(buf, n, "%.0f %s", v, d->unit);
}

static const char *const rnames[RR_N] = {
	"Pending", "Helped", "NoChange", "Worse", "Unknown", "Needed", "NotNeeded",
};
static const char *const rwords[RR_N] = {
	"being measured", "helped", "no change", "made it worse", "not known", "needed", "not needed",
};

const char *repair_result_name(int r)
{
	return r >= 0 && r < RR_N ? rnames[r] : "Unknown";
}

int repair_result_by_name(const char *s)
{
	int r;

	for (r = 0; s && r < RR_N; r++)
		if (!strcmp(rnames[r], s))
			return r;
	return RR_UNKNOWN;
}

const char *repair_result_words(int r)
{
	return r >= 0 && r < RR_N ? rwords[r] : "not known";
}

void repair_params_default(struct repair_params *p)
{
	p->min_samples = 3;
	p->min_rel = 0.2f;
}

static int known(float v)
{
	return v > REPAIR_NONE + 1;
}

/* ---------- before: the prediction ---------- */

void repair_predict(struct repair_pred *p, float hint)
{
	const struct repair_mdef *d = repair_metric(p->metric);
	float e;

	if (!d || !known(p->before)) {
		p->expected = d && known(hint) ? hint : REPAIR_NONE;
		return;
	}
	if (known(hint))
		e = hint;
	else if (known(d->target))
		/* back to good, but no better than this home's normal for the hour */
		e = known(p->normal) && (d->higher_better ? p->normal < d->target : p->normal > d->target) ? p->normal :
		    d->target;
	else if (known(p->normal))
		e = d->higher_better ? p->normal - d->effect : p->normal + d->effect;
	else
		e = d->higher_better ? p->before + 2 * d->effect : p->before / 2;
	/* never a prediction of worse than now: then it is "stays at" */
	if (d->higher_better ? e < p->before : e > p->before)
		e = p->before;
	p->expected = e;
}

static void within_words(long s, char *buf, size_t n)
{
	if (s <= 0)
		snprintf(buf, n, "when its trial is judged");
	else if (s < 120)
		snprintf(buf, n, "within %ld s", s);
	else if (s < 7200)
		snprintf(buf, n, "within %ld min", (s + 30) / 60);
	else
		snprintf(buf, n, "within %ld h", (s + 1800) / 3600);
}

void repair_pred_words(const struct repair_pred *p, const char *subject, char *buf, size_t n)
{
	const struct repair_mdef *d = repair_metric(p->metric);
	char b[24], e[24], w[40];
	size_t l = 0;

	if (!n)
		return;
	buf[0] = '\0';
	if (subject && *subject)
		l = (size_t)snprintf(buf, n, "%s: ", subject);
	if (l >= n)
		return;
	if (!d) {
		snprintf(buf + l, n - l, "nothing it can be measured on");
		return;
	}
	within_words(p->within_s, w, sizeof(w));
	repair_value(p->metric, p->before, b, sizeof(b));
	repair_value(p->metric, p->expected, e, sizeof(e));
	if (!known(p->before) && !known(p->expected))
		snprintf(buf + l, n - l, "%s not measured before the fix; judged on what follows", d->words);
	else if (!known(p->before))
		snprintf(buf + l, n - l, "%s not measured before the fix; expected %s %s %s", d->words,
			 d->higher_better ? "at least" : "under", e, w);
	else if (fabsf(p->expected - p->before) < 0.01f)
		snprintf(buf + l, n - l, "%s %s, expected to stay so %s", d->words, b, w);
	else
		snprintf(buf + l, n - l, "%s %s -> %s %s %s", d->words, b, d->higher_better ? "about" : "under", e, w);
}

/* ---------- after: the verdict ---------- */

static int unknown_why(const struct repair_obs *o, const struct repair_pred *p, const struct repair_params *P,
		       struct repair_verdict *v)
{
	if (o->by_person)
		snprintf(v->why, sizeof(v->why), "undone by a person before it could be judged");
	else if (o->gone)
		snprintf(v->why, sizeof(v->why), "it left before it could be judged: not a result of the fix");
	else if (o->confounded)
		snprintf(v->why, sizeof(v->why), "an area outage while it was measured: says nothing about the fix");
	else if (!repair_metric(p->metric))
		snprintf(v->why, sizeof(v->why), "nothing it can be measured on");
	else if (!known(p->before))
		snprintf(v->why, sizeof(v->why), "not measured before the fix");
	else if (!known(o->after) || (o->trial < 0 && o->n < P->min_samples))
		snprintf(v->why, sizeof(v->why), "too little measured after it (%d of %d)", o->n, P->min_samples);
	else
		return 0;
	v->result = RR_UNKNOWN;
	return 1;
}

/* the value after, taken back to the hour of the value before (a busier hour isn't the fix's) */
static float adjusted(const struct repair_pred *p, const struct repair_obs *o, float *shift)
{
	*shift = known(p->normal) && known(o->normal_after) ? o->normal_after - p->normal : 0;
	return o->after - *shift;
}

int repair_judge(const struct repair_pred *p, const struct repair_obs *o, const struct repair_params *P,
		 struct repair_verdict *v)
{
	const struct repair_mdef *d = repair_metric(p->metric);
	float shift, adj, gain, rel;
	char b[24], a[24], e[24], s[24];
	int better, worse, hb;

	memset(v, 0, sizeof(*v));
	if (unknown_why(o, p, P, v))
		return v->result;
	hb = d->higher_better;
	adj = adjusted(p, o, &shift);
	gain = hb ? adj - p->before : p->before - adj;
	v->gain = gain;
	v->gain_pct = fabsf(p->before) > 0.01f && !hb ? gain / fabsf(p->before) * 100 : 0;
	/* a share of the value before; dBm and counts near zero have no share: the effect alone */
	rel = !hb && p->before > 0 ? P->min_rel * p->before : 0;
	better = gain >= d->effect && gain >= rel;
	worse = -gain >= d->effect && -gain >= rel;
	v->result = better ? RR_HELPED : worse ? RR_WORSE : RR_NONE;
	v->met = known(p->expected) && (hb ? adj >= p->expected - 0.01f : adj <= p->expected + 0.01f);
	repair_value(p->metric, p->before, b, sizeof(b));
	repair_value(p->metric, o->after, a, sizeof(a));
	repair_value(p->metric, p->expected, e, sizeof(e));
	{
		size_t l = (size_t)snprintf(v->why, sizeof(v->why), "%s %s -> %s", d->words, b, a);

		if (known(p->expected) && l < sizeof(v->why))
			l += (size_t)snprintf(v->why + l, sizeof(v->why) - l, " (expected %s %s%s)",
					      hb ? "about" : "under", e, v->met ? ", met" : "");
		if (fabsf(shift) >= d->effect / 2 && l < sizeof(v->why)) {
			repair_value(p->metric, fabsf(shift), s, sizeof(s));
			l += (size_t)snprintf(v->why + l, sizeof(v->why) - l, "; the hour is %s than the one before by %s "
					      "normally, counted", (shift > 0) != (hb != 0) ? "worse" : "better", s);
		}
		/* its own trial measured it more closely: it decides between helped and no change */
		if (o->trial == 1 && v->result == RR_NONE) {
			v->result = RR_HELPED;
			if (l < sizeof(v->why))
				snprintf(v->why + l, sizeof(v->why) - l, "; its trial found it clearly better");
		} else if (o->trial == 0 && v->result == RR_HELPED) {
			v->result = RR_NONE;
			if (l < sizeof(v->why))
				snprintf(v->why + l, sizeof(v->why) - l, "; not clearly better, its trial found");
		}
	}
	return v->result;
}

int repair_judge_shadow(const struct repair_pred *p, const struct repair_obs *o, const struct repair_params *P,
			struct repair_verdict *v)
{
	const struct repair_mdef *d = repair_metric(p->metric);
	float shift, adj, goal;
	char a[24], e[24];
	int hb, still;

	memset(v, 0, sizeof(*v));
	if (unknown_why(o, p, P, v))
		return v->result;
	hb = d->higher_better;
	adj = adjusted(p, o, &shift);
	goal = known(p->expected) ? p->expected : hb ? p->before + d->effect : p->before - d->effect;
	still = hb ? adj < goal - 0.01f : adj > goal + 0.01f;
	v->gain = hb ? adj - p->before : p->before - adj;      /* what changed without the fix */
	v->result = still ? RR_NEEDED : RR_NOT_NEEDED;
	repair_value(p->metric, o->after, a, sizeof(a));
	repair_value(p->metric, goal, e, sizeof(e));
	if (still)
		snprintf(v->why, sizeof(v->why), "without the fix, %s was still %s (with it, %s %s was expected): the fix "
			 "was needed", d->words, a, hb ? "about" : "under", e);
	else
		snprintf(v->why, sizeof(v->why), "without the fix, %s went to %s by itself: the fix was not needed",
			 d->words, a);
	return v->result;
}

/* ---------- the evidence ---------- */

static struct repair_day *slot(struct repair_row *r, long day)
{
	struct repair_day *s = &r->d[(day % REPAIR_DAYS + REPAIR_DAYS) % REPAIR_DAYS];

	if (s->day != day) {
		memset(s, 0, sizeof(*s));
		s->day = day;
	}
	return s;
}

static void inc(uint16_t *c)
{
	if (*c < 0xffff)
		(*c)++;
}

void repair_count(struct repair_row *r, long day, int shadow, const struct repair_verdict *v, int undone,
		  float predicted_gain)
{
	struct repair_day *s = slot(r, day);

	if (shadow) {
		inc(&s->shadows);
		if (v->result == RR_NEEDED)
			inc(&s->needed);
		else if (v->result == RR_NOT_NEEDED)
			inc(&s->not_needed);
		if (known(predicted_gain))
			s->shadow_gain_sum += predicted_gain;
		return;
	}
	switch (v->result) {
	case RR_HELPED: inc(&s->helped); break;
	case RR_NONE: inc(&s->none); break;
	case RR_WORSE: inc(&s->worse); break;
	default: inc(&s->unknown); break;
	}
	if (v->result == RR_HELPED || v->result == RR_NONE || v->result == RR_WORSE)
		s->gain_pct_sum += v->gain_pct;
	if (undone)
		inc(&s->undone);
}

void repair_count_call(struct repair_row *r, long day)
{
	inc(&slot(r, day)->calls);
}

void repair_sum_rates(struct repair_sum *s)
{
	s->judged = s->helped + s->none + s->worse;
	s->fixes = s->judged + s->unknown;
	s->fix_rate = s->judged ? (int)((s->helped * 100 + s->judged / 2) / s->judged) : -1;
	s->undo_rate = s->fixes ? (int)((s->undone * 100 + s->fixes / 2) / s->fixes) : -1;
	s->mean_gain_pct = s->judged ? s->gain_pct_sum / (float)s->judged : 0;
	s->shadow_gain = s->shadows ? s->shadow_gain_sum / (float)s->shadows : 0;
}

void repair_sum_add(struct repair_sum *to, const struct repair_sum *s)
{
	to->helped += s->helped;
	to->none += s->none;
	to->worse += s->worse;
	to->unknown += s->unknown;
	to->undone += s->undone;
	to->calls += s->calls;
	to->shadows += s->shadows;
	to->needed += s->needed;
	to->not_needed += s->not_needed;
	to->gain_pct_sum += s->gain_pct_sum;
	to->shadow_gain_sum += s->shadow_gain_sum;
}

void repair_sum_row(const struct repair_row *r, long today, int days, struct repair_sum *out)
{
	int i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < REPAIR_DAYS; i++) {
		const struct repair_day *s = &r->d[i];

		if (!s->day || s->day > today || s->day <= today - days)
			continue;
		out->helped += s->helped;
		out->none += s->none;
		out->worse += s->worse;
		out->unknown += s->unknown;
		out->undone += s->undone;
		out->calls += s->calls;
		out->shadows += s->shadows;
		out->needed += s->needed;
		out->not_needed += s->not_needed;
		out->gain_pct_sum += s->gain_pct_sum;
		out->shadow_gain_sum += s->shadow_gain_sum;
	}
	repair_sum_rates(out);
}

int repair_gate_ok(const struct repair_sum *month, int min_rate, int min_trials, char *why, size_t n)
{
	if ((int)month->judged < min_trials) {
		snprintf(why, n, "measured on %u of the %d trials the confidence gate needs", month->judged, min_trials);
		return 0;
	}
	if (month->fix_rate < min_rate) {
		snprintf(why, n, "fix rate %d%% over %u trials, below the %d%% the confidence gate needs", month->fix_rate,
			 month->judged, min_rate);
		return 0;
	}
	snprintf(why, n, "fix rate %d%% over %u trials (the gate: %d%% over %d)", month->fix_rate, month->judged, min_rate,
		 min_trials);
	return 1;
}

int repair_confidence(const struct repair_sum *month)
{
	return month->judged >= 3 ? month->fix_rate : -1;
}
