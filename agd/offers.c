/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW signals for the ISP (the rules: core/libag/offer.c): each day of the
 * home, counted from the timeline's samples (agd/why.c):
 *   - how long the line was used to its speed, of it in the evening, and while
 *     the internet answered slowly meanwhile (the rest of the home suffered)
 *   - the busiest sample's download
 *   - each device's time on a weak Wi-Fi signal (the day's 16 weakest)
 *   - short drops: a device off Wi-Fi and back within 2 minutes
 * kept for 31 days with agd's state. Judged every hour with proactive care's
 * days: a faster plan, smart queueing, mesh Wi-Fi, a customer who may leave, a
 * line that needs repair. A signal that starts is told to the ISP once
 * (Device.X_AIGW_.OfferFound!), and every signal is in the data model
 * (Device.X_AIGW_.Offers.) and in what the bridge sends cortex, in words
 * without names: device names stay home (agctl offers).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libubox/blobmsg.h>

#include "ag/care.h"
#include "ag/offer.h"
#include "ag/why.h"
#include "agd.h"

#define DROP_S   120                    /* off Wi-Fi and back within this: a drop, not leaving */
#define JUDGE_S  3600
#define LEAVES   32

static struct offer_day days[OFFER_DAYS];
static struct offer_result res;
static long judged_at;
static long told[OF_N];                 /* the signal's since, when it was told (0: not active) */
static int loaded;
static int untold;                      /* a signal started while agd wasn't on ubus yet */

static struct {
	uint8_t mac[6];
	long at;
} leaves[LEAVES];
static unsigned leave_head;

static const char *const dm_names[OF_N] = { "PlanUpgrade", "SmartQueue", "MeshWiFi", "Retention", "LineRepair" };

static int mac_bytes(const char *s, uint8_t m[6])
{
	unsigned v[6];
	int i;

	if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++)
		m[i] = (uint8_t)v[i];
	return 0;
}

static struct offer_day *today(long now)
{
	long day = now / 86400;
	struct offer_day *d = &days[day % OFFER_DAYS];

	if (d->day != day) {
		memset(d, 0, sizeof(*d));
		d->day = day;
	}
	return d;
}

static float line_mbps(int *sold)
{
	double mbps = 0;

	*sold = cfg.care_speed_mbps > 0;
	if (cfg.care_speed_mbps > 0)
		return (float)cfg.care_speed_mbps;
	if (care_speed_last(&mbps) > 0 && mbps > 0)
		return (float)mbps;
	return 0;
}

void offers_on_sample(const struct why_min *m, long secs, float peak_down, float peak_up)
{
	struct offer_day *d;
	time_t t = (time_t)m->t;
	struct tm tm;
	float n = 0;
	int sold, have, hour, slow;
	float speed = line_mbps(&sold);

	if (!cfg.offers || secs <= 0)
		return;
	if (secs > 3L * cfg.why_interval)
		secs = cfg.why_interval;
	d = today((long)m->t);
	localtime_r(&t, &tm);
	hour = tm.tm_hour;
	d->watched_s += (uint32_t)secs;
	if (m->down_kbps > d->peak_kbps)
		d->peak_kbps = m->down_kbps;
	if (!m->route || !why_full(m, speed, peak_down, peak_up))
		return;
	have = care_normal_hour(CM_INET_MS, hour, &n) == 0;
	slow = m->in_ms != WHY_NONE && m->in_sent && m->in_lost < m->in_sent && m->in_ms >= why_slow_ms(n, have);
	/* the speed not known: the most seen is only a guess at the line's limit (a quiet home's is far below it); full
	   only when the internet slowed too, as on the timeline (ag/why.h) - no bigger plan for a line that isn't full */
	if (speed <= 0 && !slow)
		return;
	d->full_s += (uint32_t)secs;
	if (hour >= 18)
		d->full_evening_s += (uint32_t)secs;
	if (slow)
		d->full_slow_s += (uint32_t)secs;
}

void offers_on_weak(const char *mac, long secs)
{
	uint8_t m[6];

	if (cfg.offers && secs > 0 && mac_bytes(mac, m) == 0)
		offer_day_weak(today((long)ag_wall_s()), m, (uint32_t)(secs > 3L * cfg.why_interval ? cfg.why_interval : secs));
}

