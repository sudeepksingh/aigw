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
	struct heal_sta busy[] = { { 40, 6, 349, 6, -70, 0 } };
	struct heal_sta far[] = { { 40, 6, 349, 6, -80, 0 } };
	struct heal_sta good[] = { { 40, 0, 4, 4, -60, 0 }, { 30, 1, 6, 5, -68, 1 } };
	struct heal_sta one_bad[] = { { 40, 0, 4, 4, -55, 0 }, { 40, 12, 40, 5, -58, 0 }, { 40, 0, 5, 5, -66, 0 } };
	struct heal_sta two_bad[] = { { 40, 4, 30, 5, -55, 0 }, { 40, 6, 60, 5, -60, 0 } };      /* 10 %, 15 % */
	struct heal_sta dead[] = { { 20, 20, -1, 5, -50, 0 }, { 25, 25, -1, 5, -62, 0 } };
	struct heal_sta hopping[] = { { 40, 1, 8, 6, -70, 6 } };
	struct heal_sta asleep[] = { { 3, 2, 900, 0, -70, 0 } };      /* too few awake probes: no verdict */
	struct heal_sta slow[] = { { 40, 0, 349, 5, -64, 0 } };       /* no loss, 70 times its usual delay */
	struct heal_sta usual[] = { { 40, 0, 120, 100, -64, 0 } };    /* slow, but it always is */
	struct heal_sta bad[] = { { 40, 12, 80, 5, -66, 0 } };        /* 30 %: poor */

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
	if (failed)
		printf("%d failed\n", failed);
	return failed ? 1 : 0;
}
