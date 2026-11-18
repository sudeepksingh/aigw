/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW proactive care, before the call (v0.40.0).
 *
 * The help-desk brief (ag/brief.h) helps once a customer calls. This acts
 * first: from what the gateway already watches (proactive care's open problem,
 * the why timeline, Self-Healing Wi-Fi, a device that keeps dropping off,
 * rough calls on a full line, the week's experience score), it finds faults
 * the customer is likely to notice and, for each one:
 *
 *   - AIGW fixes it itself when it can, as the act mode allows: in enforce
 *     mode it applies the fix; in advise mode it only proposes it (a person
 *     says yes); then it records how it went
 *   - otherwise it raises a ticket to the ISP: the brief, the evidence, the
 *     next step, and a short note in plain words the ISP can send the customer
 *     ("we noticed X and fixed it", "we noticed X and we're on it")
 *
 * "Worth acting on" is a points rule (pro_points), all four things that make
 * a customer pick up the phone:
 *
 *   how bad       a warning 1, a problem 2 (below a warning: never)
 *   how long      10 minutes or more +1, 30 minutes or more +2
 *   how many      2 devices or more +1, 4 or more +2
 *   repeats       its 2nd episode in a day +1, its 4th +2
 *   (and a home whose experience score this week is under 90%: +1)
 *
 * It is worth acting on at `threshold` points (3), once it has lasted
 * `min_s` (a blip never is), for faults on the line, in the home's Wi-Fi or on
 * a device (never "a service beyond the ISP", "nothing wrong", "not known"),
 * and for a full line only when calls or games were rough on it.
 *
 * Tickets are kept per fault (its key: the code, and the radio or device):
 * one open ticket per key, however often it is seen. A fault that clears and
 * comes back within `cooldown_s` reopens the same ticket (a flap): the ISP is
 * not told again, and each flap doubles how long it must stay away before the
 * ticket closes. At most `max_per_day` new tickets reach the ISP in 24 hours;
 * more are kept at home (suppressed). A home in a known outage across the area
 * (v0.39) gets no tickets and no customer notes at all: the ISP knows already.
 *
 * Pure (no I/O): agd (agd/proactive.c) gathers, acts and tells the ISP.
 * Unit test: tests/test_proactive.c.
 */
#ifndef AG_PROACTIVE_H
#define AG_PROACTIVE_H

#include <stddef.h>
#include <stdint.h>

#define PRO_TICKETS   16
#define PRO_EPISODES  64
#define PRO_SENT      32
#define PRO_KEY       64
#define PRO_TEXT      320
#define PRO_EVIDENCE  640

/* what became of a ticket */
enum pro_status {
	PS_WATCHING,    /* worth acting on; Self-Healing Wi-Fi has its grace first */
	PS_FIXING,      /* AIGW applied a fix (enforce mode): waiting to see it work */
	PS_PROPOSED,    /* a fix waits for a person's yes (advise mode); the ISP was told */
	PS_RAISED,      /* a ticket to the ISP: AIGW can't fix it */
	PS_SUPPRESSED,  /* kept at home: the day's limit */
	PS_FIXED,       /* over, and AIGW changed something for it */
	PS_RESOLVED,    /* over, after the ISP was told */
	PS_PASSED,      /* over by itself before anyone had to be told */
	PS_N
};

struct pro_params {
	int threshold;          /* points worth acting on (3) */
	long min_s;             /* it must have lasted this long (180) */
	long long1, long2;      /* +1 at this long, +2 at this long (600, 1800) */
	long clear_s;           /* gone this long: over (600); doubled by each flap */
	long cooldown_s;        /* back within this: the same ticket, no new word to the ISP (21600) */
	int max_per_day;        /* new tickets to the ISP in 24 hours (3) */
	long wifi_grace;        /* Self-Healing Wi-Fi's time before a Wi-Fi ticket (1800) */
	int score_poor;         /* a week's score under this: +1 (90) */
};

/* one fault seen now */
struct pro_signal {
	char key[PRO_KEY];      /* code, and ":" radio or device MAC */
	const char *code;       /* Diagnose's or the timeline's (ag/diag.h) */
	int fault;              /* enum brief_fault (ag/brief.h) */
	int sev;                /* enum diag_sev */
	long since;             /* first seen (wall s) */
	int devices;            /* devices it touches (the whole home: those online) */
	int rough_calls;        /* calls or games that were rough on it (a full line) */
	const char *target;     /* a device's MAC, a radio; NULL the home */
	const char *source;     /* care, timeline, wifi, device, calls */
};

struct pro_ticket {
	int used;
	uint32_t id;
	int open;               /* 0: closed (kept for the cooldown, then reused) */
	char key[PRO_KEY], code[24], target[24], source[12];
	int fault, sev, sev_max;
	int status;             /* enum pro_status */
	int points;
	char why[160];          /* the points in words */
	int devices;
	int episodes;           /* times it came, within the cooldown of each other */
	int flaps;              /* came back after it had cleared */
	int notified;           /* the ISP heard of it (an event) */
	int quiet;              /* came back soon after the ISP heard it was over: no word unless it stays */
	int called;             /* the customer asked the help desk while it was open */
	int acted;              /* AIGW changed something for it */
	uint32_t decision;      /* AIGW's fix (applied or proposed) */
	long since;             /* the fault's first sign (wall s) */
	long opened, last_seen, closed, notified_at, watch_until;
	char summary[PRO_TEXT]; /* the brief's verdict */
	char evidence[PRO_EVIDENCE];
	char next_step[PRO_EVIDENCE];
	char note[PRO_TEXT];    /* for the customer, in plain words */
};

struct pro_counts {
	uint32_t tickets;       /* worth acting on */
	uint32_t raised;        /* the ISP was told (a ticket, or a fix waiting for a yes) */
	uint32_t fixed;         /* AIGW fixed it itself */
	uint32_t proposed;      /* AIGW proposed its fix (advise mode) */
	uint32_t suppressed;    /* kept at home (the day's limit) */
	uint32_t deduped;       /* seen again while known: no new word to the ISP */
	uint32_t flaps;         /* came back after clearing */
	uint32_t called;        /* the customer called anyway (a brief asked while open) */
	uint32_t avoided;       /* calls likely avoided: fixed, or told first, and nobody called */
	uint32_t area_held;     /* faults worth a ticket while the home was in a known area outage: none made */
	long since;
};

struct pro_state {
	struct pro_ticket t[PRO_TICKETS];
	uint32_t next_id;
	struct { uint32_t h; long t, last; int held; } ep[PRO_EPISODES];  /* episodes (key, start, last seen): repeats */
	int area_hold;          /* set before each step: the home is in a known area outage */
	int ep_head;
	long sent[PRO_SENT];    /* when the ISP was told of a new ticket: the day's limit */
	int sent_head;
	struct pro_counts c;
};

/* what a step changed, for agd to act on */
enum pro_change_kind {
	PC_NEW,                 /* a new ticket worth acting on: fix, propose, watch or raise */
	PC_REOPENED,            /* a closed one came back (a flap) */
	PC_GRACE_OVER,          /* a watched one outlived Self-Healing Wi-Fi's grace, or AIGW's fix didn't clear it */
	PC_CLEARED,             /* an open one is over */
};

struct pro_change {
	int kind;
	struct pro_ticket *t;
};

void pro_params_default(struct pro_params *p);
const char *pro_status_name(int st);    /* Watching, Fixing, ... */
/* points for one signal now (score_week -1: not known); why, in words */
int pro_points(const struct pro_signal *s, long now, int repeats, int score_week, const struct pro_params *p,
	       char *why, size_t n);
/* worth acting on: the points, how long, and what kind of fault */
int pro_worth(const struct pro_signal *s, int points, long now, const struct pro_params *p);
/* episodes of this key that started in the last 24 hours (this one included) */
int pro_repeats(const struct pro_state *st, const char *key, long now);
/*
 * One look: the signals seen now. Tickets are found, opened, reopened,
 * cleared; returns the number of changes in ch (at most max).
 */
int pro_step(struct pro_state *st, const struct pro_signal *sig, int n, long now, int score_week,
	     const struct pro_params *p, struct pro_change *ch, int max);
/* new tickets the ISP may still hear of in the 24 hours up to now */
int pro_budget(const struct pro_state *st, long now, const struct pro_params *p);
/* the ISP was told of this ticket now (counts against the day's limit once) */
void pro_notified(struct pro_state *st, struct pro_ticket *t, long now);
/*
 * A home in a known area outage (st->area_hold) gets no proactive tickets and
 * no customer notes: the ISP knows of the outage and tells its customers
 * itself. While it lasts, no ticket is opened or reopened and none outlives a
 * grace (the open ones wait); each fault that would have made one is counted
 * once (area_held).
 */
/* the ticket is over: Fixed, Resolved or Passed, and the counts */
int pro_close(struct pro_state *st, struct pro_ticket *t, long now);
/* the customer asked the help desk: open tickets were not in time */
void pro_on_called(struct pro_state *st);
struct pro_ticket *pro_find(struct pro_state *st, uint32_t id);
int pro_open_count(const struct pro_state *st);
/*
 * The note for the customer, in plain words: "fixed" (AIGW fixed it) or "on it",
 * at a local hour (this morning, this afternoon, this evening, tonight).
 */
void pro_note(const char *code, int fixed, int hour, char *buf, size_t n);
/* 1 when the text has none of the words a customer wouldn't know (codes, units, acronyms) */
int pro_plain(const char *s);

#endif
