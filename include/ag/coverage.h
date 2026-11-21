/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW coverage placement advice (v0.46.0): which device is weak, when, where
 * in the home, and what would fix it. No floor plan, no AI model.
 *
 * "Move the device closer, or the access point higher; an extender would
 * help" is true and useless. A help desk needs which device, at what time,
 * where in the home, and whether an extender would fix it (and roughly where).
 * agd (agd/coverage.c) samples every Wi-Fi device each minute from what it
 * already measures: the signal on its link and which access point and band it
 * is on, what the mesh's other access points hear of it (EasyMesh Data
 * Elements), the loss the gateway's own probes see, resends and rate. This
 * keeps a short history per device and judges it:
 *
 *   the device   its usual signal (median, and the 10th and 90th percentile),
 *                how much of the time it is weak, at which hours, on how many
 *                of the last 7 days, and whether it suffers then (loses traffic)
 *
 *     Sometimes      weak part of the time, fine at others: it moves into a far
 *                    spot (the phone in the bedroom at night)
 *     Always         weak all the time: it lives in a far spot (the TV)
 *     WeakTransmitter weak all the time and steady, yet it loses little where
 *                    other devices that weak lose a lot (or the others are fine,
 *                    and it is a smart-home device): its own radio is weak (a
 *                    cheap plug), not the coverage
 *
 *   spots        each device's weak samples, clustered by their signature (what
 *                each access point hears there, 2.4 GHz brought to 5 GHz terms)
 *                and the time of day. Spots of different devices with the same
 *                signature are one spot of the home, named by the devices seen
 *                there ("where tv-lounge and pixel-7 are, at night").
 *
 *     a spot two or more suffering devices share: the access point is badly
 *     placed for it. Move the gateway toward that spot, or put an extender
 *     about halfway (docs/mesh.md: halfway is best for its own link).
 *     a spot of one device: move that device (or use it nearer).
 *
 * With one access point a signature is one number per radio: AIGW can tell how
 * far a spot is, not in which direction, so two far rooms at the same distance
 * look alike. A mesh tells them apart, and says which access point hears the
 * spot best.
 *
 * Pure (no I/O): unit test tests/test_coverage.c.
 */
#ifndef AG_COVERAGE_H
#define AG_COVERAGE_H

#include <stddef.h>
#include <stdint.h>

#define COV_EARS   6            /* access points that hear a device: the gateway (0) and mesh extenders */
#define COV_SPOTS  3            /* weak spots kept per device */
#define COV_BINS   36           /* its signal: 2 dB bins from COV_BIN0 */
#define COV_BIN0   (-105)
#define COV_DEVS   32           /* devices kept */
#define COV_HOME   6            /* the home's spots */
#define COV_HOME_DEVS 6         /* devices named per spot */
#define COV_OFF24  7            /* 2.4 GHz is heard about this much stronger than 5 GHz from the same place */

enum cov_kind { CK_LEARNING, CK_FINE, CK_SOMETIMES, CK_ALWAYS, CK_WEAK_TX, CK_N };
/* UseExtender: a mesh's extender already hears the shared spot well: the devices there belong on it */
enum cov_verdict { CV_LEARNING, CV_FINE, CV_WEAK_DEVICE, CV_MOVE_DEVICE, CV_EXTENDER, CV_USE_EXTENDER, CV_N };

struct cov_params {
	int weak_dbm;           /* a link at or below this is weak (wifi.heal_weak: -75) */
	int loss_pct;           /* a device losing this much when weak suffers (10) */
	int learn;              /* samples before a device is judged (60: an hour) */
	int spot_min;           /* weak samples before a spot counts (10) */
	int same_db;            /* signatures this close are one spot (6) */
	int steady_db;          /* a signal that spreads less than this (10th to 90th percentile) stays put (8) */
	int good_dbm;           /* an access point hears a spot well at this (-68) */
	uint32_t decay;         /* the history halves at this many samples (10080: a week of minutes) */
};

/* one look at one device */
struct cov_sample {
	long t;
	int ear;                /* the access point it is on (0 the gateway) */
	int band;               /* 2 or 5, 0 not known */
	int dbm;                /* its signal there; 0: none (not sampled) */
	int heard[COV_EARS];    /* what the other access points hear of it (dBm), 0 not heard */
	int hour;               /* local hour */
	long day;               /* local day number */
	int loss_pct;           /* the gateway's own probes, -1 not measured */
	int retry_pct;          /* sent again, -1 not known */
	int rate_mbps;          /* to the device, 0 not known */
};

