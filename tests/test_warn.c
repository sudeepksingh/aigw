/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Early warning: weeks of a simulated home line, a check every 10 minutes (and
 * a speed test each night): what is normal stays quiet, a slow drift and a
 * sub-fault shift are seen before they are faults, a fault is never learned.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/care.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0    (20000L * 86400)
#define STEP  600
#define DAY   (86400 / STEP)

static unsigned long seed = 12345;

static float uni(void)
{
	seed = seed * 6364136223846793005UL + 1442695040888963407UL;
	return (float)((seed >> 33) & 0xffffff) / (float)0x1000000;
}

/* about normal, deviation 1 */
static float gauss(void)
{
	return (uni() + uni() + uni() + uni() - 2.0f) * 1.732f;
}

struct sim {
	struct care_learn L;
	struct care_learn_params p;
	long t;
	int opened, cleared, changed, checks;
	long first_open;
	struct care_warning at_open;
	/* the line: added to the normal */
	float gw_add, inet_add, loss_p, dns_add;
	float speed;                    /* tonight's speed */
	unsigned quiet;
};

static void init(struct sim *s)
{
	memset(s, 0, sizeof(*s));
	care_learn_params_default(&s->p);
	s->t = T0;
	s->loss_p = 0.03f;
	s->speed = 95;
}

/* one check: a normal home line (busier evenings) plus what the test adds */
static int step(struct sim *s)
{
	struct care_sample x;
	int hour = (int)(s->t / 3600 % 24), ev;
	float evening = hour >= 19 && hour <= 22 ? 20.0f : 0.0f;

	memset(&x, 0, sizeof(x));
	x.have[CM_GW_MS] = x.have[CM_INET_MS] = x.have[CM_LOSS] = x.have[CM_DNS_MS] = 1;
	x.v[CM_GW_MS] = 1.0f + 0.3f * fabsf(gauss()) + s->gw_add;
	x.v[CM_INET_MS] = 15 + evening + 2 * gauss() + s->inet_add;
	if (x.v[CM_INET_MS] < 1)
		x.v[CM_INET_MS] = 1;
	x.v[CM_LOSS] = uni() < s->loss_p ? 10.0f : 0.0f;      /* 10 echo requests: 1 lost is 10% */
	x.v[CM_DNS_MS] = 10 + 3 * fabsf(gauss()) + s->dns_add;
	if (s->t % 86400 == 3 * 3600) {                          /* the nightly speed test, 03:00 */
		x.have[CM_SPEED] = 1;
		x.v[CM_SPEED] = s->speed + 2 * gauss();
	}
	ev = care_learn_step(&s->L, &x, hour, s->t / 86400, s->t, s->quiet, &s->p);
	if (ev & CW_OPENED) {
		s->opened++;
		if (!s->first_open) {
			s->first_open = s->t;
			s->at_open = s->L.w;
		}
	}
	if (ev & CW_CLEARED)
		s->cleared++;
	if (ev & CW_CHANGED)
		s->changed++;
	s->checks++;
	s->t += STEP;
	return ev;
}

static void run(struct sim *s, int checks)
{
	while (checks-- > 0)
		step(s);
}

static void say(const struct sim *s)
{
	char text[400];

	care_warning_text(&s->L.w, &s->p, text, sizeof(text));
	printf("     %s: %s\n", care_metric_name(s->L.w.metric), text);
}