void offers_on_wifi(const char *mac, int joined)
{
	uint8_t m[6];
	long now = (long)ag_wall_s();
	unsigned i;

	if (!cfg.offers || mac_bytes(mac, m) < 0)
		return;
	if (!joined) {
		memcpy(leaves[leave_head].mac, m, 6);
		leaves[leave_head].at = now;
		leave_head = (leave_head + 1) % LEAVES;
		return;
	}
	for (i = 0; i < LEAVES; i++)
		if (leaves[i].at && !memcmp(leaves[i].mac, m, 6)) {
			if (now - leaves[i].at <= DROP_S)
				today(now)->drops++;
			leaves[i].at = 0;
		}
}

static const char *name_cb(const uint8_t mac[6], void *ctx)
{
	char s[18];
	struct device *d;

	(void)ctx;
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	d = dev_find_mac(s, 0);
	return d && d->hostname[0] ? d->hostname : NULL;
}

static void judge(long now)
{
	static struct care_day cd[CARE_DAYS];
	struct offer_params p;
	struct offer_in in;
	int k;

	memset(&in, 0, sizeof(in));
	in.today = now / 86400;
	in.d = days;
	in.nd = OFFER_DAYS;
	in.nc = care_days_get(cd, CARE_DAYS);
	in.c = cd;
	in.line_mbps = line_mbps(&in.line_sold);
	in.name = name_cb;
	in.sqm_on = sqm_on();
	{
		static char why[200];
		int h = home_extender_helps(why, sizeof(why));     /* the home model: is an extender worth selling */

		in.extender = h < 0 ? 0 : h ? 1 : -1;
		in.extender_why = why;
	}
	/* proven repair: the week's fixes AIGW made itself, proven to have helped (all; Wi-Fi's own) */
	repair_week(0, &in.proven_fixes, &in.proven_judged);
	repair_week(1, &in.wifi_fixes, &in.wifi_judged);
	offer_params_default(&p);
	offer_judge(&in, &p, &res);
	judged_at = now;
	untold = 0;
	for (k = 0; k < OF_N; k++) {
		const struct offer *o = &res.o[k];

		if (!o->active) {
			told[k] = 0;
			continue;
		}
		if (told[k])
			continue;
		if (!agd_connected()) {
			untold = 1;
			continue;
		}
		told[k] = o->since ? o->since : now;
		AG_INFO("offers: %s (%s, %s confidence): %s", o->code, o->product, offer_confidence_name(o->confidence),
			o->evidence);
		if (cfg.offers_share)
			dm_on_offer(o->code, o->kind, o->product, offer_confidence_name(o->confidence), o->evidence);
	}
}

void offers_tick(void)
{
	long now = (long)ag_wall_s();

	if (cfg.offers && (!judged_at || now - judged_at >= JUDGE_S || now / 86400 != judged_at / 86400 ||
			   (untold && agd_connected())))
		judge(now);
}

static void fresh(void)
{
	if (cfg.offers && (!judged_at || (long)ag_wall_s() - judged_at >= 60))
		judge((long)ag_wall_s());
}

/* the signals (names: the devices by name, for the home's own view only) */
/* proven repair's week, as the signals count it: fixes AIGW made itself that measurably helped */
static void proven_blob(struct blob_buf *b)
{
	unsigned h, j, wh, wj;
	void *t;

	repair_week(0, &h, &j);
	repair_week(1, &wh, &wj);
	t = blobmsg_open_table(b, "proven_fixes");
	blobmsg_add_u32(b, "helped", h);
	blobmsg_add_u32(b, "judged", j);
	blobmsg_add_u32(b, "wifi_helped", wh);
	blobmsg_add_u32(b, "wifi_judged", wj);
	blobmsg_close_table(b, t);
}

