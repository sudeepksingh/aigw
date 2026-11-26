/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fleet learning ("every home teaches the next"): pure rules, no I/O
 * (core/libag/fleet.c, unit test tests/test_fleet.c). agd (agd/fleet.c) keeps
 * the home's own outcomes and the fleet's priors; cortex (fleetlearn.py) adds
 * up the homes' summaries.
 *
 *   outcome ── a fix (or waiting) for a fault code, in a coarse context: it
 *     │        helped, changed nothing, made it worse, or a person undid it
 *     ▼
 *   cell ───── counts per fault code x action x context bucket (here only)
 *     │
 *   summary ── counts, capped and noised, plus the shape of the learned normal:
 *     │        no MACs, IPs, names, SNIs or timelines (docs/fleet-learning.md)
 *     ▼
 *   priors ─── the fleet's counts per bucket, only where k homes or more add up
 *              to it, as a signed file (the model-update path, models.c)
 *
 * Choosing a fix: each candidate's chance to help and to make things worse is a
 * Dirichlet over (helped, no change, worse or undone): the fleet's counts as a
 * prior of at most `strength` tries, then the home's own outcomes added. Local
 * evidence dominates as it grows (the fleet's share is strength / (strength +
 * local tries)). The choice only moves away from the default fix when another
 * candidate is better by a margin, and only among what the existing rules
 * allow: fleet priors never cause an action by themselves.
 */
#ifndef AG_FLEET_H
#define AG_FLEET_H

#include <stddef.h>
#include <stdint.h>

/* the context of an outcome: band, width class, congestion class, device class,
   hour band, line type; "-" unknown, "*" not split (a coarser bucket) */
#define FL_DIMS   6
#define FL_LEVELS 5             /* 0 every dimension .. 4 none (the code and action only) */
#define FL_KEY    48
#define FL_CODE   24
#define FL_ACTION 16

struct fl_ctx {
	char f[FL_DIMS][10];
};

enum { FL_HELPED, FL_NO_CHANGE, FL_WORSE, FL_UNDONE, FL_RESULTS };

/* tried counts every outcome; no change is tried - helped - worse - undone */
struct fl_counts {
	uint32_t tried, helped, worse, undone;
};

struct fl_prior {
	char code[FL_CODE], action[FL_ACTION], key[FL_KEY];
	uint32_t homes;
	struct fl_counts c;
};

/* the fleet's normal of a care metric (enum care_metric) in an hour band, for a line type */
struct fl_norm {
	char line[10];
	int metric, band;
	uint32_t homes;
	float mean, dev;
};

struct fl_priors {
	int version, k;
	char published[24];
	uint32_t homes;                 /* homes the aggregator heard from */
	int n, nn;
	struct fl_prior *p;
	struct fl_norm *nm;
};

struct fl_blend {
	double helped, bad, score;      /* expected chance to help, to make worse (or be undone); helped - bad */
	double fleet_share;             /* how much of it is the fleet's (0 .. 1) */
	uint32_t fleet_tried, fleet_helped, fleet_homes, local_tried, local_helped;
	int level;                      /* the fleet bucket used (0 most specific), -1 none */
};

struct fl_cand {
	const char *action;
	struct fl_blend b;
};

/* 0 .. 3 from the hour of the day: night (0-5), morning (6-11), afternoon (12-17), evening (18-23) */
int fl_hour_band(int hour);
const char *fl_hour_band_name(int band);
const char *fl_result_name(int r);      /* helped, no-change, worse, undone */
/* the device class of a device type ("Android" -> phone): phone, laptop, tv, iot, other */
const char *fl_device_class(const char *type);
/* the key of a context at a level (coarser levels have "*" for the dimensions left out) */
void fl_key(const struct fl_ctx *c, int level, char *buf, size_t n);
int fl_key_level(const char *key);      /* -1 not a key */
int fl_word_ok(const char *s, size_t max);      /* a code, action or key field: [A-Za-z0-9_.+-*], 1..max-1 */
void fl_count(struct fl_counts *c, int result, int sign);
int fl_counts_ok(const struct fl_counts *c);

/* the signed priors file ("# aigw-priors fleet-priors"); NULL with err */
struct fl_priors *fl_priors_parse(const char *text, size_t len, char *err, size_t en);
void fl_priors_free(struct fl_priors *P);
/* the most specific bucket with at least k homes; *level set; NULL none */
const struct fl_prior *fl_prior_find(const struct fl_priors *P, const char *code, const char *action,
				     const struct fl_ctx *c, int k, int *level);
const struct fl_norm *fl_norm_find(const struct fl_priors *P, const char *line, int metric, int band, int k);

/* fleet counts (NULL: none) blended with the home's own (NULL: none) */
void fl_blend(const struct fl_counts *fleet, uint32_t homes, int level, const struct fl_counts *local,
	      double strength, struct fl_blend *out);
/* the candidate to use: 0 (the default) unless another one scores better by margin, and both have
   at least min_evidence tries behind them (fleet's and the home's) */
int fl_choose(const struct fl_cand *c, int n, double margin, unsigned min_evidence);
/* a blend in words: "helped 31% (fleet: 412 tries in 57 homes; here 1 of 3)" */
void fl_blend_words(const struct fl_blend *b, char *buf, size_t n);

#endif
