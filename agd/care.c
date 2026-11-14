/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW proactive care: the gateway runs Diagnose by itself (care.interval,
 * sooner when Self-Healing Wi-Fi turns a radio Poor) and acts on what it
 * finds before anyone calls (the rules: core/libag/care.c):
 *   - a problem on the line, the ISP's network or DNS: reported to the ISP
 *     at once (Device.X_AIGW_.ProblemDetected!), and when it is over
 *     (ProblemCleared!)
 *   - a Wi-Fi problem: Self-Healing Wi-Fi's to fix; reported only if it
 *     lasts care.wifi_grace
 * and keeps a daily experience score: the share of checks without a problem,
 * with the main cause on bad days (agctl care, Device.X_AIGW_.Care.).
 */
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "agd.h"

#define BUSY_RETRY_MS 15000             /* a diagnosis someone asked for is running */
#define POKE_MIN_S    120               /* a radio turning Poor checks now, at most this often */

static struct care_state st;
static struct care_report rep;
static struct care_params par;
static struct uloop_timeout timer;
static int running;
static long next_at, last_poke;

static void schedule(int secs)
{
	next_at = (long)ag_wall_s() + secs;
	uloop_timeout_set(&timer, secs * 1000);
}

/* the next check: soon while something is wrong (to confirm it, then to see it clear) */
static int next_secs(void)
{
	return st.active || st.bad_run ? cfg.care_recheck : cfg.care_interval;
}

static void tell(const char *event)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", event);
	blobmsg_add_string(&b, "code", st.code);
	blobmsg_add_string(&b, "area", st.area);
	blobmsg_add_string(&b, "severity", diag_sev_name(st.sev));
	blobmsg_add_string(&b, "summary", st.text);
	blobmsg_add_u64(&b, "since", (uint64_t)st.since);
	if (!strcmp(event, "cleared"))
		blobmsg_add_u64(&b, "duration_s", (uint64_t)st.duration);
	agd_emit(AG_EV_CARE, &b);
	blob_buf_free(&b);
}

static void done(void *ctx, const struct diag_result *r, const struct diag_input *in)
{
	const struct diag_finding *f = r->n ? &r->f[0] : NULL;
	struct care_verdict v = { f ? f->code : "OK", f ? f->area : "All", f ? f->sev : DS_OK, f ? f->text : "" };
	int ev;

	(void)ctx;
	(void)in;
	running = 0;
	par.wifi_grace = cfg.care_wifi_grace;
	par.max_gap = 3L * (cfg.care_interval > cfg.care_recheck ? cfg.care_interval : cfg.care_recheck);
	ev = care_step(&st, &rep, &v, (long)ag_wall_s(), &par);
	if (ev & CARE_STARTED) {
		AG_INFO("care: problem found: %s (%s) -- %s; %s", st.code, st.area, st.text,
			care_isp_owned(st.area) ? "reporting it to the ISP" :
			care_area_index(st.area) == CA_WIFI ? "Self-Healing Wi-Fi is on it" : "not the ISP's");
		tell("found");
	}
	if (ev & CARE_CHANGED) {
		AG_INFO("care: the problem is now %s (%s) -- %s", st.code, st.area, st.text);
		if (st.reported && !(ev & CARE_REPORT))
			dm_on_care(1);          /* the ISP was told: tell it the new cause */
		tell("changed");
	}
	if (ev & CARE_REPORT) {
		AG_INFO("care: reported to the ISP: %s", st.code);
		dm_on_care(1);
		tell("reported");
	}
	if (ev & CARE_CLEARED) {
		AG_INFO("care: %s is over after %lds%s", st.code, st.duration,
			st.reported ? "" : care_area_index(st.area) == CA_WIFI ? " (fixed by AIGW, not reported)" : "");
		if (st.reported)
			dm_on_care(0);
		tell("cleared");
		st.reported = 0;
	}
	schedule(next_secs());
}

static void check(struct uloop_timeout *t)
{
	(void)t;
	if (running)
		return;
	if (diag_check(done, NULL) < 0) {
		uloop_timeout_set(&timer, BUSY_RETRY_MS);
		return;
	}
	running = 1;
}

