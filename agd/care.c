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
 * and keeps a daily experience score: the share of the time without a problem,
 * with the main cause on bad days (agctl care, Device.X_AIGW_.Care.).
 *
 * Early warning (core/libag/care_learn.c): every check's delays, loss and DNS
 * time teach the gateway this home's normal, per hour; a drift away from it is
 * told to the ISP before it is a fault (EarlyWarning!), with when it would be
 * one at that rate. Whether a problem followed is counted: how often, and how
 * long before, the gateway saw it coming.
 *
 * The nightly speed test (care.speed_url): a TR-143 download from the ISP's
 * own server at a random minute of care.speed_hour. Its speed is learned like
 * the rest; below care.speed_min_pct of the line's speed (care.speed_mbps) on
 * two tests in a row it is a problem, LINE_SLOW_SPEED, reported at once.
 *
 * Other speed tests teach the line's speed too (v0.44.1): a TR-143 download
 * the ISP runs (USP or TR-069) and one a person asks for with a server of
 * their own (agctl care --speed <url>), when it completed and ran for 2 s or
 * more. Without any of these, and no care.speed_mbps, the line's speed isn't
 * known, and the timeline only guesses it from the most it has seen.
 */
#define _GNU_SOURCE                     /* tm_gmtoff */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/tr143.h"
#include "agd.h"

#define BUSY_RETRY_MS 15000             /* a diagnosis someone asked for is running */
#define POKE_MIN_S    120               /* a radio turning Poor checks now, at most this often */
#define SEEN_COMING_S (48 * 3600)       /* a problem this soon after a warning in its area: seen coming */
#define SPEED_CONFIRM 900               /* a slow test: another one this much later */
#define SPEED_WATCH   3600              /* while the line is slow: a test this often, to see it end */
#define SPEED_RETRY   1800              /* a test that failed: again (3 times a night at most) */
#define SPEED_MIN_US  2000000LL         /* another test counts once it ran this long (TCP is past its slow start) */
#define SPEED_MIN_RX  (1024ULL * 1024)  /* ... and moved at least this much */

static struct care_state st;
static struct care_report rep;
static struct care_params par;
static struct uloop_timeout timer;
static int running;
static long next_at, last_poke;

static struct care_learn L;
static struct care_learn_params lp;
static int warn_became;                 /* a problem started while the open warning was on */
static long last_sample;
static int last_online = -1;            /* the internet answered on the last check */

static struct care_state sp;            /* the nightly speed test's problem */
static struct care_params sp_par;
static struct uloop_timeout speed_timer;
static struct {
	long at, next, day_done;        /* last test, next one (wall s); the local day tested */
	double mbps;
	int status;                     /* 0 none, 1 ok, 2 low, 3 failed */
	int tries;
	char why[160];
	char by[24];                    /* who ran the last one: "" the nightly test, "the ISP", "a person" */
	int once;                       /* a person's test is running */
	char once_why[160];             /* why a person's last test didn't count */
} spd;

static void schedule(int secs)
{
	next_at = (long)ag_wall_s() + secs;
	uloop_timeout_set(&timer, secs * 1000);
}

/* the next check: soon while something is wrong (to confirm it, then to see it clear) */
static int next_secs(void)
{
	int s = st.active || st.bad_run ? cfg.care_recheck : cfg.care_interval;
	int q = upgrade_quick();        /* early in a firmware upgrade's watch: more often */

	return q && q < s ? q : s;
}

int care_online(void)
{
	return last_online;
}

static void local_now(long now, int *hour, long *day)
{
	time_t t = (time_t)now;
	struct tm tm;

	localtime_r(&t, &tm);
	*hour = tm.tm_hour;
	*day = (now + tm.tm_gmtoff) / 86400;
}

static void lparams(void)
{
	care_learn_params_default(&lp);
	lp.learn_s = (long)cfg.care_learn_days * 86400;
	lp.fault[CM_SPEED] = cfg.care_speed_mbps ? (float)cfg.care_speed_mbps * (float)cfg.care_speed_min_pct / 100.0f : 0;
	lp.prior = fleet_care_prior();  /* fleet learning: warnings from day one, "from the fleet" */
}

int care_normal_band(int metric, int band, float *mean, float *dev)
{
	float m, d, sm = 0, sd = 0;
	int h, n = 0;

	lparams();
	if (band < 0 || band > 3 || care_learning(&L, (long)ag_wall_s(), &lp))
		return -1;
	for (h = band * 6; h < band * 6 + 6; h++)
		if (care_normal(&L, metric, h, &m, &d) == 0) {
			sm += m;
			sd += d;
			n++;
		}
	if (!n)
		return -1;
	*mean = sm / (float)n;
	*dev = sd / (float)n;
	return 0;
}

static void tell_state(const char *event, const struct care_state *s)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", event);
	blobmsg_add_string(&b, "code", s->code);
	blobmsg_add_string(&b, "area", s->area);
	blobmsg_add_string(&b, "severity", diag_sev_name(s->sev));
	blobmsg_add_string(&b, "summary", s->text);
	blobmsg_add_u64(&b, "since", (uint64_t)s->since);
	if (!strcmp(event, "cleared"))
		blobmsg_add_u64(&b, "duration_s", (uint64_t)s->duration);
	agd_emit(AG_EV_CARE, &b);
	blob_buf_free(&b);
}

static void tell(const char *event)
{
	tell_state(event, &st);
}

/* ---------- early warning ---------- */

void care_warning_words(char *buf, size_t n)
{
	lparams();
	care_warning_text(&L.w, &lp, buf, n);
}

