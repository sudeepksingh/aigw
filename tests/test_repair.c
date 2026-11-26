/* SPDX-License-Identifier: Apache-2.0 */
/* Proven self-repair: predictions, verdicts, shadows, the evidence and the confidence gate (ag/repair.h) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/care.h"
#include "ag/repair.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N REPAIR_NONE
#define TODAY 20800L

static struct repair_params P;

static struct repair_pred pred(int m, float before, float normal, float hint, long within)
{
	struct repair_pred p;

	memset(&p, 0, sizeof(p));
	p.metric = m;
	p.before = before;
	p.normal = normal;
	p.within_s = within;
	repair_predict(&p, hint);
	return p;
}

static struct repair_obs obs(float after, int n)
{
	struct repair_obs o;

	memset(&o, 0, sizeof(o));
	o.after = after;
	o.n = n;
	o.normal_after = N;
	o.trial = -1;
	return o;
}

static void predictions(void)
{
	struct repair_pred p;
	char w[200];

	printf("== before: the prediction\n");
	p = pred(RM_DEV_LOSS, 26, N, N, 600);
	repair_pred_words(&p, "OnePlus", w, sizeof(w));
	CHECK(fabsf(p.expected - 5) < 0.01f && !strcmp(w, "OnePlus: loss 26% -> under 5% within 10 min"),
	      "a device's loss: back to good (5%%): \"%s\"", w);
	p = pred(RM_DEV_LOSS, 26, 8, N, 600);
	CHECK(fabsf(p.expected - 8) < 0.01f, "no better than this home's normal for the hour (8%%): %.1f", p.expected);
	p = pred(RM_DEV_LOSS, 26, 1, N, 600);
	CHECK(fabsf(p.expected - 5) < 0.01f, "a normal better than good: good is enough (5%%): %.1f", p.expected);
	p = pred(RM_AIR, 40, N, 28, 0);
	repair_pred_words(&p, "Wi-Fi 5 GHz (wlan0)", w, sizeof(w));
	CHECK(fabsf(p.expected - 28) < 0.01f && strstr(w, "40% -> under 28% when its trial is judged"),
	      "the proposer's own figure (a plan's gain): \"%s\"", w);
	p = pred(RM_FULL_MS, 180, 20, N, 0);
	CHECK(fabsf(p.expected - 35) < 0.01f, "the line: its normal and an allowance (20 + 15 ms): %.0f", p.expected);
	p = pred(RM_FULL_MS, 180, N, N, 0);
	CHECK(fabsf(p.expected - 90) < 0.01f, "nothing known: half the way (90 ms): %.0f", p.expected);
	p = pred(RM_SIGNAL, -78, N, -60, 120);
	repair_pred_words(&p, "Pixel", w, sizeof(w));
	CHECK(fabsf(p.expected + 60) < 0.01f && strstr(w, "signal -78 dBm -> about -60 dBm within 2 min"),
	      "higher is better: what the target hears (-60 dBm): \"%s\"", w);
	p = pred(RM_DEV_LOSS, 2, N, N, 600);
	repair_pred_words(&p, "TV", w, sizeof(w));
	CHECK(fabsf(p.expected - 2) < 0.01f && strstr(w, "expected to stay so"),
	      "already good: never a prediction of worse, it stays: \"%s\"", w);
	p = pred(RM_DEV_LOSS, N, N, N, 600);
	repair_pred_words(&p, NULL, w, sizeof(w));
	CHECK(p.expected <= N + 1 && strstr(w, "not measured before the fix"), "nothing measured: nothing expected: \"%s\"", w);
	p = pred(RM_HOPS, 4, N, N, 600);
	CHECK(fabsf(p.expected - 1) < 0.01f, "band changes: at most one in 10 minutes: %.0f", p.expected);
	CHECK(repair_metric_by_name("DeviceLoss") == RM_DEV_LOSS && repair_metric_by_name("x") == RM_NONE &&
	      !strcmp(repair_metric_name(RM_FULL_MS), "FullLineDelay"), "metric names both ways");
	repair_value(RM_DEV_LOSS, 2.5f, w, sizeof(w));
	CHECK(!strcmp(w, "2.5%"), "a value with its unit: %s", w);
	repair_value(RM_DEV_LOSS, N, w, sizeof(w));
	CHECK(!strcmp(w, "-"), "a value not measured: %s", w);
}

static void verdicts(void)
{
	struct repair_pred p = pred(RM_DEV_LOSS, 26, N, N, 600);
	struct repair_obs o;
	struct repair_verdict v;

	printf("== after: the verdict\n");
	o = obs(3, 10);
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_HELPED && v.met && fabsf(v.gain - 23) < 0.01f && fabsf(v.gain_pct - 88.5f) < 0.1f,
	      "26%% -> 3%%: helped, as predicted, 88%% better: %s", v.why);
	o = obs(12, 10);
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_HELPED && !v.met, "26%% -> 12%%: helped, but not to 5%%: %s", v.why);
	o = obs(24, 10);
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_NONE, "26%% -> 24%%: no change: %s", v.why);
	o = obs(40, 10);
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_WORSE && v.gain < 0, "26%% -> 40%%: made it worse: %s", v.why);
	o = obs(3, 2);
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_UNKNOWN && strstr(v.why, "too little"), "2 measurements: unknown: %s", v.why);
	o = obs(3, 10);
	o.gone = 1;
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_UNKNOWN && strstr(v.why, "left"), "the device left: unknown, not a result: %s", v.why);
	o = obs(3, 10);
	o.confounded = 1;
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_UNKNOWN && strstr(v.why, "area outage"), "an area outage meanwhile: unknown: %s", v.why);
	o = obs(3, 10);
	o.by_person = 1;
	repair_judge(&p, &o, &P, &v);
	CHECK(v.result == RR_UNKNOWN && strstr(v.why, "person"), "undone by a person first: unknown: %s", v.why);
	{
		struct repair_pred q = pred(RM_DEV_LOSS, N, N, N, 600);

		o = obs(3, 10);
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_UNKNOWN && strstr(v.why, "before"), "not measured before: unknown: %s", v.why);
	}
	/* small numbers: an effect, not just a share */
	{
		struct repair_pred q = pred(RM_DEV_LOSS, 4, N, N, 600);

		o = obs(2, 10);
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_NONE, "4%% -> 2%%: half, but under the 3 points that count: no change: %s", v.why);
	}
	{
		struct repair_pred q = pred(RM_DEV_LOSS, 0, N, N, 600);

		o = obs(6, 10);
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_WORSE, "0%% -> 6%%: worse (a share of nothing is no bar): %s", v.why);
	}
	{
		struct repair_pred q = pred(RM_SIGNAL, -78, N, -60, 120);

		o = obs(-62, 5);
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_HELPED && !v.met && fabsf(v.gain - 16) < 0.01f, "signal -78 -> -62 dBm: helped: %s", v.why);
		o = obs(-84, 5);
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_WORSE, "signal -78 -> -84 dBm: worse: %s", v.why);
	}
	printf("== against the home's normal for that hour\n");
	{
		/* the hour of the fix is normally at 8%%: the next one at 16%% (busier), or at 2%% (quieter) */
		struct repair_pred q = pred(RM_DEV_LOSS, 14, 8, N, 600);

		o = obs(13, 10);
		o.normal_after = 16;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_HELPED && strstr(v.why, "worse than the one before"),
		      "14%% -> 13%% into an hour 8 points worse by nature: helped: %s", v.why);
		o = obs(8, 10);
		o.normal_after = 2;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_NONE, "14%% -> 8%% into an hour 6 points better by nature: no change: %s", v.why);
	}
	printf("== its own trial decides between helped and no change\n");
	{
		struct repair_pred q = pred(RM_RADIO_LOSS, 9, N, N, 180);

		o = obs(7, 1);
		o.trial = 1;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_HELPED && strstr(v.why, "trial"), "9%% -> 7%%, kept by its trial: helped: %s", v.why);
		o = obs(4, 1);
		o.trial = 0;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_NONE, "9%% -> 4%%, its trial found it not clearly better: no change: %s", v.why);
		o = obs(20, 1);
		o.trial = 1;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_WORSE, "9%% -> 20%%, kept by its trial: still worse by the numbers: %s", v.why);
		o = obs(N, 0);
		o.trial = -1;
		o.gone = 1;
		repair_judge(&q, &o, &P, &v);
		CHECK(v.result == RR_UNKNOWN, "its trial couldn't judge it: unknown");
	}
}

