/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW help-desk assistant (the rules: core/libag/brief.c, ag/brief.h).
 *
 * Asked about a period ("now", "last hour", "tonight 20:00-22:00"), agd first
 * asks the home itself, if wanted: a fresh quiet Diagnose (a few seconds) and
 * the speed test (care.speed_url). Then it gathers what it already knows:
 *   - the why timeline over the period (agd/why.c): its verdict and findings,
 *     how much was recorded, the calls and games in it
 *   - proactive care's open problem and early warning, the week's score, the
 *     nightly speed tests (agd/care.c)
 *   - each access point's Wi-Fi health now (agd/heal.c)
 *   - smart queueing (agd/sqm.c), the last firmware upgrade (agd/upgrade.c)
 *   - AIGW's decisions (agd/act.c)
 * and the rules make one short answer of it.
 *
 * Names: the ISP sees device kinds ("a laptop") unless brief.names is 1;
 * every text is scrubbed of the home's device names, MACs and addresses.
 * agctl at home sees the names. With calls.share 0 the calls stay out.
 *
 * A fix is never applied: with "propose" it becomes a proposed decision that
 * waits for a person's yes (agctl approve, the data model's Approve()).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/brief.h"
#include "ag/calls.h"
#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/why.h"
#include "agd.h"

#define SPEED_WAIT_S 90
#define DEC_SEEN     64

static struct {
	int busy;
	struct brief_ask a;
	brief_cb cb;
	void *ctx;
	int want_check, want_speed;
	int have_check;
	struct diag_result check;
	int check_loss, check_inet, check_gw, check_dns;
	long speed_before;
	int speed_ran;
	int64_t started;
	struct uloop_timeout t;
} Q;

/* briefs asked (agctl, the data model), by fault: the proof for the ISP (kept in the state file) */
static struct {
	uint32_t total, fault[BF_N], visits, remote, proposed;
	long since;
} C;

/* ---------- names ---------- */

#define SITE_MAX  256
#define NAMES_MAX (DEV_MAX * 3 + 2 * SITE_MAX)
static char nm_buf[NAMES_MAX][64], kd_buf[NAMES_MAX][40];
static const char *nm[NAMES_MAX], *kd[NAMES_MAX];
static int nnm;

static void names_build(void)
{
	int i;

	nnm = 0;
	for (i = 0; i < DEV_MAX; i++) {
		const struct device *d = &devs[i];
		char k[40];

		if (!d->used)
			continue;
		brief_kind(d->type, d->hostname, k, sizeof(k));
		if (d->hostname[0] && strlen(d->hostname) >= 2) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%.63s", d->hostname);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
		if (d->mac[0]) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%s", d->mac);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
		if (d->ip[0]) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%s", d->ip);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
	}
	/* the sites alerts and decisions name (a domain a device looked up, one blocked): never for the ISP */
	{
		uint32_t ids[SITE_MAX];
		int k, n = alerts_ids(ids, SITE_MAX);

		for (k = 0; k < n && nnm < NAMES_MAX; k++)
			if (alerts_field(ids[k], "subject", nm_buf[nnm], sizeof(nm_buf[0])) == 0 && strlen(nm_buf[nnm]) >= 3) {
				snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "a site");
				nnm++;
			}
		n = act_ids(ids, SITE_MAX);
		for (k = 0; k < n && nnm < NAMES_MAX; k++) {
			char act[32];

			if (act_field(ids[k], "action", act, sizeof(act)) == 0 && !strcmp(act, "block") &&
			    act_field(ids[k], "target", nm_buf[nnm], sizeof(nm_buf[0])) == 0 && strlen(nm_buf[nnm]) >= 3) {
				snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "a site");
				nnm++;
			}
		}
	}
	for (i = 0; i < nnm; i++) {
		nm[i] = nm_buf[i];
		kd[i] = kd_buf[i];
	}
}

static void scrub(char *s, size_t n)
{
	brief_scrub(s, n, nm, kd, nnm);
}

/* ---------- gathering ---------- */

static struct why_result wr;
static struct why_in wi;
static struct brief_finding why_more[WHY_FINDINGS];
static struct brief_radio radios[WIFI_AP_MAX];
static char radio_name[WIFI_AP_MAX][48];
static struct brief_decision decs[DEC_SEEN];
static char dec_action[DEC_SEEN][32], dec_words[DEC_SEEN][96], dec_reason[DEC_SEEN][160];
static char care_text[400], warn_text[400], up_state[24], up_from[64], up_to[64], sqm_state[24], host_words[64];
static char host_mac[18];