static void tell_warning(const char *event)
{
	struct blob_buf b = { 0 };
	char text[400];

	care_warning_words(text, sizeof(text));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", event);
	blobmsg_add_string(&b, "metric", care_metric_name(L.w.metric));
	blobmsg_add_string(&b, "area", care_area_name(care_metric_area(L.w.metric)));
	blobmsg_add_string(&b, "summary", text);
	blobmsg_add_u64(&b, "since", (uint64_t)L.w.since);
	blobmsg_add_u8(&b, "from_fleet", (uint8_t)L.w.fleet);
	agd_emit(AG_EV_CARE, &b);
	blob_buf_free(&b);
}

static void warn_events(int ev, long now)
{
	struct care_day *d = care_report_day(&rep, now);
	char text[400];

	if (ev & (CW_OPENED | CW_CHANGED)) {
		d->warnings++;
		warn_became = 0;
		care_warning_words(text, sizeof(text));
		AG_INFO("care: early warning (%s, %s): %s", care_metric_name(L.w.metric),
			L.w.kind == CW_TREND ? "trend" : "shift", text);
		dm_on_warning(1);
		tell_warning("warning");
	}
	if (ev & CW_CLEARED) {
		AG_INFO("care: early warning over (%s) after %lds%s", care_metric_name(L.w.metric), L.w.duration,
			warn_became ? ": a problem followed" : "");
		dm_on_warning(0);
		tell_warning("warning-cleared");
	}
}

/* a problem started in area at since: had a warning seen it coming? */
static void seen_coming(const char *area, long since, long now)
{
	struct care_day *d = care_report_day(&rep, now);
	int a = care_area_index(area);
	long from = 0;

	if (a < 0)
		return;
	if (L.w.active && care_metric_area(L.w.metric) == a) {
		from = L.w.since;
		warn_became = 1;
	} else if (L.last_end && now - L.last_end <= SEEN_COMING_S && L.last_area == a) {
		from = L.last_since;
	}
	if (!from || since < from)
		return;
	d->warned++;
	d->lead_secs += (unsigned)(since - from);
	AG_INFO("care: an early warning saw this coming %lds before", since - from);
}

/* the areas with a problem open, or bad in this check: nothing learned there */
static unsigned quiet_mask(int bad_area)
{
	unsigned q = 0;
	int a;

	if (st.active && (a = care_area_index(st.area)) >= 0)
		q |= 1u << a;
	if (sp.active)
		q |= 1u << CA_INTERNET;
	if (bad_area >= 0)
		q |= 1u << bad_area;
	return q;
}

static void learn_one(const struct care_sample *s, long now, int bad_area)
{
	int hour, ev;
	long day;

	lparams();
	local_now(now, &hour, &day);
	ev = care_learn_step(&L, s, hour, day, now, quiet_mask(bad_area), &lp);
	if (!cfg.care_warn) {
		L.w.active = 0;         /* learned, not told */
		return;
	}
	warn_events(ev, now);
}

/* what a check measured, for the normal */
static void sample_of(const struct diag_input *in, struct care_sample *s)
{
	memset(s, 0, sizeof(*s));
	if (!in || !in->route)
		return;
	if (in->gw_ping.sent && in->gw_ping.lost < in->gw_ping.sent && in->gw_ping.rtt_ms >= 0) {
		s->have[CM_GW_MS] = 1;
		s->v[CM_GW_MS] = (float)in->gw_ping.rtt_ms;
	}
	if (in->inet.sent && in->inet.lost < in->inet.sent) {
		s->have[CM_LOSS] = 1;
		s->v[CM_LOSS] = (float)in->inet.lost * 100.0f / (float)in->inet.sent;
		if (in->inet.rtt_ms >= 0) {
			s->have[CM_INET_MS] = 1;
			s->v[CM_INET_MS] = (float)in->inet.rtt_ms;
		}
	}
	if (in->dns == 1 && in->dns_rcode == 0) {
		s->have[CM_DNS_MS] = 1;
		s->v[CM_DNS_MS] = (float)in->dns_ms;
	}
}

const struct care_warning *care_warning_now(void)
{
	return &L.w;
}

int care_warning_learning(void)
{
	lparams();
	return cfg.care && cfg.care_warn && care_learning(&L, (long)ag_wall_s(), &lp);
}

int care_warning_became(void)
{
	return warn_became;
}

int care_normal_now(int metric, float *v)
{
	int hour;
	long day;
	float dev;

	local_now((long)ag_wall_s(), &hour, &day);
	return care_normal(&L, metric, hour, v, &dev);
}

int care_days_get(struct care_day *out, int max)
{
	int i, n = 0;

	for (i = 0; i < CARE_DAYS && n < max; i++)
		if (rep.d[i].day)
			out[n++] = rep.d[i];
	return n;
}

int care_normal_hour(int metric, int hour, float *v)
{
	float dev;

	return care_normal(&L, metric, hour, v, &dev);
}

int care_learned_days(void)
{
	return L.started ? (int)(((long)ag_wall_s() - L.started) / 86400) : 0;
}

/* ---------- the checks ---------- */

/* a check, for the firmware upgrade's watch: with the home's normal at this hour */
static void to_upgrade(const struct diag_input *in, long now)
{
	struct care_sample s, nm;
	float mean, dev;
	int hour, m;
	long day;

	sample_of(in, &s);
	memset(&nm, 0, sizeof(nm));
	local_now(now, &hour, &day);
	for (m = 0; m < CARE_METRICS; m++)
		if (care_normal(&L, m, hour, &mean, &dev) >= 0) {
			nm.have[m] = 1;
			nm.v[m] = mean;
		}
	upgrade_on_check(&s, &nm, last_online == 1, st.active, par.max_gap);
}

