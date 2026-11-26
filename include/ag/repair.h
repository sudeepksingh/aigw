/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Proven self-repair: every fix AIGW makes carries a prediction, is measured
 * afterwards, and is judged; a fix that didn't help is undone. The receipts
 * and the fix rate are the evidence an ISP looks at before it lets AIGW act.
 *
 *   before   the prediction: a metric of a device or a radio, its value now,
 *            the value expected after the fix, and by when
 *            ("OnePlus: loss 26% -> under 5% within 10 min")
 *   after    the metric measured over the window, against the value before and
 *            the home's normal for that hour (a busier hour is not the fix's
 *            fault): helped, no change, made worse, or unknown (the device
 *            left, an area outage, too little measured, a person undid it)
 *   shadow   in advise mode, what AIGW would have done: the same prediction,
 *            and whether the problem stayed without the fix (needed) or went
 *            by itself (not needed). Never applied, and marked so.
 *   stats    per fault and kind of fix, by day: the fix rate (helped of those
 *            judged; unknowns are left out), the mean improvement, the undo
 *            rate; over the last day, 7 and 30 days
 *   gate     a policy: act on its own only with kinds of fix whose measured fix
 *            rate is at least X% over at least N judged trials
 *
 * Pure (no I/O): agd (agd/repair.c) measures and acts; this predicts, judges,
 * counts and words it. Unit test: tests/test_repair.c.
 */
#ifndef AG_REPAIR_H
#define AG_REPAIR_H

#include <stddef.h>
#include <stdint.h>

#define REPAIR_NONE  (-9999.0f)  /* a value not measured */
#define REPAIR_DAYS  31

/* what a fix is judged on */
enum repair_metric {
	RM_NONE,
	RM_DEV_LOSS,            /* a device's Wi-Fi loss (%): band steering, a device kept on 2.4 GHz */
	RM_RADIO_LOSS,          /* the devices' loss on an access point (%): a channel change */
	RM_AIR,                 /* air used by others on the channel (%): a planned channel move */
	RM_FULL_MS,             /* the internet's delay while the line is full (ms): smart queueing */
	RM_SIGNAL,              /* a device's signal (dBm, higher is better): a mesh steer */
	RM_RETRY,               /* frames sent again (%): a mesh radio's channel */
	RM_HOPS,                /* a device's band changes in 10 minutes: a device kept on 2.4 GHz */
	RM_N
};

struct repair_mdef {
	const char *name;       /* DeviceLoss, ... (the data model's words) */
	const char *words;      /* "loss", ... */
	const char *unit;       /* "%", "ms", "dBm" */
	int higher_better;
	float effect;           /* the smallest change that counts */
	float target;           /* a good value (the expected one, unless the caller knows better); NONE */
};
const struct repair_mdef *repair_metric(int m);       /* NULL outside the table */
const char *repair_metric_name(int m);                /* "" for RM_NONE */
int repair_metric_by_name(const char *name);          /* RM_NONE when unknown */
void repair_value(int m, float v, char *buf, size_t n);       /* "26%", "180 ms", "-78 dBm", "-" */

enum repair_result {
	RR_PENDING, RR_HELPED, RR_NONE, RR_WORSE, RR_UNKNOWN,   /* a fix applied */
	RR_NEEDED, RR_NOT_NEEDED,                               /* a shadow: not applied */
	RR_N
};
const char *repair_result_name(int r);  /* Pending, Helped, NoChange, Worse, Unknown, Needed, NotNeeded */
int repair_result_by_name(const char *s);
const char *repair_result_words(int r); /* "helped", "no change", "made it worse", ... */

struct repair_params {
	int min_samples;        /* measurements after the fix needed to judge (3) */
	float min_rel;          /* and the change must be at least this share of the value before (0.2) */
};
void repair_params_default(struct repair_params *p);

