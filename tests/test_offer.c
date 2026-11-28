/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW signals for the ISP: weeks of homes, judged (ag/offer.h) */
#include <stdio.h>
#include <string.h>

#include "ag/offer.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define TODAY 20800L

static const uint8_t TV[6] = { 2, 0, 0, 0, 0, 1 }, LAPTOP[6] = { 2, 0, 0, 0, 0, 2 }, CAM[6] = { 2, 0, 0, 0, 0, 3 };

static const char *name_of(const uint8_t mac[6], void *ctx)
{
	(void)ctx;
	return !memcmp(mac, TV, 6) ? "TV" : !memcmp(mac, LAPTOP, 6) ? "laptop" : NULL;
}

static struct offer_day d[14];
static struct care_day c[14];
static struct offer_in in;
static struct offer_params p;
static struct offer_result r;

/* two quiet weeks: watched all day, nothing full or weak; care fine */
static void quiet(void)
{
	int i;

	memset(d, 0, sizeof(d));
	memset(c, 0, sizeof(c));
	for (i = 0; i < 14; i++) {
		d[i].day = TODAY - i;
		d[i].watched_s = 86400;
		d[i].peak_kbps = 20000;
		c[i].day = TODAY - i;
		c[i].checks = 144;
		c[i].good = 144;
		c[i].watched_secs = 86400;
		c[i].ok_secs = 86400;
	}
	memset(&in, 0, sizeof(in));
	in.today = TODAY;
	in.d = d;
	in.nd = 14;
	in.c = c;
	in.nc = 14;
	in.line_mbps = 50;
	in.line_sold = 1;
	in.name = name_of;
	offer_params_default(&p);
}

static int active(void)
{
	int k, n = 0;

	for (k = 0; k < OF_N; k++)
		n += r.o[k].active;
	return n;
}

static void show(void)
{
	int k;

	for (k = 0; k < OF_N; k++)
		if (r.o[k].active)
			printf("     %s (%s, %s): %s%s%s\n", r.o[k].code, r.o[k].product, offer_confidence_name(r.o[k].confidence),
			       r.o[k].evidence, r.o[k].named[0] ? " -- " : "", r.o[k].named);
}