static void done(void *ctx, const struct diag_result *r, const struct diag_input *in)
{
	const struct diag_finding *f = r->n ? &r->f[0] : NULL;
	struct care_verdict v = { f ? f->code : "OK", f ? f->area : "All", f ? f->sev : DS_OK, f ? f->text : "" };
	long now = (long)ag_wall_s();
	int ev;

	(void)ctx;
	running = 0;
	par.wifi_grace = cfg.care_wifi_grace;
	par.max_gap = 3L * (cfg.care_interval > cfg.care_recheck ? cfg.care_interval : cfg.care_recheck);
	last_online = in && in->route && in->inet.sent && in->inet.lost < in->inet.sent;
	ev = care_step(&st, &rep, &v, now, &par);
	to_upgrade(in, now);
	if (ev & CARE_STARTED) {
		AG_INFO("care: problem found: %s (%s) -- %s; %s", st.code, st.area, st.text,
			care_isp_owned(st.area) ? "reporting it to the ISP" :
			care_area_index(st.area) == CA_WIFI ? "Self-Healing Wi-Fi is on it" : "not the ISP's");
		seen_coming(st.area, st.since, now);
		tell("found");
	}
	if (ev & CARE_CHANGED) {
		AG_INFO("care: the problem is now %s (%s) -- %s", st.code, st.area, st.text);
		if (st.reported && !(ev & CARE_REPORT))
			dm_on_care(&st, 1);     /* the ISP was told: tell it the new cause */
		tell("changed");
	}
	if (ev & CARE_REPORT) {
		AG_INFO("care: reported to the ISP: %s", st.code);
		dm_on_care(&st, 1);
		tell("reported");
	}
	if (ev & CARE_CLEARED) {
		AG_INFO("care: %s is over after %lds%s", st.code, st.duration,
			st.reported || care_area_index(st.area) != CA_WIFI ? "" :
			st.acted_at && st.acted_at >= st.since - CARE_ACTED_BEFORE ? " (fixed by AIGW, not reported)" :
			" (cleared by itself: AIGW changed nothing; not reported)");
		if (st.reported)
			dm_on_care(&st, 0);
		tell("cleared");
		st.reported = 0;
	}
	/* the normal: from the regular checks (not the quick ones that confirm a problem) */
	if (!last_sample || now - last_sample >= cfg.care_interval / 2) {
		struct care_sample s;

		sample_of(in, &s);
		learn_one(&s, now, care_bad(&v) ? care_area_index(v.area) : -1);
		last_sample = now;
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

/* ---------- the nightly speed test ---------- */

static void speed_in(int secs)
{
	spd.next = (long)ag_wall_s() + secs;
	uloop_timeout_set(&speed_timer, secs * 1000);
}

/* tonight's test (or tomorrow's, once tonight's ran), at a random minute of care.speed_hour */
static void speed_plan(void)
{
	time_t now = time(NULL), t;
	struct tm tm;
	int hour;
	long day;

	if (!cfg.care || !cfg.care_speed_url[0])
		return;
	localtime_r(&now, &tm);
	tm.tm_hour = cfg.care_speed_hour;
	tm.tm_min = rand() % 60;
	tm.tm_sec = rand() % 60;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	local_now((long)t, &hour, &day);
	if (t <= now + 60 || day == spd.day_done) {
		tm.tm_mday += 1;
		tm.tm_isdst = -1;
		t = mktime(&tm);
	}
	speed_in((int)(t - now));
	AG_DEBUG("care: the next speed test in %lds", (long)(t - now));
}

static void speed_tonight_done(void)
{
	int hour;
	long day;

	local_now((long)ag_wall_s(), &hour, &day);
	spd.day_done = day;
	spd.tries = 0;
	speed_plan();
}

/* a test that completed: the line's speed now, the day's numbers, the learned normal; 1 when slow */
static int speed_record(const struct tr143_out *o, long now, const char *by)
{
	struct care_day *d = care_report_day(&rep, now);
	struct care_sample s;
	double min = cfg.care_speed_mbps * cfg.care_speed_min_pct / 100.0;
	int low;

	spd.at = now;
	snprintf(spd.why, sizeof(spd.why), "%s", o->why);
	snprintf(spd.by, sizeof(spd.by), "%s", by);
	spd.mbps = (double)o->test_rx * 8.0 / (double)(o->eom_us - o->bom_us);
	low = cfg.care_speed_mbps && spd.mbps < min;
	spd.status = low ? 2 : 1;
	d->speed_tests++;
	d->speed_kbps_sum += (unsigned)(spd.mbps * 1000);
	if (d->speed_tests == 1 || (unsigned)(spd.mbps * 1000) < d->speed_kbps_min)
		d->speed_kbps_min = (unsigned)(spd.mbps * 1000);
	if (low)
		d->speed_low++;
	AG_INFO("care: speed test%s%s: %.1f Mbit/s%s", by[0] ? " by " : "", by, spd.mbps, low ? " (slow)" : "");
	upgrade_on_speed((float)spd.mbps);
	memset(&s, 0, sizeof(s));
	s.have[CM_SPEED] = 1;
	s.v[CM_SPEED] = (float)spd.mbps;
	learn_one(&s, now, low ? CA_INTERNET : -1);
	return low;
}

/* another test's result: why it doesn't count, or NULL when it does */
static const char *speed_unfit(const struct tr143_out *o)
{
	if (o->status != TS_COMPLETE)
		return tr143_status_name(o->status);
	if (o->eom_us - o->bom_us < SPEED_MIN_US)
		return "it ran for less than 2 s";
	if (o->test_rx < SPEED_MIN_RX)
		return "it moved less than 1 MB";
	return NULL;
}

/* a TR-143 download the ISP ran (agd/dm.c): it teaches the line's speed too */
void care_speed_seen(const struct tr143_out *o, const char *who)
{
	const char *why = speed_unfit(o);

	if (!cfg.care)
		return;
	if (why) {
		AG_DEBUG("care: %s's download test isn't the line's speed: %s", who, why);
		return;
	}
	speed_record(o, (long)ag_wall_s(), "the ISP");
}

/* agctl care --speed <url>: a person's test against their own server */
static void speed_once_done(void *ctx, int kind, const struct tr143_out *o)
{
	const char *why = speed_unfit(o);

	(void)ctx;
	(void)kind;
	spd.once = 0;
	if (why) {
		snprintf(spd.once_why, sizeof(spd.once_why), "%s%s%.100s", why, o->why[0] ? ": " : "", o->why);
		AG_INFO("care: the speed test asked for didn't count: %s", spd.once_why);
		return;
	}
	spd.once_why[0] = 0;
	speed_record(o, (long)ag_wall_s(), "a person");
}

static void speed_done(void *ctx, int kind, const struct tr143_out *o)
{
	long now = (long)ag_wall_s();
	struct care_day *d = care_report_day(&rep, now);
	struct care_verdict ok = { "OK", "All", DS_OK, "" }, slow = { "LINE_SLOW_SPEED", "Internet", DS_WARN, "" };
	struct care_report scratch;
	char text[300];
	double min = cfg.care_speed_mbps * cfg.care_speed_min_pct / 100.0;
	int low, ev;

	(void)ctx;
	(void)kind;
	if (o->status != TS_COMPLETE || o->eom_us <= o->bom_us || !o->test_rx) {
		spd.at = now;
		snprintf(spd.why, sizeof(spd.why), "%s", o->why);
		spd.by[0] = 0;
		spd.status = 3;
		AG_INFO("care: the speed test failed (%s): %s", tr143_status_name(o->status), o->why);
		if (++spd.tries < 3)
			speed_in(SPEED_RETRY);
		else
			speed_tonight_done();
		return;
	}
	spd.tries = 0;
	low = speed_record(o, now, "");
	/* its own problem: two slow tests in a row, over after one fine */
	snprintf(text, sizeof(text), "Downloads reach %.1f Mbit/s, under %d%% of the line's %d Mbit/s (%.0f Mbit/s at "
		 "least): the line is slow.", spd.mbps, cfg.care_speed_min_pct, cfg.care_speed_mbps, min);
	slow.text = text;
	memset(&scratch, 0, sizeof(scratch));
	ev = care_step(&sp, &scratch, low ? &slow : &ok, now, &sp_par);
	if (ev & CARE_STARTED) {
		d->problems++;
		AG_INFO("care: problem found: %s -- %s; reporting it to the ISP", sp.code, sp.text);
		seen_coming(sp.area, sp.since, now);
		tell_state("found", &sp);
	}
	if (ev & CARE_REPORT) {
		d->reported++;
		dm_on_care(&sp, 1);
		tell_state("reported", &sp);
	}
	if (ev & CARE_CLEARED) {
		AG_INFO("care: %s is over after %lds", sp.code, sp.duration);
		if (sp.reported)
			dm_on_care(&sp, 0);
		tell_state("cleared", &sp);
		sp.reported = 0;
	}
	if (sp.bad_run && !sp.active)
		speed_in(SPEED_CONFIRM);
	else if (sp.active)
		speed_in(SPEED_WATCH);
	else
		speed_tonight_done();
}

static void speed_run(struct uloop_timeout *t)
{
	struct tr143_in in;
	char err[160];

	(void)t;
	if (!cfg.care || !cfg.care_speed_url[0])
		return;
	tr143_defaults(TK_DOWNLOAD, &in);
	snprintf(in.url, sizeof(in.url), "%s", cfg.care_speed_url);
	in.duration_s = (unsigned)cfg.care_speed_seconds;
	if (tr143_start(TK_DOWNLOAD, &in, "care", speed_done, NULL, err, sizeof(err)) < 0) {
		AG_DEBUG("care: speed test: %s; again in 10 min", err);
		speed_in(600);
	}
}

const char *care_speed_status(void)
{
	static const char *const n[] = { "None", "OK", "Low", "Error" };

	return n[spd.status >= 0 && spd.status <= 3 ? spd.status : 0];
}

long care_speed_last(double *mbps)
{
	if (mbps)
		*mbps = spd.status == 1 || spd.status == 2 ? spd.mbps : 0;
	return spd.at;
}

/* ---------- start, and what agctl and the data model ask ---------- */

void care_init(void)
{
	care_params_default(&par);
	par.wifi_grace = cfg.care_wifi_grace;
	care_params_default(&sp_par);
	sp_par.confirm = 2;
	sp_par.clear = 1;
	sp_par.max_gap = 2L * SPEED_WATCH;
	timer.cb = check;
	speed_timer.cb = speed_run;
	srand((unsigned)(time(NULL) ^ getpid()));
	if (!cfg.care) {
		AG_INFO("care: off in config (care.enabled)");
		return;
	}
	schedule(30);                   /* the first check, once Wi-Fi and the line were seen */
	AG_INFO("care: checking the home every %ds (problems on the line and DNS reported at once, Wi-Fi after %ds)%s",
		cfg.care_interval, cfg.care_wifi_grace, cfg.care_warn ? "; early warnings once the home's normal is learned" : "");
	if (cfg.care_speed_url[0]) {
		speed_plan();
		AG_INFO("care: a speed test every night at %02d:xx (%s, %d s)%s", cfg.care_speed_hour, cfg.care_speed_url,
			cfg.care_speed_seconds, cfg.care_speed_mbps ? "" : "; care.speed_mbps not set: no LINE_SLOW_SPEED");
	}
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

/* agctl care --speed: the speed test now */
int care_speed_now(void)
{
	if (!cfg.care || !cfg.care_speed_url[0])
		return -1;
	speed_in(1);
	return 0;
}

/* agctl care --speed <url>: one test against a person's own server (http://, a big file); -1 with words */
int care_speed_url_now(const char *url, char *err, size_t en)
{
	struct tr143_in in;

	if (!cfg.care) {
		snprintf(err, en, "proactive care is off (care.enabled)");
		return -1;
	}
	if (spd.once) {
		snprintf(err, en, "a speed test is running already");
		return -1;
	}
	tr143_defaults(TK_DOWNLOAD, &in);
	snprintf(in.url, sizeof(in.url), "%s", url);
	in.duration_s = (unsigned)(cfg.care_speed_seconds > 0 ? cfg.care_speed_seconds : 8);
	if (tr143_start(TK_DOWNLOAD, &in, "agctl", speed_once_done, NULL, err, en) < 0)
		return -1;
	spd.once = 1;
	spd.once_why[0] = 0;
	return 0;
}

const struct care_state *care_now(void)
{
	return &st;
}

const struct care_state *care_open(void)
{
	return st.active ? &st : sp.active ? &sp : NULL;
}

int care_report_of(int days, struct care_day *out)
{
	care_report_sum(&rep, (long)ag_wall_s(), days, out);
	return 0;
}

static void add_dec(struct blob_buf *b, const char *name, double v)
{
	char s[32];

	snprintf(s, sizeof(s), "%.1f", v);
	blobmsg_add_string(b, name, s);
}

void care_blob(struct blob_buf *b)
{
	static const struct { const char *name; int days; } per[] = { { "day", 1 }, { "week", 7 }, { "month", 30 } };
	const struct care_state *op = care_open();
	void *t;
	unsigned k;
	int m;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.care);
	blobmsg_add_u32(b, "interval_s", (uint32_t)cfg.care_interval);
	blobmsg_add_u32(b, "wifi_grace_s", (uint32_t)cfg.care_wifi_grace);
	repair_care_blob(b);            /* proven repair: the week's fixes, measured */
	blobmsg_add_string(b, "status", op ? "problem" : "ok");
	if (st.last_check)
		blobmsg_add_u64(b, "last_check", (uint64_t)st.last_check);
	if (cfg.care && next_at)
		blobmsg_add_u64(b, "next_check", (uint64_t)next_at);
	if (op) {
		t = blobmsg_open_table(b, "problem");
		blobmsg_add_string(b, "code", op->code);
		blobmsg_add_string(b, "area", op->area);
		blobmsg_add_string(b, "severity", diag_sev_name(op->sev));
		blobmsg_add_string(b, "summary", op->text);
		blobmsg_add_string(b, "advice", coverage_advice(op->code, NULL, 1));
		blobmsg_add_u64(b, "since", (uint64_t)op->since);
		blobmsg_add_u8(b, "reported", (uint8_t)op->reported);
		blobmsg_add_string(b, "owner", care_isp_owned(op->area) ? "ISP" :
				   care_area_index(op->area) == CA_WIFI ? "AIGW" : "home");
		blobmsg_close_table(b, t);
	}
	/* early warning */
	t = blobmsg_open_table(b, "early_warning");
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.care_warn);
	blobmsg_add_string(b, "status", L.w.active ? "warning" : care_warning_learning() ? "learning" : "none");
	blobmsg_add_u32(b, "learned_days", (uint32_t)care_learned_days());
	blobmsg_add_u32(b, "learn_days", (uint32_t)cfg.care_learn_days);
	/* fleet learning: while learning, warnings are judged against the fleet's normal */
	blobmsg_add_u8(b, "fleet_normal", (uint8_t)(care_warning_learning() && lp.prior));
	if (L.w.active) {
		char text[400], v[24];

		blobmsg_add_u8(b, "from_fleet", (uint8_t)L.w.fleet);

		care_warning_words(text, sizeof(text));
		blobmsg_add_string(b, "metric", care_metric_name(L.w.metric));
		blobmsg_add_string(b, "area", care_area_name(care_metric_area(L.w.metric)));
		blobmsg_add_string(b, "kind", L.w.kind == CW_TREND ? "trend" : "shift");
		blobmsg_add_string(b, "summary", text);
		blobmsg_add_string(b, "advice", care_warning_advice(L.w.metric));
		blobmsg_add_u64(b, "since", (uint64_t)L.w.since);
		care_metric_value(L.w.metric, L.w.normal, v, sizeof(v));
		blobmsg_add_string(b, "normal", v);
		care_metric_value(L.w.metric, L.w.current, v, sizeof(v));
		blobmsg_add_string(b, "current", v);
		blobmsg_add_u32(b, "predicted_fault_days", (uint32_t)(L.w.predict < 0 ? 0 : L.w.predict));
		blobmsg_add_u8(b, "predicted", (uint8_t)(L.w.predict >= 0));
	}
	blobmsg_close_table(b, t);
	t = blobmsg_open_table(b, "normal");
	for (m = 0; m < CARE_METRICS; m++) {
		float v;

		if (care_normal_now(m, &v) == 0)
			add_dec(b, care_metric_name(m), v);
	}
	blobmsg_close_table(b, t);
	/* the nightly speed test */
	t = blobmsg_open_table(b, "speed");
	blobmsg_add_u8(b, "enabled", (uint8_t)(cfg.care && cfg.care_speed_url[0]));
	blobmsg_add_string(b, "status", care_speed_status());
	if (spd.at)
		blobmsg_add_u64(b, "last", (uint64_t)spd.at);
	if (spd.next && cfg.care_speed_url[0])
		blobmsg_add_u64(b, "next", (uint64_t)spd.next);
	if (spd.status == 1 || spd.status == 2)
		add_dec(b, "mbps", spd.mbps);
	if (spd.status == 3)
		blobmsg_add_string(b, "why", spd.why);
	if (spd.at && spd.by[0])
		blobmsg_add_string(b, "by", spd.by);
	if (spd.once)
		blobmsg_add_u8(b, "running", 1);
	if (spd.once_why[0])
		blobmsg_add_string(b, "asked_why", spd.once_why);
	/* what the timeline, smart queueing and the ISP signals take as the line's speed */
	blobmsg_add_string(b, "line", cfg.care_speed_mbps > 0 ? "set" : spd.status == 1 || spd.status == 2 ? "tested" : "unknown");
	blobmsg_add_u32(b, "expected_mbps", (uint32_t)cfg.care_speed_mbps);
	blobmsg_add_u32(b, "min_pct", (uint32_t)cfg.care_speed_min_pct);
	blobmsg_close_table(b, t);
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
		blobmsg_add_u32(b, "cleared_by_itself", s.cleared);
		blobmsg_add_u32(b, "problem_s", s.problem_secs);
		blobmsg_add_u32(b, "watched_s", s.watched_secs);
		if ((a = care_main_cause(&s)) >= 0)
			blobmsg_add_string(b, "main_cause", care_area_name(a));
		for (a = 0; a < CARE_AREAS; a++)
			if (s.bad[a])
				blobmsg_add_u32(b, care_area_name(a), s.bad[a]);
		blobmsg_add_u32(b, "early_warnings", s.warnings);
		blobmsg_add_u32(b, "warned_of", s.warned);
		if (s.warned)
			blobmsg_add_u32(b, "warning_lead_s", s.lead_secs / s.warned);
		blobmsg_add_u32(b, "speed_tests", s.speed_tests);
		if (s.speed_tests) {
			add_dec(b, "speed_mbps", s.speed_kbps_sum / 1000.0 / s.speed_tests);
			add_dec(b, "speed_min_mbps", s.speed_kbps_min / 1000.0);
			blobmsg_add_u32(b, "speed_low", s.speed_low);
		}
		blobmsg_close_table(b, pt);
	}
	blobmsg_close_table(b, t);
}