static long num_field(int (*get)(const char *, char *, size_t), const char *f)
{
	char b[64];

	return get(f, b, sizeof(b)) == 0 ? strtol(b, NULL, 10) : 0;
}

static void gather(struct brief_in *in, const struct brief_ask *a, long now)
{
	const struct care_state *cs;
	const struct care_warning *w;
	struct care_day days[CARE_DAYS], week;
	char err[160], b[64];
	uint32_t ids[DEC_SEEN * 2];
	int i, n, k;
	double mbps = 0;

	memset(in, 0, sizeof(*in));
	in->now = now;
	in->from = a->from;
	in->to = a->to;
	in->score_week = -1;
	in->check_loss_pct = in->check_inet_ms = in->check_gw_ms = in->check_dns_ms = -1;
	/* the device asked about */
	if (a->host[0]) {
		struct device *d = act_find_device(a->host);

		if (d) {
			if (a->names)
				snprintf(host_words, sizeof(host_words), "%s", d->hostname[0] ? d->hostname : d->mac);
			else
				brief_kind(d->type, d->hostname, host_words, sizeof(host_words));
			snprintf(host_mac, sizeof(host_mac), "%s", d->mac);
			in->host = host_words;
			in->host_target = host_mac;
		}
	}
	/* the fresh check */
	if (Q.have_check) {
		const struct diag_finding *v = Q.check.n ? &Q.check.f[0] : NULL;

		in->have_check = 1;
		in->check.code = v ? v->code : "OK";
		in->check.area = v ? v->area : "All";
		in->check.sev = v ? v->sev : DS_OK;
		in->check.text = v ? v->text : "Everything the gateway can measure is fine.";
		in->check.since = now;
		in->check_loss_pct = Q.check_loss;
		in->check_inet_ms = Q.check_inet;
		in->check_gw_ms = Q.check_gw;
		in->check_dns_ms = Q.check_dns;
	}
	/* the timeline */
	why_hide_calls(a->hide_calls);
	if (why_ask(a->from, a->to, a->host, &wr, &wi, err, sizeof(err)) == 0 && (wr.recorded || wi.recorded_since)) {
		in->have_why = 1;
		in->why.code = wr.f[0].code;
		in->why.area = wr.f[0].area;
		in->why.sev = wr.f[0].sev;
		in->why.since = wr.f[0].since;
		in->why.until = wr.f[0].until;
		in->why.text = wr.f[0].text;
		for (i = 1, k = 0; i < wr.n; i++, k++) {
			why_more[k].code = wr.f[i].code;
			why_more[k].area = wr.f[i].area;
			why_more[k].sev = wr.f[i].sev;
			why_more[k].since = wr.f[i].since;
			why_more[k].until = wr.f[i].until;
			why_more[k].text = wr.f[i].text;
		}
		in->why_more = why_more;
		in->nwhy_more = k;
		{
			/* of the part since the timeline began: before it, NOT_RECORDED says so already */
			long start = wi.recorded_since > a->from ? wi.recorded_since : a->from;
			long exp = a->to > start ? (a->to - start) / (cfg.why_interval > 0 ? cfg.why_interval : 60) : 0;

			in->recorded_pct = exp <= 0 ? 100 : (int)(wr.recorded * 100 / exp);
			if (in->recorded_pct > 100)
				in->recorded_pct = 100;
		}
		for (i = 0; i < wi.n; i++)
			if ((long)wi.m[i].t > in->last_sample)
				in->last_sample = (long)wi.m[i].t;
		for (i = 0; i < wi.ne; i++)
			if (wi.e[i].kind == WE_CALL && (long)wi.e[i].t >= a->from && (long)wi.e[i].t < a->to) {
				in->calls++;
				in->calls_rough += wi.e[i].flag == CV_ROUGH;
			}
	}
	why_hide_calls(0);
	/* proactive care */
	if ((cs = care_open())) {
		snprintf(care_text, sizeof(care_text), "%s", cs->text);
		in->care_open = 1;
		in->care.code = cs->code;
		in->care.area = cs->area;
		in->care.sev = cs->sev;
		in->care.since = cs->since;
		in->care.text = care_text;
		in->care_reported = cs->reported;
	}
	if ((w = care_warning_now()) && w->active) {
		care_warning_words(warn_text, sizeof(warn_text));
		in->warning = warn_text;
	}
	if (cfg.care && care_report_of(7, &week) == 0)
		in->score_week = care_score(&week);
	in->speed_at = care_speed_last(&mbps);
	in->speed_mbps = (float)mbps;
	in->line_mbps = (float)cfg.care_speed_mbps;
	n = care_days_get(days, CARE_DAYS);
	for (i = 0; i < n; i++)
		if (days[i].day > now / 86400 - 7)
			in->speed_low_nights += days[i].speed_low > 0;
	/* Wi-Fi now */
	for (i = 0, k = 0; i < wifi_n_aps && k < WIFI_AP_MAX; i++) {
		const struct heal_radio *h = heal_for(wifi_aps[i].iface);

		if (!h)
			continue;
		snprintf(radio_name[k], sizeof(radio_name[k]), "%s (%.23s)", wifi_aps[i].band == 5 ? "5 GHz" : "2.4 GHz",
			 wifi_aps[i].iface);
		radios[k].name = radio_name[k];
		radios[k].status = h->status;
		radios[k].cause = h->cause;
		radios[k].since = (long)h->since;
		radios[k].text = h->explanation;
		k++;
	}
	in->radio = radios;
	in->nradio = k;
	/* mesh Wi-Fi now: an extender's backhaul (a warning when it slows the devices behind it) */
	{
		static struct mesh_issue mi[4];
		static struct brief_finding mf[4];
		int m = mesh_issues(mi, 4);

		for (i = 0; i < m; i++) {
			if (!a->names)
				brief_scrub_isp(mi[i].text, sizeof(mi[i].text));
			mf[i].code = mi[i].code;
			mf[i].area = "WiFi";
			mf[i].sev = mi[i].hurts ? DS_WARN : DS_INFO;
			mf[i].since = mi[i].since;
			mf[i].text = mi[i].text;
		}
		in->mesh = mf;
		in->nmesh = m;
	}
	/* smart queueing, the last upgrade */
	if (cfg.sqm && sqm_field("Status", sqm_state, sizeof(sqm_state)) == 0) {
		uint32_t id;
		int st;

		in->sqm = sqm_state;
		if (!strcmp(sqm_state, "Proposed") && act_find_action("sqm", &id, &st, NULL, 0, NULL, NULL, 0) == 0 && !st)
			in->sqm_decision = id;
	}
	if (upgrade_field("Status", up_state, sizeof(up_state)) == 0 && strcmp(up_state, "None")) {
		in->upgrade = up_state;
		in->upgrade_at = num_field(upgrade_field, "Activated");
		upgrade_field("FromVersion", up_from, sizeof(up_from));
		upgrade_field("ToVersion", up_to, sizeof(up_to));
		in->upgrade_from = up_from;
		in->upgrade_to = up_to;
	}
	/* decisions: made in the period (or the hour before), and live ones */
	n = act_ids(ids, (int)(sizeof(ids) / sizeof(ids[0])));
	for (i = n - 1, k = 0; i >= 0 && k < DEC_SEEN; i--) {
		struct brief_decision *d = &decs[k];
		char st[16], dev[NAME_LEN], tgt[NAME_LEN], origin[64], aut[8];
		long created;

		if (act_field(ids[i], "state", st, sizeof(st)) < 0)
			continue;
		act_field(ids[i], "created", b, sizeof(b));
		created = strtol(b, NULL, 10);
		d->state = !strcmp(st, "proposed") ? BD_PROPOSED : !strcmp(st, "active") ? BD_ACTIVE :
			   !strcmp(st, "kept") ? BD_KEPT : BD_ENDED;
		if (d->state == BD_ENDED && (created < a->from - 3600 || created > a->to))
			continue;
		d->id = ids[i];
		act_field(ids[i], "action", dec_action[k], sizeof(dec_action[k]));
		act_field(ids[i], "device", dev, sizeof(dev));
		act_field(ids[i], "target", tgt, sizeof(tgt));
		act_field(ids[i], "origin", origin, sizeof(origin));
		act_field(ids[i], "automatic", aut, sizeof(aut));
		act_field(ids[i], "reason", dec_reason[k], sizeof(dec_reason[k]));
		if (!a->names)
			dec_reason[k][0] = '\0';        /* a reason can name the site behind an alert: it stays home */
		snprintf(dec_words[k], sizeof(dec_words[k]), "%.95s", dev[0] ? dev : tgt);
		d->action = dec_action[k];
		d->words = dec_words[k];
		d->reason = dec_reason[k];
		d->t = created;
		d->by_aigw = atoi(aut) || !strncmp(origin, "policy ", 7) || !strncmp(origin, "mesh", 4);
		k++;
	}
	in->dec = decs;
	in->ndec = k;
	/* an area outage cortex told of */
	{
		static char held[200];
		struct area_now an;

		if (area_get(&an)) {
			area_held_words(held, sizeof(held));
			in->area = an.incident;
			in->area_kind = an.kind;
			in->area_since = an.since;
			in->area_homes = an.homes;
			in->area_of = an.of;
			in->area_held = held;
		}
	}
}

