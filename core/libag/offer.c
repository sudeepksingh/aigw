/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW signals for the ISP: the rules (see ag/offer.h). A signal needs the
 * same thing on several days of the last week, so one busy evening isn't a
 * sale and one bad day isn't a customer about to leave. Its confidence says
 * how sure the gateway is: the line's sold speed known, how many days.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/offer.h"

void offer_params_default(struct offer_params *p)
{
	memset(p, 0, sizeof(*p));
	p->days = 7;
	p->min_days = 3;
	p->watched_min_s = 6 * 3600;
	p->plan_full_s = 1800;
	p->plan_days = 3;
	p->sqm_slow_s = 900;
	p->sqm_pct = 50;
	p->sqm_days = 3;
	p->mesh_dev_s = 1800;
	p->mesh_devices = 2;
	p->mesh_drops = 10;
	p->mesh_days = 3;
	p->churn_score = 90;
	p->churn_drop = 5;
	p->churn_problems = 4;
	p->repair_low = 2;
}

const char *offer_confidence_name(int c)
{
	return c == OC_HIGH ? "high" : c == OC_MEDIUM ? "medium" : "low";
}

const char *offer_code(int k)
{
	static const char *const n[OF_N] = { "PLAN_UPGRADE", "SMART_QUEUE", "MESH_WIFI", "RETENTION_RISK", "LINE_REPAIR" };

	return k >= 0 && k < OF_N ? n[k] : "?";
}

void offer_day_weak(struct offer_day *d, const uint8_t mac[6], uint32_t secs)
{
	int i, low = 0;

	d->weak_s += secs;
	for (i = 0; i < OFFER_WEAK; i++)
		if (d->weak[i].secs && !memcmp(d->weak[i].mac, mac, 6)) {
			d->weak[i].secs += secs;
			return;
		}
	for (i = 0; i < OFFER_WEAK; i++) {
		if (!d->weak[i].secs) {
			low = i;
			break;
		}
		if (d->weak[i].secs < d->weak[low].secs)
			low = i;
	}
	if (d->weak[low].secs && d->weak[low].secs >= secs)
		return;                         /* full, and all weaker longer than this one */
	memcpy(d->weak[low].mac, mac, 6);
	d->weak[low].secs = secs;
}

static void dur(long s, char *buf, size_t n)
{
	if (s < 3600)
		snprintf(buf, n, "%ld min", (s + 30) / 60);
	else if (s % 3600 < 300)
		snprintf(buf, n, "%ld h", s / 3600);
	else
		snprintf(buf, n, "%ld h %ld min", s / 3600, (s % 3600) / 60);
}

static void set(struct offer *o, int k, const char *kind, const char *product)
{
	memset(o, 0, sizeof(*o));
	o->code = offer_code(k);
	o->kind = kind;
	o->product = product;
}

/* the window's days, newest first, and how many were watched */
static int window(const struct offer_in *in, const struct offer_params *p, const struct offer_day **w)
{
	int i, k = 0;

	for (i = 0; i < in->nd; i++) {
		const struct offer_day *d = &in->d[i];

		if (d->day && d->day <= in->today && d->day > in->today - p->days && d->watched_s >= p->watched_min_s)
			w[k++] = d;
	}
	/* newest first */
	for (i = 1; i < k; i++) {
		int j;

		for (j = i; j > 0 && w[j]->day > w[j - 1]->day; j--) {
			const struct offer_day *t = w[j];

			w[j] = w[j - 1];
			w[j - 1] = t;
		}
	}
	return k;
}

static void care_sum(const struct offer_in *in, long first, long last, struct care_day *s, int *days)
{
	int i, a;

	memset(s, 0, sizeof(*s));
	*days = 0;
	for (i = 0; i < in->nc; i++) {
		const struct care_day *c = &in->c[i];

		if (!c->day || c->day < first || c->day > last)
			continue;
		(*days)++;
		s->checks += c->checks;
		s->good += c->good;
		for (a = 0; a < CARE_AREAS; a++)
			s->bad[a] += c->bad[a];
		s->problems += c->problems;
		s->reported += c->reported;
		s->watched_secs += c->watched_secs;
		s->ok_secs += c->ok_secs;
		s->speed_tests += c->speed_tests;
		s->speed_low += c->speed_low;
	}
}