/* the last days' rows, for cortex's fleet report (plug-in query "care") */
void care_days_blob(struct blob_buf *b, int days)
{
	long today = (long)ag_wall_s() / 86400;
	void *arr = blobmsg_open_array(b, "days");
	int i, a;

	for (i = 0; i < CARE_DAYS; i++) {
		const struct care_day *d = &rep.d[i];
		void *t;

		if (!d->day || d->day > today || d->day <= today - days)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_u64(b, "day", (uint64_t)d->day);
		blobmsg_add_u32(b, "checks", d->checks);
		blobmsg_add_u32(b, "good", d->good);
		for (a = 0; a < CARE_AREAS; a++)
			blobmsg_add_u32(b, care_area_name(a), d->bad[a]);
		blobmsg_add_u32(b, "problems", d->problems);
		blobmsg_add_u32(b, "reported", d->reported);
		blobmsg_add_u32(b, "fixed_by_aigw", d->fixed);
		blobmsg_add_u32(b, "problem_s", d->problem_secs);
		blobmsg_add_u32(b, "early_warnings", d->warnings);
		blobmsg_add_u32(b, "warned_of", d->warned);
		blobmsg_add_u32(b, "warning_lead_s", d->lead_secs);
		blobmsg_add_u32(b, "speed_tests", d->speed_tests);
		blobmsg_add_u32(b, "speed_kbps_sum", d->speed_kbps_sum);
		blobmsg_add_u32(b, "speed_kbps_min", d->speed_kbps_min);
		blobmsg_add_u32(b, "speed_low", d->speed_low);
		blobmsg_add_u32(b, "watched_s", d->watched_secs);
		blobmsg_add_u32(b, "ok_s", d->ok_secs);
		blobmsg_add_u32(b, "cleared_by_itself", d->cleared);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

/* ---------- saved with agd's state (devices.c) ---------- */

/* AIGW changed something on Wi-Fi (act.c: a channel trial took effect, a device kept on 2.4 GHz) */
void care_on_acted(const char *what)
{
	care_acted(&st, (long)ag_wall_s());
	if (st.active || st.bad_run)
		AG_INFO("care: AIGW acted on %s (%s): %s", st.active ? st.code : "what it saw", st.area[0] ? st.area : "WiFi", what);
}

static void save_open(struct blob_buf *b, const char *name, const struct care_state *s)
{
	void *p;

	if (!s->active)
		return;
	p = blobmsg_open_table(b, name);
	blobmsg_add_string(b, "code", s->code);
	blobmsg_add_string(b, "area", s->area);
	blobmsg_add_u32(b, "severity", (uint32_t)s->sev);
	blobmsg_add_string(b, "summary", s->text);
	blobmsg_add_u64(b, "since", (uint64_t)s->since);
	blobmsg_add_u8(b, "reported", (uint8_t)s->reported);
	blobmsg_close_table(b, p);
}

static void save_norm(struct blob_buf *b, const struct care_norm *c)
{
	blobmsg_add_double(b, NULL, c->mean);
	blobmsg_add_double(b, NULL, c->dev);
	blobmsg_add_u32(b, NULL, c->n);
}

static void save_learn(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "learn"), *arr, *row;
	int m, h, k;

	blobmsg_add_u64(b, "started", (uint64_t)L.started);
	if (L.w.active) {
		void *w = blobmsg_open_table(b, "warning");

		blobmsg_add_u32(b, "metric", (uint32_t)L.w.metric);
		blobmsg_add_u32(b, "kind", (uint32_t)L.w.kind);
		blobmsg_add_u64(b, "since", (uint64_t)L.w.since);
		blobmsg_add_double(b, "normal", L.w.normal);
		blobmsg_add_double(b, "current", L.w.current);
		blobmsg_add_double(b, "per_day", L.w.per_day);
		blobmsg_add_u32(b, "days", (uint32_t)L.w.days);
		blobmsg_add_double(b, "predict", L.w.predict);
		blobmsg_add_u8(b, "became", (uint8_t)warn_became);
		if (L.w.fleet)
			blobmsg_add_u8(b, "fleet", 1);
		blobmsg_close_table(b, w);
	}
	if (L.last_end) {
		blobmsg_add_u32(b, "last_area", (uint32_t)L.last_area);
		blobmsg_add_u64(b, "last_since", (uint64_t)L.last_since);
		blobmsg_add_u64(b, "last_end", (uint64_t)L.last_end);
	}
	/* per metric: the whole day's normal, cusum, recent, then 24 hours (mean, dev, n each) */
	arr = blobmsg_open_array(b, "metrics");
	for (m = 0; m < CARE_METRICS; m++) {
		row = blobmsg_open_array(b, NULL);
		save_norm(b, &L.m[m].all);
		blobmsg_add_double(b, NULL, L.m[m].cusum);
		blobmsg_add_double(b, NULL, L.m[m].recent);
		for (h = 0; h < 24; h++)
			save_norm(b, &L.m[m].hour[h]);
		blobmsg_close_array(b, row);
	}
	blobmsg_close_array(b, arr);
	arr = blobmsg_open_array(b, "days");
	for (m = 0; m < CARE_METRICS; m++)
		for (k = 0; k < CARE_TREND_DAYS; k++) {
			const struct care_mday *d = &L.m[m].days[k];

			if (!d->day || !d->n)
				continue;
			row = blobmsg_open_array(b, NULL);
			blobmsg_add_u32(b, NULL, (uint32_t)m);
			blobmsg_add_u64(b, NULL, (uint64_t)d->day);
			blobmsg_add_double(b, NULL, d->level_sum);
			blobmsg_add_double(b, NULL, d->excess_sum);
			blobmsg_add_u32(b, NULL, d->n);
			blobmsg_close_array(b, row);
		}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

void care_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "care"), *arr;
	int k, a;

	save_open(b, "open", &st);
	save_open(b, "speed_open", &sp);
	/* v0.29.2: a restart (an upgrade) doesn't lose the time watched, or what AIGW did */
	if (st.last_check)
		blobmsg_add_u64(b, "last_check", (uint64_t)st.last_check);
	if (st.acted_at)
		blobmsg_add_u64(b, "acted_at", (uint64_t)st.acted_at);
	if (spd.at) {
		void *s = blobmsg_open_table(b, "speed");

		blobmsg_add_u64(b, "at", (uint64_t)spd.at);
		blobmsg_add_double(b, "mbps", spd.mbps);
		blobmsg_add_u32(b, "status", (uint32_t)spd.status);
		blobmsg_add_u64(b, "day_done", (uint64_t)spd.day_done);
		if (spd.by[0])
			blobmsg_add_string(b, "by", spd.by);
		blobmsg_close_table(b, s);
	}
	save_learn(b);
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
		/* v0.29 */
		blobmsg_add_u32(b, NULL, d->warnings);
		blobmsg_add_u32(b, NULL, d->warned);
		blobmsg_add_u32(b, NULL, d->lead_secs);
		blobmsg_add_u32(b, NULL, d->speed_tests);
		blobmsg_add_u32(b, NULL, d->speed_kbps_sum);
		blobmsg_add_u32(b, NULL, d->speed_kbps_min);
		blobmsg_add_u32(b, NULL, d->speed_low);
		/* v0.29.1 */
		blobmsg_add_u32(b, NULL, d->watched_secs);
		blobmsg_add_u32(b, NULL, d->ok_secs);
		/* v0.29.2 */
		blobmsg_add_u32(b, NULL, d->cleared);
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

static double jdbl(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	case BLOBMSG_TYPE_INT64: return (double)(int64_t)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	default: return 0;
	}
}

