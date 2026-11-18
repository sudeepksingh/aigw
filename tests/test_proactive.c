/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW proactive care before the call: what is worth acting on, one ticket per fault, flaps,
   the day's limit, and the customer's note in plain words (ag/proactive.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/brief.h"
#include "ag/diag.h"
#include "ag/proactive.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Wed 2026-11-18 21:30 UTC */
#define NOW 1795469400L

static struct pro_params P;
static struct pro_state S;
static struct pro_change ch[32];

static struct pro_signal sig(const char *code, int sev, long since, int devices, const char *target)
{
	struct pro_signal s;

	memset(&s, 0, sizeof(s));
	snprintf(s.key, sizeof(s.key), "%s%s%s", code, target ? ":" : "", target ? target : "");
	s.code = code;
	s.fault = brief_fault_of(code);
	s.sev = sev;
	s.since = since;
	s.devices = devices;
	s.target = target;
	s.source = "test";
	return s;
}

static int step1(const struct pro_signal *s, long now, int score)
{
	return pro_step(&S, s, s ? 1 : 0, now, score, &P, ch, 32);
}

static int has(int n, int kind)
{
	int i;

	for (i = 0; i < n; i++)
		if (ch[i].kind == kind)
			return 1;
	return 0;
}

static void points(void)
{
	struct pro_signal s;
	char why[160];
	int p;

	printf("-- points: how bad, how long, how many, how often --\n");
	s = sig("ISP_DOWN", DS_PROBLEM, NOW - 200, 6, NULL);
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(p == 4 && pro_worth(&s, p, NOW, &P), "the internet down for 6 devices, 3 min: %d points (%s), worth it", p, why);
	s = sig("ISP_DOWN", DS_PROBLEM, NOW - 60, 6, NULL);
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(!pro_worth(&s, p, NOW, &P), "... but a minute is a blip (min_s %ld)", P.min_s);
	s = sig("WIFI_CONGESTION", DS_WARN, NOW - 700, 1, NULL);
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(p == 2 && !pro_worth(&s, p, NOW, &P), "crowded Wi-Fi, 1 device, 12 min: %d points (%s), not yet", p, why);
	s.since = NOW - 1900;
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(p == 3 && pro_worth(&s, p, NOW, &P), "... 32 min: %d points (%s), worth it", p, why);
	s.since = NOW - 700;
	p = pro_points(&s, NOW, 2, -1, &P, why, sizeof(why));
	CHECK(p == 3 && pro_worth(&s, p, NOW, &P), "... 12 min, its 2nd time today: %d points (%s)", p, why);
	p = pro_points(&s, NOW, 1, 85, &P, why, sizeof(why));
	CHECK(p == 3 && strstr(why, "85% week"), "... 12 min in a home with an 85%% week: %d points (%s)", p, why);
	p = pro_points(&s, NOW, 1, 95, &P, why, sizeof(why));
	CHECK(p == 2, "... a 95%% week adds nothing: %d", p);
	s = sig("LINE_SLOW", DS_INFO, NOW - 4000, 9, NULL);
	p = pro_points(&s, NOW, 5, 50, &P, why, sizeof(why));
	CHECK(p == 0 && !pro_worth(&s, p, NOW, &P), "below a warning never counts: %d", p);
	s = sig("WIFI_DROPS", DS_WARN, NOW - 2000, 1, "aa:bb:cc:dd:ee:01");
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(p == 3 && pro_worth(&s, p, NOW, &P) && s.fault == BF_DEVICE, "a device dropping off for half an hour: %d (%s)", p, why);
	s = sig("OUTSIDE_SERVICE", DS_PROBLEM, NOW - 4000, 9, NULL);
	p = pro_points(&s, NOW, 3, -1, &P, why, sizeof(why));
	CHECK(p >= 3 && !pro_worth(&s, p, NOW, &P), "a service beyond the ISP is never a ticket (%d points)", p);
	s = sig("NO_DATA", DS_WARN, NOW - 4000, 9, NULL);
	CHECK(!pro_worth(&s, 9, NOW, &P), "not known is never a ticket");
	s = sig("LINE_FULL", DS_WARN, NOW - 2000, 5, NULL);
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(!pro_worth(&s, p, NOW, &P), "a full line nobody noticed: no ticket");
	s.rough_calls = 2;
	CHECK(pro_worth(&s, p, NOW, &P), "... with 2 rough calls on it: worth acting on (%d points)", p);
	s = sig("ISP_SLOW", DS_WARN, NOW - 400, 5, NULL);
	p = pro_points(&s, NOW, 1, -1, &P, why, sizeof(why));
	CHECK(p == 3 && pro_worth(&s, p, NOW, &P), "the line slow for the whole home (5 devices), 7 min: %d (%s)", p, why);
	P.threshold = 4;
	CHECK(!pro_worth(&s, p, NOW, &P), "... not at a threshold of 4");
	P.threshold = 3;
}

static void tickets(void)
{
	struct pro_signal s = sig("ISP_DOWN", DS_PROBLEM, NOW - 300, 6, NULL), t2;
	struct pro_ticket *t;
	long now = NOW;
	int n, id, i;

	printf("-- one ticket per fault, however often it is seen --\n");
	memset(&S, 0, sizeof(S));
	n = step1(&s, now, -1);
	CHECK(n == 1 && ch[0].kind == PC_NEW, "a new ticket");
	t = ch[0].t;
	id = (int)t->id;
	CHECK(t->open && !strcmp(t->code, "ISP_DOWN") && t->since == NOW - 300 && t->devices == 6 && t->fault == BF_LINE,
	      "ticket %d: ISP_DOWN since its first sign, 6 devices, LINE", id);
	t->status = PS_RAISED;
	pro_notified(&S, t, now);
	for (i = 1; i <= 10; i++) {
		n = step1(&s, now + 60 * i, -1);
		if (n)
			break;
	}
	CHECK(n == 0 && pro_open_count(&S) == 1 && S.c.tickets == 1 && S.c.raised == 1,
	      "seen 10 more times: no change, still one ticket, the ISP told once");
	t2 = s;
	n = pro_step(&S, (struct pro_signal[]){ s, t2 }, 2, now + 660, -1, &P, ch, 32);
	CHECK(n == 0 && S.c.deduped == 1, "two sources of the same fault in one look: one ticket (deduplicated %u)", S.c.deduped);

	printf("-- over after clear_s away; back soon: the same ticket, no new word --\n");
	n = step1(NULL, now + 660 + 300, -1);
	CHECK(n == 0, "gone 5 min: not over yet (clear_s %ld)", P.clear_s);
	n = step1(NULL, now + 660 + 600, -1);
	CHECK(n == 1 && ch[0].kind == PC_CLEARED, "gone 10 min: over");
	CHECK(pro_close(&S, ch[0].t, now + 1260) == PS_RESOLVED && S.c.avoided == 1,
	      "closed: Resolved (the ISP was told), a call likely avoided");
	s.since = now + 2000;
	n = step1(&s, now + 2000 + 400, -1);
	CHECK(n == 1 && ch[0].kind == PC_REOPENED && (int)ch[0].t->id == id && ch[0].t->flaps == 1,
	      "back 20 min later: ticket %d reopened (flap 1), not a new one", id);
	CHECK(S.c.tickets == 1 && S.c.flaps == 1, "still 1 ticket worth acting on, 1 flap");
	n = step1(NULL, now + 2400 + 600, -1);
	CHECK(n == 0, "a flapping ticket waits longer before it is over (10 min is not enough)");
	n = step1(NULL, now + 2400 + 1200, -1);
	CHECK(n == 1 && ch[0].kind == PC_CLEARED, "... 20 min is (each flap doubles it)");
	pro_close(&S, ch[0].t, now + 3600);
	s.since = now + 3600 + P.cooldown_s + 100;
	n = step1(&s, s.since + 400, -1);
	CHECK(n == 1 && ch[0].kind == PC_NEW && (int)ch[0].t->id != id, "back after the cooldown: a new ticket");
	CHECK(pro_repeats(&S, s.key, s.since + 400) >= 1, "its episodes counted for repeats");
}

static void flaps(void)
{
	struct pro_signal s = sig("LINE_LOSS", DS_PROBLEM, NOW - 300, 4, NULL);
	struct pro_ticket *t;
	long now = NOW;
	int n;

	printf("-- a flap the ISP heard was over: no word, unless it stays --\n");
	memset(&S, 0, sizeof(S));
	step1(&s, now, -1);
	t = ch[0].t;
	t->status = PS_RAISED;
	pro_notified(&S, t, now);
	n = step1(NULL, now + 700, -1);
	CHECK(n == 1 && pro_close(&S, t, now + 700) == PS_RESOLVED && S.c.avoided == 1, "raised, then over: Resolved");
	s.since = now + 800;
	n = step1(&s, now + 1000, -1);
	CHECK(n == 1 && ch[0].kind == PC_REOPENED, "back: reopened");
	t->quiet = 1;                   /* agd: the ISP knows; no word unless it stays */
	t->watch_until = now + 1000 + 2 * P.clear_s;
	n = step1(NULL, now + 1000 + 2 * P.clear_s + 10, -1);
	CHECK(n == 1 && ch[0].kind == PC_CLEARED, "gone again (each flap doubles the wait)");
	CHECK(pro_close(&S, t, now + 2300) == PS_RESOLVED && S.c.avoided == 1 && S.c.raised == 1 && !t->quiet,
	      "over again: Resolved, counted once (avoided %u, told %u)", S.c.avoided, S.c.raised);
	s.since = now + 2400;
	step1(&s, now + 2600, -1);
	t->quiet = 1;
	t->watch_until = now + 2600 + 2 * P.clear_s;
	n = step1(&s, now + 2600 + P.clear_s, -1);
	CHECK(n == 0, "back once more: quiet while it may be a flap");
	n = step1(&s, now + 2600 + 2 * P.clear_s, -1);
	CHECK(n == 1 && ch[0].kind == PC_GRACE_OVER, "... it stays: the ISP hears it isn't over");
}

static void repeats(void)
{
	struct pro_signal s = sig("WIFI_CONGESTION", DS_WARN, 0, 1, NULL);
	long now = NOW;
	int n, k;

	printf("-- short episodes that repeat become worth acting on --\n");
	memset(&S, 0, sizeof(S));
	for (k = 0; k < 3; k++) {
		s.since = now;
		step1(&s, now, -1);
		n = step1(&s, now + 300, -1);
		CHECK(n == 0, "episode %d: 5 min of crowded Wi-Fi, 1 device: nothing", k + 1);
		now += 300 + P.clear_s + 60;    /* gone long enough: a new episode */
		step1(NULL, now, -1);
	}
	s.since = now;
	step1(&s, now, -1);
	n = step1(&s, now + 300, -1);
	CHECK(n == 1 && ch[0].kind == PC_NEW && ch[0].t->episodes == 4,
	      "episode 4 the same evening: worth acting on (%s)", n ? ch[0].t->why : "");
}

static void limit(void)
{
	static const char *const codes[] = { "ISP_DOWN", "DNS_FAILING", "LINE_LOSS", "ISP_SLOW", "WIFI_RADIO" };
	struct pro_signal s;
	long now = NOW;
	int i, k, n;

	printf("-- at most max_per_day new tickets to the ISP in 24 hours --\n");
	memset(&S, 0, sizeof(S));
	for (i = 0; i < 5; i++) {
		s = sig(codes[i], DS_PROBLEM, now - 600, 5, NULL);
		n = step1(&s, now, -1);
		CHECK(has(n, PC_NEW), "%s: a new ticket", codes[i]);
		for (k = 0; k < n; k++)
			if (ch[k].kind == PC_NEW && pro_budget(&S, now, &P) > 0) {
				ch[k].t->status = PS_RAISED;
				pro_notified(&S, ch[k].t, now);
			}
		now += 600;
	}
	CHECK(S.c.raised == 3 && pro_budget(&S, now, &P) == 0, "5 faults: the ISP told of 3, then none left today");
	CHECK(pro_budget(&S, NOW + 86400 + 1300, &P) == 3, "a day later: 3 again");

}

static void area(void)
{
	struct pro_signal line = sig("ISP_DOWN", DS_PROBLEM, NOW - 600, 5, NULL);
	struct pro_signal wifi = sig("WIFI_CONGESTION", DS_WARN, NOW - 1900, 3, NULL);
	struct pro_signal both[2];
	struct pro_ticket *t;
	int n, k;

	printf("-- a home in a known area outage: no tickets, no notes --\n");
	memset(&S, 0, sizeof(S));
	S.area_hold = 1;
	both[0] = line;
	both[1] = wifi;
	for (k = 0; k < 5; k++) {
		n = pro_step(&S, both, 2, NOW + 60 * k, -1, &P, ch, 32);
		CHECK(n == 0, "look %d: the line down and crowded Wi-Fi, worth acting on, but no ticket", k + 1);
	}
	CHECK(pro_open_count(&S) == 0 && S.c.tickets == 0 && S.c.area_held == 2,
	      "nothing opened; each fault counted once as held (%u)", S.c.area_held);
	S.area_hold = 0;
	n = pro_step(&S, both, 2, NOW + 300, -1, &P, ch, 32);
	CHECK(n == 2 && has(n, PC_NEW) && S.c.area_held == 2, "the outage over and the faults still there: tickets now");

	printf("-- an area outage starts while a ticket is open: it waits, nothing reopens --\n");
	memset(&S, 0, sizeof(S));
	step1(&wifi, NOW, -1);
	t = ch[0].t;
	t->watch_until = NOW + 60;
	S.area_hold = 1;
	n = step1(&wifi, NOW + 120, -1);
	CHECK(n == 0 && t->open, "Self-Healing Wi-Fi's grace is over, but no raise during the outage");
	S.area_hold = 0;
	n = step1(&wifi, NOW + 180, -1);
	CHECK(n == 1 && ch[0].kind == PC_GRACE_OVER, "after the outage: raised");
	t->status = PS_RAISED;
	pro_notified(&S, t, NOW + 180);
	step1(NULL, NOW + 900, -1);
	pro_close(&S, t, NOW + 900);
	S.area_hold = 1;
	n = step1(&wifi, NOW + 1000, -1);
	CHECK(n == 0 && !t->open && t->flaps == 0, "back during an area outage: not reopened");
}

static void watch_and_close(void)
{
	struct pro_signal s = sig("WIFI_CONGESTION", DS_WARN, NOW - 1900, 3, NULL);
	struct pro_ticket *t;
	int n;

	printf("-- Self-Healing Wi-Fi's grace; fixed by AIGW; the customer called --\n");
	memset(&S, 0, sizeof(S));
	n = step1(&s, NOW, -1);
	t = ch[0].t;
	CHECK(n == 1, "crowded Wi-Fi for 32 min, 3 devices: a ticket");
	t->watch_until = NOW + 300;
	n = step1(&s, NOW + 120, -1);
	CHECK(n == 0, "watched: nothing during the grace");
	n = step1(&s, NOW + 360, -1);
	CHECK(n == 1 && ch[0].kind == PC_GRACE_OVER, "still there after the grace: raise it");
	t->status = PS_FIXING;
	t->acted = 1;
	step1(NULL, NOW + 2000, -1);
	CHECK(pro_close(&S, t, NOW + 2000) == PS_FIXED && S.c.fixed == 1 && S.c.avoided == 1,
	      "over after AIGW acted: Fixed, a call likely avoided");

	memset(&S, 0, sizeof(S));
	s = sig("LINE_LOSS", DS_PROBLEM, NOW - 900, 4, NULL);
	n = step1(&s, NOW, -1);
	t = ch[0].t;
	t->status = PS_RAISED;
	pro_notified(&S, t, NOW);
	pro_on_called(&S);
	pro_on_called(&S);
	CHECK(t->called && S.c.called == 1, "the customer called while it was open (counted once)");
	CHECK(pro_close(&S, t, NOW + 3000) == PS_RESOLVED && S.c.avoided == 0, "... so no call avoided");

	memset(&S, 0, sizeof(S));
	s = sig("WIFI_HOPPING", DS_WARN, NOW - 2000, 1, "aa:bb:cc:dd:ee:02");
	step1(&s, NOW, -1);
	t = ch[0].t;
	CHECK(pro_close(&S, t, NOW + 900) == PS_PASSED && !S.c.avoided, "over by itself before anyone was told: Passed, not counted");
	CHECK(!strcmp(t->target, "aa:bb:cc:dd:ee:02") && !strcmp(t->key, "WIFI_HOPPING:aa:bb:cc:dd:ee:02"),
	      "a device's ticket is keyed by its MAC");
}

static void full(void)
{
	struct pro_signal many[PRO_TICKETS + 4];
	char codes[PRO_TICKETS + 4][24];
	int i, n;

	printf("-- more faults than tickets: the open ones are kept --\n");
	memset(&S, 0, sizeof(S));
	for (i = 0; i < PRO_TICKETS + 4; i++) {
		snprintf(codes[i], sizeof(codes[i]), "WIFI_DROPS");
		many[i] = sig(codes[i], DS_WARN, NOW - 2000, 1, NULL);
		snprintf(many[i].key, sizeof(many[i].key), "WIFI_DROPS:dev%d", i);
	}
	n = pro_step(&S, many, PRO_TICKETS + 4, NOW, -1, &P, ch, 32);
	CHECK(n == PRO_TICKETS && pro_open_count(&S) == PRO_TICKETS, "%d new, %d open: the rest wait", n, pro_open_count(&S));
}

static void notes(void)
{
	static const char *const codes[] = { "LINE_DOWN", "NO_ROUTE", "ISP_DOWN", "LINE_LOSS", "ISP_LOSS", "LINE_SLOW",
					     "ISP_SLOW", "LINE_SLOW_SPEED", "DNS_FAILING", "DNS_SLOW", "WIFI_CONGESTION",
					     "WIFI_HOPPING", "WIFI_COVERAGE", "WIFI_RADIO", "WIFI_PROBLEM", "WIFI_DEVICE",
					     "WIFI_DROPS", "DEVICE_OFFLINE", "LINE_FULL", "SOMETHING_NEW", NULL };
	char note[PRO_TEXT];
	int i, h, fx, bad = 0, total = 0;

	printf("-- the customer's note: plain words, no codes --\n");
	for (i = 0; codes[i]; i++)
		for (fx = 0; fx < 2; fx++)
			for (h = 0; h < 24; h++) {
				pro_note(codes[i], fx, h, note, sizeof(note));
				total++;
				if (!pro_plain(note) || strlen(note) < 40 || strlen(note) >= sizeof(note) - 1) {
					bad++;
					printf("     %s/%d/%d: %s\n", codes[i], fx, h, note);
				}
			}
	CHECK(!bad, "%d notes (every code, fixed or not, every hour) in plain words", total);
	pro_note("WIFI_CONGESTION", 1, 20, note, sizeof(note));
	CHECK(!strncmp(note, "We noticed", 10) && strstr(note, "this evening") && strstr(note, "nothing you need to do"),
	      "fixed: \"%s\"", note);
	pro_note("ISP_DOWN", 0, 8, note, sizeof(note));
	CHECK(strstr(note, "this morning") && strstr(note, "no need to call"), "on it: \"%s\"", note);
	pro_note("ISP_DOWN", 0, 2, note, sizeof(note));
	CHECK(strstr(note, "tonight") != NULL, "at 2 am: tonight");
	CHECK(!pro_plain("Your line has 4% packet loss"), "\"packet loss\" is jargon");
	CHECK(!pro_plain("We fixed WIFI_CONGESTION"), "a code is jargon");
	CHECK(!pro_plain("Your DNS was slow"), "DNS is jargon");
	CHECK(!pro_plain("We moved it to channel 44"), "a channel is jargon");
	CHECK(!pro_plain("Signal -71 dBm"), "dBm is jargon");
	CHECK(pro_plain("We noticed your Wi-Fi was slow this evening and fixed it."), "Wi-Fi is plain");
	CHECK(pro_plain("Some programs were slow."), "\"programs\" is not \"ms\"");
}

int main(void)
{
	pro_params_default(&P);
	points();
	tickets();
	flaps();
	repeats();
	limit();
	area();
	watch_and_close();
	full();
	notes();
	CHECK(!strcmp(pro_status_name(PS_FIXED), "Fixed") && !strcmp(pro_status_name(PS_SUPPRESSED), "Suppressed"),
	      "status names");
	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