/* ---------- the answer ---------- */

static struct brief B;
static struct brief_meta M;

static void make(void)
{
	struct brief_in in;
	long now = (long)ag_wall_s();
	char err[200];
	int i;

	gather(&in, &Q.a, now);
	brief_judge(&in, &B);
	memset(&M, 0, sizeof(M));
	M.from = Q.a.from;
	M.to = Q.a.to;
	M.checked = Q.have_check;
	M.speed_test = Q.speed_ran;
	M.names = Q.a.names;
	M.data_age = Q.have_check ? 0 : in.last_sample ? now - in.last_sample : -1;
	if (M.data_age < 0 && in.care_open)
		M.data_age = 0;
	/* only proposed, never applied */
	if (B.fix.possible && !strcmp(B.fix.action, "approve"))
		M.decision = B.fix.decision;
	else if (B.fix.possible && Q.a.propose) {
		char origin[64], reason[300];
		uint32_t id = 0;
		int rc;

		snprintf(origin, sizeof(origin), "help desk (%.40s)", Q.a.who[0] ? Q.a.who : "you");
		snprintf(reason, sizeof(reason), "The help desk's brief: %.200s", B.verdict);
		if (!Q.a.names) {
			names_build();
			scrub(reason, sizeof(reason));
		}
		if (!strcmp(B.fix.action, "sqm"))
			rc = sqm_propose(origin, reason, &id, err, sizeof(err));
		else
			rc = act_propose(B.fix.action, B.fix.target, 86400, origin, reason, &id, err, sizeof(err));
		if (rc == 0 && id) {
			M.decision = id;
			M.proposed = 1;
			AG_INFO("brief: proposed %s as decision %u (waits for a yes)", B.fix.action, id);
		} else
			snprintf(M.propose_error, sizeof(M.propose_error), "%.159s", rc < 0 ? err : "not proposed");
	}
	/* names out */
	if (!Q.a.names) {
		names_build();
		scrub(B.verdict, sizeof(B.verdict));
		scrub(B.tell, sizeof(B.tell));
		scrub(B.fix.text, sizeof(B.fix.text));
		scrub(B.visit_why, sizeof(B.visit_why));
		for (i = 0; i < B.nev; i++)
			scrub(B.ev[i].text, sizeof(B.ev[i].text));
		for (i = 0; i < B.ndone; i++)
			scrub(B.done[i].text, sizeof(B.done[i].text));
		if (strcmp(B.fix.action ? B.fix.action : "", "sqm"))
			B.fix.target[0] = '\0';         /* a device's MAC: the decision number is enough */
	}
	AG_INFO("brief%s%s: %s %s (%s), visit %s", Q.a.host[0] && Q.a.names ? " " : "", Q.a.names ? Q.a.host : "", brief_fault_name(B.fault),
		B.code, brief_conf_name(B.confidence), brief_visit_name(B.visit));
}