static void load_open(struct blob_attr *cur, struct care_state *s)
{
	struct blob_attr *v;
	size_t r;

	blobmsg_for_each_attr(v, cur, r) {
		const char *k = blobmsg_name(v);

		if (!strcmp(k, "code") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
			snprintf(s->code, sizeof(s->code), "%s", blobmsg_get_string(v));
		else if (!strcmp(k, "area") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
			snprintf(s->area, sizeof(s->area), "%s", blobmsg_get_string(v));
		else if (!strcmp(k, "summary") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
			snprintf(s->text, sizeof(s->text), "%s", blobmsg_get_string(v));
		else if (!strcmp(k, "severity"))
			s->sev = (int)jnum(v);
		else if (!strcmp(k, "since"))
			s->since = (long)jnum(v);
		else if (!strcmp(k, "reported"))
			s->reported = jnum(v) != 0;
	}
	/* still open: the next checks see whether it is over (and the ISP isn't told twice) */
	if (s->code[0] && s->since) {
		s->active = 1;
		s->bad_run = 1;
	}
}

static void load_norms(struct blob_attr *row, int m)
{
	struct blob_attr *v;
	double x[5 + 3 * 24];
	size_t r;
	int k = 0, h;

	memset(x, 0, sizeof(x));
	blobmsg_for_each_attr(v, row, r)
		if (k < (int)(sizeof(x) / sizeof(x[0])))
			x[k++] = jdbl(v);
	if (k < 5 + 3 * 24)
		return;
	L.m[m].all.mean = (float)x[0];
	L.m[m].all.dev = (float)x[1];
	L.m[m].all.n = (unsigned)x[2];
	L.m[m].cusum = (float)x[3];
	L.m[m].recent = (float)x[4];
	for (h = 0; h < 24; h++) {
		L.m[m].hour[h].mean = (float)x[5 + 3 * h];
		L.m[m].hour[h].dev = (float)x[6 + 3 * h];
		L.m[m].hour[h].n = (unsigned)x[7 + 3 * h];
	}
}

static void load_learn(struct blob_attr *t)
{
	struct blob_attr *cur, *row, *v;
	size_t r1, r2, r3;

	blobmsg_for_each_attr(cur, t, r1) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "started"))
			L.started = (long)jnum(cur);
		else if (!strcmp(k, "last_area"))
			L.last_area = (int)jnum(cur);
		else if (!strcmp(k, "last_since"))
			L.last_since = (long)jnum(cur);
		else if (!strcmp(k, "last_end"))
			L.last_end = (long)jnum(cur);
		else if (!strcmp(k, "warning") && blobmsg_type(cur) == BLOBMSG_TYPE_TABLE) {
			blobmsg_for_each_attr(v, cur, r2) {
				const char *n = blobmsg_name(v);

				if (!strcmp(n, "metric"))
					L.w.metric = (int)jnum(v) % CARE_METRICS;
				else if (!strcmp(n, "kind"))
					L.w.kind = (int)jnum(v);
				else if (!strcmp(n, "since"))
					L.w.since = (long)jnum(v);
				else if (!strcmp(n, "normal"))
					L.w.normal = (float)jdbl(v);
				else if (!strcmp(n, "current"))
					L.w.current = (float)jdbl(v);
				else if (!strcmp(n, "per_day"))
					L.w.per_day = (float)jdbl(v);
				else if (!strcmp(n, "days"))
					L.w.days = (int)jnum(v);
				else if (!strcmp(n, "predict"))
					L.w.predict = (int)jdbl(v);
				else if (!strcmp(n, "became"))
					warn_became = jnum(v) != 0;
				else if (!strcmp(n, "fleet"))
					L.w.fleet = jnum(v) != 0;
			}
			L.w.active = L.w.since != 0;
		} else if (!strcmp(k, "metrics") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			int m = 0;

			blobmsg_for_each_attr(row, cur, r2)
				if (m < CARE_METRICS && blobmsg_type(row) == BLOBMSG_TYPE_ARRAY)
					load_norms(row, m++);
		} else if (!strcmp(k, "days") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(row, cur, r2) {
				double x[5] = { 0 };
				int n = 0;
				struct care_mday *d;

				if (blobmsg_type(row) != BLOBMSG_TYPE_ARRAY)
					continue;
				blobmsg_for_each_attr(v, row, r3)
					if (n < 5)
						x[n++] = jdbl(v);
				if (n < 5 || x[0] < 0 || x[0] >= CARE_METRICS || x[1] <= 0)
					continue;
				d = &L.m[(int)x[0]].days[(long)x[1] % CARE_TREND_DAYS];
				d->day = (long)x[1];
				d->level_sum = (float)x[2];
				d->excess_sum = (float)x[3];
				d->n = (unsigned)x[4];
			}
		}
	}
}

