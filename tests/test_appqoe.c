/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW per-app experience (ag/appqoe.h): the rules file, server-name families, each kind told from synthetic
 * connections, quality estimates against known delay, jitter and loss, causes, the words, the score, and the cost.
 *
 *   test_appqoe [config/apps.rules]     (the file must be the built-in rules, word for word)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ag/appqoe.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct app_rules R;

/* a connection: n looks of 10 s, packets a second and the mean packet each way */
static struct app_flow flow(uint8_t proto, uint16_t port, int n, uint32_t up_pps, uint32_t up_pkt, uint32_t down_pps,
			    uint32_t down_pkt)
{
	struct app_flow f;
	int i;

	memset(&f, 0, sizeof(f));
	f.proto = proto;
	f.port = port;
	f.fam = -1;
	f.age_s = 600;
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

/* a look's down rate (kbit/s) in 1448-byte packets, acks up */
static void tcp_look(struct app_iv *v, uint32_t kbps)
{
	v->ms = 10000;
	v->down_b = kbps * 10 * 1000 / 8;
	v->down_pk = v->down_b / 1448;
	v->up_pk = v->down_pk / 2;
	v->up_b = v->up_pk * 52;
}

static int fam(const char *name)
{
	int f = app_family_of(&R, name);

	return f;
}

static const char *fam_name(int f)
{
	return f >= 0 ? R.fam[f].name : "(none)";
}

static int kind(const struct app_flow *f, int *conf)
{
	int c;
	int k = app_classify(&R, f, &c);

	if (conf)
		*conf = c;
	return k;
}

static void rules_file(const char *path)
{
	FILE *f = fopen(path, "r");
	static char buf[64 * 1024];
	size_t n;

	if (!f) {
		CHECK(0, "%s readable", path);
		return;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	{
		static struct app_rules a, b;

		app_rules_parse(&a, buf);
		app_rules_parse(&b, app_rules_default());
		CHECK(!memcmp(&a, &b, sizeof(a)) && !a.err[0], "%s is the built-in rules, with comments (the same %d rules, %d families)",
		      path, a.nrule, a.nfam);
	}
}

static void test_rules(void)
{
	struct app_rules r;
	int n;

	n = app_rules_parse(&R, app_rules_default());
	CHECK(n >= 12 && !R.err[0], "the built-in rules: %d kinds, %d families, %d names, no error (%s)", n, R.nfam, R.nsuf, R.err);
	CHECK(R.browse_new == 4, "browsing: 4 new web connections a minute");
	n = app_rules_parse(&r, "kind VoiceCall proto=udp pps=10-\nkind Nonsense x=1\nkind VideoCall down=abc\n"
			    "family Foo_Bar VideoStreaming foo.example\nbrowse 7\nfrobnicate\n");
	CHECK(n == 1 && r.nfam == 1 && r.browse_new == 7, "a file with bad lines: the good ones kept (%d rules, %d families)", n,
	      r.nfam);
	CHECK(!strncmp(r.err, "line 2:", 7), "the first bad line is said: %s", r.err);
	CHECK(!strcmp(r.fam[0].name, "Foo Bar"), "a family's _ is a space: '%s'", r.fam[0].name);
	CHECK(r.rule[0].proto == AP_UDP && r.rule[0].pps.lo == 10 && r.rule[0].pps.hi == 0xffffffffu, "pps=10-: from 10, no top");
	CHECK(app_rules_parse(&r, "# nothing\n") < 0, "a file without rules: -1 (agd falls back to the built-in ones)");
	n = app_rules_parse(&r, "kind VideoCall port=1,2,3,4,5,6,7\n");
	CHECK(n < 0 && strstr(r.err, "port"), "too many ports refused: %s", r.err);
}

static void test_families(void)
{
	CHECK(!strcmp(fam_name(fam("eu01web.zoom.us")), "Zoom"), "eu01web.zoom.us is Zoom");
	CHECK(!strcmp(fam_name(fam("rr3---sn-a5mekn7s.googlevideo.com")), "YouTube"), "a googlevideo.com edge is YouTube");
	CHECK(!strcmp(fam_name(fam("ipv4-c001-del001-ix.1.oca.nflxvideo.net")), "Netflix"), "nflxvideo.net is Netflix");
	CHECK(!strcmp(fam_name(fam("x.zoom.us (nearby)")), "Zoom"), "a nearby name counts too");
	CHECK(!strcmp(fam_name(fam("WWW.NETFLIX.COM.")), "Netflix"), "any case, a trailing dot");
	CHECK(!strcmp(fam_name(fam("eus.gssv-play-prod.xboxlive.com")), "Xbox Cloud"), "the longest suffix wins: Xbox Cloud");
	CHECK(!strcmp(fam_name(fam("title.mgt.xboxlive.com")), "Xbox Live"), "... else Xbox Live");
	CHECK(fam("notzoom.us") < 0, "notzoom.us is not Zoom (a label boundary)");
	CHECK(fam("") < 0 && fam(NULL) < 0, "no name, no family");
}

static void test_kinds(void)
{
	struct app_flow f;
	int c, i;

	f = flow(17, 3478, 3, 50, 172, 50, 172);
	CHECK(kind(&f, &c) == AK_VOICE && c == 75, "voice call: UDP 3478, 50 packets a second of 172 B each way (%d%%)", c);
	f.fam = fam("zoom.us");
	CHECK(kind(&f, &c) == AK_VOICE && c == 98, "... on Zoom's servers: surer (%d%%)", c);
	f = flow(17, 8801, 3, 120, 1000, 150, 1100);
	CHECK(kind(&f, &c) == AK_VIDEO, "video call: Zoom's port, ~1 Mbit/s each way (%s)", app_kind_name(kind(&f, NULL)));
	f = flow(17, 50000, 3, 136, 1100, 170, 1100);
	CHECK(kind(&f, &c) == AK_VIDEO && c == 55, "video call by its traits alone: a port of its own, 1.5 Mbit/s each way (%s %d%%)",
	      app_kind_name(kind(&f, NULL)), c);
	f.fam = fam("secret.nflxvideo.net (nearby)");
	CHECK(kind(&f, &c) == AK_VIDEO && !app_family_has(&R, f.fam, AK_VIDEO),
	      "... named after a Netflix server nearby: still a video call, and Netflix is not its app");
	f.n = 1;
	CHECK(kind(&f, &c) == AK_NONE, "... its first look, with that name: nothing yet (a name alone makes no UDP stream)");
	f = flow(17, 50000, 3, 200, 1200, 30, 400);
	CHECK(kind(&f, &c) == AK_SCREEN, "screen share: 1.9 Mbit/s up, 100 kbit/s down (%s)", app_kind_name(kind(&f, NULL)));
	f = flow(17, 49005, 3, 150, 200, 1900, 1300);
	CHECK(kind(&f, &c) == AK_CLOUDGAME, "cloud gaming: 20 Mbit/s down in big packets, inputs up (%s)",
	      app_kind_name(kind(&f, NULL)));
	f = flow(17, 3074, 3, 30, 90, 30, 140);
	CHECK(kind(&f, &c) == AK_GAME, "online game: Xbox Live's port, 30 small packets a second (%s)", app_kind_name(kind(&f, NULL)));
	f = flow(17, 40000, 3, 30, 90, 30, 140);
	CHECK(kind(&f, &c) == AK_VOICE, "... the same on an unknown port is taken for a voice call (games need a name or a port)");
	f.fam = fam("api.steamserver.net");
	CHECK(kind(&f, &c) == AK_GAME, "... with Steam's name: an online game (%s %d%%)", app_kind_name(kind(&f, NULL)), c);

	f = flow(6, 443, 6, 0, 0, 0, 0);
	{
		static const uint32_t k[6] = { 20000, 0, 8000, 0, 15000, 2000 };

		for (i = 0; i < 6; i++)
			tcp_look(&f.iv[i], k[i]);
	}
	CHECK(kind(&f, &c) == AK_STREAM, "video streaming: TCP 443 in bursts, about 5 Mbit/s (%s)", app_kind_name(kind(&f, NULL)));
	f = flow(6, 443, 4, 0, 0, 0, 0);
	for (i = 0; i < 4; i++)
		tcp_look(&f.iv[i], 50000 + (uint32_t)i * 500);
	CHECK(kind(&f, &c) == AK_DOWNLOAD, "large download: 50 Mbit/s, steady, every look (%s)", app_kind_name(kind(&f, NULL)));
	f.fam = fam("download.windowsupdate.com");
	CHECK(kind(&f, &c) == AK_DOWNLOAD && c == 75, "... Windows Update's: surer (%d%%)", c);
	f = flow(17, 443, 4, 0, 0, 0, 0);
	for (i = 0; i < 4; i++)
		tcp_look(&f.iv[i], 6000);
	f.fam = fam("rr1.googlevideo.com");
	CHECK(kind(&f, &c) == AK_STREAM, "QUIC from YouTube, 6 Mbit/s every look: a stream, not a download (%s)",
	      app_kind_name(kind(&f, NULL)));
	f = flow(6, 443, 3, 0, 0, 0, 0);
	f.fam = fam("ipv4-c001.oca.nflxvideo.net");
	for (i = 0; i < 3; i++)
		f.iv[i].ms = 10000;
	CHECK(kind(&f, &c) == AK_STREAM && c == 60, "Netflix idle (its buffer full): still the stream, by its name (%d%%)", c);
	f = flow(6, 443, 3, 2, 300, 2, 300);
	f.fam = fam("us04web.zoom.us");
	CHECK(kind(&f, &c) == AK_NONE, "Zoom's web connection is not a call (a call app's name alone gives no call)");
	f = flow(6, 8883, 3, 1, 120, 1, 120);
	CHECK(kind(&f, &c) == AK_NONE, "a trickle from a laptop is nothing");
	f.iot = 1;
	CHECK(kind(&f, &c) == AK_IOT, "... from a smart-home device: the smart home (%s)", app_kind_name(kind(&f, NULL)));
	f = flow(17, 3478, 2, 50, 172, 50, 172);
	CHECK(kind(&f, &c) == AK_NONE, "two looks are not enough (3 needed)");
}

static void test_quality(void)
{
	struct app_win w;
	struct app_q q;
	int r, m;

	m = app_emodel(20, 0, &r);
	CHECK(r == 93 && m == 44, "E-model: 20 ms, nothing lost: R %d, MOS %d.%d", r, m / 10, m % 10);
	m = app_emodel(50, 20, &r);
	CHECK(r >= 83 && r <= 86 && m >= 41 && m <= 43, "50 ms, 2%% lost: R %d, MOS %d.%d (good)", r, m / 10, m % 10);
	m = app_emodel(50, 100, &r);
	CHECK(r >= 59 && r <= 62 && m >= 30 && m <= 32, "50 ms, 10%% lost: R %d, MOS %d.%d (choppy)", r, m / 10, m % 10);
	m = app_emodel(400, 0, &r);
	CHECK(r >= 59 && r <= 63, "400 ms one way, nothing lost: R %d (people talk over each other)", r);

	memset(&w, 0, sizeof(w));
	w.kind = AK_VIDEO;
	w.ms = 60000;
	w.looks = w.active = 6;
	w.ref_pps = 300;
	w.rtt_ms = 40;
	w.jitter_ms = 5;
	w.loss10 = 0;
	w.wifi_rtt_ms = 3;
	w.wifi_loss_pct = 0;
	w.rtt_usual_ms = 35;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD && q.mos10 >= 42 && q.conf >= 70, "a video call, 43 ms, jitter 5 ms, nothing lost: good, MOS %d.%d, %d%%",
	      q.mos10 / 10, q.mos10 % 10, q.conf);
	w.gap_ms = 8200;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_FREEZE && q.seconds == 8, "... 8.2 s of its packets missing between usual looks: froze ~%d s",
	      q.seconds);
	w.gap_ms = 0;
	w.loss10 = 60;
	w.jitter_ms = 40;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_STUTTER && q.mos10 < 36, "... 6%% lost, jitter 40 ms: choppy, MOS %d.%d",
	      q.mos10 / 10, q.mos10 % 10);
	w.kind = AK_VOICE;
	w.loss10 = 0;
	w.jitter_ms = 5;
	w.gap_ms = 9000;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD, "a voice call quiet for 9 s on a clean network: silence, not a freeze");
	w.loss10 = 30;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_FREEZE, "... the same while 3%% was lost: it froze");
	w.gap_ms = 0;
	w.rtt_ms = w.wifi_rtt_ms = w.loss10 = w.wifi_loss_pct = w.jitter_ms = -1;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD && !q.mos10 && q.conf < 70, "nothing measured: no MOS, less sure (%d%%)", q.conf);

	memset(&w, 0, sizeof(w));
	w.kind = AK_STREAM;
	w.ms = 60000;
	w.looks = w.active = 6;
	w.ref_kbps = 5000;
	w.down_kbps = 2000;
	w.rtt_ms = w.loss10 = w.wifi_rtt_ms = w.wifi_loss_pct = -1;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_REBUFFER && q.seconds == 26, "a 5 Mbit/s stream fetching all minute at 2: "
	      "36 s played beyond what came in, a 10 s buffer: ~%d s rebuffering", q.seconds);
	w.down_kbps = 4000;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_REBUFFER && q.seconds == 12, "... then a minute at 4: 12 s more (%d)", q.seconds);
	w.active = 2;
	w.down_kbps = 3000;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD && w.drain_s == 0, "it paused fetching (buffer full again): fine");
	w.active = 6;
	w.ms = 15000;
	w.down_kbps = 2500;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_LOWRES, "15 s at half its bitrate: likely a lower picture quality, no stall yet");
	w.ref_kbps = 0;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD, "its bitrate not known yet: no verdict");

	memset(&w, 0, sizeof(w));
	w.kind = AK_GAME;
	w.ms = 60000;
	w.looks = w.active = 6;
	w.rtt_usual_ms = 30;
	w.rtt_ms = 200;
	w.wifi_rtt_ms = 2;
	w.loss10 = 0;
	w.wifi_loss_pct = 0;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_LAG, "a game, the internet at 200 ms (usually 30): lag spikes");
	w.rtt_ms = 40;
	app_quality(&w, &q);
	CHECK(q.state == AQ_GOOD, "... at 40 ms: fine");
	w.gap_ms = 1200;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_LAG && q.seconds == 1, "... 1.2 s of server updates missing: a lag spike");

	memset(&w, 0, sizeof(w));
	w.kind = AK_CLOUDGAME;
	w.ms = 60000;
	w.looks = w.active = 6;
	w.ref_kbps = 20000;
	w.down_kbps = 9000;
	w.rtt_ms = 20;
	w.loss10 = w.wifi_loss_pct = 0;
	w.wifi_rtt_ms = 2;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_LOWRES, "cloud gaming at 9 of its 20 Mbit/s: a worse picture");
	w.down_kbps = 19000;
	w.gap_ms = 1500;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_FREEZE && q.seconds == 2, "... 1.5 s stalled: it froze");

	memset(&w, 0, sizeof(w));
	w.kind = AK_BROWSE;
	w.ms = 60000;
	w.looks = w.active = 1;
	w.rtt_usual_ms = 30;
	w.rtt_ms = 300;
	w.wifi_rtt_ms = w.wifi_loss_pct = w.loss10 = -1;
	w.dns_ms = 20;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_SLOW, "browsing, the internet at 300 ms: pages likely slow");
	w.rtt_ms = 35;
	w.dns_fail_pct = 25;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD, "... a quarter of the lookups failing: slow too");

	memset(&w, 0, sizeof(w));
	w.kind = AK_IOT;
	w.offline_s = 120;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_OFFLINE && q.seconds == 120, "a doorbell away for 2 min: lost its connection");

	memset(&w, 0, sizeof(w));
	w.kind = AK_DOWNLOAD;
	w.ms = 60000;
	w.looks = w.active = 6;
	w.ref_kbps = 50000;
	w.down_kbps = 5000;
	app_quality(&w, &q);
	CHECK(q.state == AQ_BAD && q.what == AW_SLOW, "a download at 5 of the 50 Mbit/s it had: slowed down");
}

