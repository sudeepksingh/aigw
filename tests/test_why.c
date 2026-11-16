/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW "why was it slow?": evenings of a home, explained afterwards (ag/why.h). Run with TZ=UTC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/why.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0   1800000000L                /* 08:00 UTC */
#define FROM (T0 + 41400)               /* 19:30 */
#define TO   (FROM + 3600)              /* 20:30 */
#define AT(hh, mm) (T0 + ((hh) - 8) * 3600L + (mm) * 60L)
#define N    60

static const uint8_t TV[6] = { 2, 0, 0, 0, 0, 1 }, LAPTOP[6] = { 2, 0, 0, 0, 0, 2 }, PHONE[6] = { 2, 0, 0, 0, 0, 3 };

static const char *name_of(const uint8_t mac[6], void *ctx)
{
	(void)ctx;
	return !memcmp(mac, TV, 6) ? "TV" : !memcmp(mac, LAPTOP, 6) ? "Laptop" : !memcmp(mac, PHONE, 6) ? "Phone" : NULL;
}

static struct why_min m[N];
static struct why_ev ev[32];
static int nev;
static struct why_in in;
static struct why_result r;

/* an ordinary evening: the line answers in 18 ms, a phone browsing, Wi-Fi fine */
static void evening(void)
{
	int i;

	memset(m, 0, sizeof(m));
	for (i = 0; i < N; i++) {
		struct why_min *s = &m[i];

		s->t = (uint32_t)(FROM + i * 60);
		s->route = 1;
		s->gw_sent = 3, s->gw_ms = 2;
		s->in_sent = 3, s->in_ms = 18;
		s->dns_q = 20, s->dns_ms = 10;
		s->down_kbps = 3000, s->up_kbps = 500;
		s->r[0].used = 1, s->r[0].status = HEAL_GOOD, s->r[0].rtt = 4, s->r[0].channel = 6;
		s->r[1].used = 1, s->r[1].status = HEAL_GOOD, s->r[1].rtt = 3, s->r[1].channel = 36;
		memcpy(s->top[0].mac, PHONE, 6);
		s->top[0].kbps = 1500;
	}
	nev = 0;
	memset(&in, 0, sizeof(in));
	in.from = FROM;
	in.to = TO;
	in.step = 60;
	in.m = m;
	in.n = N;
	in.e = ev;
	in.recorded_since = FROM - 2 * 86400;
	in.norm[WN_GW] = 2, in.norm[WN_INET] = 18, in.norm[WN_LOSS] = 0.3f, in.norm[WN_DNS] = 10;
	in.have_norm[WN_GW] = in.have_norm[WN_INET] = in.have_norm[WN_LOSS] = in.have_norm[WN_DNS] = 1;
	in.down_mbps = 50;
	in.radio[0] = "2.4 GHz (wlan0)";
	in.radio[1] = "5 GHz (wlan1)";
	in.name = name_of;
}

static void event(long t, int kind, const uint8_t *mac, int flag, const char *text)
{
	struct why_ev *e = &ev[nev++];

	memset(e, 0, sizeof(*e));
	e->t = (uint32_t)t;
	e->kind = (uint8_t)kind;
	if (mac)
		memcpy(e->mac, mac, 6);
	e->flag = (uint8_t)flag;
	snprintf(e->text, sizeof(e->text), "%s", text);
	in.ne = nev;
}

static void judge(void)
{
	why_judge(&in, &r);
}

static int has(const char *code)
{
	int i;

	for (i = 0; i < r.n; i++)
		if (!strcmp(r.f[i].code, code))
			return i;
	return -1;
}

static int story_has(const char *s)
{
	int i;

	for (i = 0; i < r.nstory; i++)
		if (strstr(r.story[i].text, s))
			return i;
	return -1;
}

static int story_sorted(void)
{
	int i;

	for (i = 1; i < r.nstory; i++)
		if (r.story[i].t < r.story[i - 1].t)
			return 0;
	return 1;
}

static void show(void)
{
	int i;

	printf("     verdict %s: %s\n", r.f[0].code, r.f[0].text);
	for (i = 0; i < r.nstory; i++) {
		char c[16];

		why_clock(r.story[i].t, TO, c, sizeof(c));
		printf("       %s  %s\n", c, r.story[i].text);
	}
}

