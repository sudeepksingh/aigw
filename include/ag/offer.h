/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW signals for the ISP: what a home would buy, or why it may leave,
 * from what the gateway saw, with the evidence.
 *
 * An ISP sells by guessing: a mesh system to whoever calls about Wi-Fi, a
 * faster plan to everyone. The gateway knows which homes fill their line
 * every evening, whose devices spend hours on a weak signal, whose calls
 * stutter while someone downloads, and which homes are having a bad month.
 * It also knows when the line itself delivers less than it should, and then
 * the answer is a repair, not a sale.
 *
 *   PLAN_UPGRADE    the line full for half an hour or more on 3 of the last 7
 *                   days: a faster plan (not when the line underdelivers)
 *   SMART_QUEUE     everything else slowed while the line was full, most of
 *                   that time: smart queueing (SQM) keeps calls and games
 *                   responsive, without a faster plan
 *   MESH_WIFI       two or more devices on a weak signal for half an hour a
 *                   day (or one, and many short drops), on 3 of 7 days: mesh
 *                   Wi-Fi or an extender
 *   RETENTION_RISK  a bad week: the experience below 90%, 5 points down on
 *                   the week before, or many problems: call before they do
 *   LINE_REPAIR     the nightly speed test below the line's speed on 2 nights
 *                   or more, or (v0.54) the line's own numbers degrading day by
 *                   day: the ISP owes a repair (and no faster plan meanwhile)
 *
 * Pure (no I/O): agd (agd/offers.c) counts each day; this judges the days.
 * Unit test: tests/test_offer.c.
 */
#ifndef AG_OFFER_H
#define AG_OFFER_H

#include <stddef.h>
#include <stdint.h>

#include "ag/care.h"

#define OFFER_DAYS 31
#define OFFER_WEAK 16                   /* devices remembered per day (their weak time) */

struct offer_dev {
	uint8_t mac[6];
	uint16_t pad;
	uint32_t secs;
};

/* one day of the home (days since 1970, UTC, like proactive care's) */
struct offer_day {
	long day;                       /* 0: empty */
	uint32_t watched_s;             /* sampled */
	uint32_t full_s;                /* the line used to its speed */
	uint32_t full_evening_s;        /* ... of it 18:00 to 23:59, local time */
	uint32_t full_slow_s;           /* ... while the internet answered slowly (the rest suffered) */
	uint32_t peak_kbps;             /* the busiest sample's download */
	uint32_t drops;                 /* devices that dropped off Wi-Fi and were back within 2 min */
	uint32_t weak_s;                /* device-seconds on a weak signal */
	struct offer_dev weak[OFFER_WEAK];
};

struct offer_params {
	int days;                       /* the window (7) */
	int min_days;                   /* days watched (6 h or more) before anything is said (3) */
	uint32_t watched_min_s;         /* a day counts with this much sampled (6 h) */
	uint32_t plan_full_s;           /* PLAN_UPGRADE: full this long a day (30 min) */
	int plan_days;                  /* ... on this many days (3) */
	uint32_t sqm_slow_s;            /* SMART_QUEUE: full and slow this long a day (15 min) */
	int sqm_pct;                    /* ... and that much of the full time (50%) */
	int sqm_days;                   /* ... on this many days (3) */
	uint32_t mesh_dev_s;            /* MESH_WIFI: a device weak this long a day (30 min) */
	int mesh_devices;               /* ... this many devices (2) */
	uint32_t mesh_drops;            /* ... or one, and this many short drops a day (10) */
	int mesh_days;                  /* ... on this many days (3) */
	int churn_score;                /* RETENTION_RISK: experience below this (90%) */
	int churn_drop;                 /* ... or this many points below the week before (5) */
	unsigned churn_problems;        /* ... or this many problems (4) */
	unsigned repair_low;            /* LINE_REPAIR: speed tests below the line's speed (2) */
};

enum { OF_PLAN, OF_SQM, OF_MESH, OF_RETAIN, OF_REPAIR, OF_N };
enum { OC_LOW, OC_MEDIUM, OC_HIGH };

struct offer_in {
	long today;                     /* day number */
	const struct offer_day *d;      /* any order; days outside the window ignored */
	int nd;
	const struct care_day *c;       /* proactive care's days (any order) */
	int nc;
	float line_mbps;                /* the line's speed: sold, else tested; 0 not known */
	int line_sold;                  /* ... it is what was sold (care.speed_mbps) */
	int sqm_on;                     /* AIGW runs smart queueing already: no SMART_QUEUE */
	/* proven repair, the window's fixes AIGW made itself: helped of those judged (all; Wi-Fi's own) */
	unsigned proven_fixes, proven_judged, wifi_fixes, wifi_judged;
	/* v0.54: the line degrades (line health, ag/line.h): its words, else NULL. A repair, not a sale */
	const char *line_degrading;
	/* a device's name, else NULL (only for the words that stay home) */
	const char *(*name)(const uint8_t mac[6], void *ctx);
	void *ctx;
};

struct offer {
	int active;
	const char *code;               /* PLAN_UPGRADE, ... */
	const char *kind;               /* upsell, retention, repair */
	const char *product;            /* "faster plan", ... */
	int confidence;                 /* enum: OC_* */
	int days, of_days;              /* days it held, of the days watched in the window */
	long since;                     /* the first of them (wall s); 0 */
	char evidence[400];             /* in words, without names: what may leave the home */
	char named[200];                /* the devices, by name (MESH_WIFI): stays home */
};

struct offer_result {
	int watched;                    /* days watched in the window */
	int learning;                   /* fewer than min_days: nothing is said yet */
	struct offer o[OF_N];
};

void offer_params_default(struct offer_params *p);
void offer_judge(const struct offer_in *in, const struct offer_params *p, struct offer_result *out);
const char *offer_confidence_name(int c);       /* low, medium, high */
const char *offer_code(int k);                  /* PLAN_UPGRADE ... */
/* a device's weak time on a day (kept in the day's table: the weakest stay when it is full) */
void offer_day_weak(struct offer_day *d, const uint8_t mac[6], uint32_t secs);

#endif
