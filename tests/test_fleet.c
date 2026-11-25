/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fleet learning (ag/fleet.h): buckets and their levels, the signed priors
 * file and what makes it refused, k-anonymity (a bucket of fewer than k homes
 * is never used), the Bayesian blend shifting from the fleet to the home as
 * the home's own outcomes grow, the choice (only by a margin, only with
 * evidence), and cold start: early warning from the fleet's normal while a
 * gateway still learns its own, handing over to the home's own.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/care.h"
#include "ag/fleet.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct fl_priors *parse(const char *text, char *err, size_t n)
{
	return fl_priors_parse(text, strlen(text), err, n);
}

static void buckets(void)
{
	struct fl_ctx c;
	char k[FL_KEY];

	printf("-- buckets and levels --\n");
	memset(&c, 0, sizeof(c));
	strcpy(c.f[0], "5"); strcpy(c.f[1], "80+"); strcpy(c.f[2], "high");
	strcpy(c.f[3], "phone"); strcpy(c.f[4], "evening"); strcpy(c.f[5], "fibre");
	fl_key(&c, 0, k, sizeof(k));
	CHECK(!strcmp(k, "5/80+/high/phone/evening/fibre") && fl_key_level(k) == 0, "level 0 keeps every dimension: %s", k);
	fl_key(&c, 1, k, sizeof(k));
	CHECK(!strcmp(k, "5/80+/high/phone/*/fibre") && fl_key_level(k) == 1, "level 1 leaves the hours out: %s", k);
	fl_key(&c, 2, k, sizeof(k));
	CHECK(!strcmp(k, "5/*/high/*/*/fibre") && fl_key_level(k) == 2, "level 2: band, congestion, line: %s", k);
	fl_key(&c, 4, k, sizeof(k));
	CHECK(!strcmp(k, "*/*/*/*/*/*") && fl_key_level(k) == 4, "level 4: the fault and action only: %s", k);
	memset(&c, 0, sizeof(c));
	fl_key(&c, 0, k, sizeof(k));
	CHECK(!strcmp(k, "-/-/-/-/-/-"), "unknown dimensions are '-': %s", k);
	CHECK(fl_key_level("5/*/high/phone/*/fibre") < 0, "a pattern of stars no level has is not a key");
	CHECK(fl_key_level("5/80+/high/phone/evening") < 0 && fl_key_level("02:00:00:00:00:01") < 0 &&
	      fl_key_level("a b/c/d/e/f/g") < 0, "five fields, a MAC, spaces: not keys");
	CHECK(!strcmp(fl_device_class("Android"), "phone") && !strcmp(fl_device_class("Windows"), "laptop") &&
	      !strcmp(fl_device_class("media player"), "tv") && !strcmp(fl_device_class("shelly"), "other") &&
	      !strcmp(fl_device_class("IoT"), "iot") && !strcmp(fl_device_class(""), "-"), "device classes from types");
	CHECK(fl_hour_band(0) == 0 && fl_hour_band(7) == 1 && fl_hour_band(13) == 2 && fl_hour_band(21) == 3,
	      "hour bands: night, morning, afternoon, evening");
}

static const char *good =
	"# aigw-priors fleet-priors\n# version 7\n# published 2026-11-25T10:00:00Z\n# k 5\n# homes 60\n"
	"# o <fault> <action> ...\n"
	"o WIFI_HOPPING stay24 5/80+/high/phone/evening/fibre 6 40 8 4 2\n"
	"o WIFI_HOPPING stay24 5/*/high/*/*/fibre 30 300 60 30 10\n"
	"o WIFI_HOPPING wait */*/*/*/*/* 50 500 350 0 0\n"
	"o LINE_FULL sqm */*/*/*/*/* 12 100 80 5 3\n"
	"n fibre 1 3 20 23.5 4\n"
	"n * 1 3 50 30 9\n";

static void priors_file(void)
{
	struct fl_priors *P;
	char err[160];

	printf("-- the priors file --\n");
	P = parse(good, err, sizeof(err));
	CHECK(P && P->version == 7 && P->k == 5 && P->homes == 60 && P->n == 4 && P->nn == 2, "a good file parses (%s)",
	      P ? P->published : err);
	fl_priors_free(P);
	CHECK(!parse("# aigw-priors fleet-priors\n# version 1\n# k 5\no A b */*/*/*/*/* 5 10 8 3 0\n", err, sizeof(err)) &&
	      strstr(err, "more than tried"), "helped + worse + undone above tried is refused: %s", err);
	CHECK(!parse("# aigw-priors fleet-priors\n# version 1\n# k 1\n", err, sizeof(err)) && strstr(err, "k must be"),
	      "k 1 (one home's counts as a prior) is refused: %s", err);
	CHECK(!parse("# aigw-priors other\n# version 1\n# k 5\n", err, sizeof(err)), "another name is refused: %s", err);
	CHECK(!parse("# aigw-priors fleet-priors\n# version 1\n# k 5\nx something\n", err, sizeof(err)),
	      "an unknown line is refused: %s", err);
	CHECK(!parse("# aigw-priors fleet-priors\n# version 1\n# k 5\no A b 02:00:00:00:00:01 5 10 8 1 0\n", err, sizeof(err)),
	      "a bucket that is not a key (a MAC) is refused: %s", err);
	CHECK(!parse("# aigw-priors fleet-priors\n# k 5\n", err, sizeof(err)), "no version: refused: %s", err);
}

