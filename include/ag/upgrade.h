/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW firmware upgrades that check themselves. A firmware handler only knows
 * whether the new image booted; AIGW knows whether the home works as well on
 * it as before. Pure (no I/O): agd (agd/upgrade.c) records the home before an
 * upgrade, measures it after, and rolls back; this judges. Unit test:
 * tests/test_upgrade.c.
 *
 *   before   when the new image is activated: the versions and images, the
 *            devices connected then (active in the last minutes), the share
 *            of the last 7 days without a problem, the mean nightly speed,
 *            whether the internet answered
 *   after    from the first start on the new image: which of those devices
 *            came back, every proactive care check (problem or not, internet
 *            or not, the delays, loss and DNS time beside the home's normal
 *            for that hour), the speed tests, how often agd started again
 *
 * Worse at once (no need to wait for the end of the watch):
 *   devices   at least `missing_min` and `missing_pct` % of them not back
 *             within `reconnect_s` (an upgrade breaking a Wi-Fi chip's driver,
 *             WPA3 for old devices, a LAN port)
 *   internet  none at all for `offline_s` since the boot, every check (at
 *             least 3) without it, while it worked before
 *   restarts  agd (or the gateway) started more than `max_starts` times
 * Worse at the end of the watch (`watch_s`), on enough samples:
 *   delays    the next hop, the internet or DNS `slower_x` times their normal
 *             and at least `slower_ms` more; loss `slower_ms[CM_LOSS]` points more
 *   speed     the nightly test under `speed_pct` % of the week before
 *   score     the time without a problem `score_drop` points under the week
 *             before (both with at least `score_min_s` watched)
 * Otherwise the upgrade is kept.
 */
#ifndef AG_UPGRADE_H
#define AG_UPGRADE_H

#include <stddef.h>
#include <stdint.h>

#include "ag/care.h"

#define UP_DEVS    64
#define UP_METRICS 4            /* CM_GW_MS, CM_INET_MS, CM_LOSS, CM_DNS_MS (ag/care.h) */

enum up_state {
	UP_NONE,                /* no upgrade watched */
	UP_ACTIVATED,           /* the new image boots next: the home recorded */
	UP_WATCHING,            /* running the new image, measuring */
	UP_KEPT,                /* no worse: kept */
	UP_WORSE,               /* worse, and not rolled back (not allowed, or nothing to go back to) */
	UP_ROLLING_BACK,        /* worse: the previous image boots next */
	UP_ROLLED_BACK,         /* worse: back on the previous image */
	UP_NOT_BOOTED,          /* the new image never ran: still on the old one */
};

struct up_before {
	long at;                /* activated (wall s) */
	char from[64], to[64];  /* versions */
	uint32_t from_image, to_image;
	int online;             /* the internet answered on the last check before: 1, 0, -1 not known */
	int ndev;
	char dev[UP_DEVS][18];  /* MACs */
	char name[UP_DEVS][32];
	uint8_t wifi[UP_DEVS];
	unsigned watched_s, ok_s;       /* the 7 days before (proactive care) */
	float speed_mbps;       /* their mean nightly speed; 0 none */
};

struct up_after {
	long booted;            /* the first start on the new image (wall s) */
	int starts;             /* agd starts on it (1: none since) */
	uint8_t back[UP_DEVS];  /* the device came back */
	long back_at[UP_DEVS];
	long last_check;
	unsigned watched_s, ok_s;
	unsigned checks, offline;       /* care checks; of them, without the internet */
	long online_at;         /* the first with it; 0 none yet */
	double v[UP_METRICS], norm[UP_METRICS];  /* sums: measured, and the normal then */
	unsigned n[UP_METRICS];
	double speed_sum;
	unsigned speed_n;
};

struct up_params {
	long watch_s;           /* how long the new image is watched (86400) */
	long reconnect_s;       /* devices have this long to come back (1800) */
	long offline_s;         /* no internet at all this long after the boot (900) */
	int max_starts;         /* more starts than this: worse (3) */
	int missing_min;        /* devices not back, at least (2) */
	int missing_pct;        /* ... and this share of them (25) */
	unsigned min_samples;   /* checks with a normal before a delay counts (6) */
	float slower_x;         /* delays this many times the normal (1.5) */
	float slower_ms[UP_METRICS];    /* and this much more (ms; loss: points) */
	int speed_pct;          /* the speed under this % of before (80) */
	int score_drop;         /* points of the time without a problem (10) */
	long score_min_s;       /* watched, before and after, for the score to count (4 h) */
};

/* why it is worse (bits) */
#define UPW_DEVICES   0x001
#define UPW_INTERNET  0x002
#define UPW_RESTARTS  0x004
#define UPW_NEXTHOP   0x008
#define UPW_LATENCY   0x010
#define UPW_LOSS      0x020
#define UPW_DNS       0x040
#define UPW_SPEED     0x080
#define UPW_SCORE     0x100
#define UPW_N         9

enum { UPV_WATCHING, UPV_KEEP, UPV_WORSE };

struct up_verdict {
	int result;             /* UPV_* */
	unsigned why;           /* UPW_* */
	int back, missing, missing_wifi;        /* devices */
	int score_before, score_after;          /* -1 not known */
	char text[900];
};

void up_params_default(struct up_params *p);
const char *up_state_name(int state);   /* None, Activated, Watching, Kept, Worse, RollingBack, RolledBack, NotBooted */
const char *up_why_name(unsigned bit);  /* DevicesMissing, NoInternet, Restarts, NextHopLatency, ... */
/* the reasons as names, comma-separated */
void up_why_names(unsigned why, char *buf, size_t n);

/* the first start on the new image */
void up_after_start(struct up_after *a, long now);
/* a device was seen (Wi-Fi, traffic, DNS, DHCP); returns its index in before if it just came back, else -1 */
int up_after_seen(const struct up_before *b, struct up_after *a, const char *mac, long now);
/* one proactive care check: problem open, internet answered, what it measured,
   the normal at this hour (have[m] 0: not known) */
void up_after_check(struct up_after *a, long now, int problem, int online, const struct care_sample *s,
		    const struct care_sample *normal, long max_gap);
void up_after_speed(struct up_after *a, float mbps);
/* the time without a problem, %; -1 nothing watched */
int up_score(unsigned watched_s, unsigned ok_s);

/* the verdict now: WATCHING (no reason yet, the watch not over), WORSE, or KEEP */
int up_judge(const struct up_before *b, const struct up_after *a, long now, const struct up_params *p,
	     struct up_verdict *v);

#endif
