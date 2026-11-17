/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW calls and games first: a call or a game told from its traits, how it went, in words (ag/calls.h) */
#include <stdio.h>
#include <string.h>

#include "ag/calls.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1800000000L

static struct call_params p;

/* a connection: n intervals of 10 s, packets a second and the mean packet each way */
static struct call_flow flow(uint8_t proto, uint16_t port, const char *name, int n, uint32_t up_pps, uint32_t up_pkt,
			     uint32_t down_pps, uint32_t down_pkt)
{
	struct call_flow f;
	int i;

	memset(&f, 0, sizeof(f));
	f.proto = proto;
	f.rport = port;
	f.name = name;
	f.n = n;
	for (i = 0; i < n; i++) {
		f.iv[i].ms = 10000;
		f.iv[i].up_pk = up_pps * 10;
		f.iv[i].down_pk = down_pps * 10;
		f.iv[i].up_b = up_pps * 10 * up_pkt;
		f.iv[i].down_b = down_pps * 10 * down_pkt;
	}
	return f;
}

static struct why_min m[60];
static struct sqm_line line;

/* an hour of minutes: the internet in 20 ms, 3 echo requests each, nothing lost, the line quiet */
static void quiet(void)
{
	int i;

	memset(m, 0, sizeof(m));
	for (i = 0; i < 60; i++) {
		m[i].t = (uint32_t)(T0 + i * 60);
		m[i].route = 1;
		m[i].in_sent = 3, m[i].in_ms = 20;
		m[i].down_kbps = 2000, m[i].up_kbps = 300;
	}
	memset(&line, 0, sizeof(line));
	line.down_mbps = 50;
	line.peak_down_mbps = 48, line.peak_up_mbps = 10;
}