void care_load(struct blob_attr *t)
{
	struct blob_attr *cur, *row, *v;
	size_t r1, r2, r3;
	int n = 0;

	blobmsg_for_each_attr(cur, t, r1) {
		const char *name = blobmsg_name(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_TABLE) {
			if (!strcmp(name, "open"))
				load_open(cur, &st);
			else if (!strcmp(name, "speed_open"))
				load_open(cur, &sp);
			else if (!strcmp(name, "learn"))
				load_learn(cur);
			else if (!strcmp(name, "speed"))
				blobmsg_for_each_attr(v, cur, r2) {
					if (!strcmp(blobmsg_name(v), "at"))
						spd.at = (long)jnum(v);
					else if (!strcmp(blobmsg_name(v), "mbps"))
						spd.mbps = jdbl(v);
					else if (!strcmp(blobmsg_name(v), "status"))
						spd.status = (int)jnum(v) % 4;
					else if (!strcmp(blobmsg_name(v), "day_done"))
						spd.day_done = (long)jnum(v);
					else if (!strcmp(blobmsg_name(v), "by") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
						snprintf(spd.by, sizeof(spd.by), "%s", blobmsg_get_string(v));
				}
			continue;
		}
		if (!strcmp(name, "last_check")) {
			st.last_check = (long)jnum(cur);
			continue;
		}
		if (!strcmp(name, "acted_at")) {
			st.acted_at = (long)jnum(cur);
			continue;
		}
		if (strcmp(name, "days") || blobmsg_type(cur) != BLOBMSG_TYPE_ARRAY)
			continue;
		blobmsg_for_each_attr(row, cur, r2) {
			unsigned long long x[3 + CARE_AREAS + 4 + 7 + 2 + 1];
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
			/* v0.29 (0 in older states) */
			d->warnings = (unsigned)x[k++];
			d->warned = (unsigned)x[k++];
			d->lead_secs = (unsigned)x[k++];
			d->speed_tests = (unsigned)x[k++];
			d->speed_kbps_sum = (unsigned)x[k++];
			d->speed_kbps_min = (unsigned)x[k++];
			d->speed_low = (unsigned)x[k++];
			/* v0.29.1 (0 in older states: that day keeps the score by checks) */
			d->watched_secs = (unsigned)x[k++];
			d->ok_secs = (unsigned)x[k++];
			/* v0.29.2 */
			d->cleared = (unsigned)x[k++];
			n++;
		}
	}
	if (n || st.active || L.started)
		AG_INFO("care: restored %d days of checks%s%s; %d days of the home's normal%s%s", n,
			st.active ? "; open problem " : "", st.active ? st.code : "", care_learned_days(),
			L.w.active ? "; early warning " : "", L.w.active ? care_metric_name(L.w.metric) : "");
}
