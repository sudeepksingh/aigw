/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW home Wi-Fi model: places, path loss, problems, what-if predictions and the words (ag/home.h) */
#include <stdio.h>
#include <string.h>

#include "ag/home.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1799971200L         /* a midnight (UTC) */

static struct hm_params P;
static struct hm_model M;
static struct hm_result R;

static const char *name_cb(const char *mac, void *ctx)
{
	(void)ctx;
	return !strcmp(mac, "02:00:00:00:00:01") ? "pixel-phone" : !strcmp(mac, "02:00:00:00:00:02") ? "tv-bedroom" :
	       !strcmp(mac, "02:00:00:00:00:03") ? "work-laptop" : !strcmp(mac, "02:00:00:00:00:04") ? "kitchen-tab" : NULL;
}

static struct hm_names NM = { name_cb, NULL, 1 };

#define PHONE  "02:00:00:00:00:01"
#define TV     "02:00:00:00:00:02"
#define LAPTOP "02:00:00:00:00:03"
#define TAB    "02:00:00:00:00:04"

/* one look at minute m: on access point `ear`, band, signal, loss; heard1: what extender 1 hears (0 none) */
static void look(const char *mac, long m, int ear, int band, int dbm, int loss, int heard1, int dual)
{
	struct cov_sample s;
	struct hm_look l;

	memset(&s, 0, sizeof(s));
	s.t = T0 + m * 60;
	s.ear = ear;
	s.band = band;
	s.dbm = dbm;
	s.loss_pct = loss;
	s.retry_pct = -1;
	s.heard[1] = heard1;
	s.hour = (int)((s.t / 3600) % 24);
	s.day = s.t / 86400;
	l.mac = mac;
	l.s = &s;
	l.dual = dual;
	l.weak_tx = 0;
	hm_add(&M, &l, &P);
}

static int find(int kind)
{
	int k;

	for (k = 0; k < R.nopt; k++)
		if (R.o[k].kind == kind)
			return k;
	return -1;
}

/*
 * A home: the living room near the gateway (-50 dBm on 5 GHz), the office (-60),
 * the bedroom far (-76 on 5 GHz for the TV, losing 20%). The phone walks
 * living room -> office -> bedroom every evening (so the places are neighbours).
 * Each "day": 3 hours of looks a minute.
 */
static void home_a(int days, int bedroom_band, int bedroom_dbm)
{
	long m;
	int d;

	for (d = 0; d < days; d++)
		for (m = 0; m < 180; m++) {
			long t = d * 1440L + 18 * 60 + m;
			int ph = m < 60 ? -50 : m < 100 ? -60 : bedroom_dbm;
			int pb = m < 100 ? 5 : bedroom_band;

			look(LAPTOP, t, 0, 5, -60 + (int)(m % 3) - 1, 0, 0, 1);
			/* the TV in the bedroom: a problem for 40 minutes, then 10 fine, ... */
			look(TV, t, 0, bedroom_band, bedroom_dbm + (int)(m % 2), (m % 50) < 40 ? 20 : 2, 0, bedroom_band == 5);
			look(PHONE, t, 0, pb, ph, m >= 100 ? 15 : 0, 0, 1);
		}
}

