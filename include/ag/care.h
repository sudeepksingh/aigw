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
};

struct care_day {
	long day;               /* days since 1970-01-01 (UTC); 0 = empty */
	unsigned checks, good;  /* checks, and those without a problem */
	unsigned bad[CARE_AREAS];  /* bad checks by area */
	unsigned problems;      /* problems that started */
	unsigned reported;      /* told the ISP */
	unsigned fixed;         /* Wi-Fi problems over before they had to be reported: fixed by AIGW */
	unsigned problem_secs;  /* time a problem was open */
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
/* would this open problem be reported (the ISP's), or is it AIGW's to fix */
int care_isp_owned(const char *area);

struct care_day *care_report_day(struct care_report *r, long wall_s);
void care_report_sum(const struct care_report *r, long wall_s, int days, struct care_day *out);
/* % of checks without a problem; -1 no checks */
int care_score(const struct care_day *d);
/* the area with most bad checks; -1 none */
int care_main_cause(const struct care_day *d);

#endif