static void finish(void)
{
	brief_cb cb = Q.cb;
	void *ctx = Q.ctx;

	uloop_timeout_cancel(&Q.t);
	make();
	Q.busy = 0;
	if (!C.since)
		C.since = (long)ag_wall_s();
	C.total++;
	C.fault[B.fault]++;
	C.visits += B.visit == BV_YES;
	C.remote += B.fix.possible;
	C.proposed += M.proposed;
	proactive_on_called();          /* somebody asked: an open ticket wasn't in time */
	dev_state_save();
	cb(ctx, &B, &M);
}

/* proactive care: the brief of a period from what is known now (no fresh check), the ISP's names rule */
int brief_quick(long from, long to, const char *host, struct brief *out)
{
	struct brief_ask a;

	if (!cfg.brief || Q.busy)
		return -1;
	memset(&a, 0, sizeof(a));
	a.from = from;
	a.to = to;
	if (host && act_find_device(host))
		snprintf(a.host, sizeof(a.host), "%s", host);
	a.names = cfg.brief_names;
	a.hide_calls = !cfg.calls_share;
	snprintf(a.who, sizeof(a.who), "proactive care");
	Q.a = a;
	Q.have_check = Q.speed_ran = 0;
	make();
	*out = B;
	return 0;
}

/* a text for the ISP: the home's device names, MACs, addresses and sites out (unless brief.names) */
void brief_scrub_isp(char *text, size_t n)
{
	if (cfg.brief_names)
		return;
	names_build();
	scrub(text, n);
}

