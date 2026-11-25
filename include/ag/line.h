/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Line health: which way each of the line's numbers is going, day by day, and
 * when it would be a fault at that rate (docs/area-prediction.md). Pure (no
 * I/O): agd (agd/line.c) keeps the days and asks; cortex has the same rules
 * in Python (cortex/aigw_cortex/linehealth.py) for its fleet simulation. Unit
 * test: tests/test_line.c, and tests/test_areapredict.py checks that both give
 * the same answers.
 *
 * One value a day for each metric: a gauge's median (sync rate, noise margin,
 * optical level, next-hop delay and loss, the speed test), a counter's rate
 * (FEC and CRC errors an hour), a count (retrains and short drops a day).
 * Of the last 14 complete days:
 *
 *   change point  the day the line stepped worse: the split where the median
 *                 after is furthest above the median before, in robust
 *                 deviations (MAD), each side 3 days or more
 *   trend         a Theil-Sen slope (the median of every pair's slope: one bad
 *                 day can't tilt it) and Kendall's tau (how steadily it goes
 *                 one way), on the days after the change point if there is one
 *   level, base   where the trend line is today; the median before it began
 *   fault         where it is a fault: fixed (3 dB of noise margin, -27 dBm
 *                 of light) or a share of the base (70% of the sync rate)
 *   days left     until the fault at this rate; 0 there already
 *   confidence    0-100, from tau and the days (a trend) or how clear the
 *                 step is (a change point)
 */
#ifndef AG_LINE_H
#define AG_LINE_H

#include <stddef.h>
#include <stdint.h>

#define LINE_METRICS 9
#define LINE_DAYS    21         /* days kept */
#define LINE_WINDOW  14         /* days a trend looks at */

enum line_metric { LM_SYNC, LM_MARGIN, LM_OPTICAL, LM_FEC, LM_CRC, LM_DROPS, LM_LATENCY, LM_LOSS, LM_SPEED };
enum line_state { LS_NONE, LS_LEARNING, LS_OK, LS_STEP, LS_DRIFT, LS_FAULT };

struct line_day {
	int32_t day;            /* local day number; 0 empty */
	float v;                /* the day's value */
	uint16_t n;             /* samples (or hours, for a rate) */
};

struct line_params {
	int min_days;           /* days before a trend is judged (5) */
	int window;             /* days looked at (14) */
	float cp_z;             /* a change point's least strength, robust deviations (4) */
	float tau;              /* a trend's least Kendall tau (0.5) */
	int horizon;            /* days left beyond this: not said (30) */
	float fault[LINE_METRICS];      /* > 0 (or < 0 for the optical level): this fault level instead */
};

struct line_trend {
	int state;              /* enum line_state */
	int ndays;              /* days with data in the window */
	float level, base;      /* now, and before it began (the metric's own units) */
	float slope;            /* a day, in its own units (signed) */
	float fault;            /* the fault level used; NAN none (its normal is past it already) */
	int32_t onset;          /* the day it began (a change point, else the first of the trend); 0 */
	int days_left;          /* to the fault at this rate; 0 there; -1 not known */
	int conf;               /* 0-100 */
};

void line_params_default(struct line_params *p);
/* the trend of metric m from its days (any order; today and later are left out) */
void line_analyse(const struct line_day *d, int n, long today, int m, const struct line_params *p,
		  struct line_trend *out);
const char *line_metric_name(int m);    /* SyncDown, NoiseMargin, OpticalPower, FECErrors, CRCErrors, Drops, ... */
const char *line_metric_key(int m);     /* sync, margin, optical, fec, crc, drops, latency, loss, speed */
const char *line_metric_unit(int m);    /* kbit/s, dB, dBm, /h, /h, /day, ms, %, Mbit/s */
int line_lower_worse(int m);
const char *line_state_name(int s);     /* "", Learning, OK, Stepped, Drifting, Fault */
const char *line_conf_name(int conf);   /* low, medium, high */
/* the trend in words: "The noise margin fell from 9.8 dB to 7.1 dB over 6 days (0.4 dB a day): at this rate it is
   a fault (3 dB) in about 9 days." */
void line_words(int m, const struct line_trend *t, char *buf, size_t n);

#endif