int main(void)
{
	int i;

	/* 1. a quiet home: nothing to sell, nobody leaving */
	quiet();
	offer_judge(&in, &p, &r);
	CHECK(!active() && r.watched == 7 && !r.learning, "a quiet week: no signal (%d active, %d days)", active(), r.watched);

	/* 2. the line full every evening, 5 of 7 days, about 1 h 40 min: a faster plan, high */
	quiet();
	for (i = 0; i < 5; i++)
		d[i].full_s = 6000, d[i].full_evening_s = 5400, d[i].peak_kbps = 48000, d[i].full_slow_s = 600;
	offer_judge(&in, &p, &r);
	show();
	CHECK(r.o[OF_PLAN].active && r.o[OF_PLAN].confidence == OC_HIGH && r.o[OF_PLAN].days == 5,
	      "full 5 of 7 evenings: PLAN_UPGRADE, high");
	CHECK(strstr(r.o[OF_PLAN].evidence, "5 of the last 7 days") && strstr(r.o[OF_PLAN].evidence, "1 h 40 min") &&
	      strstr(r.o[OF_PLAN].evidence, "90% of it in the evening") && strstr(r.o[OF_PLAN].evidence, "48 of 50"),
	      "the evidence: how often, how long, when, how much");
	CHECK(r.o[OF_PLAN].since == (TODAY - 4) * 86400, "since the first of those days");
	CHECK(!r.o[OF_SQM].active, "the rest didn't suffer much: no SMART_QUEUE");
	/* ... with the line's speed only tested, not sold: medium; not known: low */
	in.line_sold = 0;
	offer_judge(&in, &p, &r);
	CHECK(r.o[OF_PLAN].confidence == OC_MEDIUM, "speed tested, not sold: medium");
	in.line_mbps = 0;
	offer_judge(&in, &p, &r);
	CHECK(r.o[OF_PLAN].confidence == OC_LOW && strstr(r.o[OF_PLAN].evidence, "isn't known"), "speed not known: low");

	/* 3. two busy evenings are not a pattern */
	quiet();
	d[0].full_s = d[3].full_s = 7200;
	offer_judge(&in, &p, &r);
	CHECK(!r.o[OF_PLAN].active, "full on 2 days: nothing");

	/* 4. full in short bursts, and everything else slow meanwhile: smart queueing */
	quiet();
	for (i = 0; i < 4; i++)
		d[i].full_s = 1500, d[i].full_slow_s = 1200;
	offer_judge(&in, &p, &r);
	show();
	CHECK(r.o[OF_SQM].active && !r.o[OF_PLAN].active && strstr(r.o[OF_SQM].evidence, "20 min a day"),
	      "bursts that slow the rest: SMART_QUEUE, not a faster plan");

	/* 5. two devices on a weak signal for hours: mesh Wi-Fi, by name at home */
	quiet();
	for (i = 0; i < 6; i++) {
		offer_day_weak(&d[i], LAPTOP, 4 * 3600);
		offer_day_weak(&d[i], TV, 2 * 3600);
		offer_day_weak(&d[i], CAM, 600);                /* a little weak: not counted */
		d[i].drops = 3;
	}
	offer_judge(&in, &p, &r);
	show();
	CHECK(r.o[OF_MESH].active && r.o[OF_MESH].confidence == OC_HIGH && r.o[OF_MESH].days == 6, "MESH_WIFI, high");
	CHECK(strstr(r.o[OF_MESH].evidence, "2 devices") && !strstr(r.o[OF_MESH].evidence, "laptop"),
	      "the evidence counts devices, without names");
	CHECK(!strcmp(r.o[OF_MESH].named, "laptop 4 h a day, TV 2 h a day"), "at home, by name: %s", r.o[OF_MESH].named);
	/* the home model (v0.52): a free change would do as much: no sale; an extender would help: said so */
	in.extender = -1;
	in.extender_why = "the home model expects a free change to fix as many coverage problems as an extender";
	offer_judge(&in, &p, &r);
	CHECK(!r.o[OF_MESH].active && strstr(r.o[OF_MESH].evidence, "Held: the home model"),
	      "the home model says a free change does as much: MESH_WIFI held");
	in.extender = 1;
	in.extender_why = "the home model expects an extender to fix 9 of the last 30 days' 12 coverage problems";
	offer_judge(&in, &p, &r);
	CHECK(r.o[OF_MESH].active && strstr(r.o[OF_MESH].evidence, "Before you buy: the home model expects an extender"),
	      "the home model says an extender helps: in the evidence");
	in.extender = 0;
	in.extender_why = NULL;
	/* ... one device, but it keeps dropping off */
	quiet();
	for (i = 0; i < 3; i++) {
		offer_day_weak(&d[i], CAM, 3600);
		d[i].drops = 14;
	}
	offer_judge(&in, &p, &r);
	CHECK(r.o[OF_MESH].active && r.o[OF_MESH].confidence == OC_MEDIUM && strstr(r.o[OF_MESH].named, "02:00:00:00:00:03"),
	      "one weak device dropping off 14 times a day: MESH_WIFI, medium (unnamed: its MAC)");

	/* 6. the day's table keeps the weakest when it is full */
	{
		struct offer_day x;
		uint8_t m[6] = { 2, 0, 0, 0, 1, 0 };

		memset(&x, 0, sizeof(x));
		for (i = 0; i < OFFER_WEAK + 4; i++) {
			m[5] = (uint8_t)i;
			offer_day_weak(&x, m, (uint32_t)(i + 1) * 60);
		}
		offer_day_weak(&x, m, 60);              /* the last one again: added up */
		for (i = 0; i < OFFER_WEAK && !(x.weak[i].mac[5] == OFFER_WEAK + 3); i++)
			;
		CHECK(i < OFFER_WEAK && x.weak[i].secs == (OFFER_WEAK + 4) * 60 + 60, "a device seen again adds up");
		for (i = 0; i < OFFER_WEAK && x.weak[i].mac[5] != 0; i++)
			;
		CHECK(i == OFFER_WEAK, "the least weak was dropped for weaker ones");
	}

	/* 7. a bad week after a good one: retention risk */
	quiet();
	for (i = 0; i < 7; i++) {
		c[i].ok_secs = 86400 * 80 / 100;
		c[i].problems = i < 3;
		c[i].reported = i < 2;
		c[i].bad[CA_INTERNET] = 20;
	}
	offer_judge(&in, &p, &r);
	show();
	CHECK(r.o[OF_RETAIN].active && r.o[OF_RETAIN].confidence == OC_HIGH && strstr(r.o[OF_RETAIN].evidence, "80%") &&
	      strstr(r.o[OF_RETAIN].evidence, "20 points below") && strstr(r.o[OF_RETAIN].evidence, "mostly Internet"),
	      "80%% after 100%%: RETENTION_RISK, high");
	/* ... a fine score but many problems */
	quiet();
	for (i = 0; i < 5; i++)
		c[i].problems = 1;
	offer_judge(&in, &p, &r);
	CHECK(r.o[OF_RETAIN].active && r.o[OF_RETAIN].confidence == OC_MEDIUM && strstr(r.o[OF_RETAIN].evidence, "5 problems"),
	      "5 problems in a week: RETENTION_RISK, medium");

	/* 8. the line underdelivers: repair it, don't sell on it */
	quiet();
	for (i = 0; i < 5; i++)
		d[i].full_s = 6000;
	for (i = 0; i < 7; i++)
		c[i].speed_tests = 1, c[i].speed_low = i < 3;
	offer_judge(&in, &p, &r);
	show();
	CHECK(r.o[OF_REPAIR].active && !r.o[OF_PLAN].active && strstr(r.o[OF_REPAIR].evidence, "3 of the last 7"),
	      "3 slow speed tests: LINE_REPAIR, and no PLAN_UPGRADE");

	/* 9. too little watched: learning */
	quiet();
	for (i = 2; i < 14; i++)
		d[i].watched_s = 3600;
	for (i = 0; i < 2; i++)
		d[i].full_s = 7200;
	offer_judge(&in, &p, &r);
	CHECK(r.learning && r.watched == 2 && !r.o[OF_PLAN].active, "2 days watched: learning");

	/* 10. the window: older days don't count */
	quiet();
	for (i = 7; i < 12; i++)
		d[i].full_s = 7200;
	offer_judge(&in, &p, &r);
	CHECK(!r.o[OF_PLAN].active, "full last week, not this one: nothing");
	CHECK(!strcmp(offer_code(OF_MESH), "MESH_WIFI") && !strcmp(offer_confidence_name(OC_HIGH), "high"), "names");

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
