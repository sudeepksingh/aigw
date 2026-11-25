/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW home Wi-Fi model (v0.52.0): a compact model of the home's Wi-Fi with no
 * floor plan, and "what if" predictions from it: what an extender, moving the
 * gateway or another band would do, before anyone buys anything.
 *
 * Coverage advice (ag/coverage.h) says which device is weak, when and where.
 * This learns the home itself from the same looks (agd/coverage.c feeds both):
 *
 *   places        each look's signature (what each access point hears of the
 *                 device, 2.4 GHz brought to 5 GHz terms) clustered into the
 *                 home's places, for all devices, weak or not. Devices that go
 *                 from one place to another between two looks make the places
 *                 neighbours: AIGW learns which places are next to which.
 *                 A person may name a place ("bedroom") by a device seen there.
 *   path loss     per place and band: what the signal loses on the way
 *                 (transmit power minus what arrives), and a "distance-like"
 *                 figure from it (indoors, walls included: not metres on a
 *                 floor plan)
 *   experience    per place, band and access point: the signal, the loss, the
 *                 rate, the problems
 *   problems      the last 30 days' coverage problems: a device that suffered
 *                 (lost traffic, or was weak without a loss measure) for some
 *                 minutes in a row, where, on which band and access point
 *
 * What if (each with the problems it would fix of the last 30 days', the ones
 * it may cause, a confidence and its limits), ranked by problems fixed:
 *
 *   Extender      an extender near a place (one whose own link to the gateway
 *                 would be good), or about halfway to the far place when no
 *                 place qualifies: the signal it adds at the far place, by the
 *                 path-loss estimate (log-distance, exponent path_exp)
 *   GatewayUp     the gateway higher, in the open: a few dB everywhere
 *   GatewayCentral the gateway moved toward the middle: far places gain, near
 *                 ones lose (assumes the far places lie on one side: AIGW
 *                 can't see directions with one access point)
 *   Band24        devices there kept on 2.4 GHz: reaches further (by the offset
 *                 learned at that place when both bands were seen there)
 *   Band5         devices there moved to 5 GHz: for strong-signal 2.4 GHz
 *                 problems (a crowded band), when the device can use 5 GHz
 *
 * "Before you buy": a free change that fixes as many problems as an extender
 * comes first, and the extender is not worth selling then.
 *
 * Proof: when an extender appears after an Extender prediction, AIGW measures
 * the far place again and keeps "predicted +N dB, measured +M dB".
 *
 * Pure (no I/O): unit test tests/test_home.c.
 */
#ifndef AG_HOME_H
#define AG_HOME_H

#include <stddef.h>
#include <stdint.h>

#include "ag/coverage.h"

#define HM_EARS    COV_EARS     /* access points: the gateway (0) and mesh extenders */
#define HM_PLACES  8            /* the home's places */
#define HM_DEVS    32           /* devices remembered */
#define HM_PROBS   64           /* coverage problems remembered (30 days) */
#define HM_OPTS    6            /* what-if options kept */
#define HM_NAME    16           /* a place's name */

enum hm_kind { HO_EXTENDER, HO_GATEWAY_UP, HO_GATEWAY_CENTRAL, HO_BAND24, HO_BAND5, HO_N };
enum hm_status { HS_LEARNING, HS_READY, HS_N };

struct hm_params {
	int weak_dbm;           /* weak at or below (wifi.heal_weak: -75) */
	int loss_pct;           /* suffering at this much loss (10) */
	int same_db;            /* signatures this close are one place (6) */
	int margin_db;          /* fixed: at least this much above weak (3) */
	int backhaul_dbm;       /* an extender's own link is good at this, 5 GHz terms (-65) */
	int strong_dbm;         /* a strong signal: loss there is not coverage (-65) */
	int tx24, tx5;          /* the gateway's transmit power, dBm (20, 23) */
	float path_exp;         /* indoor path-loss exponent (3.0) */
	int up_db;              /* GatewayUp: what it adds (4) */
	int learn;              /* looks before the model is Ready (120) */
	int min_problem;        /* looks in a row that make a problem (5) */
	int days;               /* problems kept this many days (30) */
	int interval_s;         /* s between looks (60): minutes from looks */
	uint32_t decay;         /* everything halves at this many looks (43200: 30 days of minutes) */
};

/* per place, band (0: 2.4 GHz, 1: 5 GHz) and access point */
struct hm_cell {
	uint16_t n, lm, prob, rn;       /* looks; with loss measured; suffering; with a rate */
	int32_t dbm;                    /* signal summed (as heard, not in 5 GHz terms) */
	uint32_t loss, rate;            /* % lost, Mbit/s summed */
};

struct hm_place {
	float sig[HM_EARS];             /* its signature (5 GHz terms), 0 not heard */
	float spread;                   /* how far its looks fall from it, dB (mean) */
	uint32_t n;
	uint16_t dn[HM_DEVS];           /* looks by device */
	uint16_t adj[HM_PLACES];        /* devices that went from here to there between two looks */
	struct hm_cell c[2][HM_EARS];
	char name[HM_NAME];             /* a person's name for it; "" none */
	long first, last;
	/* after an extender appeared: the best signal here since (for the proof) */
	int32_t chk_sum;
	uint16_t chk_n;
};

struct hm_dev {
	char mac[18];
	int8_t place;                   /* where it was last, -1 */
	uint8_t dual;                   /* can use 5 GHz */
	long last;
	/* the problem going on now */
	int8_t ep_place, ep_band, ep_ear, ep_kind;      /* kind 0: weak, 1: lossy with a strong signal */
	uint16_t ep_n, ep_miss;
	int32_t ep_dbm;
	long ep_start;
};

/* one coverage problem */
struct hm_prob {
	long t;                         /* when it started (wall s); 0 free */
	uint16_t minutes;
	int8_t place, dev, band, ear, kind;
	int8_t dbm;                     /* its signal there, mean (as heard) */
};

struct hm_model {
	struct hm_place p[HM_PLACES];
	int np;
	struct hm_dev d[HM_DEVS];
	struct hm_prob pr[HM_PROBS];
	uint32_t looks;
	int ears;                       /* access points seen (1: the gateway alone) */
	/* the proof: the Extender prediction standing when an extender appeared */
	int8_t chk_place;               /* -1 none */
	int8_t chk_pred;                /* predicted gain, dB */
	int8_t chk_before;              /* the signal there before */
	int8_t chk_measured;            /* measured gain, 127 not yet */
	long chk_at;
	/* the standing Extender prediction (kept for the proof) */
	int8_t ext_place, ext_gain;
};

/* one look (coverage's sample) and what else is known of the device */
struct hm_look {
	const char *mac;
	const struct cov_sample *s;
	int dual;                       /* can use 5 GHz */
	int weak_tx;                    /* coverage judged its own radio weak: not the home's problem */
};

struct hm_option {
	int kind;                       /* enum hm_kind */
	int place;                      /* the place it helps most (index), -1 the home */
	int at;                         /* Extender: near this place (index); -1 about halfway */
	int gain_db;                    /* at that place */
	int plus_minus;                 /* the gain's uncertainty, dB */
	int fixed, of, broken;          /* problems fixed, of the last days', places it may make weak */
	int confidence;                 /* 0..100 */
	int buy;                        /* costs money (an extender) */
	int backhaul_dbm;               /* Extender: its own link to the gateway (5 GHz terms) */
};

struct hm_result {
	int status;                     /* enum hm_status */
	int places, problems, devices;
	int nopt;
	struct hm_option o[HM_OPTS];    /* best first */
	int best;                       /* -1 nothing helps (or nothing to fix) */
	int best_free, best_buy;        /* index in o[], -1 none */
	int extender_helps;             /* -1 not known yet, 0 no (nothing to fix, or a free change does as much), 1 yes */
	int target;                     /* the place with most problems, -1 none */
};

/* names for the words: a place's (its name, or the devices seen there) and a device's */
struct hm_names {
	const char *(*dev)(const char *mac, void *ctx);         /* NULL: by kind */
	void *ctx;
	int zones;                      /* places by the names people gave them */
};

void hm_params_default(struct hm_params *p);
void hm_init(struct hm_model *m);
void hm_add(struct hm_model *m, const struct hm_look *l, const struct hm_params *p);
/* everything older than the window forgotten; problems going on ended */
void hm_expire(struct hm_model *m, long now, const struct hm_params *p);
/* the device's place now (index), -1 not known */
int hm_place_of(const struct hm_model *m, const char *mac);
/* name a place (index); "" clears; -1 no such place */
int hm_name(struct hm_model *m, int place, const char *name);
void hm_judge(const struct hm_model *m, long now, const struct hm_params *p, struct hm_result *r);

/* per place and band: the mean signal at the gateway (as heard), its path loss, distance-like m; 0 not seen */
int hm_place_dbm(const struct hm_place *pl, int band, int ear);
int hm_path_loss(const struct hm_place *pl, int band, const struct hm_params *p);
float hm_distance(const struct hm_place *pl, const struct hm_params *p);
/* how much stronger 2.4 GHz is heard there than 5 GHz (learned, else COV_OFF24); *learned 1 when learned */
int hm_offset24(const struct hm_place *pl, int *learned);
/* the band and access point that do best there: band 2 or 5, ear; -1 not known */
int hm_best_link(const struct hm_place *pl, int *ear);
int hm_place_problems(const struct hm_model *m, int place, long now, const struct hm_params *p);

const char *hm_kind_name(int k);        /* Extender, GatewayUp, GatewayCentral, Band24, Band5 */
const char *hm_status_name(int s);      /* Learning, Ready */
const char *hm_confidence_name(int c);  /* Low, Medium, High */
/* prep 0: "the bedroom", "the spot where tv-lounge and pixel-7 are", "place 3" (no names);
   prep 1: "in the bedroom", "where tv-lounge and pixel-7 are", "at place 3" */
void hm_place_words(const struct hm_model *m, int place, const struct hm_names *nm, int prep, char *buf, size_t n);
/* one option in a sentence, with its confidence and limits */
void hm_option_words(const struct hm_model *m, const struct hm_result *r, int i, const struct hm_names *nm, char *buf,
		     size_t n);
/* the advice: the best option, "before you buy"; "" nothing to say yet */
void hm_advice(const struct hm_model *m, const struct hm_result *r, const struct hm_names *nm, char *buf, size_t n);
/* the proof in words; "" none */
void hm_proof_words(const struct hm_model *m, const struct hm_names *nm, char *buf, size_t n);

#endif
