/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW self-tuning smart queueing: bufferbloat found, fixed, and the fix
 * proved, by the gateway itself.
 *
 * When the line is full, a router's queues fill and everything else waits
 * behind the download: calls stutter, games lag, pages hang. Smart queueing
 * (SQM: cake, or htb with fq_codel) shapes the line just below its speed, so
 * the queue stays in the gateway, where it is kept short and fair. Elsewhere
 * someone has to know to turn it on and what speed to give it.
 *
 * AIGW's timeline (ag/why.h) sees the bloat: the internet answering slowly
 * whenever the line is full. It proposes SQM (advise mode) or tries it
 * (enforce), at the line's measured speed, and then compares, from the
 * timeline: the delay while the line was full, and the download speed then,
 * before and after. Kept only when the delay is clearly lower and downloads
 * kept most of their speed; otherwise undone, and not tried again for a day.
 *
 * Pure (no I/O): agd (agd/sqm.c) samples, applies (scripts/aigw-sqm) and
 * judges; this decides. Unit test: tests/test_sqm.c.
 */
#ifndef AG_SQM_H
#define AG_SQM_H

#include <stddef.h>
#include <stdint.h>

#include "ag/why.h"

struct sqm_params {
	int window;                     /* samples looked at for bloat (60: an hour) */
	int detect;                     /* full and slow on at least this many of them (10) */
	int slow_pct;                   /* ... and that share of the full ones (50%) */
	int trial;                      /* full samples needed after it is on, to judge (30) */
	int grace;                      /* samples after it went on that aren't counted (2) */
	int keep_pct;                   /* kept: the delay while full at most this share of before (60%) */
	int keep_ms;                    /* ... or at most the normal plus this (30 ms) */
	int speed_pct;                  /* ... and downloads at least this share of before (75%) */
	int line_pct;                   /* shaped at this share of the line's speed (90%) */
	int peak_pct;                   /* ... or of the most seen, when the speed isn't known (85%) */
};

/* the samples on which the line was full: how many, the internet's median delay, the median download */
struct sqm_stats {
	int n;
	int ms;                         /* -1 none answered */
	uint32_t kbps;
};

void sqm_params_default(struct sqm_params *p);
/* the line is full in s (the speed sold or tested; else near the most seen) */
struct sqm_line {
	float down_mbps;                /* sold, else tested; 0 not known */
	float peak_down_mbps, peak_up_mbps;
	float normal_ms;                /* the internet's normal delay (0 not known) */
	int have_normal;
};
/* the stats of the full samples among m[0..n) (any order) with from <= t < to */
void sqm_stats_of(const struct why_min *m, int n, long from, long to, const struct sqm_line *l, struct sqm_stats *out);
/* bloat in the last p->window samples before `now` (the newest first or any order): 1 with the words */
int sqm_bloated(const struct why_min *m, int n, long now, int step, const struct sqm_line *l, const struct sqm_params *p,
		char *why, size_t wn);
/* the rates to shape at (kbit/s); up 0: upload not shaped (not known). -1 nothing to go by */
int sqm_rates(const struct sqm_line *l, const struct sqm_params *p, uint32_t *down_kbps, uint32_t *up_kbps);

enum { SQ_WAIT, SQ_KEEP, SQ_UNDO };
/* before (the day before it went on) and after (since): wait, keep or undo, in words */
int sqm_judge(const struct sqm_stats *before, const struct sqm_stats *after, const struct sqm_line *l,
	      const struct sqm_params *p, char *why, size_t wn);

#endif