int main(void)
{
	struct call_flow f;
	struct call_class c;
	struct call_quality q;
	struct call_report r;
	char w[600];
	int by;
	const char *app;

	call_params_default(&p);

	printf("== the traits\n");
	f = flow(17, 8801, NULL, 3, 50, 900, 120, 1100);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_CALL && c.by == CB_PORT && c.app && !strcmp(c.app, "Zoom"),
	      "Zoom's port, both ways at a steady rate: a call (Zoom)");
	f = flow(17, 3478, NULL, 2, 50, 160, 50, 160);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_CALL && c.by == CB_PORT && !c.app,
	      "voice to a STUN/TURN port: a call, app not known");
	f = flow(17, 51000, "media-eu.teams.microsoft.com", 2, 50, 200, 50, 200);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_CALL && c.by == CB_NAME && !strcmp(c.app, "Microsoft Teams"),
	      "Teams by the name the laptop looked up");
	f = flow(17, 40000, "g.whatsapp.net (nearby)", 2, 50, 200, 50, 200);
	CHECK(call_classify(&f, &p, &c) && c.by == CB_NAME && !strcmp(c.app, "WhatsApp"), "a nearby name counts too");
	f = flow(17, 40000, "notzoom.us", 2, 50, 200, 50, 200);
	CHECK(call_classify(&f, &p, &c) && c.by == CB_TRAITS && c.kind == CK_RT, "notzoom.us isn't Zoom: by its traits only");
	f = flow(17, 3074, NULL, 2, 30, 80, 60, 120);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_GAME && !strcmp(c.app, "Xbox Live"), "Xbox Live's port: a game");
	f = flow(17, 61000, "eu.riotgames.com", 2, 64, 70, 64, 140);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_GAME && c.by == CB_NAME, "a game by its name");
	f = flow(17, 61000, NULL, 2, 40, 120, 40, 120);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_RT, "small packets both ways, no hint: a call or game");
	f = flow(17, 61000, NULL, 2, 40, 1000, 150, 1100);
	CHECK(call_classify(&f, &p, &c) && c.kind == CK_CALL, "big packets both ways, no hint: a video call");

	printf("== not a call\n");
	f = flow(17, 8801, NULL, 1, 50, 200, 50, 200);
	CHECK(!call_classify(&f, &p, &c), "one interval only: not yet");
	f = flow(17, 443, "zoom.us", 3, 50, 200, 50, 200);
	CHECK(!call_classify(&f, &p, &c), "QUIC (443): never, even to zoom.us");
	f = flow(17, 51820, NULL, 3, 50, 200, 50, 200);
	CHECK(!call_classify(&f, &p, &c), "a WireGuard tunnel: never (it may carry a download)");
	f = flow(6, 8801, NULL, 3, 50, 200, 50, 200);
	CHECK(!call_classify(&f, &p, &c), "TCP: not real time");
	f = flow(17, 8801, NULL, 3, 2, 100, 2, 100);
	CHECK(!call_classify(&f, &p, &c), "Zoom's port, but only keep-alives: no call");
	f = flow(17, 3478, NULL, 3, 20, 80, 1000, 1200);
	CHECK(!call_classify(&f, &p, &c), "one way (a stream): not a conversation");
	f = flow(17, 3478, NULL, 3, 1200, 1250, 1200, 1250);
	CHECK(!call_classify(&f, &p, &c), "12 Mbit/s each way: a transfer, not a call");
	f = flow(17, 3478, NULL, 3, 50, 1450, 50, 1450);
	CHECK(!call_classify(&f, &p, &c), "packets bigger than a call's");
	f = flow(17, 3478, NULL, 3, 50, 200, 50, 200);
	f.iv[2].up_pk = 30 * 10, f.iv[2].down_pk = 30 * 10;
	f.iv[1].up_pk = 120 * 10, f.iv[1].down_pk = 120 * 10;
	CHECK(!call_classify(&f, &p, &c), "bursts (30 then 120 packets a second): not steady");
	f.iv[1].up_pk = 45 * 10, f.iv[1].down_pk = 45 * 10;
	CHECK(call_classify(&f, &p, &c), "45 then 30 packets a second: steady enough");
	f = flow(17, 3478, NULL, 2, 50, 200, 50, 200);
	f.iv[1].ms = 0;
	CHECK(!call_classify(&f, &p, &c), "an interval of no time: no rate");

	printf("== hints alone\n");
	CHECK(call_hint(17, 19305, NULL, &by, &app) == CK_CALL && by == CB_PORT && !strcmp(app, "Google Meet"), "Meet's port");
	CHECK(call_hint(6, 3074, NULL, &by, &app) == CK_NONE, "a port says nothing for TCP");
	CHECK(call_hint(6, 443, "zoom.us.", &by, &app) == CK_CALL && by == CB_NAME, "a name does (with its final dot)");
	CHECK(call_hint(17, 5000, "example.com", &by, &app) == CK_NONE && !app, "nothing known");
	CHECK(!strcmp(call_kind_name(CK_RT), "Realtime") && !strcmp(call_kind_words(CK_RT), "call or game"), "kind names");

	printf("== how it went\n");
	quiet();
	call_quality_of(m, 60, T0 + 10 * 60, T0 + 22 * 60, &line, &q);
	CHECK(q.samples == 12 && q.full == 0 && q.judged == 12 && q.ms == 20 && q.jitter_ms == 0 && q.loss10 == 0 &&
	      call_verdict(&q, &p) == CV_SMOOTH, "a quiet line: smooth, judged on all 12 samples (%d %d %d)", q.samples,
	      q.ms, q.jitter_ms);
	/* the TV downloads at 45 Mbit/s for 9 of the 12 minutes; the internet 24-32 ms, 8 ms apart */
	{
		int i;

		for (i = 13; i < 22; i++) {
			m[i].down_kbps = 45000;
			m[i].in_ms = (uint16_t)(i % 2 ? 24 : 32);
		}
	}
	call_quality_of(m, 60, T0 + 10 * 60, T0 + 22 * 60, &line, &q);
	CHECK(q.full == 9 && q.judged == 9 && q.line_kbps == 45000 && q.jitter_ms == 8 && q.ms >= 24 && q.ms <= 32 &&
	      call_verdict(&q, &p) == CV_SMOOTH, "full for 9 minutes: judged on those, smooth, jitter 8 ms (%d %d %u)", q.full,
	      q.jitter_ms, q.line_kbps);
	memset(&r, 0, sizeof(r));
	r.kind = CK_CALL, r.app = "Zoom", r.device = "Laptop", r.top = "TV";
	r.start = T0 + 10 * 60, r.end = T0 + 22 * 60;
	r.verdict = call_verdict(&q, &p), r.prioritized = 1, r.sqm_on = 1, r.q = q;
	call_words(&r, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(strstr(w, "The call on Laptop (Zoom) stayed smooth for 12 min while the line was full for 9 min (TV downloading, "
			"45.0 Mbit/s)") && strstr(w, "jitter 8 ms, no loss") && strstr(w, "went first"), "at home: with the names");
	r.app = r.device = r.top = NULL;
	call_words(&r, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(strstr(w, "A call stayed smooth for 12 min while the line was full for 9 min (45.0 Mbit/s)") == w &&
	      !strstr(w, "Laptop") && !strstr(w, "Zoom") && !strstr(w, "TV"), "for the ISP: no device or app");

	/* bufferbloat: 250-400 ms while full, a lost echo now and then, SQM off */
	{
		int i;

		for (i = 13; i < 22; i++) {
			m[i].in_ms = (uint16_t)(i % 2 ? 250 : 400);
			m[i].in_lost = (uint8_t)(i % 3 == 0);
		}
	}
	call_quality_of(m, 60, T0 + 10 * 60, T0 + 22 * 60, &line, &q);
	r.q = q, r.verdict = call_verdict(&q, &p), r.prioritized = 0, r.sqm_on = 0, r.kind = CK_GAME;
	call_words(&r, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(r.verdict == CV_ROUGH && q.jitter_ms == 150 && q.loss10 == 111 && strstr(w, "A game was rough") &&
	      strstr(w, "11.1% lost") && strstr(w, "Smart queueing was off"), "bufferbloat: rough, and why (%d %d)",
	      q.jitter_ms, q.loss10);

	/* nothing answered: not known */
	{
		int i;

		for (i = 0; i < 60; i++)
			m[i].in_lost = 3;
	}
	call_quality_of(m, 60, T0 + 10 * 60, T0 + 22 * 60, &line, &q);
	r.q = q, r.verdict = call_verdict(&q, &p);
	call_words(&r, w, sizeof(w));
	CHECK(r.verdict == CV_UNKNOWN && q.loss10 == 1000 && strstr(w, "wasn't measured"), "nothing answered: unknown");
	call_quality_of(m, 60, T0 + 100 * 60, T0 + 110 * 60, &line, &q);
	CHECK(q.samples == 0 && call_verdict(&q, &p) == CV_UNKNOWN, "no samples: unknown");

	call_duration(45, w, sizeof(w));
	CHECK(!strcmp(w, "45 s"), "45 s");
	call_duration(3900, w, sizeof(w));
	CHECK(!strcmp(w, "1 h 05 min"), "1 h 05 min");

	if (failed) {
		printf("%d FAILED\n", failed);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