int main(void)
{
	int i, x;

	/* 1. an ordinary evening */
	evening();
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "OK"), "an ordinary evening: OK (%s)", r.f[0].code);
	CHECK(has("INTERNET_OK") > 0 && strstr(r.f[has("INTERNET_OK")].text, "18 ms"), "the internet's delay is told");
	CHECK(r.recorded == 60 && r.expected == 60, "60 of 60 samples (%d of %d)", r.recorded, r.expected);
	CHECK(r.nstory == 0, "nothing to tell (%d lines)", r.nstory);

	/* 2. the TV fills the line from 20:02 to 20:26: everything else is slower */
	evening();
	for (i = 32; i < 56; i++) {
		m[i].down_kbps = 47000;
		memcpy(m[i].top[0].mac, TV, 6);
		m[i].top[0].kbps = 38000;
		memcpy(m[i].top[1].mac, PHONE, 6);
		m[i].top[1].kbps = 1500;
		m[i].in_ms = 140, m[i].gw_ms = 120;
	}
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "LINE_FULL"), "the TV filled the line: LINE_FULL (%s)", r.f[0].code);
	CHECK(strstr(r.f[0].text, "TV") && strstr(r.f[0].text, "47 of 50 Mbit/s"), "it names the TV and the line's speed");
	CHECK(strstr(r.f[0].text, "140 ms") && strstr(r.f[0].text, "normally 18 ms"), "and how slow the rest was");
	CHECK(has("LINE_SLOW") < 0 && has("ISP_SLOW") < 0, "the slowness is the full line's, not the ISP's");
	CHECK(story_has("TV started downloading (38 Mbit/s)") >= 0, "the story: the TV started downloading");
	CHECK(story_has("the line was full: TV, until 20:26") >= 0, "the story: the line full until 20:26");
	CHECK(r.f[0].since == AT(20, 2) && r.f[0].until == AT(20, 26), "from 20:02 to 20:26");
	CHECK(!strncmp(why_advice("LINE_FULL"), "Nothing to repair", 17), "advice: nothing to repair, SQM, a faster plan");

	/* 3. the ISP is slow, the home isn't using much */
	evening();
	for (i = 10; i < 41; i++)
		m[i].in_ms = 160;
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "ISP_SLOW"), "slow beyond the home: ISP_SLOW (%s)", r.f[0].code);
	CHECK(strstr(r.f[0].text, "31 min") && strstr(r.f[0].text, "19:40 to 20:11"), "31 min, 19:40 to 20:11");

	/* 4. the line itself is slow: the next router answers slowly too */
	evening();
	for (i = 10; i < 30; i++)
		m[i].gw_ms = 60, m[i].in_ms = 80;
	judge();
	CHECK(!strcmp(r.f[0].code, "LINE_SLOW"), "the next router slow: LINE_SLOW (%s)", r.f[0].code);

	/* 5. one slow minute is a blip */
	evening();
	m[30].in_ms = 400;
	judge();
	CHECK(!strcmp(r.f[0].code, "OK"), "one slow minute: OK (%s)", r.f[0].code);

	/* 6. ten minutes without internet outweigh fifteen slow ones */
	evening();
	for (i = 20; i < 30; i++)
		m[i].in_lost = 3, m[i].in_ms = WHY_NONE;
	for (i = 40; i < 55; i++)
		m[i].in_ms = 200;
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "ISP_DOWN"), "the internet down, the next router fine: ISP_DOWN (%s)", r.f[0].code);
	CHECK(has("ISP_SLOW") > 0, "the slow quarter of an hour is a finding too");
	CHECK(story_has("the internet was down beyond the home, until 20:00") >= 0, "the story: down until 20:00");

	/* 7. ... and with the next router not answering either: the line */
	for (i = 20; i < 30; i++)
		m[i].gw_lost = 3, m[i].gw_ms = WHY_NONE;
	judge();
	CHECK(!strcmp(r.f[0].code, "LINE_DOWN") && strstr(r.f[0].text, "nor did the next router"),
	      "neither answers: LINE_DOWN (%s)", r.f[0].code);

	/* 8. the gateway off for 25 minutes (a power cut): it started again at 20:15 */
	evening();
	memmove(&m[20], &m[45], sizeof(m[0]) * 15);
	in.n = 35;
	event(AT(20, 15), WE_START, NULL, 1, "The gateway started (it was off for 25 min).");
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "GATEWAY_OFF") && strstr(r.f[0].text, "19:50 to 20:15"),
	      "the gateway off 19:50 to 20:15: GATEWAY_OFF (%s)", r.f[0].code);
	CHECK(r.recorded == 35, "35 samples recorded (%d)", r.recorded);
	/* ... only agd restarted (the gateway kept working): not counted against anyone */
	ev[0].flag = 0;
	judge();
	CHECK(!strcmp(r.f[0].code, "OK") && has("NOT_RECORDED") > 0, "only AIGW restarted: OK, with NOT_RECORDED (%s)",
	      r.f[0].code);

	/* 9. names fail for ten minutes */
	evening();
	for (i = 5; i < 15; i++)
		m[i].dns_fail = 15;
	judge();
	CHECK(!strcmp(r.f[0].code, "DNS_FAILING") && strstr(r.f[0].text, "150 of 1200"), "DNS_FAILING (%s): %s",
	      r.f[0].code, r.f[0].text);

	/* 10. 5 GHz is poor for 20 minutes: a busy channel */
	evening();
	for (i = 30; i < 50; i++)
		m[i].r[1].status = HEAL_POOR, m[i].r[1].cause = HC_CONGESTION, m[i].r[1].loss = 22;
	judge();
	CHECK(!strcmp(r.f[0].code, "WIFI_CONGESTION") && strstr(r.f[0].text, "5 GHz (wlan1)") &&
	      strstr(r.f[0].text, "22%"), "5 GHz poor: WIFI_CONGESTION, named (%s)", r.f[0].code);
	CHECK(has("WIFI_OK") < 0, "no WIFI_OK then");

	/* 11. the laptop: moved to 2.4 GHz at 19:50 with a weak signal, dropped off twice */
	evening();
	event(AT(19, 0), WE_JOIN, LAPTOP, 5, "Laptop joined Wi-Fi on 5 GHz (-60 dBm)");
	event(AT(19, 50), WE_ROAM, LAPTOP, 2, "Laptop moved to 2.4 GHz (-79 dBm)");
	event(AT(19, 50), WE_WEAK, LAPTOP, 79, "Laptop's signal turned weak (-79 dBm)");
	event(AT(20, 0), WE_LEAVE, LAPTOP, 0, "Laptop left Wi-Fi");
	event(AT(20, 1), WE_JOIN, LAPTOP, 2, "Laptop joined Wi-Fi on 2.4 GHz (-80 dBm)");
	event(AT(20, 10), WE_LEAVE, LAPTOP, 0, "Laptop left Wi-Fi");
	event(AT(20, 11), WE_JOIN, LAPTOP, 2, "Laptop joined Wi-Fi on 2.4 GHz (-78 dBm)");
	event(AT(20, 5), WE_JOIN, PHONE, 5, "Phone joined Wi-Fi on 5 GHz (-50 dBm)");
	in.host = LAPTOP;
	in.host_name = "Laptop";
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "WIFI_COVERAGE") && strstr(r.f[0].text, "40 min") && strstr(r.f[0].text, "-79 dBm"),
	      "the laptop's signal weak for 40 min: WIFI_COVERAGE (%s)", r.f[0].code);
	CHECK((x = has("DEVICE_ON_24GHZ")) > 0 && r.f[x].sev == DS_WARN && strstr(r.f[x].text, "19:50"),
	      "it moved to 2.4 GHz at 19:50 (a warning)");
	CHECK((x = has("WIFI_DROPS")) > 0 && strstr(r.f[x].text, "2 times (20:00, 20:10)"), "it dropped off twice");
	CHECK(story_has("Laptop moved to 2.4 GHz") >= 0 && story_has("Phone joined") < 0,
	      "the story: the laptop's events, not the phone's");
	CHECK(story_sorted(), "the story in order of time");

	/* 12. the TV kept off by AIGW from 19:40 to 20:10 */
	evening();
	event(AT(19, 40), WE_DECISION, TV, WF_BLOCKS, "AIGW quarantined TV");
	event(AT(20, 10), WE_DECISION, TV, WF_BLOCKS | WF_ENDED, "AIGW let TV back (reverted)");
	in.host = TV;
	in.host_name = "TV";
	judge();
	CHECK(!strcmp(r.f[0].code, "DEVICE_BLOCKED") && strstr(r.f[0].text, "30 min"), "AIGW kept it off 30 min (%s)",
	      r.f[0].code);
	CHECK(has("DEVICE_OFFLINE") < 0, "not offline: AIGW kept it off");

	/* 13. AIGW changed the channel at 20:00, the internet slow from 20:02: told */
	evening();
	event(AT(20, 0), WE_CHANNEL, NULL, 0, "5 GHz (wlan1) moved from channel 36 to 44");
	for (i = 32; i < 50; i++)
		m[i].in_ms = 120;
	judge();
	show();
	CHECK(!strcmp(r.f[0].code, "ISP_SLOW") && strstr(r.f[0].text, "2 min after a change at 20:00"),
	      "a change just before the problem is told with it");
	CHECK(has("AIGW_CHANGED") > 0, "and listed: AIGW_CHANGED");
	CHECK(story_has("moved from channel 36 to 44") >= 0, "the story: the channel change");

	/* 14. the normal, from the timeline itself */
	{
		static struct why_min h[2000];
		float nrm[WN_N];
		int have[WN_N];

		memset(h, 0, sizeof(h));
		for (i = 0; i < 2000; i++) {
			h[i].t = (uint32_t)(T0 + i * 60);
			h[i].route = 1;
			h[i].gw_sent = 3, h[i].gw_ms = 3;
			h[i].in_sent = 3, h[i].in_ms = (uint16_t)(i >= 1000 && i < 1060 ? 300 : 20 + i % 5);
			h[i].in_lost = i % 50 == 0;
			h[i].dns_q = 5, h[i].dns_ms = 12;
		}
		why_baseline(h, 2000, T0 + 1000 * 60, T0 + 1060 * 60, nrm, have);
		CHECK(have[WN_INET] && nrm[WN_INET] >= 20 && nrm[WN_INET] <= 24, "normal internet delay %.0f ms (20-24)",
		      nrm[WN_INET]);
		CHECK(have[WN_LOSS] && nrm[WN_LOSS] > 0.5f && nrm[WN_LOSS] < 0.8f, "normal loss %.2f%%", nrm[WN_LOSS]);
		CHECK(have[WN_GW] && nrm[WN_GW] == 3 && have[WN_DNS] && nrm[WN_DNS] == 12, "next hop and DNS");
		why_baseline(h, 10, 0, 1, nrm, have);
		CHECK(!have[WN_INET], "too few samples: no normal");
	}

	/* 15. slow is the home's own: 60 ms on a 45 ms line is fine, 50 ms on an 8 ms one isn't */
	evening();
	in.norm[WN_INET] = 45;
	for (i = 0; i < N; i++)
		m[i].in_ms = 60;
	judge();
	CHECK(!strcmp(r.f[0].code, "OK"), "60 ms where 45 is normal: OK (%s)", r.f[0].code);
	in.norm[WN_INET] = 8;
	for (i = 0; i < N; i++)
		m[i].in_ms = (uint16_t)(i < 20 ? 8 : 50);
	judge();
	CHECK(!strcmp(r.f[0].code, "ISP_SLOW"), "50 ms where 8 is normal: ISP_SLOW (%s)", r.f[0].code);
	in.have_norm[WN_INET] = 0;
	judge();
	CHECK(!strcmp(r.f[0].code, "OK"), "no normal known: Diagnose's 150 ms (OK: %s)", r.f[0].code);

	/* 16. the line's speed not known: near the most seen on it */
	evening();
	in.down_mbps = 0;
	in.peak_down_mbps = 40;
	for (i = 10; i < 20; i++) {
		m[i].down_kbps = 36000;
		memcpy(m[i].top[0].mac, LAPTOP, 6);
		m[i].top[0].kbps = 34000;
	}
	judge();
	CHECK(!strcmp(r.f[0].code, "LINE_FULL") && strstr(r.f[0].text, "the most this gateway has seen") &&
	      strstr(r.f[0].text, "Laptop"), "full by the most seen: LINE_FULL, the laptop (%s)", r.f[0].code);

	/* 17. nothing recorded; before the timeline starts */
	evening();
	in.n = 0;
	in.recorded_since = 0;
	judge();
	CHECK(!strcmp(r.f[0].code, "NO_DATA") && r.recorded == 0, "no timeline: NO_DATA (%s)", r.f[0].code);
	evening();
	in.recorded_since = FROM + 1800;
	in.n = 30;
	memmove(&m[0], &m[30], sizeof(m[0]) * 30);
	judge();
	CHECK(!strcmp(r.f[0].code, "OK") && has("NOT_RECORDED") > 0 && strstr(r.f[has("NOT_RECORDED")].text, "20:00"),
	      "half before the timeline: OK, and since when it is known");
	CHECK(has("GATEWAY_OFF") < 0, "before the timeline isn't a gap");

	/* 18. packet loss on the internet, the next router fine */
	evening();
	for (i = 0; i < 30; i++)
		m[i].in_lost = 1;
	judge();
	CHECK(!strcmp(r.f[0].code, "ISP_LOSS") && strstr(r.f[0].text, "16%"), "a third lost for half an hour: ISP_LOSS (%s)",
	      r.f[0].code);

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