void offers_blob(struct blob_buf *b, int names)
{
	void *a, *t;
	int k;

	fresh();
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.offers);
	blobmsg_add_u8(b, "shared", (uint8_t)cfg.offers_share);
	blobmsg_add_u32(b, "days_watched", (uint32_t)res.watched);
	blobmsg_add_u8(b, "learning", (uint8_t)res.learning);
	{
		const struct offer_day *d = today((long)ag_wall_s());   /* so far today, as counted */

		t = blobmsg_open_table(b, "today");
		blobmsg_add_u32(b, "watched_s", d->watched_s);
		blobmsg_add_u32(b, "full_s", d->full_s);
		blobmsg_add_u32(b, "full_evening_s", d->full_evening_s);
		blobmsg_add_u32(b, "full_slow_s", d->full_slow_s);
		blobmsg_add_u32(b, "peak_kbps", d->peak_kbps);
		blobmsg_add_u32(b, "weak_s", d->weak_s);
		blobmsg_add_u32(b, "drops", d->drops);
		blobmsg_close_table(b, t);
	}
	a = blobmsg_open_array(b, "signals");
	for (k = 0; k < OF_N; k++) {
		const struct offer *o = &res.o[k];

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "code", o->code);
		blobmsg_add_string(b, "kind", o->kind);
		blobmsg_add_string(b, "product", o->product);
		blobmsg_add_u8(b, "active", (uint8_t)o->active);
		if (o->active) {
			blobmsg_add_string(b, "confidence", offer_confidence_name(o->confidence));
			blobmsg_add_u32(b, "days", (uint32_t)o->days);
			blobmsg_add_u32(b, "of_days", (uint32_t)o->of_days);
			blobmsg_add_u64(b, "since", (uint64_t)o->since);
			blobmsg_add_string(b, "evidence", o->evidence);
			if (names && o->named[0])
				blobmsg_add_string(b, "devices", o->named);
		}
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	proven_blob(b);
}

/* for cortex (inside proactive care's report): the active ones, without names; nothing unless shared */
void offers_care_blob(struct blob_buf *b)
{
	void *a, *t;
	int k;

	if (!cfg.offers || !cfg.offers_share)
		return;
	fresh();
	a = blobmsg_open_array(b, "offers");
	for (k = 0; k < OF_N; k++) {
		const struct offer *o = &res.o[k];

		if (!o->active)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "code", o->code);
		blobmsg_add_string(b, "kind", o->kind);
		blobmsg_add_string(b, "product", o->product);
		blobmsg_add_string(b, "confidence", offer_confidence_name(o->confidence));
		blobmsg_add_u32(b, "days", (uint32_t)o->days);
		blobmsg_add_u32(b, "of_days", (uint32_t)o->of_days);
		blobmsg_add_u64(b, "since", (uint64_t)o->since);
		blobmsg_add_string(b, "evidence", o->evidence);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	proven_blob(b);
}

/* Device.X_AIGW_.Offers.: Enable, Shared, Signals; <Name>.Active, Confidence, Days, Since, Evidence */
int offers_field(const char *f, char *buf, size_t n)
{
	const char *dot;
	int k;

	fresh();
	if (!strcmp(f, "Enable")) {
		snprintf(buf, n, "%s", cfg.offers ? "true" : "false");
		return 0;
	}
	if (!strcmp(f, "Shared")) {
		snprintf(buf, n, "%s", cfg.offers_share ? "true" : "false");
		return 0;
	}
	if (!strcmp(f, "Signals")) {
		size_t l = 0;

		buf[0] = '\0';
		for (k = 0; k < OF_N && cfg.offers_share; k++)
			if (res.o[k].active && l < n)
				l += (size_t)snprintf(buf + l, n - l, "%s%s", l ? "," : "", res.o[k].code);
		return 0;
	}
	if (!(dot = strchr(f, '.')))
		return -1;
	for (k = 0; k < OF_N; k++)
		if (!strncmp(f, dm_names[k], (size_t)(dot - f)) && strlen(dm_names[k]) == (size_t)(dot - f))
			break;
	if (k == OF_N)
		return -1;
	{
		const struct offer *o = &res.o[k];
		int on = o->active && cfg.offers_share;

		dot++;
		if (!strcmp(dot, "Active"))
			snprintf(buf, n, "%s", on ? "true" : "false");
		else if (!strcmp(dot, "Confidence"))
			snprintf(buf, n, "%s", on ? offer_confidence_name(o->confidence) : "");
		else if (!strcmp(dot, "Days"))
			snprintf(buf, n, "%d", on ? o->days : 0);
		else if (!strcmp(dot, "Since"))
			snprintf(buf, n, "%ld", on ? o->since : 0L);
		else if (!strcmp(dot, "Evidence"))
			snprintf(buf, n, "%s", on ? o->evidence : "");
		else
			return -1;
	}
	return 0;
}

/* ---------- saved with agd's state ---------- */