static void test_free_fix_first(void)
{
	char w[700];
	int e, b, up, k, bed, off, liv;
	float dbed, doff;

	hm_init(&M);
	home_a(3, 5, -76);
	hm_expire(&M, T0 + 3 * 86400L, &P);
	hm_judge(&M, T0 + 3 * 86400L, &P, &R);
	CHECK(R.status == HS_READY, "ready after %u looks", M.looks);
	CHECK(R.places >= 3 && R.places <= 5, "the home's places: %d (living room, office, bedroom)", R.places);
	bed = hm_place_of(&M, TV);
	off = hm_place_of(&M, LAPTOP);
	liv = -1;
	for (k = 0; k < M.np; k++)
		if (M.p[k].n && M.p[k].sig[0] > -53)
			liv = k;
	CHECK(bed >= 0 && off >= 0 && liv >= 0 && bed != off && off != liv, "the TV's, the laptop's and the living room's places");
	dbed = hm_distance(&M.p[bed], &P);
	doff = hm_distance(&M.p[off], &P);
	CHECK(dbed > 2 * doff && doff > hm_distance(&M.p[liv], &P), "distance-like: bedroom %.0f > office %.0f > living room",
	      dbed, doff);
	CHECK(hm_path_loss(&M.p[bed], 5, &P) == 23 + 76 || hm_path_loss(&M.p[bed], 5, &P) == 23 + 75,
	      "the bedroom's path loss on 5 GHz: %d dB", hm_path_loss(&M.p[bed], 5, &P));
	CHECK(M.p[off].adj[bed] || M.p[bed].adj[off], "office and bedroom are neighbours (the phone walks between them)");
	CHECK(R.problems >= 6, "problems in the last 30 days: %d (the TV's, the phone's)", R.problems);
	CHECK(R.target == bed, "the place with most problems: the bedroom");
	e = find(HO_EXTENDER);
	b = find(HO_BAND24);
	up = find(HO_GATEWAY_UP);
	CHECK(e >= 0 && R.o[e].fixed >= 1 && R.o[e].gain_db >= 4 && R.o[e].place == bed, "an extender fixes %d (+%d dB)",
	      e >= 0 ? R.o[e].fixed : -1, e >= 0 ? R.o[e].gain_db : 0);
	CHECK(b >= 0 && R.o[b].fixed == R.problems, "2.4 GHz there fixes all %d (+%d dB)", b >= 0 ? R.o[b].fixed : -1,
	      b >= 0 ? R.o[b].gain_db : 0);
	CHECK(up >= 0 && R.o[up].confidence < 45, "the gateway higher: low confidence (%d)", up >= 0 ? R.o[up].confidence : -1);
	CHECK(R.best >= 0 && !R.o[R.best].buy && R.o[R.best].kind == HO_BAND24, "before you buy: the free change first (%s)",
	      R.best >= 0 ? hm_kind_name(R.o[R.best].kind) : "none");
	CHECK(R.extender_helps == 0, "an extender is not worth selling: a free change fixes as much");
	hm_advice(&M, &R, &NM, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(strstr(w, "2.4 GHz") && strstr(w, "Try this before buying an extender") && strstr(w, "where tv-bedroom"),
	      "the advice says what, where, and before buying");
	CHECK(hm_name(&M, bed, "bedroom") == 0, "the bedroom named");
	hm_judge(&M, T0 + 3 * 86400L, &P, &R);
	hm_option_words(&M, &R, find(HO_EXTENDER), &NM, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(strstr(w, "in the bedroom") && strstr(w, "near the spot where") && strstr(w, "one access point"),
	      "an extender in words: near a place, by name, with its limit");
	NM.dev = NULL;
	NM.zones = 0;
	hm_option_words(&M, &R, find(HO_EXTENDER), &NM, w, sizeof(w));
	CHECK(strstr(w, "place ") && !strstr(w, "bedroom") && !strstr(w, "tv-"), "for the ISP: places by number, no names");
	NM.dev = name_cb;
	NM.zones = 1;
}

static void test_extender_wins(void)
{
	char w[700];
	int e, best;

	/* the bedroom's devices already on 2.4 GHz at -77: only an extender reaches it */
	hm_init(&M);
	home_a(3, 2, -77);
	hm_expire(&M, T0 + 3 * 86400L, &P);
	hm_judge(&M, T0 + 3 * 86400L, &P, &R);
	e = find(HO_EXTENDER);
	best = R.best;
	CHECK(find(HO_BAND24) < 0, "on 2.4 GHz already: no Band24 option");
	CHECK(e >= 0 && best == e && R.o[e].fixed >= 1, "the extender is the best option (fixes %d of %d)",
	      e >= 0 ? R.o[e].fixed : -1, R.problems);
	CHECK(R.extender_helps == 1, "an extender helps: the ISP may offer one");
	hm_advice(&M, &R, &NM, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(strstr(w, "extender") && strstr(w, "of the last 30 days"), "the advice: the extender, with the problems it fixes");

	/* the proof: an extender appears and hears the bedroom at -58 */
	M.ext_place = (int8_t)R.o[e].place;
	M.ext_gain = (int8_t)R.o[e].gain_db;
	{
		long m;

		for (m = 0; m < 40; m++)
			look(TV, 4 * 1440L + m, 0, 2, -77, 0, -58, 0);
	}
	hm_proof_words(&M, &NM, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(M.chk_place >= 0 && M.chk_measured != 127 && M.chk_measured >= 10, "measured after the extender: %+d dB",
	      M.chk_measured);
	CHECK(strstr(w, "predicted") && strstr(w, "measured"), "the proof in words");
}

static void test_crowded_24(void)
{
	long m;
	int b;

	/* a tablet in the kitchen on 2.4 GHz with a strong signal but 15% lost: a crowded band */
	hm_init(&M);
	for (m = 0; m < 400; m++) {
		look(TAB, m, 0, 2, -52, (m % 30) < 20 ? 15 : 1, 0, 1);
		look(LAPTOP, m, 0, 5, -55, 0, 0, 1);
	}
	hm_expire(&M, T0 + 400 * 60L, &P);
	hm_judge(&M, T0 + 400 * 60L, &P, &R);
	b = find(HO_BAND5);
	CHECK(R.problems >= 5 && b >= 0 && R.o[b].fixed == R.problems && R.best == b,
	      "a crowded 2.4 GHz: moving to 5 GHz fixes %d of %d", b >= 0 ? R.o[b].fixed : -1, R.problems);
	CHECK(find(HO_EXTENDER) < 0 && R.extender_helps == 0, "no extender for a crowded band");
}

static void test_limits(void)
{
	long m;
	int k, named = -1, ok = 1;

	/* many distinct signatures: the places stay at HM_PLACES, a named one is kept */
	hm_init(&M);
	for (m = 0; m < 60; m++)
		look(LAPTOP, m, 0, 5, -45, 0, 0, 1);
	CHECK(hm_name(&M, hm_place_of(&M, LAPTOP), "desk") == 0, "a place named by a device seen there");
	for (m = 0; m < 2000; m++)
		look(PHONE, 100 + m, 0, 5, -48 - (int)((m / 20) % 40), 0, 0, 1);
	for (k = 0; k < M.np; k++)
		if (!strcmp(M.p[k].name, "desk"))
			named = k;
	CHECK(M.np <= HM_PLACES && named >= 0, "%d places of at most %d; the named one kept", M.np, HM_PLACES);
	for (k = 0; k < HM_DEVS; k++)
		if (M.d[k].mac[0] && (M.d[k].place < -1 || M.d[k].place >= M.np))
			ok = 0;
	CHECK(ok, "devices point at places that exist after merging");

	/* problems older than 30 days are forgotten */
	hm_init(&M);
	home_a(2, 5, -78);
	hm_expire(&M, T0 + 2 * 86400L, &P);
	k = hm_place_problems(&M, -1, T0 + 2 * 86400L, &P);
	hm_expire(&M, T0 + 40 * 86400L, &P);
	CHECK(k > 0 && hm_place_problems(&M, -1, T0 + 40 * 86400L, &P) == 0, "problems forgotten after 30 days (%d before)", k);
	hm_judge(&M, T0 + 40 * 86400L, &P, &R);
	CHECK(R.extender_helps == 0 && R.best < 0, "nothing to fix: nothing to buy");

	/* learning: nothing said yet */
	hm_init(&M);
	look(TV, 0, 0, 5, -80, 30, 0, 1);
	hm_judge(&M, T0, &P, &R);
	CHECK(R.status == HS_LEARNING && R.extender_helps == -1, "learning: not known yet");
	CHECK(sizeof(struct hm_model) < 8192, "the model is compact: %zu bytes", sizeof(struct hm_model));
}

int main(void)
{
	hm_params_default(&P);
	P.learn = 60;
	test_free_fix_first();
	test_extender_wins();
	test_crowded_24();
	test_limits();
	printf("%s\n", failed ? "SOME TESTS FAILED" : "all home model tests passed");
	return failed ? 1 : 0;
}
