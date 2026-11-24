/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW coverage placement advice: a device's history, spots, the home's verdict and the words (ag/coverage.h) */
#include <stdio.h>
#include <string.h>

#include "ag/coverage.h"
#include "ag/proactive.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1799971200L         /* a midnight (UTC) */

static struct cov_params p;
static struct cov_dev D[COV_DEVS];
static struct cov_result R;
static const char *names[COV_DEVS];
static int iot_dev = -1;

static const char *dev_cb(int i, void *ctx) { (void)ctx; return names[i]; }
static const char *ear_cb(int e, void *ctx) { (void)ctx; return e == 1 ? "the extender 'bedroom'" : e ? "an extender" : "the gateway"; }
static int iot_cb(int i, void *ctx) { (void)ctx; return i == iot_dev; }

static struct cov_names NM = { dev_cb, ear_cb, NULL, 1 };

static void reset(void)
{
	memset(D, 0, sizeof(D));
	memset(names, 0, sizeof(names));
	iot_dev = -1;
	NM.ears = 1;
}

static void dev(int i, const char *mac, const char *name)
{
	snprintf(D[i].mac, sizeof(D[i].mac), "%s", mac);
	names[i] = name;
}

/* one sample of device i at minute m (from T0, local time = UTC here) */
static void at(int i, long m, int band, int dbm, int loss, int heard1)
{
	struct cov_sample s;

	memset(&s, 0, sizeof(s));
	s.t = T0 + m * 60;
	s.band = band;
	s.dbm = dbm;
	s.hour = (int)((s.t / 3600) % 24);
	s.day = s.t / 86400;
	s.loss_pct = loss;
	s.retry_pct = -1;
	s.heard[1] = heard1;
	cov_add(&D[i], &s, &p);
}

static int hour_of(long m) { return (int)(((T0 + m * 60) / 3600) % 24); }

/* the bedroom phone: fine in the living room (-55) by day, weak in the bedroom from 21:00 to 01:00 (-84, losing 38%) */
static void phone(int i, int days, int band_at_night, int heard1)
{
	long m;

	for (m = 0; m < days * 1440L; m++) {
		int h = hour_of(m), night = h >= 21 || h < 1;

		if (h >= 1 && h < 7)
			continue;       /* off, charging */
		if (night)
			at(i, m, band_at_night, band_at_night == 2 ? -77 : -84, 38, heard1);
		else
			at(i, m, 5, -55, 0, 0);
	}
}

static void tv(int i, int days, int dbm, int loss, int heard1)
{
	long m;

	for (m = 0; m < days * 1440L; m++)
		if (hour_of(m) >= 18 || hour_of(m) < 1)
			at(i, m, 5, dbm + (int)(m % 3) - 1, loss, heard1);
}

static void judge(void)
{
	cov_judge(D, COV_DEVS, &p, iot_cb, NULL, &R);
}

