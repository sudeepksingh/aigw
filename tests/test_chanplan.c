/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW neighbour-aware channels: learning by hour, the choice, the trial's verdict (ag/chanplan.h) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/chanplan.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1800000000L
#define DAY 86400L

static const int ch24[] = { 1, 6, 11 };
static struct cp_table t;
static struct cp_params p;

/* the home's own use: busy 19:00-24:00 */
static void home_evenings(void)
{
	int h, k;

	for (h = 0; h < CP_HOURS; h++)
		for (k = 0; k < 20; k++)
			cp_add(&t.use[h], &t.n_use[h], h >= 19 ? 30.0f : 2.0f);
}

/* one scan at an hour: neighbours (channel, dBm, BSS Load or -1) */
static void scan_at(int hour, const int (*bss)[3], int n)
{
	int k, i;

	for (k = 0; k < t.nch; k++) {
		float w = 0;

		for (i = 0; i < n; i++)
			w += cp_nb_weight(2, t.ch[k], 20, bss[i][0], bss[i][1], bss[i][2]);
		cp_add(&t.c[k][hour].nb, &t.c[k][hour].n_nb, w);
	}
	t.scans++;
}

static void air(int slot, int hour, float v, int n)
{
	while (n--)
		cp_add(&t.c[slot][hour].air, &t.c[slot][hour].n_air, v);
}