void care_init(void)
{
	care_params_default(&par);
	par.wifi_grace = cfg.care_wifi_grace;
	timer.cb = check;
	if (!cfg.care) {
		AG_INFO("care: off in config (care.enabled)");
		return;
	}
	schedule(30);                   /* the first check, once Wi-Fi and the line were seen */
	AG_INFO("care: checking the home every %ds (problems on the line and DNS reported at once, Wi-Fi after %ds)",
		cfg.care_interval, cfg.care_wifi_grace);
}

/* heal.c: a radio's verdict changed; turning Poor is worth a check now */
void care_on_heal(int status)
{
	long now = (long)ag_wall_s();

	if (!cfg.care || running || status != HEAL_POOR || now - last_poke < POKE_MIN_S)
		return;
	last_poke = now;
	schedule(5);
}

/* agctl care --check, and the tests */
int care_check_now(void)
{
	if (!cfg.care)
		return -1;
	if (!running)
		schedule(1);
	return 0;
}

const struct care_state *care_now(void)
{
	return &st;
}

int care_report_of(int days, struct care_day *out)
{
	care_report_sum(&rep, (long)ag_wall_s(), days, out);
	return 0;
}

void care_blob(struct blob_buf *b)
{
	static const struct { const char *name; int days; } per[] = { { "day", 1 }, { "week", 7 }, { "month", 30 } };
	void *t;
	unsigned k;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.care);
	blobmsg_add_u32(b, "interval_s", (uint32_t)cfg.care_interval);
	blobmsg_add_u32(b, "wifi_grace_s", (uint32_t)cfg.care_wifi_grace);
	blobmsg_add_string(b, "status", st.active ? "problem" : "ok");
	if (st.last_check)
		blobmsg_add_u64(b, "last_check", (uint64_t)st.last_check);
	if (cfg.care && next_at)
		blobmsg_add_u64(b, "next_check", (uint64_t)next_at);
	if (st.active) {
		t = blobmsg_open_table(b, "problem");
		blobmsg_add_string(b, "code", st.code);
		blobmsg_add_string(b, "area", st.area);
		blobmsg_add_string(b, "severity", diag_sev_name(st.sev));
		blobmsg_add_string(b, "summary", st.text);
		blobmsg_add_string(b, "advice", diag_advice(st.code));
		blobmsg_add_u64(b, "since", (uint64_t)st.since);
		blobmsg_add_u8(b, "reported", (uint8_t)st.reported);
		blobmsg_add_string(b, "owner", care_isp_owned(st.area) ? "ISP" :
				   care_area_index(st.area) == CA_WIFI ? "AIGW" : "home");
		blobmsg_close_table(b, t);
	}
	t = blobmsg_open_table(b, "report");
	for (k = 0; k < sizeof(per) / sizeof(per[0]); k++) {
		struct care_day s;
		void *pt;
		int a;

		care_report_sum(&rep, (long)ag_wall_s(), per[k].days, &s);
		pt = blobmsg_open_table(b, per[k].name);
		blobmsg_add_u32(b, "checks", s.checks);
		if (care_score(&s) >= 0)
			blobmsg_add_u32(b, "score", (uint32_t)care_score(&s));
		blobmsg_add_u32(b, "problems", s.problems);
		blobmsg_add_u32(b, "reported", s.reported);
		blobmsg_add_u32(b, "fixed_by_aigw", s.fixed);
		blobmsg_add_u32(b, "problem_s", s.problem_secs);
		if ((a = care_main_cause(&s)) >= 0)
			blobmsg_add_string(b, "main_cause", care_area_name(a));
		for (a = 0; a < CARE_AREAS; a++)
			if (s.bad[a])
				blobmsg_add_u32(b, care_area_name(a), s.bad[a]);
		blobmsg_close_table(b, pt);
	}
	blobmsg_close_table(b, t);
}

/* ---------- saved with agd's state (devices.c) ---------- */

