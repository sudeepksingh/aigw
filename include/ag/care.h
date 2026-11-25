/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW proactive care: the gateway checks itself (Diagnose, without anyone
 * asking) and fixes problems, or tells the ISP, before the customer calls.
 * Pure (no I/O): agd (agd/care.c) runs the checks and sends the events; this
 * decides when a problem starts, whether the ISP must hear of it, when it is
 * over, and keeps the daily experience score. Unit test: tests/test_care.c.
 *
 * A problem is a verdict of warning or worse on `confirm` checks in a row
 * (one bad check is a blip); it is over after `clear` fine checks in a row.
 * Who deals with it:
 *   - the line, the ISP's network, DNS: the ISP. Reported at once.
 *   - Wi-Fi: AIGW itself (Self-Healing Wi-Fi). Reported only if it lasts
 *     `wifi_grace` seconds; otherwise it counts as fixed by AIGW.
 *   - a device, or AIGW's own action on one: nobody's to report.
 */
#ifndef AG_CARE_H
#define AG_CARE_H

#include <stddef.h>

#define CARE_DAYS  31
#define CARE_AREAS 5            /* Internet, DNS, WiFi, Device, AIGW */

enum care_area { CA_INTERNET, CA_DNS, CA_WIFI, CA_DEVICE, CA_AIGW };

struct care_params {
	int confirm;            /* bad checks in a row before it is a problem (2) */
	int clear;              /* fine checks in a row before it is over (2) */
	long wifi_grace;        /* s before a Wi-Fi problem is the ISP's to hear of (1800) */
	long max_gap;           /* longest time between two checks that counts as problem time (s) */
};

/* one check's verdict (Diagnose's first finding) */
struct care_verdict {
	const char *code;       /* e.g. "ISP_DOWN", "OK" */
	const char *area;       /* Internet, DNS, WiFi, Device, AIGW, All */
	int sev;                /* enum diag_sev (ag/diag.h): warning or worse is bad */
	const char *text;
};

struct care_state {
	int active;             /* a problem is open */
	int reported;           /* ... and the ISP was told */
	int bad_run, ok_run;    /* checks in a row */
	char code[24], area[12];
	int sev;
	char text[400];
	long since;             /* the problem's first bad check (wall s) */
	long last_check;        /* wall s; 0 none yet */
	long duration;          /* set on CARE_CLEARED: since -> the check that ended it */
	long pending;           /* s since the first bad check of a run not yet a problem */
	long acted_at;          /* wall s: AIGW last changed something on Wi-Fi (a channel, a device kept on 2.4 GHz) */
};

struct care_day {
	long day;               /* days since 1970-01-01 (UTC); 0 = empty */
	unsigned checks, good;  /* checks, and those without a problem */
	unsigned bad[CARE_AREAS];  /* bad checks by area */
	unsigned problems;      /* problems that started */
	unsigned reported;      /* told the ISP */
	unsigned fixed;         /* Wi-Fi problems AIGW changed something for, over before they had to be reported */
	unsigned problem_secs;  /* time a problem was open */
	unsigned warnings;      /* early warnings that started */
	unsigned warned;        /* problems an early warning had seen coming */
	unsigned lead_secs;     /* ... how long before (sum) */
	unsigned speed_tests;   /* nightly speed tests that ran */
	unsigned speed_kbps_sum, speed_kbps_min;
	unsigned speed_low;     /* ... below what the line must reach */
	/* v0.29.1: the time watched (between two checks, gaps while agd was down
	   left out), and of that the time without a problem: the score */
	unsigned watched_secs, ok_secs;
	/* v0.29.2: Wi-Fi problems over before they had to be reported, though AIGW
	   changed nothing (advise mode, nobody answered; or it passed): not AIGW's fix */
	unsigned cleared;
};

struct care_report {
	struct care_day d[CARE_DAYS];   /* d[day % CARE_DAYS] */
};

/* what a check changed (bits) */
#define CARE_STARTED  1         /* a problem started */
#define CARE_REPORT   2         /* tell the ISP now (ProblemDetected!) */
#define CARE_CLEARED  4         /* the problem is over (tell the ISP if it was reported: ProblemCleared!) */
#define CARE_CHANGED  8         /* the open problem has another cause now */

void care_params_default(struct care_params *p);
int care_area_index(const char *area);  /* -1: none (All) */
const char *care_area_name(int idx);
int care_bad(const struct care_verdict *v);
/* one check at wall time now: updates the state and the report; returns CARE_* bits */
int care_step(struct care_state *s, struct care_report *r, const struct care_verdict *v, long now,
	      const struct care_params *p);
/* AIGW changed something on Wi-Fi now: a channel trial took effect, a device
   was kept on 2.4 GHz. A Wi-Fi problem over before it had to be reported is
   "fixed by AIGW" only if AIGW acted during it (or up to CARE_ACTED_BEFORE s
   before its first bad check: the same trouble); otherwise "cleared by itself" */
#define CARE_ACTED_BEFORE 600
void care_acted(struct care_state *s, long now);
/* would this open problem be reported (the ISP's), or is it AIGW's to fix */
int care_isp_owned(const char *area);