void offers_save(struct blob_buf *b)
{
	void *t, *a, *r, *w, *x;
	int i, j, k;

	t = blobmsg_open_table(b, "offers");
	a = blobmsg_open_array(b, "days");
	for (i = 0; i < OFFER_DAYS; i++) {
		const struct offer_day *d = &days[i];

		if (!d->day)
			continue;
		r = blobmsg_open_table(b, NULL);
		blobmsg_add_u64(b, "day", (uint64_t)d->day);
		blobmsg_add_u32(b, "watched_s", d->watched_s);
		blobmsg_add_u32(b, "full_s", d->full_s);
		blobmsg_add_u32(b, "full_evening_s", d->full_evening_s);
		blobmsg_add_u32(b, "full_slow_s", d->full_slow_s);
		blobmsg_add_u32(b, "peak_kbps", d->peak_kbps);
		blobmsg_add_u32(b, "drops", d->drops);
		blobmsg_add_u32(b, "weak_s", d->weak_s);
		w = blobmsg_open_array(b, "weak");
		for (j = 0; j < OFFER_WEAK; j++) {
			char mac[18];

			if (!d->weak[j].secs)
				continue;
			x = blobmsg_open_table(b, NULL);
			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", d->weak[j].mac[0], d->weak[j].mac[1],
				 d->weak[j].mac[2], d->weak[j].mac[3], d->weak[j].mac[4], d->weak[j].mac[5]);
			blobmsg_add_string(b, "mac", mac);
			blobmsg_add_u32(b, "s", d->weak[j].secs);
			blobmsg_close_table(b, x);
		}
		blobmsg_close_array(b, w);
		blobmsg_close_table(b, r);
	}
	blobmsg_close_array(b, a);
	r = blobmsg_open_table(b, "told");
	for (k = 0; k < OF_N; k++)
		if (told[k])
			blobmsg_add_u64(b, offer_code(k), (uint64_t)told[k]);
	blobmsg_close_table(b, r);
	blobmsg_close_table(b, t);
}

static long jnum(struct blob_attr *a)
{
	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (long)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (long)blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	}
	return 0;
}

static struct blob_attr *get(struct blob_attr *t, const char *k)
{
	struct blob_attr *c;
	size_t rem;

	blobmsg_for_each_attr(c, t, rem)
		if (!strcmp(blobmsg_name(c), k))
			return c;
	return NULL;
}

void offers_load(struct blob_attr *t)
{
	struct blob_attr *a = get(t, "days"), *r, *w, *x, *tl = get(t, "told");
	size_t rem, r2;
	int k;

	loaded = 1;
	if (a && blobmsg_type(a) == BLOBMSG_TYPE_ARRAY)
		blobmsg_for_each_attr(r, a, rem) {
			long day = jnum(get(r, "day"));
			struct offer_day *d;

			if (day <= 0)
				continue;
			d = &days[day % OFFER_DAYS];
			if (d->day > day)
				continue;
			memset(d, 0, sizeof(*d));
			d->day = day;
			d->watched_s = (uint32_t)jnum(get(r, "watched_s"));
			d->full_s = (uint32_t)jnum(get(r, "full_s"));
			d->full_evening_s = (uint32_t)jnum(get(r, "full_evening_s"));
			d->full_slow_s = (uint32_t)jnum(get(r, "full_slow_s"));
			d->peak_kbps = (uint32_t)jnum(get(r, "peak_kbps"));
			d->drops = (uint32_t)jnum(get(r, "drops"));
			if ((w = get(r, "weak")) && blobmsg_type(w) == BLOBMSG_TYPE_ARRAY)
				blobmsg_for_each_attr(x, w, r2) {
					uint8_t m[6];
					struct blob_attr *ma = get(x, "mac");

					if (ma && blobmsg_type(ma) == BLOBMSG_TYPE_STRING && mac_bytes(blobmsg_get_string(ma), m) == 0)
						offer_day_weak(d, m, (uint32_t)jnum(get(x, "s")));
				}
			d->weak_s = (uint32_t)jnum(get(r, "weak_s"));
		}
	if (tl && blobmsg_type(tl) == BLOBMSG_TYPE_TABLE)
		for (k = 0; k < OF_N; k++)
			told[k] = jnum(get(tl, offer_code(k)));
}

void offers_init(void)
{
	if (!cfg.offers)
		return;
	AG_INFO("offers: signals for the ISP from the last 7 days%s%s", cfg.offers_share ? "" : " (kept home: offers.share 0)",
		loaded ? ", days restored" : "");
}