/* the prediction, made before the fix */
struct repair_pred {
	int metric;
	float before;           /* REPAIR_NONE: not measured */
	float expected;         /* REPAIR_NONE: none (nothing measured, nothing to expect) */
	long within_s;          /* the deadline, from when it took effect */
	float normal;           /* the home's normal for that hour; REPAIR_NONE: not known */
};
/*
 * Fills in what to expect: the caller's own value when it has one (a channel plan's
 * gain, what the target access point hears), else the metric's good value, else
 * half the way there; never worse than the value now. hint: REPAIR_NONE for none.
 */
void repair_predict(struct repair_pred *p, float hint);
/* "OnePlus: loss 26% -> under 5% within 10 min" (subject NULL: without it) */
void repair_pred_words(const struct repair_pred *p, const char *subject, char *buf, size_t n);

/* what was seen after it */
struct repair_obs {
	float after;            /* the mean over the window; REPAIR_NONE: not measured */
	int n;                  /* measurements */
	float normal_after;     /* the normal at that hour; REPAIR_NONE */
	int gone;               /* the device or radio left */
	int confounded;         /* an area outage while it was measured */
	int by_person;          /* a person undid it before it could be judged */
	int trial;              /* a trial of its own decided: 1 kept, 0 undone, -1 none (or not judged) */
};

struct repair_verdict {
	int result;             /* enum repair_result */
	float gain;             /* better by (in the metric's unit; positive is better) */
	float gain_pct;         /* ... as a share of the value before (0 when that is 0) */
	int met;                /* the prediction came true */
	char why[200];
};
/* a fix that was applied: helped / no change / worse / unknown */
int repair_judge(const struct repair_pred *p, const struct repair_obs *o, const struct repair_params *P,
		 struct repair_verdict *v);
/* a shadow (not applied): was the fix needed, measured without it */
int repair_judge_shadow(const struct repair_pred *p, const struct repair_obs *o, const struct repair_params *P,
			struct repair_verdict *v);

/* ---------- the evidence, per fault and kind of fix, by day ---------- */

struct repair_day {
	long day;               /* days since 1970 (UTC); 0: empty */
	uint16_t helped, none, worse, unknown, undone, calls;
	uint16_t shadows, needed, not_needed;
	float gain_pct_sum;     /* of the judged ones (helped, none, worse) */
	float shadow_gain_sum;  /* predicted gain of the shadows (the metric's unit) */
};

struct repair_row {
	char fault[24];         /* the alert or ticket code: wifi-congestion, WIFI_HOPPING, ... */
	char action[16];        /* channel, stay24, sqm, meshsteer, meshchan */
	uint32_t id;            /* its instance number (data model) */
	struct repair_day d[REPAIR_DAYS];       /* d[day % REPAIR_DAYS] */
};

/* one judged receipt (shadow 1: a shadow's verdict); undone: AIGW undid it */
void repair_count(struct repair_row *r, long day, int shadow, const struct repair_verdict *v, int undone,
		  float predicted_gain);
void repair_count_call(struct repair_row *r, long day);        /* a call it likely avoided */

struct repair_sum {
	unsigned fixes;         /* judged or not: every fix applied */
	unsigned judged;        /* helped + none + worse */
	unsigned helped, none, worse, unknown, undone, calls;
	unsigned shadows, needed, not_needed;
	int fix_rate;           /* % helped of those judged; -1 none judged */
	int undo_rate;          /* % undone of the fixes; -1 no fixes */
	float mean_gain_pct;    /* of those judged */
	float shadow_gain;      /* mean predicted gain of the shadows */
	float gain_pct_sum, shadow_gain_sum;    /* (the sums the means come from) */
};
/* the last `days` days up to today (1, 7, 30) */
void repair_sum_row(const struct repair_row *r, long today, int days, struct repair_sum *out);
void repair_sum_add(struct repair_sum *to, const struct repair_sum *s);  /* then repair_sum_rates */
void repair_sum_rates(struct repair_sum *s);

/* the confidence gate: 1 may act on its own; 0 not, why in words */
int repair_gate_ok(const struct repair_sum *month, int min_rate, int min_trials, char *why, size_t n);
/* how sure a shadow can be: the measured fix rate (%), -1 when fewer than 3 judged */
int repair_confidence(const struct repair_sum *month);

#endif