int main(void)
{
	struct cp_choice c;
	struct cp_feel b, a;
	char why[300], words[64];
	float w[CP_HOURS], avg = 0;
	uint16_t n = 0;
	uint8_t avoid[CP_CH] = { 0 };
	int i, h;

	cp_params_default(&p);

	/* ---- the moving average ---- */
	for (i = 1; i <= 4; i++)
		cp_add(&avg, &n, (float)i * 10);
	CHECK(fabsf(avg - 25) < 0.01f && n == 4, "a running mean at first (%.1f of 4 samples)", avg);
	for (i = 0; i < 200; i++)
		cp_add(&avg, &n, 80);
	CHECK(fabsf(avg - 80) < 0.5f, "then it follows the new level (%.1f)", avg);

	/* ---- neighbours' weight ---- */
	CHECK(cp_nb_weight(2, 11, 20, 11, -50, -1) == 3, "a strong network on the channel weighs 3");
	CHECK(fabsf(cp_nb_weight(2, 11, 20, 13, -50, -1) - 1.8f) < 0.01f, "one on 13 overlaps 11 (3 x 0.6)");
	CHECK(cp_nb_weight(2, 6, 20, 11, -50, -1) == 0 && cp_nb_weight(2, 1, 20, 6, -40, -1) == 0,
	      "1, 6 and 11 don't overlap each other");
	CHECK(cp_nb_weight(2, 6, 20, 6, -85, 255) > cp_nb_weight(2, 6, 20, 6, -85, 0) * 4,
	      "a network that says its channel is busy (BSS Load) weighs more");
	CHECK(cp_nb_weight(5, 36, 80, 44, -50, -1) == 3 && cp_nb_weight(5, 36, 20, 44, -50, -1) == 0 &&
	      cp_nb_weight(5, 36, 80, 149, -50, -1) == 0, "5 GHz: the same 80 MHz block counts, others don't");

	/* ---- busy hours ---- */
	cp_init(&t, 2, ch24, 3);
	CHECK(cp_busy_hours(&t, w) == 0 && w[3] == 1 && w[21] == 1, "busy hours not known yet: every hour counts");
	home_evenings();
	CHECK(cp_busy_hours(&t, w) == 5 && w[21] == 1 && w[10] < 0.5f, "the home is busy 19:00-24:00 (5 hours)");
	cp_hours_words(w, words, sizeof(words));
	CHECK(!strcmp(words, "19:00-24:00"), "in words: %s", words);

	/* ---- the field case: channel 11 crowded by networks on 11 and 13; 1 has some; 6 hardly any ---- */
	{
		static const int crowd[][3] = {
			{ 11, -55, 200 }, { 11, -62, -1 }, { 13, -58, 180 }, { 13, -70, -1 },  /* Airtel / AirFiber */
			{ 1, -72, 60 }, { 1, -80, -1 }, { 3, -82, -1 },
			{ 6, -88, -1 },
		};
		static const int empty1[][3] = { { 11, -55, 200 }, { 13, -58, 180 }, { 6, -88, -1 } };

		t.first = T0;
		scan_at(3, empty1, 3);          /* one moment where channel 1 looks empty */
		c.status = -1;
		cp_choose(&t, 11, avoid, T0 + DAY, &p, &c);
		CHECK(c.status == CP_LEARNING, "one day of history: still learning (%s)", c.words);
		cp_choose(&t, 11, avoid, T0 + 4 * DAY, &p, &c);
		CHECK(c.status == CP_STAY, "one scan isn't enough to move on (%s)", c.words);
		for (h = 0; h < 4; h++)
			scan_at(h == 0 ? 2 : h == 1 ? 4 : 21, crowd, 8);
		cp_choose(&t, 11, avoid, T0 + 4 * DAY, &p, &c);
		CHECK(c.status == CP_BETTER && c.best == 6 && c.metric == CP_NB,
		      "learned: 6 is clearly better than 11, not 1 (%s)", c.words);
		CHECK(c.gain_pct >= 30, "by %d%%", c.gain_pct);
		avoid[cp_slot(&t, 6)] = 1;
		cp_choose(&t, 11, avoid, T0 + 4 * DAY, &p, &c);
		CHECK(c.best != 6, "6 avoided (it didn't help lately): %s", c.words);
		avoid[cp_slot(&t, 6)] = 0;
		cp_choose(&t, 6, avoid, T0 + 4 * DAY, &p, &c);
		CHECK(c.status == CP_STAY, "on 6, no move back (%s)", c.words);
	}

	/* ---- the air, where both channels were measured in the busy hours ---- */
	cp_init(&t, 2, ch24, 3);
	home_evenings();
	t.first = T0;
	for (h = 0; h < CP_HOURS; h++) {
		air(2, h, h >= 19 ? 55.0f : 15.0f, 10);        /* 11 */
		air(1, h, h >= 19 ? 20.0f : 10.0f, 10);        /* 6 */
		air(0, h, h >= 19 ? 50.0f : 5.0f, 10);         /* 1: quiet by day, busy at night */
	}
	cp_choose(&t, 11, avoid, T0 + 5 * DAY, &p, &c);
	CHECK(c.status == CP_BETTER && c.best == 6 && c.metric == CP_AIR, "by the air in the busy hours: 6 (%s)",
	      c.words);
	CHECK(c.cur_load > 40 && c.best_load < 25, "weighted over the busy hours: %.0f%% vs %.0f%%", c.cur_load,
	      c.best_load);
	/* 6 is quieter on average but much worse in one busy hour */
	air(1, 22, 90.0f, 60);
	cp_choose(&t, 11, avoid, T0 + 5 * DAY, &p, &c);
	CHECK(c.best != 6, "worse in one of the busy hours: ruled out (%s)", c.words);
	/* a small difference is not clearly better */
	cp_init(&t, 2, ch24, 3);
	home_evenings();
	t.first = T0;
	for (h = 0; h < CP_HOURS; h++) {
		air(2, h, 40.0f, 10);
		air(1, h, 32.0f, 10);
		air(0, h, 45.0f, 10);
	}
	cp_choose(&t, 11, avoid, T0 + 5 * DAY, &p, &c);
	CHECK(c.status == CP_STAY && !c.best, "8 points less (20%%) is not clearly better (%s)", c.words);

	/* ---- the trial's verdict ---- */
	memset(&b, 0, sizeof(b));
	memset(&a, 0, sizeof(a));
	b.loss = 4, b.rtt = 126, b.n_dev = 100;
	a.loss = 3, a.rtt = 175, a.n_dev = 100;
	CHECK(!cp_verdict(&b, &a, 30, why, sizeof(why)), "the field trial (4%%/126 ms -> 3%%/175 ms): back (%s)", why);
	a.loss = 1, a.rtt = 60;
	b.loss = 30;
	CHECK(cp_verdict(&b, &a, 30, why, sizeof(why)), "loss 30%% -> 1%%: kept (%s)", why);
	b.loss = 4;
	a.loss = 4, a.rtt = 120;
	CHECK(!cp_verdict(&b, &a, 30, why, sizeof(why)), "about the same: back (%s)", why);
	b.rate = 50, b.n_rate = 50;
	a.rate = 72, a.n_rate = 50;
	CHECK(cp_verdict(&b, &a, 30, why, sizeof(why)), "link rates clearly higher, nothing worse: kept (%s)", why);
	a.loss = 12;
	CHECK(!cp_verdict(&b, &a, 30, why, sizeof(why)), "higher rates but more loss: back (%s)", why);
	a.loss = 4;
	a.rate = 52;
	b.air = 50, b.n_air = 100;
	a.air = 22, a.n_air = 100;
	CHECK(cp_verdict(&b, &a, 30, why, sizeof(why)), "others use clearly less of the air: kept (%s)", why);
	memset(&a, 0, sizeof(a));
	a.air = 10, a.n_air = 10;
	CHECK(!cp_verdict(&b, &a, 30, why, sizeof(why)), "no device measured: back (%s)", why);

	/* ---- before, from what was learned ---- */
	cp_init(&t, 2, ch24, 3);
	home_evenings();
	for (h = 0; h < CP_HOURS; h++)
		for (i = 0; i < 10; i++) {
			struct cp_cell *x = &t.c[2][h];
			uint16_t k = x->n_dev;

			cp_add(&x->loss, &k, h >= 19 ? 30.0f : 2.0f);
			cp_add(&x->rtt, &x->n_dev, h >= 19 ? 400.0f : 20.0f);
		}
	cp_before(&t, 2, &b);
	CHECK(b.loss > 20 && b.rtt > 250 && b.n_dev == 240, "before = the busy hours' (%.0f%%, %.0f ms)", b.loss, b.rtt);

	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