struct cov_spot {
	float sig[COV_EARS];    /* its signature: what each access point hears there (5 GHz terms), 0 not heard */
	uint16_t n;             /* weak samples here */
	uint16_t measured, lossy;       /* ... with the loss measured; losing a lot */
	uint16_t when[4];       /* by part of the day: night (0-6), morning, afternoon, evening (18-24) */
	long first, last;
};

/* a device's history (kept with agd's state) */
struct cov_dev {
	char mac[18];           /* "" free */
	uint32_t n, weak;       /* samples; weak ones */
	uint16_t bins[COV_BINS];
	uint16_t hn[24], hw[24];        /* samples by local hour; weak ones */
	uint32_t loss_w, loss_o;        /* % lost summed: when weak, when fine */
	uint16_t lw, lo;                /* ... how many measured */
	uint32_t rate_w, retry_w;       /* when weak: Mbit/s, % sent again (summed) */
	uint16_t rw, tw;                /* ... how many known */
	uint16_t on24;          /* samples on 2.4 GHz */
	uint16_t drops, roams;
	uint8_t days;           /* weak on day0 - k: bit k */
	long day0;
	long first, last;
	struct cov_spot spot[COV_SPOTS];
};

/* what is judged of a device */
struct cov_sum {
	int kind;               /* enum cov_kind */
	int usual, low, high;   /* dBm: median, 10th and 90th percentile */
	int weak_pct;
	int from_h, to_h;       /* mostly weak from this hour to that (to excluded), -1 none */
	int days;               /* weak on this many of the last 7 days */
	int loss_weak, loss_ok; /* % lost when weak, when fine, -1 not measured */
	int suffers;            /* loses traffic when weak (not measured: assumed) */
	int on24_pct;
	int spot;               /* its main spot of the home's, -1 none */
};

struct cov_home_spot {
	float sig[COV_EARS];
	int n;                  /* weak samples, all its devices */
	int ndev, dev[COV_HOME_DEVS];   /* the devices seen there (indices), most first */
	uint32_t when[4];
	int suffering;          /* devices that suffer there */
	int shared;             /* two or more suffer there: the access point's place, not a device's */
	int best_ear, best_dbm; /* the access point that hears it best */
	int dbm;                /* what the gateway hears there (else the best) */
	int covered;            /* an extender hears it well (a mesh) */
};

struct cov_result {
	int verdict;            /* enum cov_verdict */
	int devices, judged, weak_devices, movers, weak_tx;
	struct cov_sum d[COV_DEVS];
	int nspot;
	struct cov_home_spot spot[COV_HOME];
	int main;               /* the spot that matters most, -1 none */
	int worst;              /* the device that matters most, -1 none */
	int shared;             /* spots shared by suffering devices */
};

/* names for the words: a device (by its index) and an access point (by its ear); ears > 1: a mesh */
struct cov_names {
	const char *(*dev)(int i, void *ctx);
	const char *(*ear)(int ear, void *ctx);
	void *ctx;
	int ears;
};

void cov_params_default(struct cov_params *p);
void cov_add(struct cov_dev *d, const struct cov_sample *s, const struct cov_params *p);
/* iot(i): the device is a smart-home kind (NULL: never) */
void cov_judge(const struct cov_dev *d, int n, const struct cov_params *p, int (*iot)(int i, void *ctx), void *ctx,
	       struct cov_result *r);
const char *cov_kind_name(int k);       /* Learning, Fine, Sometimes, Always, WeakTransmitter */
const char *cov_verdict_name(int v);    /* Learning, Fine, WeakDevice, MoveDevice, Extender, UseExtender */
/* "at night", "in the evening and at night", "at any time of day" */
const char *cov_part_words(const uint32_t when[4]);
/* "where tv-lounge and pixel-7 are, in the evening (about -84 dBm at the gateway)" */
void cov_spot_words(const struct cov_result *r, int spot, const struct cov_names *nm, char *buf, size_t n);
/* one device's history in a line */
void cov_dev_words(const struct cov_result *r, int i, const struct cov_names *nm, char *buf, size_t n);
/* what to do: for a device (i), or the home (-1); "" when nothing is known yet */
void cov_advice(const struct cov_result *r, int i, const struct cov_names *nm, char *buf, size_t n);
/* the customer's note in plain words (no names, no numbers); "" when nothing is known yet */
void cov_note(const struct cov_result *r, int i, char *buf, size_t n);

#endif