int main(void)
{
	char b[600];

	cov_params_default(&p);
	p.weak_dbm = -75;

	printf("== learning: a phone seen for half an hour\n");
	reset();
	dev(0, "02:00:00:00:00:01", "pixel-7");
	for (long m = 0; m < 30; m++)
		at(0, m, 5, -84, 40, 0);
	judge();
	CHECK(R.d[0].kind == CK_LEARNING && R.verdict == CV_LEARNING, "learning (%s, %s)", cov_kind_name(R.d[0].kind),
	      cov_verdict_name(R.verdict));
	cov_advice(&R, -1, &NM, b, sizeof(b));
	CHECK(!b[0], "no advice while learning: callers keep the generic line (%s)", b);

	printf("== one phone weak only at night in one spot (the PC access point's field case)\n");
	reset();
	dev(0, "02:00:00:00:00:01", "pixel-7");
	dev(1, "02:00:00:00:00:02", "laptop");
	phone(0, 3, 5, 0);
	for (long m = 0; m < 3 * 1440L; m++)
		if (hour_of(m) >= 8 && hour_of(m) < 20)
			at(1, m, 5, -58, 1, 0);
	judge();
	CHECK(R.d[0].kind == CK_SOMETIMES, "the phone: sometimes weak (%s)", cov_kind_name(R.d[0].kind));
	CHECK(R.d[0].from_h == 21 && R.d[0].to_h == 1, "weak 21:00-01:00 (%d-%d)", R.d[0].from_h, R.d[0].to_h);
	CHECK(R.d[0].days == 3 && R.d[0].loss_weak == 38 && R.d[0].suffers, "on 3 days, losing 38%% then (%d, %d)",
	      R.d[0].days, R.d[0].loss_weak);
	CHECK(R.d[0].usual >= -56 && R.d[0].usual <= -54 && R.d[0].low <= -83, "usually -55 dBm, down to -84 (%d, %d)", R.d[0].usual, R.d[0].low);
	CHECK(R.d[1].kind == CK_FINE, "the laptop: fine (%s)", cov_kind_name(R.d[1].kind));
	CHECK(R.verdict == CV_MOVE_DEVICE && R.worst == 0 && R.nspot == 1 && !R.spot[0].shared,
	      "verdict: move the device, not the gateway (%s, %d spots)", cov_verdict_name(R.verdict), R.nspot);
	cov_advice(&R, -1, &NM, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(strstr(b, "pixel-7 is weak only some of the time") && strstr(b, "mostly 21:00-01:00") &&
	      strstr(b, "on 3 of the last 7 days") && strstr(b, "where pixel-7 is, in the evening (about -84 dBm at the gateway)") &&
	      strstr(b, "about halfway"), "advice: which device, when, where, and where an extender would go");
	cov_dev_words(&R, 0, &NM, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(strstr(b, "usually -5") && strstr(b, "loses 38% when weak, 0% otherwise"), "the device's line");
	cov_note(&R, -1, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(b[0] && pro_plain(b) && strstr(b, "one part of your home") && !strstr(b, "pixel"),
	      "the customer's note: plain, no names");

	printf("== the same spot on 2.4 GHz: brought to 5 GHz terms, still one spot\n");
	reset();
	dev(0, "02:00:00:00:00:01", "pixel-7");
	phone(0, 2, 5, 0);
	phone(0, 2, 2, 0);
	judge();
	CHECK(R.nspot == 1 && D[0].spot[1].n == 0, "one spot, whichever band (%d spots, %u %u)", R.nspot, D[0].spot[0].n,
	      D[0].spot[1].n);

	printf("== the TV lives where the phone goes at night: a spot two devices share\n");
	reset();
	dev(0, "02:00:00:00:00:01", "pixel-7");
	dev(1, "02:00:00:00:00:03", "tv-lounge");
	phone(0, 3, 5, 0);
	tv(1, 3, -86, 22, 0);
	judge();
	CHECK(R.d[1].kind == CK_ALWAYS, "the TV: always weak (%s)", cov_kind_name(R.d[1].kind));
	CHECK(R.verdict == CV_EXTENDER && R.main >= 0 && R.spot[R.main].shared && R.spot[R.main].ndev == 2,
	      "verdict: the gateway's place, an extender (%s)", cov_verdict_name(R.verdict));
	cov_advice(&R, -1, &NM, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(strstr(b, "2 devices are weak in the same spot") && strstr(b, "Move the gateway toward that spot") &&
	      strstr(b, "about halfway between the gateway and it") && strstr(b, "tv-lounge") && strstr(b, "pixel-7"),
	      "advice: the shared spot, by the devices seen there");
	cov_advice(&R, 0, &NM, b, sizeof(b));
	CHECK(strstr(b, "pixel-7 is weak in a spot 1 other device share"), "the phone's own advice names the shared spot: %s", b);
	cov_note(&R, -1, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(pro_plain(b) && strstr(b, "extender placed about halfway"), "the note: an extender, about halfway");

	printf("== a weak smart plug that loses nothing, while others that weak lose a lot\n");
	dev(2, "02:00:00:00:00:04", "plug-1");
	for (long m = 0; m < 3 * 1440L; m++)
		at(2, m, 2, -82 + (int)(m % 2), 0, 0);
	judge();
	CHECK(R.d[2].kind == CK_WEAK_TX && !R.d[2].suffers, "the plug: a weak transmitter (%s)", cov_kind_name(R.d[2].kind));
	CHECK(R.spot[R.main].ndev == 2, "not in the shared spot (%d devices there)", R.spot[R.main].ndev);
	cov_advice(&R, 2, &NM, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(strstr(b, "own radio is weak") && strstr(b, "not a coverage problem"), "advice: nothing to fix in the Wi-Fi");

	printf("== a smart-home device nobody measured, in a home that is otherwise fine\n");
	reset();
	dev(0, "02:00:00:00:00:05", "sensor");
	dev(1, "02:00:00:00:00:02", "laptop");
	iot_dev = 0;
	for (long m = 0; m < 2 * 1440L; m++) {
		at(0, m, 2, -80, -1, 0);
		at(1, m, 5, -52, 0, 0);
	}
	judge();
	CHECK(R.d[0].kind == CK_WEAK_TX && R.verdict == CV_WEAK_DEVICE, "a weak transmitter, likely (%s, %s)",
	      cov_kind_name(R.d[0].kind), cov_verdict_name(R.verdict));
	cov_note(&R, -1, b, sizeof(b));
	CHECK(pro_plain(b) && strstr(b, "signal of its own"), "the note: your Wi-Fi is fine (%s)", b);

	printf("== two weak devices in different spots (-84 and -77: not the same distance)\n");
	reset();
	dev(0, "02:00:00:00:00:01", "pixel-7");
	dev(1, "02:00:00:00:00:02", "laptop");
	phone(0, 2, 5, 0);
	for (long m = 0; m < 2 * 1440L; m++) {
		int h = hour_of(m);

		if (h >= 7 && h < 9)
			at(1, m, 5, -77, 25, 0);
		else if (h >= 9 && h < 18)
			at(1, m, 5, -50, 0, 0);
	}
	judge();
	CHECK(R.verdict == CV_MOVE_DEVICE && R.nspot == 2 && !R.shared, "two spots of one device each: move the devices (%s, %d)",
	      cov_verdict_name(R.verdict), R.nspot);
	CHECK(!strcmp(cov_part_words(R.spot[R.d[1].spot].when), "in the morning"), "the laptop's spot: in the morning (%s)",
	      cov_part_words(R.spot[R.d[1].spot].when));

	printf("== a mesh: the extender 'bedroom' hears the spot at -60\n");
	reset();
	NM.ears = 2;
	dev(0, "02:00:00:00:00:01", "pixel-7");
	dev(1, "02:00:00:00:00:03", "tv-lounge");
	phone(0, 2, 5, -60);
	tv(1, 2, -86, 22, -61);
	judge();
	CHECK(R.verdict == CV_USE_EXTENDER && R.spot[R.main].best_ear == 1 && R.spot[R.main].best_dbm >= -62,
	      "the spot is heard best by the extender: use it, no new one (%s, ear %d, %d dBm)", cov_verdict_name(R.verdict),
	      R.spot[R.main].best_ear, R.spot[R.main].best_dbm);
	cov_advice(&R, -1, &NM, b, sizeof(b));
	printf("     %s\n", b);
	CHECK(strstr(b, "but the extender 'bedroom' hears it well (about -6") && strstr(b, "they belong on it"),
	      "advice: which access point hears the spot best");
	cov_advice(&R, 0, &NM, b, sizeof(b));
	CHECK(strstr(b, "The extender 'bedroom' hears that spot best") != NULL, "the phone's advice too: %s", b);
	cov_note(&R, -1, b, sizeof(b));
	CHECK(pro_plain(b) && strstr(b, "extender reaches it"), "the note: the extender reaches it (%s)", b);
	reset();
	NM.ears = 2;
	dev(0, "02:00:00:00:00:01", "pixel-7");
	dev(1, "02:00:00:00:00:03", "tv-lounge");
	phone(0, 2, 5, -95);
	tv(1, 2, -86, 22, -94);
	judge();
	cov_advice(&R, -1, &NM, b, sizeof(b));
	CHECK(R.verdict == CV_EXTENDER && strstr(b, "No extender hears it better than the gateway"),
	      "no extender hears it: one more, halfway (%s)", b);

	printf("== the history halves after a week of samples\n");
	reset();
	dev(0, "02:00:00:00:00:02", "laptop");
	for (long m = 0; m < 10080 + 5; m++)
		at(0, m, 5, -50, -1, 0);
	CHECK(D[0].n < 6000 && D[0].n > 5000, "halved (%u samples kept)", D[0].n);
	judge();
	CHECK(R.d[0].kind == CK_FINE && R.verdict == CV_FINE, "still fine (%s)", cov_kind_name(R.d[0].kind));
	cov_advice(&R, -1, &NM, b, sizeof(b));
	CHECK(strstr(b, "nothing to move") != NULL, "fine: %s", b);

	printf("%s\n", failed ? "SOME TESTS FAILED" : "all coverage tests passed");
	return failed ? 1 : 0;
}