void care_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "care"), *arr;
	int k, a;

	if (st.active) {
		void *p = blobmsg_open_table(b, "open");

		blobmsg_add_string(b, "code", st.code);
		blobmsg_add_string(b, "area", st.area);
		blobmsg_add_u32(b, "severity", (uint32_t)st.sev);
		blobmsg_add_string(b, "summary", st.text);
		blobmsg_add_u64(b, "since", (uint64_t)st.since);
		blobmsg_add_u8(b, "reported", (uint8_t)st.reported);
		blobmsg_close_table(b, p);
	}
	arr = blobmsg_open_array(b, "days");
	for (k = 0; k < CARE_DAYS; k++) {
		const struct care_day *d = &rep.d[k];
		void *row;

		if (!d->day)
			continue;
		row = blobmsg_open_array(b, NULL);
		blobmsg_add_u64(b, NULL, (uint64_t)d->day);
		blobmsg_add_u32(b, NULL, d->checks);
		blobmsg_add_u32(b, NULL, d->good);
		for (a = 0; a < CARE_AREAS; a++)
			blobmsg_add_u32(b, NULL, d->bad[a]);
		blobmsg_add_u32(b, NULL, d->problems);
		blobmsg_add_u32(b, NULL, d->reported);
		blobmsg_add_u32(b, NULL, d->fixed);
		blobmsg_add_u32(b, NULL, d->problem_secs);
		blobmsg_close_array(b, row);
	}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

static unsigned long long jnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (unsigned long long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (unsigned long long)blobmsg_get_double(a);
	default: return 0;
	}
}

void care_load(struct blob_attr *t)
{
	struct blob_attr *cur, *row, *v;
	size_t r1, r2, r3;
	int n = 0;

	blobmsg_for_each_attr(cur, t, r1) {
		if (!strcmp(blobmsg_name(cur), "open") && blobmsg_type(cur) == BLOBMSG_TYPE_TABLE) {
			blobmsg_for_each_attr(v, cur, r2) {
				const char *k = blobmsg_name(v);

				if (!strcmp(k, "code") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
					snprintf(st.code, sizeof(st.code), "%s", blobmsg_get_string(v));
				else if (!strcmp(k, "area") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
					snprintf(st.area, sizeof(st.area), "%s", blobmsg_get_string(v));
				else if (!strcmp(k, "summary") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
					snprintf(st.text, sizeof(st.text), "%s", blobmsg_get_string(v));
				else if (!strcmp(k, "severity"))
					st.sev = (int)jnum(v);
				else if (!strcmp(k, "since"))
					st.since = (long)jnum(v);
				else if (!strcmp(k, "reported"))
					st.reported = jnum(v) != 0;
			}
			/* still open: the next checks see whether it is over (and the ISP isn't told twice) */
			if (st.code[0] && st.since) {
				st.active = 1;
				st.bad_run = 1;
			}
		}
		if (strcmp(blobmsg_name(cur), "days") || blobmsg_type(cur) != BLOBMSG_TYPE_ARRAY)
			continue;
		blobmsg_for_each_attr(row, cur, r2) {
			unsigned long long x[3 + CARE_AREAS + 4];
			struct care_day *d;
			int k = 0, a;

			if (blobmsg_type(row) != BLOBMSG_TYPE_ARRAY)
				continue;
			memset(x, 0, sizeof(x));
			blobmsg_for_each_attr(v, row, r3)
				if (k < (int)(sizeof(x) / sizeof(x[0])))
					x[k++] = jnum(v);
			if (!x[0])
				continue;
			d = &rep.d[x[0] % CARE_DAYS];
			memset(d, 0, sizeof(*d));
			d->day = (long)x[0];
			d->checks = (unsigned)x[1];
			d->good = (unsigned)x[2];
			for (a = 0; a < CARE_AREAS; a++)
				d->bad[a] = (unsigned)x[3 + a];
			k = 3 + CARE_AREAS;
			d->problems = (unsigned)x[k++];
			d->reported = (unsigned)x[k++];
			d->fixed = (unsigned)x[k++];
			d->problem_secs = (unsigned)x[k++];
			n++;
		}
	}
	if (n || st.active)
		AG_INFO("care: restored %d days of checks%s%s", n, st.active ? "; open problem " : "", st.active ? st.code : "");
}