static void shadows(void)
{
	struct repair_pred p = pred(RM_DEV_LOSS, 26, N, N, 600);
	struct repair_obs o;
	struct repair_verdict v;

	printf("== advise mode: shadows, never applied\n");
	o = obs(24, 10);
	repair_judge_shadow(&p, &o, &P, &v);
	CHECK(v.result == RR_NEEDED && strstr(v.why, "was needed"), "without the fix, still 24%%: needed: %s", v.why);
	o = obs(2, 10);
	repair_judge_shadow(&p, &o, &P, &v);
	CHECK(v.result == RR_NOT_NEEDED && strstr(v.why, "by itself"), "went to 2%% by itself: not needed: %s", v.why);
	o = obs(2, 10);
	o.gone = 1;
	repair_judge_shadow(&p, &o, &P, &v);
	CHECK(v.result == RR_UNKNOWN, "the device left: a shadow says nothing either");
	CHECK(!strcmp(repair_result_name(RR_NOT_NEEDED), "NotNeeded") && repair_result_by_name("NoChange") == RR_NONE,
	      "result names both ways");
}

static struct repair_verdict verdict(int r, float pct)
{
	struct repair_verdict v;

	memset(&v, 0, sizeof(v));
	v.result = r;
	v.gain_pct = pct;
	return v;
}