static void k_anonymity(void)
{
	struct fl_priors *P;
	struct fl_ctx c;
	const struct fl_prior *p;
	char err[160];
	int level;

	printf("-- k-anonymity --\n");
	P = parse(good, err, sizeof(err));
	memset(&c, 0, sizeof(c));
	strcpy(c.f[0], "5"); strcpy(c.f[1], "80+"); strcpy(c.f[2], "high");
	strcpy(c.f[3], "phone"); strcpy(c.f[4], "evening"); strcpy(c.f[5], "fibre");
	p = fl_prior_find(P, "WIFI_HOPPING", "stay24", &c, 5, &level);
	CHECK(p && level == 0 && p->homes == 6, "the most specific bucket (6 homes, k 5) is used");
	p = fl_prior_find(P, "WIFI_HOPPING", "stay24", &c, 10, &level);
	CHECK(p && level == 2 && p->homes == 30, "this gateway's k 10: that bucket is skipped, a coarser one of 30 homes "
	      "is used (level %d)", level);
	p = fl_prior_find(P, "WIFI_HOPPING", "stay24", &c, 100, &level);
	CHECK(!p && level == -1, "no bucket of 100 homes: none is used");
	p = fl_prior_find(P, "WIFI_HOPPING", "wait", &c, 5, &level);
	CHECK(p && level == 4, "waiting: only the fleet-wide bucket exists (level 4)");
	CHECK(!fl_prior_find(P, "WIFI_DROPS", "stay24", &c, 5, &level), "a fault the fleet hasn't seen: nothing");
	CHECK(fl_norm_find(P, "fibre", 1, 3, 5) && fl_norm_find(P, "fibre", 1, 3, 5)->homes == 20,
	      "the fibre homes' evening internet delay normal");
	CHECK(fl_norm_find(P, "dsl", 1, 3, 5) && !strcmp(fl_norm_find(P, "dsl", 1, 3, 5)->line, "*"),
	      "no DSL normal: every line's together");
	CHECK(!fl_norm_find(P, "fibre", 1, 3, 60), "k 60: neither normal (20, 50 homes) is used");
	fl_priors_free(P);
}

static void blend(void)
{
	struct fl_counts fleet = { 200, 160, 10, 0 }, local = { 0, 0, 0, 0 };
	struct fl_blend b;
	char w[200];

	printf("-- the blend: from the fleet to the home --\n");
	fl_blend(NULL, 0, -1, NULL, 20, &b);
	CHECK(fabs(b.helped - 1.0 / 3) < 1e-9 && b.score == 0 && b.level == -1, "nothing known: helped 1/3, score 0");
	fl_blend(&fleet, 40, 0, &local, 20, &b);
	CHECK(b.helped > 0.70 && b.helped < 0.76 && b.fleet_share == 1, "cold start: the fleet's 80%% (helped %.2f, "
	      "the fleet's share %.0f%%)", b.helped, 100 * b.fleet_share);
	fl_blend_words(&b, w, sizeof(w));
	CHECK(strstr(w, "fleet: 160 of 200 tries in 40 homes") && strstr(w, "never tried here"), "in words: %s", w);
	local.tried = 10;
	local.helped = 1;
	fl_blend(&fleet, 40, 0, &local, 20, &b);
	CHECK(b.helped > 0.45 && b.helped < 0.60 && fabs(b.fleet_share - 20.0 / 30) < 1e-9,
	      "10 tries here, 1 helped: %.2f (the fleet's share 67%%)", b.helped);
	local.tried = 400;
	local.helped = 40;
	fl_blend(&fleet, 40, 0, &local, 20, &b);
	CHECK(b.helped < 0.16 && b.fleet_share < 0.05, "400 tries here, 10%% helped: the home's own dominates (%.2f, "
	      "the fleet's share %.0f%%)", b.helped, 100 * b.fleet_share);
	fl_blend(&fleet, 40, 0, NULL, 0, &b);
	CHECK(fabs(b.helped - 1.0 / 3) < 1e-9, "strength 0: the fleet weighs nothing");
	fleet.worse = 120;
	fleet.helped = 40;
	fl_blend(&fleet, 40, 0, NULL, 20, &b);
	CHECK(b.bad > b.helped && b.score < 0, "often made worse: a negative score (%.2f)", b.score);
}

