/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW self-tuning smart queueing: bloat found, the rates, the trial judged (ag/sqm.h) */
#include <stdio.h>
#include <string.h>

#include "ag/sqm.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1800000000L
#define N  120

static struct why_min m[N];
static struct sqm_line line;
static struct sqm_params p;

/* two hours of minutes: quiet, 18 ms */
static void quiet(void)
{
	int i;

	memset(m, 0, sizeof(m));
	for (i = 0; i < N; i++) {
		m[i].t = (uint32_t)(T0 + i * 60);
		m[i].route = 1;
		m[i].in_sent = 3, m[i].in_ms = 18;
		m[i].down_kbps = 3000, m[i].up_kbps = 400;
	}
	memset(&line, 0, sizeof(line));
	line.down_mbps = 50;
	line.peak_down_mbps = 48, line.peak_up_mbps = 10;
	line.normal_ms = 18, line.have_normal = 1;
	sqm_params_default(&p);
}

/* minutes [a, b): the line full (47 Mbit/s), the internet answering in ms */
static void full(int a, int b, int ms, uint32_t kbps)
{
	int i;

	for (i = a; i < b; i++)
		m[i].down_kbps = kbps, m[i].in_ms = (uint16_t)ms;
}

int main(void)
{
	char why[400];
	struct sqm_stats before, after;
	uint32_t d, u;
	long now = T0 + N * 60;

	/* 1. quiet: no bloat */
	quiet();
	CHECK(!sqm_bloated(m, N, now, 60, &line, &p, why, sizeof(why)), "a quiet hour: no bloat");

	/* 2. the line full for 20 minutes, the internet at 180 ms meanwhile: bloat */
	quiet();
	full(90, 110, 180, 47000);
	CHECK(sqm_bloated(m, N, now, 60, &line, &p, why, sizeof(why)), "full and slow 20 of 60 minutes: bloat");
	printf("     %s\n", why);
	CHECK(strstr(why, "full 20 times, and 20 of them") && strstr(why, "180 ms") && strstr(why, "normally 18 ms"),
	      "in words");
	/* ... but full and fast is a line well used, not bloat */
	quiet();
	full(90, 110, 22, 47000);
	CHECK(!sqm_bloated(m, N, now, 60, &line, &p, why, sizeof(why)), "full but fast: no bloat");
	/* ... slow only now and then while full: not most of the time */
	quiet();
	full(70, 110, 22, 47000);
	full(90, 101, 180, 47000);
	CHECK(!sqm_bloated(m, N, now, 60, &line, &p, why, sizeof(why)), "slow on 11 of 40 full minutes: not bloat");
	/* ... and an hour ago doesn't count */
	quiet();
	full(10, 40, 180, 47000);
	CHECK(!sqm_bloated(m, N, now, 60, &line, &p, why, sizeof(why)), "bloat two hours ago: not now");

	/* 3. the rates: 90% of the sold speed, 85% of the most seen upload */
	quiet();
	CHECK(sqm_rates(&line, &p, &d, &u) == 0 && d == 45000 && u == 8500, "50 Mbit/s sold: 45000 / 8500 kbit/s (%u / %u)", d,
	      u);
	line.down_mbps = 0;
	CHECK(sqm_rates(&line, &p, &d, &u) == 0 && d == 40800, "not known: 85%% of the most seen, 40800 (%u)", d);
	line.peak_down_mbps = 1, line.peak_up_mbps = 0.2f;
	CHECK(sqm_rates(&line, &p, &d, &u) < 0 && !u, "nothing to go by: no rates");

	/* 4. the trial: before 180 ms at 47 Mbit/s; after 35 ms at 43: kept */
	quiet();
	full(0, 40, 180, 47000);
	full(60, 100, 35, 43000);
	sqm_stats_of(m, N, T0, T0 + 60 * 60, &line, &before);
	sqm_stats_of(m, N, T0 + 60 * 60, now, &line, &after);
	CHECK(before.n == 40 && before.ms == 180 && before.kbps == 47000, "before: 40 full, 180 ms, 47 Mbit/s");
	CHECK(after.n == 40 && after.ms == 35, "after: 40 full, 35 ms");
	CHECK(sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_KEEP && strstr(why, "35 ms (before 180 ms)"),
	      "kept: %s", why);
	/* ... too few full minutes yet: wait */
	sqm_stats_of(m, N, T0 + 60 * 60, T0 + 80 * 60, &line, &after);
	CHECK(after.n == 20 && sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_WAIT, "20 full minutes: wait");
	/* ... no better: undone */
	quiet();
	full(0, 40, 180, 47000);
	full(60, 100, 150, 47000);
	sqm_stats_of(m, N, T0, T0 + 60 * 60, &line, &before);
	sqm_stats_of(m, N, T0 + 60 * 60, now, &line, &after);
	CHECK(sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_UNDO && strstr(why, "still answered in 150 ms"),
	      "150 ms after 180: undone (%s)", why);
	/* ... better, but downloads far slower: undone */
	quiet();
	full(0, 40, 180, 47000);
	full(60, 100, 30, 30000);
	line.peak_down_mbps = 48;
	sqm_stats_of(m, N, T0, T0 + 60 * 60, &line, &before);
	/* (30 Mbit/s isn't "full" against a 50 Mbit/s line: the after samples, by the line shaped at 45) */
	line.down_mbps = 33;
	sqm_stats_of(m, N, T0 + 60 * 60, now, &line, &after);
	line.down_mbps = 50;
	CHECK(after.n == 40 && sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_UNDO &&
	      strstr(why, "slowed to 30.0 Mbit/s"), "30 ms but 30 of 47 Mbit/s: undone (%s)", why);
	/* ... before not measured: kept only when plainly fine */
	before.n = 0, before.ms = -1, before.kbps = 0;
	quiet();
	full(60, 100, 35, 43000);
	sqm_stats_of(m, N, T0 + 60 * 60, now, &line, &after);
	CHECK(sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_KEEP && strstr(why, "not measured"),
	      "before unknown, 35 ms after: kept");
	full(60, 100, 90, 43000);
	sqm_stats_of(m, N, T0 + 60 * 60, now, &line, &after);
	CHECK(sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_UNDO, "before unknown, 90 ms after: undone");

	/* ... shaped from an earlier, lower peak: the speed given up on purpose isn't "too slow" (CI, v0.33.0) */
	before.n = 40, before.ms = 200, before.kbps = 10400;
	after.n = 40, after.ms = 1, after.kbps = 7700;
	CHECK(sqm_judge(&before, &after, &line, &p, 0, why, sizeof(why)) == SQ_UNDO, "7.7 of 10.4 Mbit/s, rate not known: undone");
	CHECK(sqm_judge(&before, &after, &line, &p, 7200, why, sizeof(why)) == SQ_KEEP,
	      "7.7 of 10.4 Mbit/s, shaped at 7.2: kept (%s)", why);
	/* ... but a rate far too low is still no excuse */
	after.kbps = 3900;
	CHECK(sqm_judge(&before, &after, &line, &p, 4000, why, sizeof(why)) == SQ_UNDO && strstr(why, "slowed to 3.9"),
	      "3.9 of 10.4 Mbit/s, shaped at 4.0: undone (%s)", why);

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