static long jnum(struct blob_attr *a)
{
	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (long)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (long)blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	}
	return 0;
}

static void counts_blob(struct blob_buf *b, const char *name)
{
	void *t = blobmsg_open_table(b, name), *f;
	int i;

	blobmsg_add_u32(b, "total", C.total);
	f = blobmsg_open_table(b, "fault");
	for (i = 0; i < BF_N; i++)
		blobmsg_add_u32(b, brief_fault_name(i), C.fault[i]);
	blobmsg_close_table(b, f);
	blobmsg_add_u32(b, "visits", C.visits);
	blobmsg_add_u32(b, "remote_fix", C.remote);
	blobmsg_add_u32(b, "proposed", C.proposed);
	blobmsg_add_u64(b, "since", (uint64_t)C.since);
	blobmsg_close_table(b, t);
}

void brief_save(struct blob_buf *b)
{
	counts_blob(b, "brief");
}

void brief_load(struct blob_attr *t)
{
	struct blob_attr *cur, *f;
	size_t rem, r2;
	int i;

	blobmsg_for_each_attr(cur, t, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "total")) C.total = (uint32_t)jnum(cur);
		else if (!strcmp(k, "visits")) C.visits = (uint32_t)jnum(cur);
		else if (!strcmp(k, "remote_fix")) C.remote = (uint32_t)jnum(cur);
		else if (!strcmp(k, "proposed")) C.proposed = (uint32_t)jnum(cur);
		else if (!strcmp(k, "since")) C.since = jnum(cur);
		else if (!strcmp(k, "fault") && blobmsg_type(cur) == BLOBMSG_TYPE_TABLE)
			blobmsg_for_each_attr(f, cur, r2)
				for (i = 0; i < BF_N; i++)
					if (!strcmp(blobmsg_name(f), brief_fault_name(i)))
						C.fault[i] = (uint32_t)jnum(f);
	}
}

int brief_parse_period(const char *words, long *from, long *to, char *err, size_t en)
{
	if (brief_period(words, (long)ag_wall_s(), from, to) < 0) {
		snprintf(err, en, "a period is \"now\", \"last hour\", \"last 2h\", \"tonight 20:00-22:00\", "
			 "\"yesterday 19:00-23:00\" or \"since 19:30\" (up to 24 hours)");
		return -1;
	}
	return 0;
}

static void step(struct uloop_timeout *t)
{
	double mbps;

	(void)t;
	if (Q.want_speed && care_speed_last(&mbps) > Q.speed_before) {
		Q.want_speed = 0;
		Q.speed_ran = 1;
	}
	if (Q.want_speed && ag_wall_s() - Q.started > SPEED_WAIT_S)
		Q.want_speed = 0;               /* answer without it */
	if (Q.want_check || Q.want_speed) {
		uloop_timeout_set(&Q.t, 500);
		return;
	}
	finish();
}

static void on_check(void *ctx, const struct diag_result *r, const struct diag_input *in)
{
	(void)ctx;
	Q.check = *r;
	Q.have_check = 1;
	Q.check_loss = in->inet.sent ? in->inet.lost * 100 / in->inet.sent : 100;
	Q.check_inet = in->inet.rtt_ms;
	Q.check_gw = in->gw_ping.rtt_ms;
	Q.check_dns = in->dns == 1 ? in->dns_ms : -1;
	Q.want_check = 0;
	uloop_timeout_set(&Q.t, 1);
}