static void test_causes(void)
{
	struct app_evidence e;
	char b[300];

	memset(&e, 0, sizeof(e));
	e.wifi_loss_pct = e.rtt_ms = e.rtt_usual_ms = e.loss10 = -1;
	e.recorded = 1;
	e.code = "WIFI_CONGESTION";
	e.band = 2;
	CHECK(app_cause_of(&e) == AC_WIFI_BUSY, "WIFI_CONGESTION: Wi-Fi congestion");
	app_cause_words(AC_WIFI_BUSY, &e, b, sizeof(b));
	CHECK(!strcmp(b, "the 2.4 GHz Wi-Fi channel was congested"), "... '%s'", b);
	e.code = "WIFI_COVERAGE";
	e.signal_dbm = -79;
	app_cause_words(app_cause_of(&e), &e, b, sizeof(b));
	CHECK(app_cause_of(&e) == AC_WIFI_WEAK && strstr(b, "-79 dBm"), "WIFI_COVERAGE: a weak signal, '%s'", b);
	e.code = "LINE_FULL";
	e.top = "TV";
	e.top_kbps = 48200;
	app_cause_words(app_cause_of(&e), &e, b, sizeof(b));
	CHECK(app_cause_of(&e) == AC_LINE_FULL && !strcmp(b, "the line was full (TV downloading, 48.2 Mbit/s)"), "LINE_FULL, and by whom: '%s'", b);
	e.code = "ISP_SLOW";
	CHECK(app_cause_of(&e) == AC_ISP && !app_cause_home(AC_ISP), "ISP_SLOW: the ISP, not the home");
	e.code = "OK";
	e.top = NULL;
	e.signal_dbm = 0;
	CHECK(app_cause_of(&e) == AC_FAR_END, "nothing wrong in the home or on the line (recorded): the far end");
	e.recorded = 0;
	CHECK(app_cause_of(&e) == AC_UNKNOWN, "... not recorded: not known");
	e.code = NULL;
	e.recorded = 1;
	e.wifi_loss_pct = 15;
	CHECK(app_cause_of(&e) == AC_WIFI, "the timeline found nothing, but the device's own Wi-Fi lost 15%%: Wi-Fi");
	e.wifi_loss_pct = -1;
	e.signal_dbm = -81;
	CHECK(app_cause_of(&e) == AC_WIFI_WEAK, "... its signal at -81 dBm: weak");
	e.signal_dbm = 0;
	e.rtt_ms = 320;
	e.rtt_usual_ms = 30;
	CHECK(app_cause_of(&e) == AC_ISP, "... the internet at 320 ms (usually 30): the ISP");
	CHECK(!strcmp(app_cause_name(AC_WIFI_BUSY), "WiFiCongestion") && !strcmp(app_cause_name(AC_FAR_END), "FarEnd"),
	      "cause names for the data model");
}

