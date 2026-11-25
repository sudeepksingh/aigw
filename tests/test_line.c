/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Line health: the robust trend, the change point and the days to a fault
 * (core/libag/line.c). tests/test_areapredict.py runs the same cases through
 * cortex's Python copy (linehealth.py) and expects the same answers.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/line.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define TODAY 20000L

/* a small, fixed wobble: +-w, the same in the Python test */
static float wob(int i, float w)
{
	static const float s[] = { 0.3f, -0.8f, 0.5f, -0.2f, 0.9f, -0.6f, 0.1f, -0.4f, 0.7f, -0.9f, 0.2f, 0.6f, -0.3f, -0.7f };

	return w * s[i % 14];
}

static struct line_trend T;
static struct line_day D[LINE_DAYS];

/* 14 days before today: v[i] for day TODAY - 14 + i */
static void run(int m, const float *v, int n)
{
	struct line_params p;
	int i;

	memset(D, 0, sizeof(D));
	for (i = 0; i < n; i++) {
		D[i].day = (int32_t)(TODAY - n + i);
		D[i].v = v[i];
		D[i].n = 96;
	}
	line_params_default(&p);
	line_analyse(D, n, TODAY, m, &p, &T);
}

int main(void)
{
	float v[LINE_DAYS];
	char w[300];
	int i, clean_conf;

	/* 1. a steady line, a little noise: steady */
	for (i = 0; i < 14; i++)
		v[i] = 10 + wob(i, 0.2f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_OK && T.days_left == -1 && fabsf(T.level - 10) < 0.3f, "steady noise margin: OK (%s, %.2f)",
	      line_state_name(T.state), T.level);

	/* 2. flat 4 days, then 0.4 dB a day worse: drifting, about 8 days to 3 dB */
	for (i = 0; i < 14; i++)
		v[i] = (i < 4 ? 10 : 10 - 0.4f * (float)(i - 3)) + wob(i, 0.05f);
	run(LM_MARGIN, v, 14);
	clean_conf = T.conf;
	CHECK(T.state == LS_DRIFT, "a falling noise margin: Drifting (%s)", line_state_name(T.state));
	CHECK(fabsf(T.slope + 0.4f) < 0.05f, "  its slope: %.3f dB a day", T.slope);
	CHECK(T.days_left >= 7 && T.days_left <= 9, "  a fault (3 dB) in %d days", T.days_left);
	CHECK(T.conf >= 70 && fabsf(T.base - 10) < 0.2f, "  high confidence (%d), base %.2f", T.conf, T.base);
	CHECK(T.onset >= TODAY - 11 && T.onset <= TODAY - 8, "  it began about day -10 (%ld)", (long)T.onset - TODAY);
	line_words(LM_MARGIN, &T, w, sizeof(w));
	CHECK(strstr(w, "The noise margin fell from 10 dB") && strstr(w, "a fault (3 dB) in about"), "  words: %s", w);

	/* 3. CRC errors step up and stay: stepped, not drifting, no fault yet */
	for (i = 0; i < 14; i++)
		v[i] = (i < 7 ? 2 : 40) + wob(i, 1);
	run(LM_CRC, v, 14);
	CHECK(T.state == LS_STEP && T.onset == TODAY - 7 && T.days_left == -1, "CRC errors 2 -> 40 an hour: Stepped on day "
	      "-7 (%s, %ld, %d)", line_state_name(T.state), (long)T.onset - TODAY, T.days_left);
	CHECK(fabsf(T.base - 2) < 1.5f && fabsf(T.level - 40) < 1.5f && T.conf >= 40, "  from %.1f to %.1f (conf %d)",
	      T.base, T.level, T.conf);
	line_words(LM_CRC, &T, w, sizeof(w));
	CHECK(strstr(w, "stepped up") && strstr(w, "stays there"), "  words: %s", w);

	/* 4. one bad day (a storm, a test): it doesn't tilt the trend */
	for (i = 0; i < 14; i++)
		v[i] = i == 9 ? 4 : 10 + wob(i, 0.2f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_OK, "one bad day among 14: still OK (%s)", line_state_name(T.state));

	/* 5. past the fault level: a fault now */
	for (i = 0; i < 14; i++)
		v[i] = 9 - 0.5f * (float)i + wob(i, 0.05f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_FAULT && T.days_left == 0, "noise margin down to %.1f dB: Fault, now (%s)", T.level,
	      line_state_name(T.state));

	/* 6. three days: learning */
	for (i = 0; i < 3; i++)
		v[i] = 10;
	run(LM_MARGIN, v, 3);
	CHECK(T.state == LS_LEARNING && T.ndays == 3, "3 days: Learning");

	/* 7. getting better is not a warning */
	for (i = 0; i < 14; i++)
		v[i] = 6 + 0.3f * (float)i + wob(i, 0.05f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_OK, "a rising noise margin: OK (%s)", line_state_name(T.state));

	/* 8. the sync rate, its fault relative to its base (70%) */
	for (i = 0; i < 14; i++)
		v[i] = (i < 6 ? 100000 : 100000 - 3000 * (float)(i - 5)) + wob(i, 200);
	run(LM_SYNC, v, 14);
	CHECK(T.state == LS_DRIFT && fabsf(T.fault - 70000) < 1000, "the sync rate falls 3000 kbit/s a day: Drifting, "
	      "fault at %.0f", T.fault);
	CHECK(T.days_left >= 2 && T.days_left <= 3, "  a fault in %d days (76000 -> 70000)", T.days_left);
	CHECK(T.onset >= TODAY - 10 && T.onset <= TODAY - 7, "  it began about day -8 (%ld)", (long)T.onset - TODAY);

	/* 9. a long line whose normal is past the usual fault level: judged by its own normal */
	for (i = 0; i < 14; i++)
		v[i] = 2.5f + wob(i, 0.1f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_OK && isnan(T.fault), "a noise margin of 2.5 dB, always: OK, no fault level (%s)",
	      line_state_name(T.state));

	/* 10. the same drift, much noisier: still seen, less sure */
	for (i = 0; i < 14; i++)
		v[i] = (i < 4 ? 10 : 10 - 0.4f * (float)(i - 3)) + wob(i, 0.8f);
	run(LM_MARGIN, v, 14);
	CHECK(T.state == LS_DRIFT && T.conf < clean_conf, "a noisy falling margin: Drifting, less sure (%d < %d)", T.conf,
	      clean_conf);

	/* 11. today and older days are left out */
	for (i = 0; i < 14; i++)
		v[i] = 10 + wob(i, 0.2f);
	run(LM_MARGIN, v, 14);
	D[0].day = TODAY - 30;
	D[1].day = TODAY;
	D[1].v = 1;
	{
		struct line_params p;

		line_params_default(&p);
		line_analyse(D, 14, TODAY, LM_MARGIN, &p, &T);
	}
	CHECK(T.ndays == 12 && T.state == LS_OK, "today and days older than 14 are left out (%d days)", T.ndays);

	/* 12. drops a day: counted, 0 is a value */
	for (i = 0; i < 14; i++)
		v[i] = i < 8 ? 0 : (float)(i - 7);
	run(LM_DROPS, v, 14);
	CHECK(T.state >= LS_STEP && T.days_left >= 0 && T.days_left <= 3, "drops 0 a day, then 1, 2 ... 6: %s, a fault "
	      "(base + 4) in %d days", line_state_name(T.state), T.days_left);

	printf(failed ? "\n%d FAILED\n" : "\nall passed\n", failed);
	return failed ? 1 : 0;
}
