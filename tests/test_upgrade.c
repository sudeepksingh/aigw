/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW firmware upgrades that check themselves: the verdict (ag/upgrade.h) */
#include <stdio.h>
#include <string.h>

#include "ag/upgrade.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1800000000L

/* a home before the upgrade: n devices (the first `wifi` on Wi-Fi), a good week */
static void home(struct up_before *b, int n, int wifi)
{
	int i;

	memset(b, 0, sizeof(*b));
	b->at = T0 - 60;
	snprintf(b->from, sizeof(b->from), "1.0");
	snprintf(b->to, sizeof(b->to), "2.0");
	b->online = 1;
	b->ndev = n;
	for (i = 0; i < n; i++) {
		snprintf(b->dev[i], sizeof(b->dev[i]), "02:00:00:00:00:%02x", i + 1);
		b->wifi[i] = i < wifi;
	}
	b->watched_s = 7 * 86400;
	b->ok_s = 7 * 86400 / 100 * 98;         /* 98% */
	b->speed_mbps = 100;
}

/* checks every 10 min from the boot to `until`, the internet answering; delays d (normal n) */
static long step = 600;

static void checks(struct up_after *a, long from, long until, int online, float d, float n, int problem)
{
	struct care_sample s, nm;
	long t;

	memset(&s, 0, sizeof(s));
	memset(&nm, 0, sizeof(nm));
	s.have[CM_INET_MS] = nm.have[CM_INET_MS] = online;
	s.v[CM_INET_MS] = d;
	nm.v[CM_INET_MS] = n;
	s.have[CM_LOSS] = nm.have[CM_LOSS] = online;
	s.v[CM_LOSS] = 0;
	nm.v[CM_LOSS] = 0.1f;
	for (t = from; t <= until; t += step)
		up_after_check(a, t, problem, online, &s, &nm, 1800);
}

static void all_back(const struct up_before *b, struct up_after *a, long now)
{
	int i;

	for (i = 0; i < b->ndev; i++)
		up_after_seen(b, a, b->dev[i], now);
}