static void plan_and_sqm(const struct offer_in *in, const struct offer_params *p, const struct offer_day **w, int n,
			 int repair, struct offer_result *out)
{
	struct offer *pl = &out->o[OF_PLAN], *sq = &out->o[OF_SQM];
	long full = 0, eve = 0, slow = 0, since = 0, ssince = 0;
	uint32_t peak = 0;
	int i, k = 0, ks = 0;
	char d1[40], d2[24];

	for (i = 0; i < n; i++) {
		const struct offer_day *d = w[i];

		if (d->full_s >= p->plan_full_s) {
			k++;
			full += d->full_s, eve += d->full_evening_s;
			since = d->day;
			if (d->peak_kbps > peak)
				peak = d->peak_kbps;
		}
		if (d->full_slow_s >= p->sqm_slow_s && d->full_s && d->full_slow_s * 100 >= d->full_s * (uint32_t)p->sqm_pct) {
			ks++;
			slow += d->full_slow_s;
			ssince = d->day;
		}
	}
	if (k >= p->plan_days && !repair) {
		pl->active = 1;
		pl->days = k;
		pl->of_days = n;
		pl->since = since * 86400;
		pl->confidence = in->line_sold && k >= 4 ? OC_HIGH : in->line_mbps > 0 ? OC_MEDIUM : OC_LOW;
		dur(full / k, d1, sizeof(d1));
		if (in->line_mbps > 0)
			snprintf(d2, sizeof(d2), " of %.0f", in->line_mbps);
		else
			d2[0] = '\0';
		snprintf(pl->evidence, sizeof(pl->evidence),
			 "The line was used to its speed on %d of the last %d days, %s a day on average, %ld%% of it in the "
			 "evening; up to %.0f%s Mbit/s.%s", k, n, d1, full ? eve * 100 / full : 0, peak / 1000.0, d2,
			 in->line_mbps > 0 ? "" : " (The line's speed isn't known: full is the most seen on it.)");
	}
	if (ks >= p->sqm_days && !in->sqm_on) {
		sq->active = 1;
		sq->days = ks;
		sq->of_days = n;
		sq->since = ssince * 86400;
		sq->confidence = ks >= 5 ? OC_HIGH : OC_MEDIUM;
		dur(slow / ks, d1, sizeof(d1));
		snprintf(sq->evidence, sizeof(sq->evidence),
			 "On %d of the last %d days everything else slowed down while the line was full, %s a day: calls, "
			 "games and browsing suffer while someone downloads. Smart queueing keeps them responsive without a "
			 "faster plan.", ks, n, d1);
	}
}

struct dev_sum {
	uint8_t mac[6];
	uint32_t secs;
	int days;
};

static void mesh(const struct offer_in *in, const struct offer_params *p, const struct offer_day **w, int n,
		 struct offer_result *out)
{
	struct offer *o = &out->o[OF_MESH];
	struct dev_sum s[OFFER_WEAK * 7];
	int ns = 0, i, j, k = 0, devs = 0, nk = 0;
	long since = 0, drops = 0;
	uint64_t weak = 0;
	char d1[40];
	size_t l = 0;

	for (i = 0; i < n; i++) {
		const struct offer_day *d = w[i];
		int m = 0;

		for (j = 0; j < OFFER_WEAK; j++)
			if (d->weak[j].secs >= p->mesh_dev_s)
				m++;
		if (m >= p->mesh_devices || (m >= 1 && d->drops >= p->mesh_drops)) {
			k++;
			since = d->day;
			devs += m;
			drops += d->drops;
			weak += d->weak_s;
			for (j = 0; j < OFFER_WEAK; j++) {
				int x;

				if (d->weak[j].secs < p->mesh_dev_s)
					continue;
				for (x = 0; x < ns && memcmp(s[x].mac, d->weak[j].mac, 6); x++)
					;
				if (x == ns) {
					if (ns >= (int)(sizeof(s) / sizeof(s[0])))
						continue;
					memset(&s[ns], 0, sizeof(s[ns]));
					memcpy(s[ns++].mac, d->weak[j].mac, 6);
				}
				s[x].secs += d->weak[j].secs;
				s[x].days++;
			}
		}
	}
	if (k < p->mesh_days)
		return;
	/* the home model says an extender would not fix it (a free change would, or nothing to fix): no sale */
	if (in->extender < 0) {
		snprintf(o->evidence, sizeof(o->evidence), "Held: %s.", in->extender_why ? in->extender_why : "the home model "
			 "expects no extender to help");
		return;
	}
	o->active = 1;
	o->days = k;
	o->of_days = n;
	o->since = since * 86400;
	o->confidence = k >= 5 && ns >= 2 ? OC_HIGH : OC_MEDIUM;
	dur((long)(weak / (uint64_t)k), d1, sizeof(d1));
	snprintf(o->evidence, sizeof(o->evidence),
		 "On %d of the last %d days, %d device%s spent a long time on a weak Wi-Fi signal (%s of device time a day)%s"
		 "%ld short drop%s off Wi-Fi a day: the access point doesn't reach every room.", k, n, ns, ns == 1 ? "" : "s",
		 d1, drops ? "; " : ", and ", drops / k, drops / k == 1 ? "" : "s");
	if (in->extender == 1 && in->extender_why && *in->extender_why) {
		size_t e = strlen(o->evidence);

		snprintf(o->evidence + e, sizeof(o->evidence) - e, " Before you buy: %s.", in->extender_why);
		o->confidence = OC_HIGH;
	}
	(void)devs;
	/* the weakest, by name, for the home's own view */
	for (i = 0; i < ns; i++)
		for (j = i + 1; j < ns; j++)
			if (s[j].secs > s[i].secs) {
				struct dev_sum t = s[i];

				s[i] = s[j];
				s[j] = t;
			}
	for (i = 0; i < ns && nk < 4 && l < sizeof(o->named); i++, nk++) {
		const char *nm = in->name ? in->name(s[i].mac, in->ctx) : NULL;
		char mac[18];

		if (!nm || !*nm) {
			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", s[i].mac[0], s[i].mac[1], s[i].mac[2],
				 s[i].mac[3], s[i].mac[4], s[i].mac[5]);
			nm = mac;
		}
		dur(s[i].secs / (uint32_t)s[i].days, d1, sizeof(d1));
		l += (size_t)snprintf(o->named + l, sizeof(o->named) - l, "%s%s %s a day", i ? ", " : "", nm, d1);
	}
	if (l >= sizeof(o->named))
		o->named[sizeof(o->named) - 1] = '\0';
}

