/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Neighbour-aware channel choice. Pure (no I/O): agd (agd/chan.c) measures and
 * acts, this learns and judges; the unit test (tests/test_chanplan.c) feeds it
 * cases.
 *
 * For each channel AIGW may use, hour by hour of the day:
 *   air   how much of the air others use (% of the time), from the radio's
 *         survey: always for the channel in use, briefly for others after a scan
 *   nb    the neighbouring networks on it or overlapping it, from scans: a
 *         strong one weighs more, one that says it is busy (BSS Load) more
 *   feel  what the home's devices felt on it (loss, replies, link rate)
 * and the home's own use of the air by hour (its busy hours).
 *
 * Another channel is chosen only when it is clearly better over the busy
 * hours, after days of history, never from one moment.
 */
#ifndef AG_CHANPLAN_H
#define AG_CHANPLAN_H

#include <stddef.h>
#include <stdint.h>

#define CP_CH    9                      /* channels per band AIGW may use (2.4 GHz: 1, 6, 11) */
#define CP_HOURS 24

struct cp_cell {
	float air;                      /* % of the air used by others */
	float nb;                       /* neighbours' weight */
	float loss, rtt, rate;          /* devices: % lost, ms, Mbit/s */
	uint16_t n_air, n_nb, n_dev, n_rate;
};

struct cp_table {
	int band;                       /* 2 or 5 */
	int nch;
	int ch[CP_CH];
	struct cp_cell c[CP_CH][CP_HOURS];
	float use[CP_HOURS];            /* the home's own share of the air (% transmit time) */
	uint16_t n_use[CP_HOURS];
	long first;                     /* first sample (wall s); 0 none yet */
	uint32_t scans;                 /* neighbour scans learned from */
};

struct cp_params {
	int days;                       /* history before a choice (3) */
	int better;                     /* % less load that is clearly better (30) */
	int min_scans;                  /* scans before neighbours are compared (3) */
	int worse_hour;                 /* air points worse in any busy hour that rule a channel out (10) */
};

void cp_params_default(struct cp_params *p);
void cp_init(struct cp_table *t, int band, const int *ch, int n);
int cp_slot(const struct cp_table *t, int ch);          /* -1: not one AIGW uses */
/* a running mean for the first 8 samples, then a moving average (1/8): for rare samples (scans) */
void cp_add(float *avg, uint16_t *n, float v);
/* the same over a window of samples: for the looks every 30 s (CP_SLOW: a few days of one hour) */
#define CP_SLOW 512
void cp_add_n(float *avg, uint16_t *n, float v, int window);

/* one neighbouring network's weight on channel ch: on it or overlapping (2.4 GHz:
   less the farther away; 5 GHz: the same 20/40/80 MHz block), its signal, and
   how busy it says its channel is (util 0-255, -1 not said) */
float cp_nb_weight(int band, int ch, int width, int bss_ch, int signal, int util);

/* how much each hour matters: the home's busy hours (its own use at least half
   of its busiest hour's) 1, the others 0.1; none known yet: all 1. Returns the
   number of busy hours (0: not known) */
int cp_busy_hours(const struct cp_table *t, float w[CP_HOURS]);
void cp_hours_words(const float w[CP_HOURS], char *buf, size_t n);     /* "19:00-24:00", "all day" */

enum { CP_AIR = 1, CP_NB = 2 };
/* the load of a channel (slot) at an hour by one measure; -1 not known. Neighbours
   change little by the hour: an hour not scanned yet takes the channel's mean */
float cp_load(const struct cp_table *t, int slot, int hour, int metric);

enum cp_status { CP_LEARNING, CP_STAY, CP_BETTER };
struct cp_choice {
	int status;                     /* enum cp_status */
	int cur, best;                  /* channels (best 0: none) */
	int metric;                     /* CP_AIR or CP_NB: what the two were compared on */
	float cur_load, best_load;      /* weighted over the busy hours */
	int gain_pct;                   /* how much less load */
	int days;                       /* days of history */
	char words[300];
};
/* avoid[slot] 1: not this one (tried lately, not allowed here). 0 always */
int cp_choose(const struct cp_table *t, int cur, const uint8_t *avoid, long now, const struct cp_params *p,
	      struct cp_choice *out);

/* a channel's trial: what devices felt (and the air) before and after */
struct cp_feel {
	float loss, rtt, rate, air;
	int n_dev, n_rate, n_air;       /* how many samples each (0: not known) */
};
/* the learned "before" of a channel over the busy hours */
void cp_before(const struct cp_table *t, int slot, struct cp_feel *f);
/* 1 keep, 0 back; why in words. Kept when nothing devices feel is clearly
   worse and something is clearly better (the air counts as one of them) */
int cp_verdict(const struct cp_feel *before, const struct cp_feel *after, int better, char *why, size_t n);
void cp_feel_words(const struct cp_feel *f, char *buf, size_t n);      /* "3% lost, 126 ms, 58 Mbit/s" */

#endif