int main(void)
{
	struct up_params p;
	struct up_before b;
	struct up_after a;
	struct up_verdict v;
	char names[200];
	int i;

	up_params_default(&p);

	/* a good upgrade: everyone back, as fast as usual: watching, then kept */
	home(&b, 8, 6);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 120);
	checks(&a, T0 + 60, T0 + 3600, 1, 21, 20, 0);
	CHECK(up_judge(&b, &a, T0 + 3600, &p, &v) == UPV_WATCHING && v.back == 8 && !v.missing,
	      "good upgrade, 1 h in: all 8 devices back, still watching");
	checks(&a, T0 + 4200, T0 + 86400, 1, 21, 20, 0);
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_KEEP && !v.why && strstr(v.text, "8 of 8 devices back"),
	      "good upgrade, 24 h: kept -- %s", v.text);
	CHECK(v.score_before == 98 && v.score_after == 100, "score 98%% before, 100%% after (got %d, %d)",
	      v.score_before, v.score_after);

	/* devices that don't come back: worse at reconnect time, not before */
	home(&b, 8, 6);
	up_after_start(&a, T0);
	for (i = 0; i < 5; i++)
		up_after_seen(&b, &a, b.dev[i], T0 + 60);
	CHECK(up_after_seen(&b, &a, b.dev[0], T0 + 70) == -1 && up_after_seen(&b, &a, "02:00:00:00:00:99", T0) == -1,
	      "seen twice, or not from before: no news");
	CHECK(up_judge(&b, &a, T0 + 600, &p, &v) == UPV_WATCHING && v.missing == 3,
	      "3 of 8 not back after 10 min: give them time");
	CHECK(up_judge(&b, &a, T0 + 1800, &p, &v) == UPV_WORSE && v.why == UPW_DEVICES && v.missing_wifi == 1 &&
	      strstr(v.text, "3 of the 8 devices") && strstr(v.text, "1 on Wi-Fi"),
	      "3 of 8 not back after 30 min: worse -- %s", v.text);

	/* one device away is a phone that left, not the firmware */
	home(&b, 8, 8);
	up_after_start(&a, T0);
	for (i = 0; i < 7; i++)
		up_after_seen(&b, &a, b.dev[i], T0 + 60);
	CHECK(up_judge(&b, &a, T0 + 1800, &p, &v) == UPV_WATCHING && v.missing == 1, "1 of 8 not back: not worse");
	/* two of a small home is enough */
	home(&b, 3, 3);
	up_after_start(&a, T0);
	up_after_seen(&b, &a, b.dev[0], T0 + 60);
	CHECK(up_judge(&b, &a, T0 + 1800, &p, &v) == UPV_WORSE && strstr(v.text, "all on Wi-Fi"),
	      "2 of 3 not back: worse -- %s", v.text);
	/* nothing connected before: nothing to miss */
	home(&b, 0, 0);
	up_after_start(&a, T0);
	CHECK(up_judge(&b, &a, T0 + 1800, &p, &v) == UPV_WATCHING, "no devices before: nothing to miss");

	/* no internet since the boot, while it worked before (agd checks every 2 min early in a watch) */
	step = 120;
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 600, 0, 0, 0, 1);
	CHECK(up_judge(&b, &a, T0 + 600, &p, &v) == UPV_WATCHING, "no internet for 10 min: not yet");
	checks(&a, T0 + 660, T0 + 960, 0, 0, 0, 1);
	CHECK(up_judge(&b, &a, T0 + 960, &p, &v) == UPV_WORSE && v.why == UPW_INTERNET && strstr(v.text, "No internet"),
	      "no internet for 16 min, every check: worse -- %s", v.text);
	b.online = 0;
	CHECK(up_judge(&b, &a, T0 + 960, &p, &v) == UPV_WATCHING, "... but not when it was down before the upgrade too");
	b.online = 1;
	checks(&a, T0 + 1200, T0 + 1200, 1, 20, 20, 0);
	CHECK(up_judge(&b, &a, T0 + 1300, &p, &v) == UPV_WATCHING, "the internet came back: not the firmware");
	step = 600;

	/* restarting */
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	a.starts = 3;
	CHECK(up_judge(&b, &a, T0 + 600, &p, &v) == UPV_WATCHING, "started 3 times: allowed");
	a.starts = 4;
	CHECK(up_judge(&b, &a, T0 + 600, &p, &v) == UPV_WORSE && v.why == UPW_RESTARTS, "started 4 times: worse");

	/* slower, judged at the end of the watch only */
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 86400, 1, 48, 21, 0);
	CHECK(up_judge(&b, &a, T0 + 3600, &p, &v) == UPV_WATCHING, "slower: not judged before the end");
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_WORSE && v.why == UPW_LATENCY &&
	      strstr(v.text, "48 ms, normally 21 ms"), "48 ms where 21 is normal: worse -- %s", v.text);
	/* a little slower is noise */
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 86400, 1, 26, 21, 0);
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_KEEP, "26 ms where 21 is normal: kept");
	/* too few samples to say */
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 2400, 1, 90, 21, 0);
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_KEEP, "slow on 4 checks only: not enough to say");

	/* the speed, and the time without a problem */
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 86400, 1, 20, 20, 0);
	up_after_speed(&a, 60);
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_WORSE && v.why == UPW_SPEED &&
	      strstr(v.text, "60 Mbit/s, 100 Mbit/s"), "speed 60 where it was 100: worse -- %s", v.text);
	home(&b, 2, 2);
	up_after_start(&a, T0);
	all_back(&b, &a, T0 + 60);
	checks(&a, T0 + 60, T0 + 43200, 1, 20, 20, 0);
	checks(&a, T0 + 43800, T0 + 86400, 1, 20, 20, 1);       /* a problem for the second half */
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_WORSE && v.why == UPW_SCORE && v.score_after < 60,
	      "a problem half the day where the week was 98%%: worse -- %s", v.text);
	b.watched_s = 3600;
	CHECK(up_judge(&b, &a, T0 + 86400, &p, &v) == UPV_KEEP, "... not when the week before was hardly watched");

	/* several reasons at once */
	home(&b, 4, 4);
	up_after_start(&a, T0);
	checks(&a, T0 + 60, T0 + 1800, 0, 0, 0, 1);
	CHECK(up_judge(&b, &a, T0 + 1800, &p, &v) == UPV_WORSE && v.why == (UPW_DEVICES | UPW_INTERNET), "devices and internet");
	up_why_names(v.why, names, sizeof(names));
	CHECK(!strcmp(names, "DevicesMissing,NoInternet"), "the reasons by name: %s", names);
	CHECK(!strcmp(up_state_name(UP_ROLLED_BACK), "RolledBack") && !strcmp(up_why_name(UPW_SCORE), "ExperienceScore"),
	      "names");

	if (failed) {
		printf("%d upgrade case(s) failed\n", failed);
		return 1;
	}
	printf("all upgrade cases passed\n");
	return 0;
}
