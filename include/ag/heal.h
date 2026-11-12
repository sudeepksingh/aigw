/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Self-Healing Wi-Fi: the diagnosis. Pure (no I/O): agd (agd/heal.c)
 * measures, this names what is wrong on one access point; the unit test
 * (tests/test_heal.c) feeds it cases.
 *
 * What a device feels is measured, not what the radio reports: small probes
 * while the device is awake (asleep, a phone answers late or not at all on a
 * perfect link), giving loss and delay, with its signal and how often it
 * changed band.
 */
#ifndef AG_HEAL_H
#define AG_HEAL_H

enum heal_status { HEAL_UNKNOWN, HEAL_GOOD, HEAL_DEGRADED, HEAL_POOR };
enum heal_cause {
	HC_NONE,
	HC_CONGESTION,  /* loss or delay with a fine signal: the channel is busy (neighbours, interference) */
	HC_COVERAGE,    /* loss or delay with a weak signal: too far, or walls */
	HC_HOPPING,     /* a device keeps changing band: at the edge of 5 GHz */
	HC_DEVICE,      /* one device suffers, the others on the access point are fine */
	HC_RADIO,       /* no device answers at all: the radio itself */
};

/* one device on the access point, over the measuring window */
struct heal_sta {
	int samples;            /* probes sent while it was awake */
	int lost;               /* of those, unanswered */
	int rtt_ms;             /* median delay of the answered ones; -1 none */
	int base_rtt_ms;        /* its usual delay, learned; 0 not yet */
	int signal;             /* dBm, smoothed */
	int roams;              /* band changes in the hopping window */
	int weak;               /* 0: judge by signal; 1 weak, 2 strong (agd, with a margin: heal_weak_step) */
};

struct heal_params {
	int min_samples;        /* awake probes before a device counts (15) */
	int loss_pct;           /* this much loss is a problem (5 %) */
	int delay_ms;           /* and this much delay (100 ms) ... */
	int delay_factor;       /* ... when it is also this many times its usual (3) */
	int weak_signal;        /* dBm at or below which the signal is weak (-75) */
	int hops;               /* band changes in the window that count as hopping (4) */
	int poor_loss_pct;      /* "poor" rather than "degraded" (20 %) */
	int poor_delay_ms;      /* (300 ms) */
};

struct heal_result {
	enum heal_status status;
	enum heal_cause cause;
	int measured;           /* devices with enough samples */
	int loss_pct;           /* over the measured devices */
	int rtt_ms;             /* worst affected device's median, or the measured ones' */
	unsigned affected;      /* bit i: device i is affected */
};

void heal_params_default(struct heal_params *p);
/*
 * A signal near the line would flip "weak" back and forth (and with it the
 * cause): weak at or below weak_signal, strong again only above
 * weak_signal + margin. prev and the result: 0 unknown, 1 weak, 2 strong.
 */
int heal_weak_step(int prev, int signal, int weak_signal, int margin);
int heal_sta_loss_pct(const struct heal_sta *s);
int heal_sta_degraded(const struct heal_sta *s, const struct heal_params *p);
/* n devices (at most 32 are looked at) */
void heal_diagnose(const struct heal_sta *s, int n, const struct heal_params *p, struct heal_result *r);
const char *heal_status_name(enum heal_status st);
const char *heal_cause_name(enum heal_cause c);

/*
 * The healing report: what the gateway found and fixed, per access point and
 * day (UTC), for the last 31 days. What an ISP measures AIGW by.
 */
#define HEAL_DAYS 31
#define HEAL_CAUSES 6                   /* HC_NONE .. HC_RADIO */

struct heal_day {
	long day;                       /* days since 1970-01-01 (UTC); 0 = empty */
	unsigned secs[4];               /* seconds Unknown, Good, Degraded, Poor */
	unsigned found[HEAL_CAUSES];    /* problems found (a new verdict that isn't Good), by cause */
	unsigned kept, tried;           /* channel trials */
	unsigned band_rules;            /* devices kept on 2.4 GHz */
	unsigned fixes, fix_secs;       /* fixes that stayed, and seconds from the problem to each */
	unsigned loss_before, loss_after;  /* sums of % over kept trials */
	unsigned rtt_before, rtt_after;    /* sums of ms over kept trials */
};

struct heal_report {
	struct heal_day d[HEAL_DAYS];   /* d[day % HEAL_DAYS] */
};

/* today's slot (emptied when it last held an older day) */
struct heal_day *heal_report_day(struct heal_report *r, long wall_s);
/* the last `days` days up to wall_s, added up */
void heal_report_sum(const struct heal_report *r, long wall_s, int days, struct heal_day *out);
unsigned heal_day_problems(const struct heal_day *d);
/* % of the measured time (not Unknown) that was Good; -1 none measured */
int heal_day_good_pct(const struct heal_day *d);

#endif