static void test_words(void)
{
	struct app_moment_words w;
	struct app_evidence e;
	struct tm tm;
	char b[600];
	long t;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2026 - 1900;
	tm.tm_mon = 10;
	tm.tm_mday = 25;
	tm.tm_hour = 10;
	tm.tm_min = 2;
	t = (long)timegm(&tm);
	memset(&e, 0, sizeof(e));
	e.code = "WIFI_CONGESTION";
	e.band = 2;
	e.recorded = 1;
	memset(&w, 0, sizeof(w));
	w.kind = AK_VIDEO;
	w.device = "laptop";
	w.app = "Zoom";
	w.t = t;
	w.now = t + 3600;
	w.what = AW_FREEZE;
	w.seconds = 8;
	w.cause = AC_WIFI_BUSY;
	w.ev = &e;
	w.fix = "AIGW moved 2.4 GHz (wlan0) from channel 11 to 6";
	w.fix_t = t + 120;
	w.since_ok = 3;
	app_moment_text(&w, b, sizeof(b));
	CHECK(!strcmp(b, "The video call on laptop (Zoom) at 10:02 froze for about 8 s: the 2.4 GHz Wi-Fi channel was congested; "
		      "then at 10:04 AIGW moved 2.4 GHz (wlan0) from channel 11 to 6, and the 3 calls since were fine."),
	      "the moment in words: %s", b);
	w.device = NULL;
	w.app = NULL;
	w.since_ok = 1;
	w.since_bad = 1;
	app_moment_text(&w, b, sizeof(b));
	CHECK(!strncmp(b, "A video call at 10:02 froze", 27) && !strstr(b, "laptop") && !strstr(b, "Zoom") &&
	      strstr(b, "but 1 of the 2 calls since had trouble too"), "for the ISP, no names: %s", b);
	w.kind = AK_STREAM;
	w.what = AW_REBUFFER;
	w.seconds = 26;
	w.fix = NULL;
	w.since_ok = w.since_bad = 0;
	w.cause = AC_FAR_END;
	app_moment_text(&w, b, sizeof(b));
	CHECK(strstr(b, "likely stopped to buffer for about 26 s: nothing in the home or on the line was wrong then"),
	      "a stream, the far end: %s", b);
	CHECK(app_score(100, 2, 10) == 98 && app_score(5, 0, 10) == -1 && app_score(20, 30, 10) == 0,
	      "the score: 98%% of 100 minutes; too few minutes: not said");
	CHECK(app_group_by_name("calls") == AGR_CALLS && app_group_by_name("Streaming") == AGR_STREAMING &&
	      app_group_by_name("games") == AGR_GAMING && app_group_by_name("nope") < 0, "the app asked about, by name");
}