static void retain_and_repair(const struct offer_in *in, const struct offer_params *p, int n, struct offer_result *out,
			      int *repair)
{
	struct offer *r = &out->o[OF_RETAIN], *f = &out->o[OF_REPAIR];
	struct care_day now, before;
	int dn, db, sn, sb, why = 0, main = -1;
	char reasons[200] = "";
	size_t l = 0;

	*repair = 0;
	care_sum(in, in->today - p->days + 1, in->today, &now, &dn);
	care_sum(in, in->today - 2 * p->days + 1, in->today - p->days, &before, &db);
	if (now.speed_low >= p->repair_low) {
		*repair = 1;
		f->active = 1;
		f->days = (int)now.speed_low;
		f->of_days = (int)now.speed_tests;
		f->since = (in->today - p->days + 1) * 86400;
		f->confidence = now.speed_low >= 3 ? OC_HIGH : OC_MEDIUM;
		snprintf(f->evidence, sizeof(f->evidence),
			 "%u of the last %u nightly speed tests were far below the line's speed: the line needs repair "
			 "before anything is sold on it.", now.speed_low, now.speed_tests);
	}
	if (dn < p->min_days || !now.watched_secs)
		return;
	sn = care_score(&now);
	sb = db >= p->min_days ? care_score(&before) : -1;
	if (sn >= 0 && sn < p->churn_score) {
		why++;
		l += (size_t)snprintf(reasons + l, sizeof(reasons) - l, "the experience was %d%% over the last %d days", sn, dn);
	}
	if (sn >= 0 && sb >= 0 && sb - sn >= p->churn_drop) {
		why++;
		l += (size_t)snprintf(reasons + l, sizeof(reasons) - l, "%s%d points below the %d days before (%d%%)",
				      l ? ", " : "the experience was ", sb - sn, db, sb);
	}
	if (now.problems >= p->churn_problems && l < sizeof(reasons)) {
		why++;
		l += (size_t)snprintf(reasons + l, sizeof(reasons) - l, "%s%u problems, %u reported to the ISP", l ? "; " : "",
				      now.problems, now.reported);
	}
	if (!why)
		return;
	main = care_main_cause(&now);
	r->active = 1;
	r->days = dn;
	r->of_days = n > dn ? n : dn;
	r->since = (in->today - p->days + 1) * 86400;
	r->confidence = why >= 2 ? OC_HIGH : OC_MEDIUM;
	snprintf(r->evidence, sizeof(r->evidence), "A bad week for this home: %s%s%s. Call before the customer does.",
		 reasons, main >= 0 ? "; mostly " : "", main >= 0 ? care_area_name(main) : "");
}

void offer_judge(const struct offer_in *in, const struct offer_params *p, struct offer_result *out)
{
	const struct offer_day *w[OFFER_DAYS];
	int n, repair;

	memset(out, 0, sizeof(*out));
	set(&out->o[OF_PLAN], OF_PLAN, "upsell", "faster plan");
	set(&out->o[OF_SQM], OF_SQM, "upsell", "smart queueing");
	set(&out->o[OF_MESH], OF_MESH, "upsell", "mesh Wi-Fi");
	set(&out->o[OF_RETAIN], OF_RETAIN, "retention", "a call from the ISP");
	set(&out->o[OF_REPAIR], OF_REPAIR, "repair", "a line repair");
	n = window(in, p, w);
	out->watched = n;
	retain_and_repair(in, p, n, out, &repair);
	if (n < p->min_days) {
		out->learning = 1;              /* LINE_REPAIR and RETENTION_RISK come from care's own days */
		return;
	}
	plan_and_sqm(in, p, w, n, repair, out);
	mesh(in, p, w, n, out);
}
