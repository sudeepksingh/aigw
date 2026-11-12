/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW Self-Healing Wi-Fi: the diagnosis, case by case */
#include <stdio.h>
#include <string.h>

#include "ag/heal.h"

static int failed;

static void expect(const char *name, const struct heal_sta *s, int n, enum heal_status st, enum heal_cause c,
		   unsigned affected)
{
	struct heal_params p;
	struct heal_result r;

	heal_params_default(&p);
	heal_diagnose(s, n, &p, &r);
	if (r.status != st || r.cause != c || r.affected != affected) {
		printf("FAIL %s: %s/%s affected %x, expected %s/%s %x\n", name, heal_status_name(r.status),
		       heal_cause_name(r.cause), r.affected, heal_status_name(st), heal_cause_name(c), affected);
		failed++;
	} else
		printf("ok   %s: %s, %s (loss %d%%, %d ms, %d measured)\n", name, heal_status_name(r.status),
		       heal_cause_name(r.cause), r.loss_pct, r.rtt_ms, r.measured);
}

int main(void)
{
	/* the real case on the PC's access point: channel 6, a phone at -70 dBm losing 15 % */
	struct heal_sta busy[] = { { 40, 6, 349, 6, -70, 0, 0 } };
	struct heal_sta far[] = { { 40, 6, 349, 6, -80, 0, 0 } };
	struct heal_sta good[] = { { 40, 0, 4, 4, -60, 0, 0 }, { 30, 1, 6, 5, -68, 1, 0 } };
	struct heal_sta one_bad[] = { { 40, 0, 4, 4, -55, 0, 0 }, { 40, 12, 40, 5, -58, 0, 0 }, { 40, 0, 5, 5, -66, 0, 0 } };
	struct heal_sta two_bad[] = { { 40, 4, 30, 5, -55, 0, 0 }, { 40, 6, 60, 5, -60, 0, 0 } };      /* 10 %, 15 % */
	struct heal_sta dead[] = { { 20, 20, -1, 5, -50, 0, 0 }, { 25, 25, -1, 5, -62, 0, 0 } };
	struct heal_sta hopping[] = { { 40, 1, 8, 6, -70, 6, 0 } };
	struct heal_sta asleep[] = { { 3, 2, 900, 0, -70, 0, 0 } };      /* too few awake probes: no verdict */
	struct heal_sta slow[] = { { 40, 0, 349, 5, -64, 0, 0 } };       /* no loss, 70 times its usual delay */
	struct heal_sta usual[] = { { 40, 0, 120, 100, -64, 0, 0 } };    /* slow, but it always is */
	struct heal_sta bad[] = { { 40, 12, 80, 5, -66, 0, 0 } };        /* 30 %: poor */

	expect("busy channel", busy, 1, HEAL_POOR, HC_CONGESTION, 1);
	expect("weak signal", far, 1, HEAL_POOR, HC_COVERAGE, 1);
	expect("all well", good, 2, HEAL_GOOD, HC_NONE, 0);
	expect("one device", one_bad, 3, HEAL_POOR, HC_DEVICE, 2);
	expect("two devices, fine signal", two_bad, 2, HEAL_DEGRADED, HC_CONGESTION, 3);
	expect("nobody answers", dead, 2, HEAL_POOR, HC_RADIO, 3);
	expect("band hopping", hopping, 1, HEAL_DEGRADED, HC_HOPPING, 1);
	expect("asleep", asleep, 1, HEAL_UNKNOWN, HC_NONE, 0);
	expect("slow", slow, 1, HEAL_POOR, HC_CONGESTION, 1);
	expect("slow as usual", usual, 1, HEAL_GOOD, HC_NONE, 0);
	expect("heavy loss", bad, 1, HEAL_POOR, HC_CONGESTION, 1);

	/* a signal at the line (the phone on the PC at -74..-76 dBm): with the margin,
	   "weak" holds until it is clearly better, so the cause stays WeakCoverage */
	{
		static const int sig[] = { -76, -74, -73, -75, -74, -72, -71, -74, -76 };
		static const int want[] = { 1, 1, 1, 1, 1, 1, 2, 2, 1 };
		struct heal_sta edge[] = { { 40, 8, 120, 6, 0, 0, 0 } };
		int k, w = 0, changes = 0, prev_cause = -1;

		for (k = 0; k < (int)(sizeof(sig) / sizeof(sig[0])); k++) {
			struct heal_params p;
			struct heal_result r;

			w = heal_weak_step(w, sig[k], -75, 3);
			if (w != want[k]) {
				printf("FAIL weak margin at %d dBm: %d, expected %d\n", sig[k], w, want[k]);
				failed++;
			}
			edge[0].signal = sig[k];
			edge[0].weak = w;
			heal_params_default(&p);
			heal_diagnose(edge, 1, &p, &r);
			if (prev_cause >= 0 && (int)r.cause != prev_cause)
				changes++;
			prev_cause = (int)r.cause;
		}
		if (changes != 2) {
			printf("FAIL weak margin: the cause changed %d times over the sequence, expected 2\n", changes);
			failed++;
		} else
			printf("ok   weak margin: -76..-72 dBm stays WeakCoverage; fine above -72; weak again at -76\n");
		if (heal_weak_step(1, 0, -75, 3) != 1 || heal_weak_step(0, -73, -75, 3) != 2) {
			printf("FAIL weak margin: no signal keeps the state; a first look in the margin is fine\n");
			failed++;
		}
	}

	/* the healing report: days in a ring of 31, summed per period */
	{
		static struct heal_report rep;
		struct heal_day s, *d;
		long t0 = 20000L * 86400 + 3600;      /* some day, 01:00 UTC */
		int k;

		for (k = 0; k < 40; k++) {          /* 40 days: the first 9 are gone from the ring */
			d = heal_report_day(&rep, t0 + k * 86400L);
			d->secs[HEAL_GOOD] += 3600;
			d->secs[HEAL_POOR] += 400;
			d->found[HC_CONGESTION]++;
			if (k % 2 == 0) {
				d->kept++;
				d->fixes++;
				d->fix_secs += 600;
				d->loss_before += 15;
				d->loss_after += 1;
			}
		}
		d = heal_report_day(&rep, t0 + 39 * 86400L + 7200);    /* the same day, later: kept */
		d->found[HC_COVERAGE]++;
		heal_report_sum(&rep, t0 + 39 * 86400L, 1, &s);
		if (s.secs[HEAL_GOOD] != 3600 || heal_day_problems(&s) != 2 || s.kept != 0 || heal_day_good_pct(&s) != 90) {
			printf("FAIL report, day: good %u s, %u problems, kept %u, %d%% good\n", s.secs[HEAL_GOOD],
			       heal_day_problems(&s), s.kept, heal_day_good_pct(&s));
			failed++;
		}
		heal_report_sum(&rep, t0 + 39 * 86400L, 7, &s);
		if (s.secs[HEAL_GOOD] != 7 * 3600 || s.found[HC_CONGESTION] != 7 || s.kept != 3 || s.fix_secs / s.fixes != 600) {
			printf("FAIL report, week: good %u s, congestion %u, kept %u\n", s.secs[HEAL_GOOD], s.found[HC_CONGESTION],
			       s.kept);
			failed++;
		}
		heal_report_sum(&rep, t0 + 39 * 86400L, 30, &s);
		if (s.found[HC_CONGESTION] != 30 || s.kept != 15 || s.loss_before / s.kept != 15 || s.loss_after / s.kept != 1) {
			printf("FAIL report, month: congestion %u, kept %u\n", s.found[HC_CONGESTION], s.kept);
			failed++;
		}
		heal_report_sum(&rep, t0 + 39 * 86400L, 365, &s);   /* at most what the ring holds */
		if (s.found[HC_CONGESTION] != 31) {
			printf("FAIL report, longer than the ring: %u days\n", s.found[HC_CONGESTION]);
			failed++;
		}
		if (!failed)
			printf("ok   healing report: day, week, month sums; old days fall out of the ring\n");
	}
	if (failed)
		printf("%d failed\n", failed);
	return failed ? 1 : 0;
}