struct care_day *care_report_day(struct care_report *r, long wall_s);
void care_report_sum(const struct care_report *r, long wall_s, int days, struct care_day *out);
/* the experience score: % of the time watched without a problem (checks are
   every care.interval when all is well, every care.recheck while something is
   wrong, so counting checks would weigh a bad minute like ten good ones); for
   days saved before v0.29.1, which have no times: % of checks without one.
   -1 nothing watched */
int care_score(const struct care_day *d);
/* the area with most bad checks; -1 none */
int care_main_cause(const struct care_day *d);

/*
 * Early warning: each home's normal, learned, and a slow drift seen before it
 * is a fault (core/libag/care_learn.c, unit-tested in tests/test_warn.c).
 *
 *   what      the next hop's delay, the internet's delay and loss, DNS's time
 *             (every check), the download speed (the nightly test)
 *   normal    per hour of the day (evenings are busier), each hour's own
 *             average blended with the whole day's while it has few samples;
 *             the average and the typical deviation, over about two weeks;
 *             nothing is learned while a problem is open (a fault is never
 *             normal), and slowly while a warning is
 *   shift     a CUSUM of how far each check is above the normal, in typical
 *             deviations: hours of "a bit worse than usual" add up
 *   trend     the day's average, day by day: steadily worse for 4 days or more
 *             (a straight line that fits), and when it would be a fault at
 *             that rate (within two weeks: a warning)
 * A warning needs a real difference too (e.g. 15 ms on the internet's delay),
 * and none comes in the first days (care.learn_days), or for what is a
 * problem already.
 */
#define CARE_METRICS    5
#define CARE_TREND_DAYS 8

enum care_metric { CM_GW_MS, CM_INET_MS, CM_LOSS, CM_DNS_MS, CM_SPEED };
enum care_wkind { CW_SHIFT, CW_TREND };

struct care_norm {
	float mean, dev;        /* average, mean absolute deviation */
	unsigned n;             /* samples (capped at the window) */
};

struct care_mday {
	long day;               /* local day number */
	float level_sum, excess_sum;
	unsigned n;
};

struct care_mstate {
	struct care_norm hour[24], all;
	float cusum;            /* shift: how much worse than normal, accumulated (deviations) */
	float recent;           /* how much worse than normal lately (raw units, smoothed) */
	struct care_mday days[CARE_TREND_DAYS];
	/* v0.53: the value itself lately (smoothed like recent): what a shift's warning says it is now. Before, that
	   was the normal less recent, and the normal had learned the bad value meanwhile ("-1 Mbit/s") */
	float level;
};

struct care_sample {
	int have[CARE_METRICS];
	float v[CARE_METRICS];  /* ms, ms, %, ms, Mbit/s */
};

struct care_learn_params {
	long learn_s;           /* no warning before this much learning (care.learn_days) */
	float k;                /* CUSUM allowance (deviations) */
	float h[CARE_METRICS];  /* CUSUM threshold, per metric (the speed is tested once a day) */
	float floor[CARE_METRICS];      /* the smallest deviation counted (noise floor) */
	float effect[CARE_METRICS];     /* the smallest difference worth a warning */
	float fault[CARE_METRICS];      /* where Diagnose calls it a fault (speed: 0 unknown) */
	int horizon_days;       /* a trend reaching a fault within this: a warning */
	int min_days;           /* days of data a trend needs */
	float r2;               /* how well the straight line must fit */
};

struct care_warning {
	int active;
	int metric, kind;       /* enum care_metric, care_wkind */
	long since;
	float normal, current;  /* at this hour; now */
	float per_day;          /* trend: worse by this much a day */
	int days;               /* trend: of data */
	int predict;            /* days until a fault at this rate; 0 already, -1 not known */
	long duration;          /* set when it clears */
};

struct care_learn {
	struct care_mstate m[CARE_METRICS];
	long started;           /* the first sample (wall s) */
	struct care_warning w;
	/* the last warning that ended: a problem soon after was seen coming */
	int last_area;
	long last_since, last_end;
};

#define CW_OPENED  1
#define CW_CLEARED 2
#define CW_CHANGED 4

void care_learn_params_default(struct care_learn_params *p);
/* one check's measurements at local hour, local day; quiet_areas: bit per enum
   care_area with a problem open (nothing learned or warned there); returns CW_* */
int care_learn_step(struct care_learn *L, const struct care_sample *s, int hour, long day, long now, unsigned quiet_areas,
		    const struct care_learn_params *p);
int care_learning(const struct care_learn *L, long now, const struct care_learn_params *p);
/* the normal of a metric at an hour; -1 when not known yet */
int care_normal(const struct care_learn *L, int metric, int hour, float *mean, float *dev);
const char *care_metric_name(int m);    /* NextHopLatency, InternetLatency, InternetLoss, DNSLatency, DownloadSpeed */
int care_metric_area(int m);            /* CA_INTERNET or CA_DNS */
/* a value with its unit: "12 ms", "0.4%", "95 Mbit/s" */
void care_metric_value(int m, float v, char *buf, size_t n);
/* the open warning, in words, and what to do */
void care_warning_text(const struct care_warning *w, const struct care_learn_params *p, char *buf, size_t n);
const char *care_warning_advice(int metric);

#endif