int main(void)
{
	struct sim s;
	float mean, dev;
	int i, at;

	/* a normal line for a month, busier in the evenings, now and then a lost packet: quiet */
	init(&s);
	run(&s, 3 * DAY);
	CHECK(care_learning(&s.L, s.t - STEP, &s.p) && !s.opened, "the first 3 days: learning, no warning");
	run(&s, 27 * DAY);
	CHECK(!s.opened, "30 days of a normal line (busy evenings, a lost packet now and then): no warning");
	CHECK(care_normal(&s.L, CM_INET_MS, 20, &mean, &dev) == 0 && mean > 30 && mean < 40, "the evening's normal: %.1f ms",
	      mean);
	CHECK(care_normal(&s.L, CM_INET_MS, 4, &mean, &dev) == 0 && mean > 12 && mean < 18, "the night's normal: %.1f ms",
	      mean);

	/* short spikes are blips */
	for (i = 0; i < 3; i++) {
		s.inet_add = 60;
		step(&s);
	}
	s.inet_add = 0;
	run(&s, DAY);
	CHECK(!s.opened, "three checks of +60 ms (30 minutes): no warning");

	/* a shift well below the fault (150 ms): +30 ms, all day */
	s.inet_add = 30;
	at = s.checks;
	while (!s.opened && s.checks - at < DAY)
		step(&s);
	CHECK(s.opened == 1 && s.at_open.metric == CM_INET_MS && s.at_open.kind == CW_SHIFT && s.checks - at <= 12,
	      "+30 ms on the internet's delay: a shift warning after %d checks", s.checks - at);
	say(&s);
	s.inet_add = 0;
	at = s.checks;
	while (!s.cleared && s.checks - at < DAY)
		step(&s);
	CHECK(s.cleared == 1 && s.checks - at <= 36, "back to normal: cleared after %d checks", s.checks - at);
	CHECK(care_normal(&s.L, CM_INET_MS, 4, &mean, &dev) == 0 && mean < 20,
	      "the shifted hours were learned slowly (night normal %.1f ms)", mean);

	/* a slow drift: the internet's delay 6 ms worse every day */
	init(&s);
	run(&s, 14 * DAY);
	at = s.checks;
	while (!s.opened && s.checks - at < 25 * DAY) {
		s.inet_add = 6.0f * (float)(s.checks - at) / DAY;
		step(&s);
	}
	CHECK(s.opened && s.at_open.metric == CM_INET_MS && s.inet_add < 40,
	      "6 ms worse a day: warned after %.1f days, at +%.0f ms (the fault is 150 ms)", (s.checks - at) / (double)DAY,
	      s.inet_add);
	say(&s);
	while (s.L.w.active && s.L.w.kind != CW_TREND && s.checks - at < 25 * DAY) {
		s.inet_add = 6.0f * (float)(s.checks - at) / DAY;
		step(&s);
	}
	CHECK(s.L.w.active && s.L.w.kind == CW_TREND && s.L.w.predict > 0 && s.L.w.predict <= 14 && s.L.w.per_day > 3,
	      "then a trend: %.1f ms a day, a fault in about %d days", s.L.w.per_day, s.L.w.predict);
	say(&s);
	CHECK(s.cleared == 0 && s.changed == 0, "one warning, kept while it lasts");

	/* a fault (a problem care reports) is never learned as normal, and isn't warned about */
	init(&s);
	run(&s, 10 * DAY);
	s.inet_add = 300;
	s.quiet = 1u << CA_INTERNET;
	run(&s, DAY);
	CHECK(!s.opened, "a day of a fault (a problem open): no warning on top");
	s.inet_add = 0;
	s.quiet = 0;
	CHECK(care_normal(&s.L, CM_INET_MS, 4, &mean, &dev) == 0 && mean < 20, "and not learned (night normal %.1f ms)", mean);
	run(&s, 2 * DAY);
	CHECK(!s.opened, "after it: still quiet");

	/* packets start to get lost: 1 lost in 10 on a third of the checks */
	init(&s);
	run(&s, 10 * DAY);
	s.loss_p = 0.33f;
	at = s.checks;
	while (!s.opened && s.checks - at < DAY)
		step(&s);
	CHECK(s.opened && s.at_open.metric == CM_LOSS, "loss creeping up (about 3%%): warned after %d checks", s.checks - at);
	say(&s);

	/* DNS slower than usual (60 ms, the fault is 300) */
	init(&s);
	run(&s, 10 * DAY);
	s.dns_add = 60;
	at = s.checks;
	while (!s.opened && s.checks - at < DAY)
		step(&s);
	CHECK(s.opened && s.at_open.metric == CM_DNS_MS && care_metric_area(CM_DNS_MS) == CA_DNS,
	      "DNS 60 ms slower: warned after %d checks (area DNS)", s.checks - at);

	/* the nightly speed: 95 Mbit/s for two weeks, then 60 */
	init(&s);
	run(&s, 14 * DAY);
	CHECK(!s.opened && care_normal(&s.L, CM_SPEED, 3, &mean, &dev) == 0 && fabsf(mean - 95) < 4,
	      "two weeks of 95 Mbit/s: its normal %.1f, no warning", mean);
	s.speed = 60;
	at = s.checks;
	while (!s.opened && s.checks - at < 5 * DAY)
		step(&s);
	CHECK(s.opened && s.at_open.metric == CM_SPEED && s.checks - at <= 2 * DAY,
	      "60 Mbit/s: warned after %d night%s", (s.checks - at + DAY - 1) / DAY, (s.checks - at + DAY - 1) / DAY == 1 ? "" : "s");
	say(&s);

	/* a speed falling 4 Mbit/s a night, with the line's speed known (at least 50 Mbit/s) */
	init(&s);
	s.p.fault[CM_SPEED] = 50;
	run(&s, 14 * DAY);
	at = s.checks;
	while (!(s.L.w.active && s.L.w.kind == CW_TREND) && s.checks - at < 12 * DAY) {
		s.speed = 95 - 4.0f * (float)(s.checks - at) / DAY;
		step(&s);
	}
	CHECK(s.L.w.active && s.L.w.kind == CW_TREND && s.L.w.metric == CM_SPEED && s.speed > 50 && s.L.w.predict > 0,
	      "4 Mbit/s less a night: a trend at %.0f Mbit/s, below 50 in about %d days", s.speed, s.L.w.predict);
	say(&s);

	/* the words */
	{
		char v[24];

		care_metric_value(CM_LOSS, 0.42f, v, sizeof(v));
		CHECK(!strcmp(v, "0.4%"), "a loss: %s", v);
		care_metric_value(CM_SPEED, 94.6f, v, sizeof(v));
		CHECK(!strcmp(v, "95 Mbit/s"), "a speed: %s", v);
		CHECK(!strcmp(care_metric_name(CM_INET_MS), "InternetLatency") && care_warning_advice(CM_GW_MS)[0],
		      "names and advice");
	}

	printf(failed ? "\n%d FAILED\n" : "\nall passed\n", failed);
	return failed ? 1 : 0;
}