static void evidence(void)
{
	struct repair_row row;
	struct repair_sum s, t;
	struct repair_verdict v;
	char why[160];
	int i;

	printf("== the evidence: by day, over the last day, 7 and 30 days\n");
	memset(&row, 0, sizeof(row));
	v = verdict(RR_HELPED, 80);
	repair_count(&row, TODAY, 0, &v, 0, N);
	repair_count(&row, TODAY, 0, &v, 0, N);
	v = verdict(RR_NONE, 5);
	repair_count(&row, TODAY - 3, 0, &v, 1, N);
	v = verdict(RR_WORSE, -50);
	repair_count(&row, TODAY - 10, 0, &v, 1, N);
	v = verdict(RR_UNKNOWN, 0);
	repair_count(&row, TODAY, 0, &v, 0, N);
	v = verdict(RR_NEEDED, 0);
	repair_count(&row, TODAY, 1, &v, 0, 21);
	repair_count_call(&row, TODAY);
	repair_sum_row(&row, TODAY, 1, &s);
	CHECK(s.fixes == 3 && s.judged == 2 && s.helped == 2 && s.unknown == 1 && s.fix_rate == 100 && s.calls == 1 &&
	      s.shadows == 1 && s.needed == 1 && fabsf(s.shadow_gain - 21) < 0.01f,
	      "the day: 3 fixes, 2 judged and helped (100%%), 1 unknown, a call avoided, a shadow needed");
	repair_sum_row(&row, TODAY, 7, &s);
	CHECK(s.judged == 3 && s.fix_rate == 67 && s.undone == 1 && s.undo_rate == 25 && fabsf(s.mean_gain_pct - 55) < 0.01f,
	      "7 days: 2 of 3 helped (67%%), 1 of 4 undone (25%%), mean improvement 55%%: %d%% %d%% %.1f",
	      s.fix_rate, s.undo_rate, s.mean_gain_pct);
	repair_sum_row(&row, TODAY, 30, &s);
	CHECK(s.judged == 4 && s.fix_rate == 50 && s.worse == 1, "30 days: 2 of 4 helped (50%%)");
	repair_sum_row(&row, TODAY + 31, 30, &s);
	CHECK(s.fixes == 0 && s.fix_rate == -1 && s.undo_rate == -1, "a month later: nothing left, rates not known");
	/* bounded: 31 days in a ring, a day's slot reused */
	v = verdict(RR_HELPED, 10);
	repair_count(&row, TODAY + REPAIR_DAYS, 0, &v, 0, N);
	repair_sum_row(&row, TODAY + REPAIR_DAYS, 1, &s);
	CHECK(s.fixes == 1 && s.helped == 1, "the same slot 31 days later starts afresh");
	memset(&row, 0, sizeof(row));
	for (i = 0; i < 70000; i++)
		repair_count(&row, TODAY, 0, &v, 0, N);
	repair_sum_row(&row, TODAY, 1, &s);
	CHECK(s.helped == 0xffff, "a day's count stops at its top (no wrap to 0): %u", s.helped);
	/* summing kinds */
	memset(&row, 0, sizeof(row));
	v = verdict(RR_HELPED, 40);
	repair_count(&row, TODAY, 0, &v, 0, N);
	repair_sum_row(&row, TODAY, 7, &s);
	repair_sum_row(&row, TODAY, 7, &t);
	repair_sum_add(&s, &t);
	repair_sum_rates(&s);
	CHECK(s.judged == 2 && s.fix_rate == 100 && fabsf(s.mean_gain_pct - 40) < 0.01f, "two rows summed");

	printf("== the confidence gate (off unless the ISP turns it on: agd's part)\n");
	memset(&s, 0, sizeof(s));
	s.helped = 8;
	s.none = 1;
	s.worse = 1;
	repair_sum_rates(&s);
	CHECK(repair_gate_ok(&s, 80, 10, why, sizeof(why)) == 1, "8 of 10 helped (80%%), 80%% over 10 needed: may act: %s", why);
	CHECK(repair_gate_ok(&s, 90, 10, why, sizeof(why)) == 0 && strstr(why, "below"), "90%% needed: held: %s", why);
	CHECK(repair_gate_ok(&s, 50, 20, why, sizeof(why)) == 0 && strstr(why, "10 of the 20"), "20 trials needed: held: %s", why);
	s.unknown = 30;
	repair_sum_rates(&s);
	CHECK(repair_gate_ok(&s, 80, 11, why, sizeof(why)) == 0, "unknowns are not trials: still 10 judged");
	CHECK(repair_confidence(&s) == 80, "a shadow's confidence: the measured fix rate (80%%)");
	memset(&s, 0, sizeof(s));
	s.helped = 2;
	repair_sum_rates(&s);
	CHECK(repair_confidence(&s) == -1, "fewer than 3 judged: no confidence claimed");
}

static void normal_rule(void)
{
	struct care_norm h[24], all;
	float mean, dev;
	int i;

	printf("== the home's normal: care's rule, for another measure\n");
	memset(h, 0, sizeof(h));
	memset(&all, 0, sizeof(all));
	CHECK(care_norm_blend(&h[20], &all, &mean, &dev) < 0, "nothing learned: no normal");
	for (i = 0; i < 40; i++)
		care_norm_add(&h[i % 2 ? 20 : 3], &all, i % 2 ? 10.0f : 2.0f);
	CHECK(care_norm_blend(&h[20], &all, &mean, &dev) == 0 && mean > 6 && mean < 10, "20:00 busier than the day: %.1f", mean);
	CHECK(care_norm_blend(&h[3], &all, &mean, &dev) == 0 && mean > 2 && mean < 6, "03:00 quieter: %.1f", mean);
}

int main(void)
{
	repair_params_default(&P);
	predictions();
	verdicts();
	shadows();
	evidence();
	normal_rule();
	if (failed) {
		printf("%d FAILED\n", failed);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
