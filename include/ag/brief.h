/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW help-desk assistant: one short answer for the ISP's support agent.
 *
 * A customer calls: "bad internet tonight?". The gateway already knows a lot:
 * a fresh quiet Diagnose, the why timeline (ag/why.h), proactive care's open
 * problem and early warning (ag/care.h), each access point's Wi-Fi health,
 * smart queueing, calls and games, the last firmware upgrade's check, and its
 * own decisions. This brings it together into a support brief:
 *
 *   fault      one word: LINE, HOME_WIFI, DEVICE, OUTSIDE, NONE, UNKNOWN
 *   verdict    one sentence
 *   evidence   times and numbers, each with where it came from
 *   done       what AIGW already did
 *   tell       what to tell the customer
 *   fix        a remote fix, if there is one (it is only proposed: a person says yes)
 *   visit      whether a technician should go out: No, IfPersists, Yes
 *
 * A known outage in the area (cortex: many homes of the uplink at once) comes
 * first for a period that overlaps it: the ISP's network, not the home; no
 * visit, nothing to fix in the home (code AREA_OUTAGE unless the home's own
 * verdict already names the line).
 *
 * A technician visit costs the ISP money, and a wrong one costs the customer a
 * day, so the rules are careful: a visit only for the line itself (the next
 * hop losing packets, the nightly speed far below the line's), and only when
 * it lasted or two sources agree. Anything AIGW or the customer can fix comes
 * first. The fault is UNKNOWN when there is too little to go on.
 *
 * Pure (no I/O, no AI model): agd (agd/brief.c) gathers, this judges.
 * Unit test: tests/test_brief.c.
 */
#ifndef AG_BRIEF_H
#define AG_BRIEF_H

#include <stddef.h>
#include <stdint.h>

#define BRIEF_LINES 12
#define BRIEF_TEXT  280

enum brief_fault { BF_NONE, BF_LINE, BF_HOME_WIFI, BF_DEVICE, BF_OUTSIDE, BF_UNKNOWN, BF_N };
enum brief_visit { BV_NO, BV_IF_PERSISTS, BV_YES };
enum brief_conf { BC_LOW, BC_MEDIUM, BC_HIGH };

/* a verdict from one source (Diagnose's codes, ag/diag.h, and the timeline's) */
struct brief_finding {
	const char *code;               /* NULL: none */
	const char *area;               /* Internet, DNS, WiFi, Device, AIGW, All */
	int sev;                        /* enum diag_sev */
	long since, until;              /* when (wall s); 0 not known */
	const char *text;               /* in words (names already taken out where they must be) */
};

struct brief_radio {
	const char *name;               /* "5 GHz (wlan1)" */
	int status, cause;              /* enum heal_status / heal_cause (ag/heal.h) */
	long since;                     /* this status since */
	const char *text;               /* Self-Healing Wi-Fi's own words */
};

enum { BD_PROPOSED, BD_ACTIVE, BD_KEPT, BD_ENDED };

struct brief_decision {
	uint32_t id;
	const char *action;             /* quarantine, channel, stay24, sqm, ... */
	const char *words;              /* what, in words ("a laptop", "wlan1 to channel 44") */
	int state;                      /* BD_* */
	int by_aigw;                    /* AIGW decided it by itself (not a person, not a plug-in) */
	long t;                         /* made */
	const char *reason;
};

struct brief_in {
	long now, from, to;             /* the period asked about (wall s) */
	const char *host;               /* the device asked about, in words; NULL the whole home */
	const char *host_target;        /* ... its MAC (for a fix on it) */
	/* a fresh quiet Diagnose, just now */
	int have_check;
	struct brief_finding check;
	int check_loss_pct, check_inet_ms, check_gw_ms, check_dns_ms;   /* -1 not measured */
	/* the why timeline over the period */
	int have_why;                   /* 0: off, or nothing recorded */
	struct brief_finding why;       /* its verdict */
	const struct brief_finding *why_more;   /* its other findings */
	int nwhy_more;
	int recorded_pct;               /* of the period recorded (0..100) */
	long last_sample;               /* the newest sample (wall s); 0 none */
	/* proactive care */
	int care_open;
	struct brief_finding care;      /* the open problem */
	int care_reported;              /* the ISP was told (ProblemDetected!) */
	const char *warning;            /* an early warning in words; NULL none */
	int score_week;                 /* the experience score, -1 not known */
	/* Wi-Fi now */
	const struct brief_radio *radio;
	int nradio;
	/* the nightly speed test */
	long speed_at;                  /* 0: none */
	float speed_mbps, line_mbps;    /* line_mbps: the speed sold; 0 not known */
	int speed_low_nights;           /* of the last 7, below what the line must reach */
	/* smart queueing: On, Trial, Proposed, Off, Tried, Unavailable; NULL off (sqm.enabled 0) */
	const char *sqm;
	uint32_t sqm_decision;          /* when Proposed */
	/* calls and games that ended in the period */
	int calls, calls_rough;
	/* the last firmware upgrade (ag/upgrade.h state names), its time and versions */
	const char *upgrade;
	long upgrade_at;
	const char *upgrade_from, *upgrade_to;
	/* AIGW's decisions: those made in or just before the period, and those live now */
	const struct brief_decision *dec;
	int ndec;
	/* an area outage cortex told of (agd/area.c): many homes of this uplink at once */
	uint32_t area;                  /* its number; 0 none */
	const char *area_kind;          /* DOWN, LOSS, SLOW, DNS */
	long area_since;
	int area_homes, area_of;        /* homes hit, of the uplink's */
	const char *area_held;          /* what AIGW held meanwhile, in words; NULL or "" nothing yet */
	/* mesh Wi-Fi (ag/mesh.h): what is wrong in it now, e.g. an extender's weak backhaul (MESH_BACKHAUL;
	   a warning when devices are behind it) */
	const struct brief_finding *mesh;
	int nmesh;
};

struct brief_line {
	long t;                         /* 0: no time */
	const char *source;             /* now, timeline, care, wifi, speed, sqm, calls, upgrade, decision */
	char text[BRIEF_TEXT];
};

struct brief_fix {
	int possible;
	const char *action;             /* sqm, stay24 (a new proposal), approve, undo (a decision); NULL */
	char target[64];                /* sqm: filled in by agd; stay24: the device's MAC */
	uint32_t decision;              /* approve / undo: the decision */
	char text[BRIEF_TEXT];
};

struct brief {
	int fault;                      /* enum brief_fault */
	const char *code;               /* the deciding finding's code ("OK" when none) */
	int confidence;                 /* enum brief_conf */
	int current;                    /* it is happening now (the fresh check, or care's open problem) */
	char verdict[320];
	int nev;
	struct brief_line ev[BRIEF_LINES];
	int ndone;
	struct brief_line done[BRIEF_LINES];
	char tell[320];
	struct brief_fix fix;
	int visit;                      /* enum brief_visit */
	char visit_why[320];
};

void brief_judge(const struct brief_in *in, struct brief *out);
const char *brief_fault_name(int fault);        /* LINE, HOME_WIFI, ... */
const char *brief_visit_name(int visit);        /* No, IfPersists, Yes */
const char *brief_conf_name(int c);             /* low, medium, high */
/* the fault a code points to (Diagnose's and the timeline's codes) */
int brief_fault_of(const char *code);
/*
 * The period asked about, in plain words, at wall time now (local time):
 *   "" or "now"         the last 15 minutes
 *   "last hour", "1h", "last 2h", "90m", "last 30 min"
 *   "tonight 20:00-22:00", "20:00-22:00", "yesterday 19:00-23:00"
 *   "since 19:30"       until now
 * A clock range later than now is yesterday's. -1 when it isn't one.
 */
int brief_period(const char *s, long now, long *from, long *to);
/* a device's kind in words ("a laptop", "an Android phone") from agd's type guess and its name */
void brief_kind(const char *type, const char *hostname, char *buf, size_t n);
/* every whole-word name[i] in text replaced by kind[i] (longest names first) */
void brief_scrub(char *text, size_t n, const char *const *name, const char *const *kind, int k);

#endif