int brief_start(const struct brief_ask *a, brief_cb cb, void *ctx, char *err, size_t en)
{
	if (!cfg.brief) {
		snprintf(err, en, "the help-desk brief is off (brief.enabled)");
		return -1;
	}
	if (Q.busy) {
		snprintf(err, en, "a brief is being made; ask again in a few seconds");
		return -1;
	}
	if (a->to <= a->from || a->to - a->from > 86400) {
		snprintf(err, en, "a period of up to 24 hours, its start before its end");
		return -1;
	}
	if (a->host[0] && !act_find_device(a->host)) {
		snprintf(err, en, "no device called %s", a->host);
		return -1;
	}
	memset(&Q.check, 0, sizeof(Q.check));
	Q.busy = 1;
	Q.a = *a;
	Q.cb = cb;
	Q.ctx = ctx;
	Q.have_check = Q.speed_ran = 0;
	Q.want_check = a->check && diag_check(on_check, NULL) == 0;
	Q.speed_before = care_speed_last(NULL);
	Q.want_speed = a->speed && care_speed_now() == 0;
	Q.started = ag_wall_s();
	Q.t.cb = step;
	uloop_timeout_set(&Q.t, Q.want_check || Q.want_speed ? 500 : 1);
	return 0;
}

/* ---------- output ---------- */

static void lines(struct blob_buf *b, const char *name, const struct brief_line *l, int n, long to)
{
	void *a = blobmsg_open_array(b, name), *t;
	char c[16];
	int i;

	for (i = 0; i < n; i++) {
		t = blobmsg_open_table(b, NULL);
		if (l[i].t) {
			blobmsg_add_u64(b, "time", (uint64_t)l[i].t);
			why_clock(l[i].t, to, c, sizeof(c));
			blobmsg_add_string(b, "clock", c);
		}
		blobmsg_add_string(b, "source", l[i].source);
		blobmsg_add_string(b, "text", l[i].text);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

void brief_blob(struct blob_buf *b, const struct brief *r, const struct brief_meta *m)
{
	void *t;

	blobmsg_add_string(b, "fault", brief_fault_name(r->fault));
	blobmsg_add_string(b, "code", r->code);
	blobmsg_add_string(b, "confidence", brief_conf_name(r->confidence));
	blobmsg_add_u8(b, "current", (uint8_t)r->current);
	blobmsg_add_string(b, "verdict", r->verdict);
	lines(b, "evidence", r->ev, r->nev, (long)ag_wall_s());
	lines(b, "done", r->done, r->ndone, (long)ag_wall_s());
	blobmsg_add_string(b, "tell", r->tell);
	t = blobmsg_open_table(b, "fix");
	blobmsg_add_u8(b, "possible", (uint8_t)r->fix.possible);
	if (r->fix.action)
		blobmsg_add_string(b, "action", r->fix.action);
	if (r->fix.target[0])
		blobmsg_add_string(b, "target", r->fix.target);
	blobmsg_add_string(b, "text", r->fix.text);
	if (m->decision)
		blobmsg_add_u32(b, "decision", m->decision);
	blobmsg_add_u8(b, "proposed", (uint8_t)m->proposed);
	if (m->propose_error[0])
		blobmsg_add_string(b, "error", m->propose_error);
	blobmsg_close_table(b, t);
	blobmsg_add_string(b, "visit", brief_visit_name(r->visit));
	blobmsg_add_string(b, "visit_reason", r->visit_why);
	blobmsg_add_u64(b, "from", (uint64_t)m->from);
	blobmsg_add_u64(b, "to", (uint64_t)m->to);
	blobmsg_add_u8(b, "checked", (uint8_t)m->checked);
	blobmsg_add_u8(b, "speed_test", (uint8_t)m->speed_test);
	blobmsg_add_u8(b, "names", (uint8_t)m->names);
	if (m->data_age >= 0)
		blobmsg_add_u32(b, "data_age_s", (uint32_t)m->data_age);
}

/*
 * For the bridge's report to cortex: the last hour, from what is known (no
 * fresh check: the report must not wait), with the ISP's names rule. cortex
 * answers its help desk from it and says how old it is.
 */
void brief_report_blob(struct blob_buf *b)
{
	struct brief_ask a;
	long now = (long)ag_wall_s();
	void *t;

	if (!cfg.brief || Q.busy)
		return;
	memset(&a, 0, sizeof(a));
	a.from = now - 3600;
	a.to = now;
	a.names = cfg.brief_names;
	a.hide_calls = !cfg.calls_share;
	snprintf(a.who, sizeof(a.who), "cortex");
	Q.a = a;
	Q.have_check = Q.speed_ran = 0;
	make();
	t = blobmsg_open_table(b, "brief");
	brief_blob(b, &B, &M);
	blobmsg_add_u64(b, "made", (uint64_t)now);
	{
		char serial[64];

		dm_serial(serial, sizeof(serial));
		blobmsg_add_string(b, "serial", serial);        /* TR-069's SerialNumber: the help desk looks homes up by it */
	}
	counts_blob(b, "asked");
	blobmsg_close_table(b, t);
}
