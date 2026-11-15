/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW proactive care: when a problem starts and ends, who hears of it, the score */
#include <stdio.h>
#include <string.h>

#include "ag/care.h"
#include "ag/diag.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct care_verdict OK_V = { "OK", "All", DS_OK, "fine" };
static const struct care_verdict INFO_V = { "DEVICE_ON_24GHZ", "Device", DS_INFO, "kept on 2.4 GHz" };
static const struct care_verdict ISP_V = { "ISP_DOWN", "Internet", DS_PROBLEM, "the internet doesn't answer" };
static const struct care_verdict DNS_V = { "DNS_FAILING", "DNS", DS_PROBLEM, "names don't resolve" };
static const struct care_verdict WIFI_V = { "WIFI_CONGESTION", "WiFi", DS_WARN, "channel 11 is busy" };

#define DAY0 (20000L * 86400 + 3600)    /* some day, 01:00 UTC */

int main(void)
{
	struct care_params p;
	struct care_state s;
	struct care_report r;
	struct care_day d;
	long t = DAY0;
	int ev;

	care_params_default(&p);
	p.wifi_grace = 1800;

	/* fine checks, and information only, are no problem */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	ev = care_step(&s, &r, &OK_V, t, &p);
	ev |= care_step(&s, &r, &INFO_V, t += 600, &p);
	CHECK(!ev && !s.active, "OK and information: nothing happens");

	/* one bad check is a blip */
	ev = care_step(&s, &r, &ISP_V, t += 600, &p);
	CHECK(!ev && !s.active && s.bad_run == 1, "one bad check: a blip, not yet a problem");
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	CHECK(!ev && !s.active && !s.bad_run, "the next check is fine: no problem at all");

	/* the line: confirmed, then reported at once */
	ev = care_step(&s, &r, &ISP_V, t += 600, &p);
	long started = t;
	ev = care_step(&s, &r, &ISP_V, t += 60, &p);
	CHECK((ev & CARE_STARTED) && (ev & CARE_REPORT) && s.active && s.reported && s.since == started,
	      "the internet down twice: a problem since the first check, reported to the ISP at once");
	ev = care_step(&s, &r, &ISP_V, t += 60, &p);
	CHECK(!ev, "still down: reported once only");
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	CHECK(!ev && s.active, "one fine check: not over yet");
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	CHECK((ev & CARE_CLEARED) && !s.active && s.duration == t - started,
	      "two fine checks: over, after %ld s", s.duration);
	care_report_sum(&r, t, 1, &d);
	CHECK(d.problems == 1 && d.reported == 1 && d.fixed == 0 && d.bad[CA_INTERNET] == 4 &&
	      care_main_cause(&d) == CA_INTERNET && d.problem_secs == 240 && d.problem_secs == s.duration,
	      "today: 1 problem, reported, mostly Internet, 240 s with the problem open, from its first bad check, "
	      "as long as it lasted (got %u s)", d.problem_secs);

	/* DNS is the ISP's too */
	memset(&s, 0, sizeof(s));
	care_step(&s, &r, &DNS_V, t += 600, &p);
	ev = care_step(&s, &r, &DNS_V, t += 60, &p);
	CHECK((ev & CARE_REPORT), "DNS failing: reported at once");

	/* Wi-Fi: AIGW's to fix first */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	t = DAY0;
	care_step(&s, &r, &WIFI_V, t, &p);
	ev = care_step(&s, &r, &WIFI_V, t += 60, &p);
	CHECK((ev & CARE_STARTED) && !(ev & CARE_REPORT) && !s.reported,
	      "Wi-Fi congestion: a problem, but Self-Healing Wi-Fi's to fix; not reported");
	care_acted(&s, t + 300);                /* a channel trial took effect */
	care_step(&s, &r, &WIFI_V, t += 600, &p);
	care_step(&s, &r, &OK_V, t += 60, &p);
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	care_report_sum(&r, t, 1, &d);
	CHECK((ev & CARE_CLEARED) && d.fixed == 1 && d.cleared == 0 && d.reported == 0,
	      "AIGW changed the channel, then it was over within the grace time: fixed by AIGW, never reported");

	/* over by itself: AIGW changed nothing (advise mode, nobody answered) */
	care_step(&s, &r, &WIFI_V, t += 600, &p);
	care_step(&s, &r, &WIFI_V, t += 60, &p);
	care_step(&s, &r, &OK_V, t += 60, &p);
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	care_report_sum(&r, t, 1, &d);
	CHECK((ev & CARE_CLEARED) && d.fixed == 1 && d.cleared == 1,
	      "over though AIGW changed nothing (its last change was before): cleared by itself, not AIGW's fix");

	/* AIGW acted shortly before the first bad check, on the same trouble (a
	   radio Poor before care's check saw it): its fix */
	care_acted(&s, t + 300);
	care_step(&s, &r, &WIFI_V, t += 600, &p);
	care_step(&s, &r, &WIFI_V, t += 60, &p);
	care_step(&s, &r, &OK_V, t += 60, &p);
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	care_report_sum(&r, t, 1, &d);
	CHECK((ev & CARE_CLEARED) && d.fixed == 2 && d.cleared == 1,
	      "acted 5 min before the first bad check: fixed by AIGW");

	/* Wi-Fi that lasts: reported after the grace time */
	memset(&s, 0, sizeof(s));
	care_step(&s, &r, &WIFI_V, t += 600, &p);
	started = t;
	care_step(&s, &r, &WIFI_V, t += 60, &p);
	ev = care_step(&s, &r, &WIFI_V, t = started + 1700, &p);
	CHECK(!(ev & CARE_REPORT), "Wi-Fi bad for 28 min: still AIGW's");
	ev = care_step(&s, &r, &WIFI_V, t = started + 1800, &p);
	CHECK((ev & CARE_REPORT) && s.reported, "Wi-Fi bad for 30 min: now the ISP hears of it");
	care_step(&s, &r, &OK_V, t += 60, &p);
	ev = care_step(&s, &r, &OK_V, t += 60, &p);
	care_report_sum(&r, t, 1, &d);
	CHECK((ev & CARE_CLEARED) && d.fixed == 2 && d.cleared == 1,
	      "over after being reported: neither fixed by AIGW nor cleared by itself");

	/* a Wi-Fi problem that becomes the line's: reported at once */
	memset(&s, 0, sizeof(s));
	care_step(&s, &r, &WIFI_V, t += 600, &p);
	care_step(&s, &r, &WIFI_V, t += 60, &p);
	ev = care_step(&s, &r, &ISP_V, t += 60, &p);
	CHECK((ev & CARE_CHANGED) && (ev & CARE_REPORT) && !strcmp(s.code, "ISP_DOWN"),
	      "Wi-Fi, then the internet down: changed, and reported at once");

	/* a gap (agd was off) is not problem time */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	t = DAY0;
	care_step(&s, &r, &ISP_V, t, &p);
	care_step(&s, &r, &ISP_V, t += 60, &p);
	care_step(&s, &r, &ISP_V, t += 7200, &p);
	care_report_sum(&r, t, 1, &d);
	CHECK(d.problem_secs == 60 && d.watched_secs == 60,
	      "2 hours without checks: neither watched nor problem time (got %u s of problem)", d.problem_secs);

	/* the score: of the time, not of the checks */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	t = DAY0;
	{
		int i;

		for (i = 0; i < 9; i++)
			care_step(&s, &r, &OK_V, t += 600, &p);
		care_step(&s, &r, &WIFI_V, t += 600, &p);
		care_step(&s, &r, &OK_V, t += 60, &p);
	}
	care_report_sum(&r, t, 1, &d);
	CHECK(care_score(&d) == 100 && d.checks == 11 && d.good == 10,
	      "one bad check among fine ones is a blip, not a problem: score 100%% (got %d)", care_score(&d));

	/* a day like a real one: fine checks every 10 min, then a 20-minute Wi-Fi
	   problem rechecked every minute; counting checks said 71%% */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	t = DAY0;
	{
		int i;

		for (i = 0; i < 36; i++)
			care_step(&s, &r, &OK_V, t += 600, &p);         /* 6 h fine */
		for (i = 0; i < 20; i++)
			care_step(&s, &r, &WIFI_V, t += i ? 60 : 600, &p);
		care_step(&s, &r, &OK_V, t += 60, &p);
		care_step(&s, &r, &OK_V, t += 60, &p);
		for (i = 0; i < 12; i++)
			care_step(&s, &r, &OK_V, t += 600, &p);         /* 2 h fine */
	}
	care_report_sum(&r, t, 1, &d);
	CHECK(d.checks == 70 && d.good == 50 && d.problems == 1, "70 checks, 20 of them bad, 1 problem");
	CHECK(d.watched_secs == 30060 && d.problem_secs == 1260 && d.ok_secs == 28800,
	      "8 h 21 m watched, 21 min of it with the problem (got %u s, %u s)", d.watched_secs, d.problem_secs);
	CHECK(care_score(&d) == 95, "the score is of the time: 95%% (got %d)", care_score(&d));

	/* days saved before v0.29.1 have no times: the score by checks */
	memset(&d, 0, sizeof(d));
	d.checks = 10;
	d.good = 9;
	CHECK(care_score(&d) == 90, "an old day: 9 of 10 checks fine, 90%% (got %d)", care_score(&d));
	memset(&d, 0, sizeof(d));
	CHECK(care_score(&d) == -1 && care_main_cause(&d) == -1, "no checks: no score, no cause");

	/* days: yesterday counts in the week, not in today */
	memset(&s, 0, sizeof(s));
	memset(&r, 0, sizeof(r));
	care_step(&s, &r, &OK_V, DAY0, &p);
	care_step(&s, &r, &WIFI_V, DAY0 + 86400, &p);
	care_report_sum(&r, DAY0 + 86400, 1, &d);
	CHECK(d.checks == 1 && care_score(&d) == 0, "today: 1 check, score 0%%");
	care_report_sum(&r, DAY0 + 86400, 7, &d);
	CHECK(d.checks == 2 && care_score(&d) == 50, "last 7 days: 2 checks, score 50%%");

	CHECK(care_isp_owned("Internet") && care_isp_owned("DNS") && !care_isp_owned("WiFi") &&
	      !care_isp_owned("Device") && !care_isp_owned("AIGW"), "who owns what: the line and DNS are the ISP's");

	if (failed) {
		printf("%d proactive care case(s) failed\n", failed);
		return 1;
	}
	printf("all proactive care cases passed\n");
	return 0;
}
