/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Wi-Fi sensing (ag/sense.h) on a simulated home, with whole days held out.
 *
 * The simulator (an assumption, not a measurement): one person; a TV (living
 * room, 5 GHz, steady traffic), a speaker (kitchen, 2.4 GHz), a smart plug
 * (bedroom, 2.4 GHz), a laptop (living room, bursty) and the person's phone
 * (bursty, goes where they go). A look every 5 s; a device sends frames
 * before a look with some probability (bursty: gaps of seconds, as RoomSense
 * measured). Quiet links spread about 0.8 dB; a person moving in a link's zone
 * adds about 3 dB and sometimes shifts it for a window (a body in the path);
 * rare stray readings of +-10 dB (interference). Weekdays: up at 6:30, out
 * 8:30-17:30 with the phone, home in the evening, asleep from 23:00 (turning
 * over now and then). Weekends: home all day. Times move +-20 min each day.
 *
 * Days 0-9 learn; days 10-13 are held out (the model learns nothing from them:
 * frozen) and scored: presence per window, movement per zone and window. Then
 * the check-in: a day the person stops moving at 10:00 (with and without a
 * phone of their own), and the held-out days for false alarms.
 *
 * Also the budget: RAM (the state's size) and CPU (time per look) on this
 * machine; agd --sense-bench measures the same on a router (aigw-selftest).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ag/sense.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 88172645463325252ULL;

static double unif(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (double)(rng >> 11) / 9007199254740992.0;
}

static double gauss(void)
{
	double a = unif() + 1e-12, b = unif();

	return sqrt(-2 * log(a)) * cos(6.283185307 * b);
}

enum { Z_LIVING, Z_KITCHEN, Z_BED, NZ };
enum { L_TV, L_SPEAKER, L_PLUG, L_LAPTOP, L_PHONE, NL };

static const struct {
	const char *mac;
	int zone, band, base;
	double send;            /* P(frames before a look) */
} LINKS[NL] = {
	{ "02:00:00:00:51:01", Z_LIVING, 5, -55, 0.9 },
	{ "02:00:00:00:51:02", Z_KITCHEN, 2, -66, 0.8 },
	{ "02:00:00:00:51:03", Z_BED, 2, -72, 0.6 },
	{ "02:00:00:00:51:04", Z_LIVING, 5, -50, 0.35 },
	{ "02:00:00:00:51:05", -1, 5, -60, 0.3 },
};

struct person {
	int home, zone, moving, phone_home;
};

/* where the person is and what they do, minute of the day m on day d (fall: stops moving from this minute, -1 none).
   Movement comes in episodes (about 3 min each); how much of the time depends on the activity. */
static void schedule(int d, int m, const int *jit, int fall, struct person *p)
{
	static int moving;
	int weekend = d % 7 >= 5, up = 390 + jit[0], out = 510 + jit[1], back = 1050 + jit[2], bed = 1380 + jit[3];
	double f = 0, r = unif();

	memset(p, 0, sizeof(*p));
	p->home = 1;
	p->phone_home = 1;
	if (m < up || m >= bed) {
		p->zone = Z_BED;
		f = 0.01;                               /* turning over */
	} else if (!weekend && m >= out && m < back) {
		p->home = 0;
		p->phone_home = 0;                      /* the phone goes with them */
	} else {
		int k = (m / 12 + d) % 5;               /* a new room every ~12 min */

		p->zone = m >= 1110 && m < 1155 ? Z_KITCHEN : k < 3 ? Z_LIVING : Z_KITCHEN;
		f = m < out ? 0.7 : p->zone == Z_KITCHEN ? 0.8 : 0.35;
	}
	if (f <= 0.02)
		moving = r < f;
	else if (moving)
		moving = r >= 1.0 / 3;                  /* an episode ends */
	else
		moving = r < f / (3 * (1 - f) + 1e-9);  /* one starts: f of the time on average */
	p->moving = p->home && moving;
	if (fall >= 0 && m >= fall) {
		p->home = 1;
		p->phone_home = 1;
		p->zone = Z_KITCHEN;
		p->moving = 0;
	}
}

struct score {
	long pres_ok, pres_n, tp, fp, fn, tn;
};

static struct sn_params P;
static struct sn_state S;
static uint32_t rx[NL];
static double look_ns;
static long looks;

/* one day through the model; returns SN_EV_* seen (and when the first INACTIVE came, minute) */
static int day(int d, int fall, int with_phone, struct score *sc, int *inactive_at, int *false_alarms)
{
	int jit[4], m, w, k, evs = 0;

	for (k = 0; k < 4; k++)
		jit[k] = (int)(unif() * 40) - 20;
	if (inactive_at)
		*inactive_at = -1;
	for (m = 0; m < 1440; m++) {
		struct person pp;

		schedule(d, m, jit, fall, &pp);
		for (w = 0; w < 2; w++) {                       /* two 30 s windows a minute */
			long t0 = 1800000000L + (long)d * 86400 + m * 60 + w * 30;
			double shift[NL];
			int lk, ev, zm[NZ] = { 0 };
			struct timespec a, b;

			for (k = 0; k < NL; k++)
				shift[k] = unif() < 0.3 ? unif() * 10 - 5 : 0;
			clock_gettime(CLOCK_MONOTONIC, &a);
			for (lk = 0; lk < P.window; lk++) {
				for (k = 0; k < NL; k++) {
					int zone = LINKS[k].zone, base = LINKS[k].base;
					double v, send = LINKS[k].send;

					if (k == L_PHONE) {
						if (!with_phone || !pp.phone_home)
							continue;
						zone = pp.home ? pp.zone : Z_BED;
						base = zone == Z_LIVING ? -50 : zone == Z_KITCHEN ? -60 : -68;
						send = pp.zone == Z_BED && !pp.moving ? 0.1 : 0.3;
					}
					v = base + gauss() * 0.8;
					if (pp.home && pp.zone == zone) {
						if (pp.moving)
							v += gauss() * 3 + shift[k] + (k == L_PHONE ? unif() * 8 - 4 : 0);
						else
							v += gauss() * 0.6;
					}
					if (unif() < 0.003)
						v += unif() < 0.5 ? 10 : -10;
					if (unif() < send)
						rx[k] += 1 + (uint32_t)(unif() * 20);
					sn_look(&S, LINKS[k].mac, LINKS[k].band, k == L_PHONE ? sn_zone(&S, zone == Z_LIVING ? "living" :
						zone == Z_KITCHEN ? "kitchen" : "bedroom") : sn_zone(&S, zone == Z_LIVING ? "living" :
						zone == Z_KITCHEN ? "kitchen" : "bedroom"), k == L_PHONE, (int)lround(v), rx[k],
						t0 + lk * 5);
					looks++;
				}
			}
			ev = sn_window(&S, t0 + 30, m / 60, 20003 + d, &P);    /* day 20003 is a Monday */
			clock_gettime(CLOCK_MONOTONIC, &b);
			look_ns += (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
			evs |= ev;
			if ((ev & SN_EV_INACTIVE) && inactive_at && *inactive_at < 0)
				*inactive_at = m;
			if ((ev & SN_EV_INACTIVE) && false_alarms)
				(*false_alarms)++;
			if (!sc)
				continue;
			sc->pres_n++;
			sc->pres_ok += S.home == pp.home;
			if (pp.home && pp.moving)
				zm[pp.zone] = 1;
			for (k = 0; k < NZ; k++) {
				const char *zn = k == Z_LIVING ? "living" : k == Z_KITCHEN ? "kitchen" : "bedroom";
				int z = sn_zone(&S, zn), said = z >= 0 && S.z[z].moving;

				if (said && zm[k])
					sc->tp++;
				else if (said)
					sc->fp++;
				else if (zm[k])
					sc->fn++;
				else
					sc->tn++;
			}
		}
	}
	return evs;
}

static void run(int with_phone, const char *what)
{
	struct score sc;
	int d, at = -1, fa = 0, f2 = 0;
	double prec, rec, f1, acc;
	char w[200];

	printf("-- %s\n", what);
	sn_init(&S);
	memset(rx, 0, sizeof(rx));
	for (d = 0; d < 10; d++)
		day(d, -1, with_phone, NULL, NULL, NULL);
	CHECK(S.days >= 9, "learned %u days", S.days);
	S.frozen = 1;                   /* the held-out days: nothing learned from them */
	memset(&sc, 0, sizeof(sc));
	for (d = 10; d < 14; d++)
		day(d, -1, with_phone, &sc, NULL, &fa);
	acc = (double)sc.pres_ok / (double)sc.pres_n;
	prec = sc.tp + sc.fp ? (double)sc.tp / (double)(sc.tp + sc.fp) : 0;
	rec = sc.tp + sc.fn ? (double)sc.tp / (double)(sc.tp + sc.fn) : 0;
	f1 = prec + rec ? 2 * prec * rec / (prec + rec) : 0;
	printf("     held-out days 10-13: presence %.1f%% of windows; movement per zone: precision %.2f, recall %.2f, F1 %.2f\n",
	       100 * acc, prec, rec, f1);
	CHECK(acc >= (with_phone ? 0.95 : 0.80), "presence on held-out days: %.1f%%", 100 * acc);
	CHECK(f1 >= 0.70, "movement per zone on held-out days: F1 %.2f", f1);
	CHECK(fa == 0, "no unusual-inactivity alarm on 4 usual held-out days (%d)", fa);
	sn_activity_words(&S, 11, &P, w, sizeof(w));
	printf("     %s\n", w);

	/* the check-in: a weekend day the person stops moving at 10:00 */
	S.inactive = 0;
	day(19, 600, with_phone, NULL, &at, &f2);
	CHECK(at >= 600 && at <= 600 + 4 * 60, "unusual inactivity found %d min after it began", at < 0 ? -1 : at - 600);
	sn_activity_words(&S, 13, &P, w, sizeof(w));
	printf("     %s\n", w);
	CHECK(S.inactive && strstr(w, "No movement"), "the activity check says so");
}

static void test_basics(void)
{
	long t = 1800000000L;
	int k, z = -1, ev = 0;

	sn_init(&S);
	/* stale readings (no new frames) are not used */
	for (k = 0; k < 6; k++)
		sn_look(&S, "02:00:00:00:52:01", 5, sn_zone(&S, "office"), 0, -60 + (k % 2) * 9, 7, t + k);
	z = sn_zone(&S, "office");
	ev = sn_window(&S, t + 30, 10, 20000, &P);
	CHECK(z == 0 && S.l[0].nbuf == 0 && !S.z[0].moving && !ev, "no new frames: nothing judged, nothing moves");
	CHECK(sn_zone(&S, "") == sn_zone(&S, "home") && S.nz == 2, "zones by name; none: 'home'");
	for (k = 0; k < SN_ZONES + 2; k++)
		sn_zone(&S, k == 0 ? "a" : k == 1 ? "b" : k == 2 ? "c" : k == 3 ? "d" : k == 4 ? "e" : k == 5 ? "f" : k == 6 ? "g" : "h");
	CHECK(S.nz == SN_ZONES && sn_zone(&S, "one-more") == -1, "at most %d zones", SN_ZONES);
	CHECK(!strcmp(sn_activity(&S, 10, &P), "Learning"), "the activity check learns first");
}

int main(void)
{
	double us;

	sn_params_default(&P);
	test_basics();
	run(1, "a home with a phone of the person's own");
	run(0, "someone living alone without a phone");
	us = look_ns / (double)looks / 1000.0;
	printf("     budget: %zu bytes of state, %.2f us a look on this machine (%ld looks)\n", sizeof(struct sn_state), us,
	       looks);
	CHECK(sizeof(struct sn_state) <= 4096, "RAM: %zu bytes of state (budget 4096)", sizeof(struct sn_state));
	CHECK(us < 5.0, "CPU: %.2f us a look here (budget 5)", us);
	printf("%s\n", failed ? "SOME TESTS FAILED" : "all sensing tests passed");
	return failed ? 1 : 0;
}