static void choose(void)
{
	struct fl_counts fix = { 300, 60, 20, 10 }, wait = { 300, 210, 0, 0 }, few = { 3, 3, 0, 0 };
	struct fl_cand c[2];

	printf("-- the choice --\n");
	memset(c, 0, sizeof(c));
	c[0].action = "stay24";
	c[1].action = "wait";
	fl_blend(&fix, 50, 0, NULL, 20, &c[0].b);
	fl_blend(&wait, 50, 0, NULL, 20, &c[1].b);
	CHECK(fl_choose(c, 2, 0.15, 5) == 1, "waiting clears it 70%% of the time, the fix helps 20%%: wait (%.2f vs %.2f)",
	      c[1].b.score, c[0].b.score);
	CHECK(fl_choose(c, 2, 0.9, 5) == 0, "not better by the margin: the default stays");
	fl_blend(&few, 2, 0, NULL, 20, &c[1].b);
	CHECK(fl_choose(c, 2, 0.15, 5) == 0, "too little evidence behind waiting (3 tries): the default stays");
	fl_blend(&wait, 50, 0, NULL, 20, &c[0].b);
	fl_blend(&fix, 50, 0, NULL, 20, &c[1].b);
	CHECK(fl_choose(c, 2, 0.15, 5) == 0, "the default is the better one: it stays");
	CHECK(fl_choose(c, 1, 0.15, 5) == 0, "one candidate: it");
}

/* ---------- cold start ---------- */

#define T0   (20000L * 86400)
#define STEP 600

static int run(const struct care_prior *prior, float base, float from_hour, float add, int hours, int *fleet)
{
	struct care_learn L;
	struct care_learn_params p;
	struct care_sample s;
	long t = T0;
	int i, opened = 0;

	memset(&L, 0, sizeof(L));
	care_learn_params_default(&p);
	p.prior = prior;
	*fleet = 0;
	for (i = 0; i < hours * 3600 / STEP; i++, t += STEP) {
		int hour = (int)(t / 3600 % 24), ev;

		memset(&s, 0, sizeof(s));
		s.have[CM_INET_MS] = 1;
		s.v[CM_INET_MS] = base + (float)((i * 7) % 5) - 2 + ((float)i * STEP / 3600.0f >= from_hour ? add : 0);
		ev = care_learn_step(&L, &s, hour, t / 86400, t, 0, &p);
		if (ev & CW_OPENED) {
			opened++;
			*fleet = L.w.fleet;
		}
	}
	return opened;
}

static void cold_start(void)
{
	struct care_prior cp;
	char text[400];
	struct care_warning w;
	struct care_learn_params p;
	int b, fleet, n;

	printf("-- cold start: warnings from the fleet's normal --\n");
	memset(&cp, 0, sizeof(cp));
	cp.have[CM_INET_MS] = 1;
	for (b = 0; b < 4; b++) {
		cp.mean[CM_INET_MS][b] = 15;
		cp.dev[CM_INET_MS][b] = 2;
	}
	n = run(NULL, 15, 6, 60, 24, &fleet);
	CHECK(n == 0, "a new gateway without the fleet: no warning in its first day, whatever happens");
	n = run(&cp, 15, 6, 60, 24, &fleet);
	CHECK(n >= 1 && fleet, "with the fleet's normal: the internet's delay going from 15 to 75 ms on day one warns, "
	      "marked from the fleet");
	n = run(&cp, 15, 6, 20, 24, &fleet);
	CHECK(n == 0, "20 ms worse is below twice the effect (30 ms): no warning from another home's normal");
	n = run(&cp, 40, 99, 0, 48, &fleet);
	CHECK(n == 0, "a home whose normal is 40 ms (the fleet's 15): no warning, as its own normal takes over");
	n = run(&cp, 15, 80, 60, 96, &fleet);
	CHECK(n >= 1 && !fleet, "after the learning days: the home's own normal again (not from the fleet)");
	memset(&w, 0, sizeof(w));
	w.active = 1;
	w.metric = CM_INET_MS;
	w.kind = CW_SHIFT;
	w.normal = 15;
	w.current = 75;
	w.fleet = 1;
	care_learn_params_default(&p);
	care_warning_text(&w, &p, text, sizeof(text));
	CHECK(strstr(text, "from the fleet") != NULL, "the warning says its normal is the fleet's: %s", text);
}

int main(void)
{
	buckets();
	priors_file();
	k_anonymity();
	blend();
	choose();
	cold_start();
	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