/* the cost: every connection classified at every look (a busy home: 160 at once, a look every 10 s) */
static void test_cost(void)
{
	struct app_flow f[8];
	struct timespec a, b;
	long n = 0;
	double us;
	int i, c, sink = 0;

	f[0] = flow(17, 3478, 3, 50, 172, 50, 172);
	f[1] = flow(17, 50000, 3, 136, 1100, 170, 1100);
	f[2] = flow(6, 443, 6, 0, 0, 0, 0);
	for (i = 0; i < 6; i++)
		tcp_look(&f[2].iv[i], i % 2 ? 0 : 9000);
	f[3] = flow(6, 443, 4, 0, 0, 0, 0);
	for (i = 0; i < 4; i++)
		tcp_look(&f[3].iv[i], 50000);
	f[4] = flow(6, 8883, 3, 1, 120, 1, 120);
	f[5] = flow(6, 443, 3, 2, 300, 2, 300);
	f[5].fam = fam("us04web.zoom.us");
	f[6] = flow(17, 49005, 3, 150, 200, 1900, 1300);
	f[7] = flow(6, 5223, 6, 1, 80, 1, 80);         /* nothing: every rule tried */
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (n = 0; n < 200000; n++)
		sink += app_classify(&R, &f[n % 8], &c);
	clock_gettime(CLOCK_MONOTONIC, &b);
	us = ((b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3) / (double)n;
	CHECK(us < 5.0, "classifying a connection: %.2f us here (%ld of them; a 160-connection look: %.0f us)", us, n, us * 160);
	CHECK(sizeof(R) <= 20 * 1024, "the rules in memory: %zu bytes", sizeof(R));
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (n = 0; n < 2000; n++)
		sink += fam("rr3---sn-a5mekn7s.googlevideo.com");
	clock_gettime(CLOCK_MONOTONIC, &b);
	us = ((b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3) / (double)n;
	CHECK(us < 20.0, "a server name's family (once per connection): %.2f us", us);
	(void)sink;
}

int main(int argc, char **argv)
{
	test_rules();
	if (argc > 1)
		rules_file(argv[1]);
	test_families();
	test_kinds();
	test_quality();
	test_causes();
	test_words();
	test_cost();
	if (failed) {
		printf("%d FAILED\n", failed);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
