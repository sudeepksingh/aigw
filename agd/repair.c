/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Proven self-repair (ag/repair.h, docs/proven-repair.md): every fix AIGW takes
 * or proposes gets a receipt.
 *
 *   decision proposed ──► shadow receipt (advise mode): the prediction, a confidence;
 *         │               measured without the fix: needed, or not needed
 *         ▼
 *   decision active ────► receipt: the prediction, the value before; measured from
 *         │               settle to the deadline (or its own trial's numbers)
 *         ▼
 *   judged: helped / no change / worse / unknown ──► didn't help: undone (enforce
 *                                                    mode, AIGW's own fixes), the
 *                                                    exact state before put back
 *
 * Fixes it judges: channel, stay24, sqm, meshsteer, meshchan (act.c's actions;
 * nothing here acts but act_undo_because). A fix with a trial of its own
 * (Self-Healing Wi-Fi's channel, a planned channel move, smart queueing on
 * bufferbloat, mesh moves) keeps it: the trial decides keep or undo, and gives
 * the receipt its numbers. A fix without one (a device kept on 2.4 GHz, a
 * channel by another policy) is measured and undone here.
 *
 * Receipts (at most RCPT_MAX, the oldest judged go first) and the evidence per
 * fault and kind of fix (31 days) are kept with agd's state.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/uloop.h>

#include "agd.h"
#include "ag/care.h"
#if AIGW_FEATURE_WIFI_HEAL
#include "ag/heal.h"
#endif
#include "ag/repair.h"

#define RCPT_MAX     64
#define ROW_MAX      24
#define HINT_MAX     8
#define HINT_S       300                /* a prediction from the part that proposes waits this long for its decision */
#define TRIAL_MAX_S  (3 * 86400)        /* a trial of its own not judged by then: unknown */
#define LEARN_S      60                 /* the home's Wi-Fi normal: a sample a minute */
#define LINE_BEFORE  3600               /* the line before a fix: the last hour's full-line delay */

struct receipt {
	uint32_t id, decision;
	int shadow;                     /* 1: not applied (what AIGW would have done) */
	int result;                     /* enum repair_result */
	int met, undone, automatic, trial, confidence, calls, gone, confounded, by_person, undoing;
	int trial_said;                 /* its trial: 1 kept, 0 undone, -1 couldn't judge, -2 hasn't said */
	char action[16], fault[24], subject[24];
	char target[64], scope[40], device[64], origin[48];
	char undone_by[40];
	struct repair_pred p;
	float t_before, t_after;        /* its trial's numbers */
	float after, normal_after, gain, gain_pct;
	double sum;
	int n;
	float last;
	int64_t created, applied, settle, deadline, judged;
	char prediction[200];
	char summary[360];
};

static struct receipt R[RCPT_MAX];
static struct repair_row rows[ROW_MAX];
static uint32_t next_id = 1, next_row = 1;
static struct care_norm wn_hour[24], wn_all;    /* the home's Wi-Fi loss, by hour (care's rule) */
#if AIGW_FEATURE_WIFI_HEAL
static int64_t last_learn;
#endif
static struct uloop_timeout tmr;

static struct {
	char action[16], target[64], fault[24];
	int metric;
	float before, expected;
	long within;
	int64_t at;
} hints[HINT_MAX];

static int known(float v)
{
	return v > REPAIR_NONE + 1;
}

/* the fixes it judges (security actions are not fixes of an experience) */
int repair_kind(const char *a)
{
	return a && (!strcmp(a, "channel") || !strcmp(a, "stay24") || !strcmp(a, "sqm") || !strcmp(a, "meshsteer") ||
		     !strcmp(a, "meshchan"));
}

static long day_of(int64_t t)
{
	return (long)(t / 86400);
}

static int hour_of(int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	localtime_r(&tt, &tm);
	return tm.tm_hour;
}

static struct receipt *find(uint32_t decision, int shadow_too)
{
	int i;

	for (i = 0; decision && i < RCPT_MAX; i++)
		if (R[i].id && R[i].decision == decision && (shadow_too || !R[i].shadow))
			return &R[i];
	return NULL;
}

static struct receipt *by_id(uint32_t id)
{
	int i;

	for (i = 0; id && i < RCPT_MAX; i++)
		if (R[i].id == id)
			return &R[i];
	return NULL;
}

/* how many receipts are kept: RCPT_MAX (AIGW_TEST_REPAIR_MAX, smaller, for the test of the bound) */
static int rmax(void)
{
	const char *t = getenv("AIGW_TEST_REPAIR_MAX");
	int n = t ? atoi(t) : 0;

	return n >= 1 && n < RCPT_MAX ? n : RCPT_MAX;
}

/* a free slot: else the oldest judged; else the oldest of all (it lapses unjudged) */
static struct receipt *slot_new(void)
{
	struct receipt *o = NULL;
	int i, m = rmax();

	for (i = 0; i < m; i++)
		if (!R[i].id)
			return &R[i];
	for (i = 0; i < m; i++)
		if (R[i].result != RR_PENDING && (!o || R[i].id < o->id))
			o = &R[i];
	if (o)
		return o;
	for (i = 0; i < m; i++)
		if (!o || R[i].id < o->id)
			o = &R[i];
	return o;
}

static struct repair_row *row_for(const char *fault, const char *action, int create)
{
	struct repair_row *o = NULL;
	long newest_o = 0;
	int i, k;

	for (i = 0; i < ROW_MAX; i++)
		if (rows[i].id && !strcmp(rows[i].fault, fault) && !strcmp(rows[i].action, action))
			return &rows[i];
	if (!create)
		return NULL;
	/* a free row, else the one quiet the longest */
	for (i = 0; i < ROW_MAX; i++) {
		long newest = 0;

		if (!rows[i].id) {
			o = &rows[i];
			break;
		}
		for (k = 0; k < REPAIR_DAYS; k++)
			if (rows[i].d[k].day > newest)
				newest = rows[i].d[k].day;
		if (!o || newest < newest_o) {
			o = &rows[i];
			newest_o = newest;
		}
	}
	memset(o, 0, sizeof(*o));
	snprintf(o->fault, sizeof(o->fault), "%s", fault);
	snprintf(o->action, sizeof(o->action), "%s", action);
	o->id = next_row++;
	return o;
}

/* one kind of fix (all faults), or everything (action NULL), over the last days */
static void sum_kind(const char *action, int days, struct repair_sum *out)
{
	long today = day_of(ag_wall_s());
	struct repair_sum s;
	int i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < ROW_MAX; i++)
		if (rows[i].id && (!action || !strcmp(rows[i].action, action))) {
			repair_sum_row(&rows[i], today, days, &s);
			repair_sum_add(out, &s);
		}
	repair_sum_rates(out);
}

/* ---------- measuring ---------- */

/* AIGW_TEST_REPAIR_FILE: "<metric> <subject> <value|gone|none>" lines stand in for the measurements (tests) */
static int test_probe(int metric, const char *subject, float *v)
{
	const char *f = getenv("AIGW_TEST_REPAIR_FILE");
	char line[160], m[32], s[64], val[32];
	int rc = 1;
	FILE *fp;

	if (!f || !*f || !(fp = fopen(f, "r")))
		return 1;
	while (fgets(line, sizeof(line), fp))
		if (sscanf(line, "%31s %63s %31s", m, s, val) == 3 && !strcmp(m, repair_metric_name(metric)) &&
		    !strcasecmp(s, subject)) {
			rc = !strcmp(val, "gone") ? -2 : !strcmp(val, "none") ? -1 : 0;
			if (!rc)
				*v = strtof(val, NULL);
		}
	fclose(fp);
	return rc;
}

/* the metric now: 0 with *v, -1 not measured now, -2 its subject is gone */
static int probe(int metric, const char *subject, int64_t from, float *v)
{
	int rc = test_probe(metric, subject, v);

	(void)from;
	if (rc <= 0)
		return rc;
	switch (metric) {
	case RM_DEV_LOSS:
	case RM_HOPS:
	case RM_SIGNAL: {
		struct device *d = dev_find_mac(subject, 0);
		int i;

		if (metric == RM_SIGNAL) {
			struct mesh_seen ms;

			for (i = 0; mesh_seen_at(i, &ms) == 0; i++)
				if (!strcasecmp(ms.mac, subject) && ms.dbm) {
					*v = (float)ms.dbm;
					return 0;
				}
		}
		if (!d || !d->wifi_connected)
			return -2;
		if (metric == RM_SIGNAL) {
			*v = (float)d->wifi_signal;
			return d->wifi_signal ? 0 : -1;
		}
		if (metric == RM_HOPS) {
			int h = heal_roams(d);

			*v = (float)h;
			return h < 0 ? -1 : 0;
		}
		{
			int loss, rtt, n;

			heal_device(d, &loss, &rtt, &n);
			*v = (float)loss;
			return n > 0 ? 0 : -1;
		}
	}
	case RM_RADIO_LOSS: {
		const struct heal_radio *r;

		if (!wifi_ap_find(subject))
			return -2;
		r = heal_for(subject);
		if (!r || !r->measured)
			return -1;
		*v = (float)r->loss_pct;
		return 0;
	}
	case RM_AIR:
		if (!wifi_ap_find(subject))
			return -2;
		return chan_air_now(subject, v);
	case RM_FULL_MS:
		return sqm_full_ms(from, ag_wall_s() + 1, v);
	case RM_RETRY: {
		struct mesh_seen ms;
		int i;

		for (i = 0; mesh_seen_at(i, &ms) == 0; i++)
			if (!strcasecmp(ms.mac, subject) && ms.retry_pct >= 0) {
				*v = (float)ms.retry_pct;
				return 0;
			}
		return -1;
	}
	}
	return -1;
}

/* the home's normal at an hour for a metric; REPAIR_NONE when it has none */
static float normal_at(int metric, int hour)
{
	float mean, dev;

	if (metric == RM_DEV_LOSS || metric == RM_RADIO_LOSS)
		return care_norm_blend(&wn_hour[hour], &wn_all, &mean, &dev) == 0 ? mean : REPAIR_NONE;
	if (metric == RM_FULL_MS && care_normal_hour(CM_INET_MS, hour, &mean) == 0)
		return mean;
	return REPAIR_NONE;
}

/* the home's Wi-Fi loss, while all is well: the normal fixes are measured against */
static void learn(int64_t now)
{
#if AIGW_FEATURE_WIFI_HEAL
	float sum = 0;
	int i, n = 0;

	if (now - last_learn < LEARN_S)
		return;
	last_learn = now;
	for (i = 0; i < RCPT_MAX; i++)
		if (R[i].id && R[i].result == RR_PENDING && (R[i].p.metric == RM_DEV_LOSS || R[i].p.metric == RM_RADIO_LOSS))
			return;                 /* a fault being fixed is never normal */
	for (i = 0; i < wifi_n_aps; i++) {
		const struct heal_radio *r = heal_for(wifi_aps[i].iface);

		if (!r || !r->measured)
			continue;
		if (r->status != HEAL_GOOD)
			return;
		sum += (float)r->loss_pct;
		n++;
	}
	if (n)
		care_norm_add(&wn_hour[hour_of(now)], &wn_all, sum / (float)n);
#else
	(void)now;
#endif
}

/* ---------- the prediction ---------- */

void repair_expect(const char *action, const char *target, const char *fault, int metric, float before, float expected,
		   long within_s)
{
	int i, o = 0;

	if (!cfg.repair || !action || !target)
		return;
	for (i = 0; i < HINT_MAX; i++)
		if (hints[i].at < hints[o].at)
			o = i;
	snprintf(hints[o].action, sizeof(hints[o].action), "%s", action);
	snprintf(hints[o].target, sizeof(hints[o].target), "%s", target);
	snprintf(hints[o].fault, sizeof(hints[o].fault), "%s", fault ? fault : "");
	hints[o].metric = metric;
	hints[o].before = before;
	hints[o].expected = expected;
	hints[o].within = within_s;
	hints[o].at = ag_wall_s();
}

static int hint_for(const char *action, const char *target)
{
	int64_t now = ag_wall_s();
	int i;

	for (i = 0; i < HINT_MAX; i++) {
		const struct device *d;

		if (!hints[i].at || now - hints[i].at > HINT_S || strcmp(hints[i].action, action))
			continue;
		if (!strcmp(action, "sqm") || !strcasecmp(hints[i].target, target))
			return i;
		d = act_find_device(hints[i].target);
		if (d && (!strcasecmp(d->mac, target) || !strcmp(d->ip, target)))
			return i;
	}
	return -1;
}

static void subject_words(const struct receipt *r, char *buf, size_t n)
{
	const struct wifi_ap *a;

	if (!strcmp(r->action, "sqm"))
		snprintf(buf, n, "the line");
	else if (!strcmp(r->action, "channel") && (a = wifi_ap_find(r->subject)))
		snprintf(buf, n, "Wi-Fi %s GHz (%s)", a->band == 5 ? "5" : "2.4", a->iface);
	else if (!strcmp(r->action, "meshchan"))
		snprintf(buf, n, "mesh radio %s", r->subject);
	else {
		const struct device *d = dev_find_mac(r->subject, 0);

		snprintf(buf, n, "%s", d && d->hostname[0] ? d->hostname : r->device[0] ? r->device : r->subject);
	}
}

static void fault_of(struct receipt *r, uint32_t alert)
{
	char k[32] = "";

	if (alert && alerts_field(alert, "kind", k, sizeof(k)) == 0 && k[0])
		snprintf(r->fault, sizeof(r->fault), "%.23s", k);
	else if (!strncmp(r->origin, "you", 3))
		snprintf(r->fault, sizeof(r->fault), "manual");
	else if (!strncmp(r->origin, "plug-in", 7))
		snprintf(r->fault, sizeof(r->fault), "plugin");
	else if (!strncmp(r->origin, "policy ", 7))
		snprintf(r->fault, sizeof(r->fault), "%.*s", (int)strcspn(r->origin + 7, ", "), r->origin + 7);
	else
		snprintf(r->fault, sizeof(r->fault), "%.*s", (int)strcspn(r->origin, ", "), r->origin);
}

/* what a decision's receipt measures, when the part that proposed it didn't say */
static void default_metric(struct receipt *r)
{
	const char *t = r->target;

	if (!strcmp(r->action, "stay24")) {
		snprintf(r->subject, sizeof(r->subject), "%.17s", t);
		r->p.metric = strcasestr(r->fault, "hop") ? RM_HOPS : RM_DEV_LOSS;
	} else if (!strcmp(r->action, "channel")) {
		snprintf(r->subject, sizeof(r->subject), "%.*s", (int)strcspn(t, ":"), t);
		r->p.metric = strstr(r->origin, "wifi_plan") ? RM_AIR : RM_RADIO_LOSS;
	} else if (!strcmp(r->action, "sqm")) {
		snprintf(r->subject, sizeof(r->subject), "%.*s", (int)strcspn(t, ":"), t);
		r->p.metric = RM_FULL_MS;
	} else if (!strcmp(r->action, "meshsteer")) {
		snprintf(r->subject, sizeof(r->subject), "%.17s", t);
		r->p.metric = RM_SIGNAL;
	} else {
		snprintf(r->subject, sizeof(r->subject), "%.*s", (int)strcspn(t, ":"), t);
		r->p.metric = RM_RETRY;
	}
}

/* its own trial decides keep or undo: AIGW's channel healing and plan, SQM on bufferbloat (and proactive care's
   with proven repair, sqm.c), every mesh move */
static int has_trial(const struct receipt *r)
{
	if (!strcmp(r->action, "channel"))
		return strstr(r->origin, "wifi_congestion") || strstr(r->origin, "wifi_plan");
	if (!strcmp(r->action, "sqm"))
		return strstr(r->origin, "bufferbloat") || strstr(r->origin, "proactive");
	return !strcmp(r->action, "meshsteer") || !strcmp(r->action, "meshchan");
}

/* the trial's nominal length, for the prediction's words (0: "when its trial is judged") */
static long trial_len(const struct receipt *r)
{
	if (!strcmp(r->action, "channel"))
		return strstr(r->origin, "wifi_plan") ? 0 : cfg.heal_trial;
	if (!strcmp(r->action, "sqm"))
		return 0;
	return cfg.mesh_trial;
}

/* the prediction, now: before measured (or the proposer's), the home's normal for the hour, what to expect */
static void predict(struct receipt *r, int h, int64_t now)
{
	float v, hint = REPAIR_NONE;
	char who[80];

	r->p.before = REPAIR_NONE;
	if (h >= 0) {
		if (hints[h].metric > RM_NONE)
			r->p.metric = hints[h].metric;
		r->p.before = hints[h].before;
		hint = hints[h].expected;
	}
	if (!known(r->p.before) && probe(r->p.metric, r->subject, now - LINE_BEFORE, &v) == 0)
		r->p.before = v;
	r->p.normal = normal_at(r->p.metric, hour_of(now));
	r->trial = !r->shadow && has_trial(r);
	r->p.within_s = h >= 0 && hints[h].within > 0 ? hints[h].within : r->trial ? trial_len(r) : cfg.repair_window;
	repair_predict(&r->p, hint);
	subject_words(r, who, sizeof(who));
	repair_pred_words(&r->p, who, r->prediction, sizeof(r->prediction));
}

static void receipt_event(const struct receipt *r, const char *what);

static void open_receipt(uint32_t decision, int shadow, int64_t now)
{
	struct receipt *r = slot_new();
	char buf[400];
	struct repair_sum month;
	int h;

	if (!r)
		return;
	memset(r, 0, sizeof(*r));
	r->id = next_id++;
	r->decision = decision;
	r->shadow = shadow;
	r->result = RR_PENDING;
	r->trial_said = -2;
	r->t_before = r->t_after = REPAIR_NONE;
	r->after = r->normal_after = REPAIR_NONE;
	r->created = now;
	act_field(decision, "action", r->action, sizeof(r->action));
	act_field(decision, "target", r->target, sizeof(r->target));
	act_field(decision, "scope", r->scope, sizeof(r->scope));
	act_field(decision, "device", r->device, sizeof(r->device));
	act_field(decision, "origin", r->origin, sizeof(r->origin));
	if (act_field(decision, "automatic", buf, sizeof(buf)) == 0)
		r->automatic = atoi(buf);
	act_field(decision, "alert", buf, sizeof(buf));
	fault_of(r, (uint32_t)strtoul(buf, NULL, 10));
	h = hint_for(r->action, r->target);
	if (h >= 0 && hints[h].fault[0])
		snprintf(r->fault, sizeof(r->fault), "%s", hints[h].fault);
	default_metric(r);
	predict(r, h, now);
	if (h >= 0)
		hints[h].at = 0;
	r->applied = shadow ? 0 : now;
	r->settle = now + cfg.repair_settle;
	r->deadline = now + (r->trial ? TRIAL_MAX_S : r->p.within_s > cfg.repair_window ? r->p.within_s : cfg.repair_window);
	sum_kind(r->action, 30, &month);
	r->confidence = repair_confidence(&month);
	AG_INFO("repair: receipt %u for decision %u (%s %s%s): %s", r->id, decision, r->action, r->target,
		shadow ? ", a shadow: not applied" : "", r->prediction);
	receipt_event(r, shadow ? "shadow" : "predicted");
}

/* ---------- after: the verdict ---------- */

/* the fault as fleet learning buckets it: a ticket's code as it is, else the code act.c gives the kind of fix */
static const char *fleet_code(const struct receipt *r)
{
	const char *a = r->action;

	if (isupper((unsigned char)r->fault[0]))
		return r->fault;
	return !strcmp(a, "channel") ? "WIFI_CONGESTION" : !strcmp(a, "stay24") ? "WIFI_HOPPING" :
	       !strcmp(a, "sqm") ? "LINE_FULL" : !strcmp(a, "meshchan") ? "MESH_BACKHAUL" : "WIFI_COVERAGE";
}

/* AIGW's own fix (a policy's, proactive care's, the mesh's, or one AIGW proposed and a person approved): a
   person's own request, or a plug-in's, is theirs to undo */
static int ours(const struct receipt *r)
{
	return r->automatic || strstr(r->origin, "approved by");
}

static void finish(struct receipt *r, int64_t now)
{
	struct repair_params P;
	struct repair_obs o;
	struct repair_verdict v;
	struct repair_row *row;
	char state[16] = "", note[400], who[80];
	float pg = REPAIR_NONE;
	size_t l;

	repair_params_default(&P);
	memset(&o, 0, sizeof(o));
	o.trial = r->trial_said >= 0 ? r->trial_said : -1;
	if (r->trial && known(r->t_after)) {
		o.after = r->t_after;
		o.n = P.min_samples;
		if (!known(r->p.before))
			r->p.before = r->t_before;
	} else {
		o.after = r->p.metric == RM_FULL_MS ? r->last : r->n ? (float)(r->sum / r->n) : REPAIR_NONE;
		o.n = r->n;
		if (r->p.metric == RM_FULL_MS && r->n)
			o.n = P.min_samples;    /* one figure over the whole window */
	}
	o.normal_after = normal_at(r->p.metric, hour_of(now));
	o.gone = r->gone || r->trial_said == -1;
	o.confounded = r->confounded;
	o.by_person = r->by_person;
	act_field(r->decision, "state", state, sizeof(state));
	if (!r->shadow && !strcmp(state, "failed")) {
		memset(&v, 0, sizeof(v));
		v.result = RR_UNKNOWN;
		snprintf(v.why, sizeof(v.why), "it could not be applied");
	} else if (r->shadow)
		repair_judge_shadow(&r->p, &o, &P, &v);
	else
		repair_judge(&r->p, &o, &P, &v);
	r->result = v.result;
	r->met = v.met;
	r->gain = v.gain;
	r->gain_pct = v.gain_pct;
	r->after = o.after;
	r->normal_after = o.normal_after;
	r->judged = now;
	subject_words(r, who, sizeof(who));
	l = (size_t)snprintf(r->summary, sizeof(r->summary), "%s: %s", who, v.why);
	if (r->trial_said == -1 && l < sizeof(r->summary))
		l += (size_t)snprintf(r->summary + l, sizeof(r->summary) - l, " (its trial couldn't judge it)");
	if (r->shadow) {
		const struct repair_mdef *d = repair_metric(r->p.metric);

		if (d && known(r->p.before) && known(r->p.expected))
			pg = d->higher_better ? r->p.expected - r->p.before : r->p.before - r->p.expected;
		if (l < sizeof(r->summary))
			snprintf(r->summary + l, sizeof(r->summary) - l, ". A shadow: not applied (%s)",
				 !strcmp(state, "proposed") ? "it waits for a yes" : state[0] ? state : "gone");
	} else if (r->result == RR_NONE || r->result == RR_WORSE) {
		if (!strcmp(state, "tried")) {
			r->undone = 1;
			snprintf(r->undone_by, sizeof(r->undone_by), "its trial");
			if (l < sizeof(r->summary))
				snprintf(r->summary + l, sizeof(r->summary) - l, "; undone by its trial");
		} else if (!strcmp(state, "active") && (!r->trial || r->result == RR_WORSE) && cfg.repair_undo &&
			   cfg.act_mode == AG_MODE_ENFORCE && ours(r)) {
			snprintf(note, sizeof(note), "undone by proven repair (receipt %u): %.300s", r->id, v.why);
			r->undoing = 1;
			if (act_undo_because(r->decision, note) == 0) {
				r->undone = 1;
				snprintf(r->undone_by, sizeof(r->undone_by), "proven repair");
				if (l < sizeof(r->summary))
					snprintf(r->summary + l, sizeof(r->summary) - l, "; undone: what was there before is back");
			}
			r->undoing = 0;
		} else if (!strcmp(state, "active") || !strcmp(state, "kept")) {
			const char *why = r->trial && r->result != RR_WORSE ? "its trial decides" :
					  !cfg.repair_undo ? "repair.undo is off" :
					  cfg.act_mode != AG_MODE_ENFORCE ? "AIGW undoes fixes itself only in enforce mode" :
					  "it was asked for, not AIGW's own";
			if (l < sizeof(r->summary))
				snprintf(r->summary + l, sizeof(r->summary) - l, "; not undone (%s): agctl undo %u", why,
					 r->decision);
		}
	} else if (r->result == RR_HELPED && l < sizeof(r->summary)) {
		snprintf(r->summary + l, sizeof(r->summary) - l, "; %s", !strcmp(state, "kept") ? "kept" :
			 !strcmp(state, "active") ? "it stays" : "it has ended since");
	}
	row = row_for(r->fault, r->action, 1);
	repair_count(row, day_of(now), r->shadow, &v, r->undone, pg);
	/* fleet learning: the measured verdict replaces what act.c guessed for this decision (same key); a
	   shadow was never applied and an unknown says nothing, so neither is an outcome */
	if (!r->shadow && (r->result == RR_HELPED || r->result == RR_NONE || r->result == RR_WORSE))
		outcome_report(fleet_code(r), r->action, r->result == RR_HELPED ? FL_HELPED :
			       r->result == RR_WORSE ? FL_WORSE : FL_NO_CHANGE, r->decision,
			       !strcmp(r->action, "stay24") || !strcmp(r->action, "meshsteer") ? r->subject : "",
			       !strcmp(r->action, "channel") ? r->subject : "");
	AG_INFO("repair: receipt %u (decision %u): %s: %s", r->id, r->decision, repair_result_name(r->result), r->summary);
	receipt_event(r, r->shadow ? "shadow-judged" : "judged");
	dm_on_repair(r->id, 0);
	if (r->undone && !strcmp(r->undone_by, "proven repair")) {
		receipt_event(r, "undone");
		dm_on_repair(r->id, 1);
	}
	dev_state_save();
}

/* act.c: a decision changed (its state's name) */
void repair_on_decision(uint32_t id, const char *state)
{
	char action[48] = "";
	struct receipt *r;
	int64_t now = ag_wall_s();

	if (!cfg.repair || act_field(id, "action", action, sizeof(action)) < 0 || !repair_kind(action))
		return;
	r = find(id, 1);
	if (!strcmp(state, "proposed")) {
		if (!r && cfg.repair_shadow)
			open_receipt(id, 1, now);
		return;
	}
	if (!strcmp(state, "active")) {
		if (r && !r->shadow)
			return;                 /* in effect already (its time restarted) */
		if (r && r->result == RR_PENDING) {
			/* approved: the shadow becomes its receipt, measured from now */
			r->id = 0;
		}
		open_receipt(id, 0, now);
		return;
	}
	if (!r || r->shadow || r->result != RR_PENDING || r->undoing)
		return;
	if (!strcmp(state, "kept")) {
		if (r->trial_said == -2)
			r->trial_said = 1;
		finish(r, now);
	} else if (!strcmp(state, "tried")) {
		if (r->trial_said == -2)
			r->trial_said = 0;
		finish(r, now);
	} else if (!strcmp(state, "reverted")) {
		r->by_person = 1;
		finish(r, now);
	} else if (!strcmp(state, "expired") || !strcmp(state, "failed")) {
		finish(r, now);
	}
}

/* a fix's own trial decided: kept 1, undone 0, couldn't judge -1; its numbers in the receipt's metric */
void repair_trial(uint32_t id, int kept, float before, float after)
{
	struct receipt *r = find(id, 0);

	if (!r || r->result != RR_PENDING)
		return;
	r->trial_said = kept;
	r->t_before = before;
	r->t_after = after;
}

static void tick(int64_t now)
{
	struct area_now an;
	int area = area_get(&an), i;

	learn(now);
	for (i = 0; i < RCPT_MAX; i++) {
		struct receipt *r = &R[i];
		float v;
		int rc;

		if (!r->id || r->result != RR_PENDING)
			continue;
		if (now >= r->settle && now < r->deadline + cfg.repair_tick) {
			rc = probe(r->p.metric, r->subject, r->p.metric == RM_FULL_MS ? r->settle : now, &v);
			if (rc == -2)
				r->gone = 1;
			else if (rc == 0) {
				r->sum += v;
				r->n++;
				r->last = v;
			}
			if (area)
				r->confounded = 1;
		}
		if (now >= r->deadline && (r->shadow || !r->trial || now >= r->created + TRIAL_MAX_S))
			finish(r, now);
	}
}

static void on_timer(struct uloop_timeout *t)
{
	tick(ag_wall_s());
	uloop_timeout_set(t, (cfg.repair_tick > 0 ? cfg.repair_tick : 10) * 1000);
}

void repair_init(void)
{
	int i, n = 0;

	if (!cfg.repair) {
		AG_INFO("proven repair: off (repair.enabled)");
		return;
	}
	for (i = 0; i < RCPT_MAX; i++)
		n += R[i].id && R[i].result == RR_PENDING;
	tmr.cb = on_timer;
	uloop_timeout_set(&tmr, (cfg.repair_tick > 0 ? cfg.repair_tick : 10) * 1000);
	AG_INFO("proven repair: every fix predicted and measured (%ds), %s, shadows %s, confidence gate %s; %d being "
		"measured", cfg.repair_window, cfg.repair_undo ? "undone when it doesn't help (enforce mode)" : "never undone",
		cfg.repair_shadow ? "on" : "off", cfg.repair_gate ? "on" : "off", n);
}

/* ---------- for other parts of agd ---------- */

int repair_proven(uint32_t decision)
{
	const struct receipt *r = find(decision, 0);

	if (!cfg.repair || !r)
		return -2;
	if (r->result == RR_PENDING)
		return -1;
	return r->result == RR_HELPED;
}

void repair_on_ticket(uint32_t decision, int call_avoided)
{
	struct receipt *r = find(decision, 0);
	struct repair_row *row;

	if (!r || !call_avoided || r->calls)
		return;
	r->calls = 1;
	if ((row = row_for(r->fault, r->action, 1)))
		repair_count_call(row, day_of(ag_wall_s()));
}

int repair_gate(const char *action, char *why, size_t n)
{
	struct repair_sum month;

	if (n)
		why[0] = '\0';
	if (!cfg.repair || !cfg.repair_gate || !repair_kind(action))
		return 1;
	sum_kind(action, 30, &month);
	return repair_gate_ok(&month, cfg.repair_gate_rate, cfg.repair_gate_trials, why, n);
}

/* ---------- words and numbers ---------- */

static void value_add(struct blob_buf *b, const char *k, int m, float v)
{
	char t[24];

	if (!known(v))
		return;
	blobmsg_add_double(b, k, (double)v);
	repair_value(m, v, t, sizeof(t));
	{
		char k2[40];

		snprintf(k2, sizeof(k2), "%s_text", k);
		blobmsg_add_string(b, k2, t);
	}
}

static void receipt_blob(struct blob_buf *b, const struct receipt *r, int names)
{
	char text[400];
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "id", r->id);
	blobmsg_add_u32(b, "decision", r->decision);
	blobmsg_add_string(b, "mode", r->shadow ? "shadow" : "applied");
	blobmsg_add_string(b, "action", r->action);
	blobmsg_add_string(b, "fault", r->fault);
	if (names) {
		blobmsg_add_string(b, "target", r->target);
		blobmsg_add_string(b, "device", r->device);
	}
	blobmsg_add_string(b, "origin", r->origin);
	blobmsg_add_string(b, "metric", repair_metric_name(r->p.metric));
	value_add(b, "before", r->p.metric, r->p.before);
	value_add(b, "expected", r->p.metric, r->p.expected);
	value_add(b, "after", r->p.metric, r->after);
	value_add(b, "normal", r->p.metric, r->p.normal);
	blobmsg_add_u32(b, "within_s", (uint32_t)(r->p.within_s > 0 ? r->p.within_s : 0));
	blobmsg_add_string(b, "result", repair_result_name(r->result));
	blobmsg_add_u8(b, "met", (uint8_t)r->met);
	blobmsg_add_u8(b, "undone", (uint8_t)r->undone);
	blobmsg_add_string(b, "undone_by", r->undone_by);
	blobmsg_add_u8(b, "trial", (uint8_t)r->trial);
	if (r->confidence >= 0)
		blobmsg_add_u32(b, "confidence", (uint32_t)r->confidence);
	blobmsg_add_u32(b, "calls_likely_avoided", (uint32_t)r->calls);
	if (r->result != RR_PENDING && r->result != RR_UNKNOWN && !r->shadow)
		blobmsg_add_double(b, "improvement_pct", (double)r->gain_pct);
	blobmsg_add_u64(b, "created", (uint64_t)r->created);
	blobmsg_add_u64(b, "applied", (uint64_t)r->applied);
	blobmsg_add_u64(b, "deadline", (uint64_t)r->deadline);
	blobmsg_add_u64(b, "judged", (uint64_t)r->judged);
	snprintf(text, sizeof(text), "%s", r->prediction);
	if (!names)
		brief_scrub_isp(text, sizeof(text));
	blobmsg_add_string(b, "prediction", text);
	snprintf(text, sizeof(text), "%s", r->summary);
	if (!names)
		brief_scrub_isp(text, sizeof(text));
	blobmsg_add_string(b, "summary", text);
	blobmsg_close_table(b, t);
}

static void sum_blob(struct blob_buf *b, const char *name, const struct repair_sum *s)
{
	void *t = blobmsg_open_table(b, name);

	blobmsg_add_u32(b, "fixes", s->fixes);
	blobmsg_add_u32(b, "judged", s->judged);
	blobmsg_add_u32(b, "helped", s->helped);
	blobmsg_add_u32(b, "no_change", s->none);
	blobmsg_add_u32(b, "worse", s->worse);
	blobmsg_add_u32(b, "unknown", s->unknown);
	blobmsg_add_u32(b, "undone", s->undone);
	if (s->fix_rate >= 0)
		blobmsg_add_u32(b, "fix_rate", (uint32_t)s->fix_rate);
	if (s->undo_rate >= 0)
		blobmsg_add_u32(b, "undo_rate", (uint32_t)s->undo_rate);
	blobmsg_add_double(b, "mean_improvement_pct", (double)s->mean_gain_pct);
	blobmsg_add_u32(b, "calls_likely_avoided", s->calls);
	blobmsg_add_u32(b, "shadows", s->shadows);
	blobmsg_add_u32(b, "shadows_needed", s->needed);
	blobmsg_add_u32(b, "shadows_not_needed", s->not_needed);
	blobmsg_close_table(b, t);
}

static int newest_first(const void *a, const void *b)
{
	const struct receipt *x = *(const struct receipt *const *)a, *y = *(const struct receipt *const *)b;

	return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

static int sorted(const struct receipt **list)
{
	int i, n = 0;

	for (i = 0; i < RCPT_MAX; i++)
		if (R[i].id)
			list[n++] = &R[i];
	qsort(list, (size_t)n, sizeof(list[0]), newest_first);
	return n;
}

static void summary_blob(struct blob_buf *b)
{
	static const struct { const char *name; int days; } per[] = { { "day", 1 }, { "week", 7 }, { "month", 30 } };
	struct repair_sum s;
	void *t = blobmsg_open_table(b, "summary");
	size_t k;

	for (k = 0; k < sizeof(per) / sizeof(per[0]); k++) {
		sum_kind(NULL, per[k].days, &s);
		sum_blob(b, per[k].name, &s);
	}
	blobmsg_close_table(b, t);
}

static void stats_blob(struct blob_buf *b)
{
	long today = day_of(ag_wall_s());
	void *a = blobmsg_open_array(b, "stats");
	int i;

	for (i = 0; i < ROW_MAX; i++) {
		struct repair_sum d, w, m;
		char why[160];
		void *t;

		if (!rows[i].id)
			continue;
		repair_sum_row(&rows[i], today, 1, &d);
		repair_sum_row(&rows[i], today, 7, &w);
		repair_sum_row(&rows[i], today, 30, &m);
		if (!m.fixes && !m.shadows)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "id", rows[i].id);
		blobmsg_add_string(b, "fault", rows[i].fault);
		blobmsg_add_string(b, "action", rows[i].action);
		blobmsg_add_string(b, "gate", repair_gate(rows[i].action, why, sizeof(why)) ? "open" : "held");
		blobmsg_add_string(b, "gate_why", why);
		sum_blob(b, "day", &d);
		sum_blob(b, "week", &w);
		sum_blob(b, "month", &m);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

static void settings_blob(struct blob_buf *b)
{
	void *g;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.repair);
	blobmsg_add_string(b, "mode", ag_mode_str(cfg.act_mode));
	blobmsg_add_u8(b, "undo", (uint8_t)cfg.repair_undo);
	blobmsg_add_u8(b, "shadow", (uint8_t)cfg.repair_shadow);
	blobmsg_add_u32(b, "window_s", (uint32_t)cfg.repair_window);
	blobmsg_add_u32(b, "settle_s", (uint32_t)cfg.repair_settle);
	g = blobmsg_open_table(b, "gate");
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.repair_gate);
	blobmsg_add_u32(b, "fix_rate", (uint32_t)cfg.repair_gate_rate);
	blobmsg_add_u32(b, "trials", (uint32_t)cfg.repair_gate_trials);
	blobmsg_close_table(b, g);
}

/* ag repair {stats, shadow, limit}: what = 0 all, 1 the stats only, 2 the shadows only */
void repair_blob(struct blob_buf *b, int what, unsigned limit)
{
	const struct receipt *list[RCPT_MAX];
	void *a;
	int i, n, k = 0, pending = 0;

	settings_blob(b);
	n = sorted(list);
	for (i = 0; i < n; i++)
		pending += list[i]->result == RR_PENDING;
	blobmsg_add_u32(b, "pending", (uint32_t)pending);
	summary_blob(b);
	stats_blob(b);
	if (what == 1)
		return;
	a = blobmsg_open_array(b, "receipts");
	for (i = 0; i < n && (!limit || (unsigned)k < limit); i++)
		if (what != 2 || list[i]->shadow) {
			receipt_blob(b, list[i], 1);
			k++;
		}
	blobmsg_close_array(b, a);
}

/* the bridge (cortex) and offers: counts, no names */
void repair_report_blob(struct blob_buf *b)
{
	void *t;

	if (!cfg.repair)
		return;
	t = blobmsg_open_table(b, "repair");
	settings_blob(b);
	summary_blob(b);
	stats_blob(b);
	blobmsg_close_table(b, t);
}

/* agctl care: a line on the last week */
void repair_care_blob(struct blob_buf *b)
{
	struct repair_sum w;
	const struct receipt *list[RCPT_MAX];
	void *t;
	int i, n, pending = 0;

	if (!cfg.repair)
		return;
	sum_kind(NULL, 7, &w);
	n = sorted(list);
	for (i = 0; i < n; i++)
		pending += list[i]->result == RR_PENDING;
	t = blobmsg_open_table(b, "repair");
	blobmsg_add_u32(b, "pending", (uint32_t)pending);
	sum_blob(b, "week", &w);
	blobmsg_close_table(b, t);
}

/* agctl why: the fixes made or judged in [from, to) */
void repair_why_blob(struct blob_buf *b, long from, long to)
{
	const struct receipt *list[RCPT_MAX];
	void *a;
	int i, n;

	if (!cfg.repair)
		return;
	n = sorted(list);
	a = blobmsg_open_array(b, "repairs");
	for (i = n - 1; i >= 0; i--)
		if ((list[i]->created >= from && list[i]->created < to) || (list[i]->judged >= from && list[i]->judged < to))
			receipt_blob(b, list[i], 1);
	blobmsg_close_array(b, a);
}

/* the 7 days' proven fixes (offers): helped, judged; Wi-Fi fixes only when wifi */
void repair_week(int wifi, unsigned *helped, unsigned *judged)
{
	struct repair_sum s, t;

	memset(&t, 0, sizeof(t));
	if (cfg.repair) {
		static const char *const kinds[] = { "channel", "stay24", "meshsteer", "meshchan", "sqm" };
		size_t k;

		for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]) - (wifi ? 1 : 0); k++) {
			sum_kind(kinds[k], 7, &s);
			repair_sum_add(&t, &s);
		}
		repair_sum_rates(&t);
	}
	*helped = t.helped;
	*judged = t.judged;
}

static void receipt_event(const struct receipt *r, const char *what)
{
	struct blob_buf b = { 0 };
	const struct device *d = dev_find_mac(r->subject, 0);

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_u32(&b, "receipt", r->id);
	blobmsg_add_u32(&b, "decision", r->decision);
	blobmsg_add_string(&b, "mode", r->shadow ? "shadow" : "applied");
	blobmsg_add_string(&b, "action", r->action);
	blobmsg_add_string(&b, "fault", r->fault);
	blobmsg_add_string(&b, "result", repair_result_name(r->result));
	if (d)
		blobmsg_add_string(&b, "mac", d->mac);
	blobmsg_add_string(&b, "prediction", r->prediction);
	blobmsg_add_string(&b, "summary", r->summary);
	agd_emit(AG_EV_REPAIR, &b);
	blob_buf_free(&b);
}

/* ---------- the data model (Device.X_AIGW_.Repair.) ---------- */

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

int repair_ids(uint32_t *out, int max)
{
	int i, n = 0;

	for (i = 0; i < RCPT_MAX && n < max; i++)
		if (R[i].id)
			out[n++] = R[i].id;
	qsort(out, (size_t)n, sizeof(*out), cmp_u32);
	return n;
}

int repair_stat_ids(uint32_t *out, int max)
{
	long today = day_of(ag_wall_s());
	int i, n = 0;

	for (i = 0; i < ROW_MAX && n < max; i++) {
		struct repair_sum m;

		if (!rows[i].id)
			continue;
		repair_sum_row(&rows[i], today, 30, &m);
		if (m.fixes || m.shadows)
			out[n++] = rows[i].id;
	}
	qsort(out, (size_t)n, sizeof(*out), cmp_u32);
	return n;
}

static void dm_value(int m, float v, char *buf, size_t n)
{
	if (!known(v))
		snprintf(buf, n, "%s", "");
	else
		snprintf(buf, n, "%.1f", v);
	(void)m;
}

/* a receipt's parameter by its TR-181 name; -1 unknown. names 0: the ISP's view (names scrubbed) */
int repair_field(uint32_t id, const char *f, int names, char *buf, size_t n)
{
	const struct receipt *r = by_id(id);

	if (!r)
		return -1;
	if (!strcmp(f, "Decision")) snprintf(buf, n, "%u", r->decision);
	else if (!strcmp(f, "Mode")) snprintf(buf, n, "%s", r->shadow ? "Shadow" : "Applied");
	else if (!strcmp(f, "Action")) snprintf(buf, n, "%s", r->action);
	else if (!strcmp(f, "Fault")) snprintf(buf, n, "%s", r->fault);
	else if (!strcmp(f, "Metric")) snprintf(buf, n, "%s", repair_metric_name(r->p.metric));
	else if (!strcmp(f, "Before")) dm_value(r->p.metric, r->p.before, buf, n);
	else if (!strcmp(f, "Expected")) dm_value(r->p.metric, r->p.expected, buf, n);
	else if (!strcmp(f, "After")) dm_value(r->p.metric, r->after, buf, n);
	else if (!strcmp(f, "Normal")) dm_value(r->p.metric, r->p.normal, buf, n);
	else if (!strcmp(f, "WithinSeconds")) snprintf(buf, n, "%ld", r->p.within_s > 0 ? r->p.within_s : 0L);
	else if (!strcmp(f, "Result")) snprintf(buf, n, "%s", repair_result_name(r->result));
	else if (!strcmp(f, "PredictionMet")) snprintf(buf, n, "%d", r->met);
	else if (!strcmp(f, "Undone")) snprintf(buf, n, "%d", r->undone);
	else if (!strcmp(f, "UndoneBy")) snprintf(buf, n, "%s", r->undone_by);
	else if (!strcmp(f, "Confidence")) snprintf(buf, n, "%d", r->confidence);
	else if (!strcmp(f, "CallsLikelyAvoided")) snprintf(buf, n, "%d", r->calls);
	else if (!strcmp(f, "ImprovementPercent")) snprintf(buf, n, "%.1f", r->shadow ? 0.0 : (double)r->gain_pct);
	else if (!strcmp(f, "Created")) snprintf(buf, n, "%lld", (long long)r->created);
	else if (!strcmp(f, "Deadline")) snprintf(buf, n, "%lld", (long long)r->deadline);
	else if (!strcmp(f, "Judged")) snprintf(buf, n, "%lld", (long long)r->judged);
	else if (!strcmp(f, "Prediction") || !strcmp(f, "Summary")) {
		snprintf(buf, n, "%s", !strcmp(f, "Prediction") ? r->prediction : r->summary);
		if (!names)
			brief_scrub_isp(buf, n);
	} else
		return -1;
	return 0;
}

static int sum_field(const struct repair_sum *s, const char *f, char *buf, size_t n)
{
	if (!strcmp(f, "Fixes")) snprintf(buf, n, "%u", s->fixes);
	else if (!strcmp(f, "Judged")) snprintf(buf, n, "%u", s->judged);
	else if (!strcmp(f, "Helped")) snprintf(buf, n, "%u", s->helped);
	else if (!strcmp(f, "NoChange")) snprintf(buf, n, "%u", s->none);
	else if (!strcmp(f, "Worse")) snprintf(buf, n, "%u", s->worse);
	else if (!strcmp(f, "Unknown")) snprintf(buf, n, "%u", s->unknown);
	else if (!strcmp(f, "Undone")) snprintf(buf, n, "%u", s->undone);
	else if (!strcmp(f, "FixRate")) snprintf(buf, n, "%d", s->fix_rate);
	else if (!strcmp(f, "UndoRate")) snprintf(buf, n, "%d", s->undo_rate);
	else if (!strcmp(f, "MeanImprovement")) snprintf(buf, n, "%.1f", (double)s->mean_gain_pct);
	else if (!strcmp(f, "CallsLikelyAvoided")) snprintf(buf, n, "%u", s->calls);
	else if (!strcmp(f, "Shadows")) snprintf(buf, n, "%u", s->shadows);
	else if (!strcmp(f, "ShadowsNeeded")) snprintf(buf, n, "%u", s->needed);
	else
		return -1;
	return 0;
}

/* a period's name in front: "Week" + "FixRate" */
static int period_field(const char *f, int *days, const char **rest)
{
	static const struct { const char *p; int days; } per[] = { { "Day", 1 }, { "Week", 7 }, { "Month", 30 } };
	size_t k;

	for (k = 0; k < sizeof(per) / sizeof(per[0]); k++)
		if (!strncmp(f, per[k].p, strlen(per[k].p))) {
			*days = per[k].days;
			*rest = f + strlen(per[k].p);
			return 0;
		}
	return -1;
}

/* a stats row's parameter: Fault, Action, Gate, GateWhy, <Day|Week|Month><Fixes|FixRate|...>; -1 unknown */
int repair_stat_field(uint32_t id, const char *f, char *buf, size_t n)
{
	struct repair_row *row = NULL;
	struct repair_sum s;
	const char *rest;
	int i, days;

	for (i = 0; i < ROW_MAX; i++)
		if (rows[i].id && rows[i].id == id)
			row = &rows[i];
	if (!row)
		return -1;
	if (!strcmp(f, "Fault")) snprintf(buf, n, "%s", row->fault);
	else if (!strcmp(f, "Action")) snprintf(buf, n, "%s", row->action);
	else if (!strcmp(f, "Gate") || !strcmp(f, "GateWhy")) {
		char why[160];
		int open = repair_gate(row->action, why, sizeof(why));

		snprintf(buf, n, "%s", !strcmp(f, "Gate") ? (open ? "Open" : "Held") : why);
	} else if (period_field(f, &days, &rest) == 0) {
		repair_sum_row(row, day_of(ag_wall_s()), days, &s);
		return sum_field(&s, rest, buf, n);
	} else
		return -1;
	return 0;
}

/* Device.X_AIGW_.Repair.<name>: settings, pending, and the whole home's <Day|Week|Month><...>; -1 unknown */
int repair_dm_field(const char *f, char *buf, size_t n)
{
	struct repair_sum s;
	const char *rest;
	int days, i, k = 0;

	if (!strcmp(f, "Enable")) snprintf(buf, n, "%d", cfg.repair);
	else if (!strcmp(f, "Undo")) snprintf(buf, n, "%d", cfg.repair_undo);
	else if (!strcmp(f, "Shadow")) snprintf(buf, n, "%d", cfg.repair_shadow);
	else if (!strcmp(f, "Gate")) snprintf(buf, n, "%d", cfg.repair_gate);
	else if (!strcmp(f, "GateFixRate")) snprintf(buf, n, "%d", cfg.repair_gate_rate);
	else if (!strcmp(f, "GateTrials")) snprintf(buf, n, "%d", cfg.repair_gate_trials);
	else if (!strcmp(f, "WindowSeconds")) snprintf(buf, n, "%d", cfg.repair_window);
	else if (!strcmp(f, "Pending")) {
		for (i = 0; i < RCPT_MAX; i++)
			k += R[i].id && R[i].result == RR_PENDING;
		snprintf(buf, n, "%d", k);
	} else if (period_field(f, &days, &rest) == 0) {
		sum_kind(NULL, days, &s);
		return sum_field(&s, rest, buf, n);
	} else
		return -1;
	return 0;
}

/* ---------- state ---------- */

/* a receipt's fields in the state file, by name (a table: one loop saves and loads them all) */
enum { F_INT, F_U32, F_FLOAT, F_TIME, F_LONG, F_DOUBLE, F_STR, F_RESULT, F_METRIC };
#define RF(n, k, m) { n, k, offsetof(struct receipt, m), sizeof(((struct receipt *)0)->m) }
static const struct { const char *name; uint8_t kind; uint16_t off, size; } RFIELDS[] = {
	RF("id", F_U32, id), RF("decision", F_U32, decision), RF("shadow", F_INT, shadow),
	RF("result", F_RESULT, result), RF("met", F_INT, met), RF("undone", F_INT, undone),
	RF("automatic", F_INT, automatic), RF("trial", F_INT, trial), RF("trial_said", F_INT, trial_said),
	RF("confidence", F_INT, confidence), RF("calls", F_INT, calls), RF("gone", F_INT, gone),
	RF("confounded", F_INT, confounded), RF("by_person", F_INT, by_person),
	RF("action", F_STR, action), RF("fault", F_STR, fault), RF("subject", F_STR, subject),
	RF("target", F_STR, target), RF("scope", F_STR, scope), RF("device", F_STR, device),
	RF("origin", F_STR, origin), RF("undone_by", F_STR, undone_by), RF("metric", F_METRIC, p.metric),
	RF("before", F_FLOAT, p.before), RF("expected", F_FLOAT, p.expected), RF("normal", F_FLOAT, p.normal),
	RF("within", F_LONG, p.within_s), RF("t_before", F_FLOAT, t_before), RF("t_after", F_FLOAT, t_after),
	RF("after", F_FLOAT, after), RF("normal_after", F_FLOAT, normal_after), RF("gain", F_FLOAT, gain),
	RF("gain_pct", F_FLOAT, gain_pct), RF("sum", F_DOUBLE, sum), RF("n", F_INT, n), RF("last", F_FLOAT, last),
	RF("created", F_TIME, created), RF("applied", F_TIME, applied), RF("settle", F_TIME, settle),
	RF("deadline", F_TIME, deadline), RF("judged", F_TIME, judged),
	RF("prediction", F_STR, prediction), RF("summary", F_STR, summary),
};

static void receipt_save(struct blob_buf *b, const struct receipt *r)
{
	size_t k;

	for (k = 0; k < sizeof(RFIELDS) / sizeof(RFIELDS[0]); k++) {
		const void *p = (const char *)r + RFIELDS[k].off;
		const char *n = RFIELDS[k].name;

		switch (RFIELDS[k].kind) {
		case F_INT: blobmsg_add_u32(b, n, (uint32_t)*(const int *)p); break;
		case F_U32: blobmsg_add_u32(b, n, *(const uint32_t *)p); break;
		case F_FLOAT:
			if (known(*(const float *)p))
				blobmsg_add_double(b, n, (double)*(const float *)p);
			break;
		case F_TIME: blobmsg_add_u64(b, n, (uint64_t)*(const int64_t *)p); break;
		case F_LONG: blobmsg_add_u64(b, n, (uint64_t)*(const long *)p); break;
		case F_DOUBLE: blobmsg_add_double(b, n, *(const double *)p); break;
		case F_STR: blobmsg_add_string(b, n, (const char *)p); break;
		case F_RESULT: blobmsg_add_string(b, n, repair_result_name(*(const int *)p)); break;
		case F_METRIC: blobmsg_add_string(b, n, repair_metric_name(*(const int *)p)); break;
		}
	}
}

void repair_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "repair"), *a, *e, *d;
	int i, k;

	blobmsg_add_u32(b, "next", next_id);
	blobmsg_add_u32(b, "next_row", next_row);
	a = blobmsg_open_array(b, "receipts");
	for (i = 0; i < RCPT_MAX; i++) {
		if (!R[i].id)
			continue;
		e = blobmsg_open_table(b, NULL);
		receipt_save(b, &R[i]);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "rows");
	for (i = 0; i < ROW_MAX; i++) {
		if (!rows[i].id)
			continue;
		e = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "id", rows[i].id);
		blobmsg_add_string(b, "fault", rows[i].fault);
		blobmsg_add_string(b, "action", rows[i].action);
		d = blobmsg_open_array(b, "days");
		for (k = 0; k < REPAIR_DAYS; k++) {
			const struct repair_day *s = &rows[i].d[k];
			void *x;

			if (!s->day)
				continue;
			/* day, helped, none, worse, unknown, undone, calls, shadows, needed, not needed, gain % x10,
			   shadow gain x10 */
			x = blobmsg_open_array(b, NULL);
			blobmsg_add_u32(b, NULL, (uint32_t)s->day);
			blobmsg_add_u32(b, NULL, s->helped);
			blobmsg_add_u32(b, NULL, s->none);
			blobmsg_add_u32(b, NULL, s->worse);
			blobmsg_add_u32(b, NULL, s->unknown);
			blobmsg_add_u32(b, NULL, s->undone);
			blobmsg_add_u32(b, NULL, s->calls);
			blobmsg_add_u32(b, NULL, s->shadows);
			blobmsg_add_u32(b, NULL, s->needed);
			blobmsg_add_u32(b, NULL, s->not_needed);
			blobmsg_add_u32(b, NULL, (uint32_t)(int32_t)lroundf(s->gain_pct_sum * 10));
			blobmsg_add_u32(b, NULL, (uint32_t)(int32_t)lroundf(s->shadow_gain_sum * 10));
			blobmsg_close_array(b, x);
		}
		blobmsg_close_array(b, d);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "wifi_normal");      /* 24 hours, then the whole day: mean, deviation, samples */
	for (i = 0; i <= 24; i++) {
		const struct care_norm *c = i < 24 ? &wn_hour[i] : &wn_all;

		e = blobmsg_open_array(b, NULL);
		blobmsg_add_double(b, NULL, (double)c->mean);
		blobmsg_add_double(b, NULL, (double)c->dev);
		blobmsg_add_u32(b, NULL, c->n);
		blobmsg_close_array(b, e);
	}
	blobmsg_close_array(b, a);
	blobmsg_close_table(b, t);
}

static double num(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (double)(int64_t)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	}
	return 0;
}

static void str(char *dst, size_t n, struct blob_attr *a)
{
	if (blobmsg_type(a) == BLOBMSG_TYPE_STRING)
		snprintf(dst, n, "%s", blobmsg_get_string(a));
}

static void load_receipt(struct blob_attr *e)
{
	struct receipt t;
	struct blob_attr *c;
	size_t rem;

	memset(&t, 0, sizeof(t));
	t.p.before = t.p.expected = t.p.normal = t.t_before = t.t_after = t.after = t.normal_after = REPAIR_NONE;
	blobmsg_for_each_attr(c, e, rem) {
		const char *k = blobmsg_name(c), *s = blobmsg_type(c) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(c) : "";
		size_t j;

		for (j = 0; j < sizeof(RFIELDS) / sizeof(RFIELDS[0]); j++) {
			void *p = (char *)&t + RFIELDS[j].off;

			if (strcmp(k, RFIELDS[j].name))
				continue;
			switch (RFIELDS[j].kind) {
			case F_INT: *(int *)p = (int)num(c); break;
			case F_U32: *(uint32_t *)p = (uint32_t)num(c); break;
			case F_FLOAT: *(float *)p = (float)num(c); break;
			case F_TIME: *(int64_t *)p = (int64_t)num(c); break;
			case F_LONG: *(long *)p = (long)num(c); break;
			case F_DOUBLE: *(double *)p = num(c); break;
			case F_STR: snprintf((char *)p, RFIELDS[j].size, "%s", s); break;
			case F_RESULT: *(int *)p = repair_result_by_name(s); break;
			case F_METRIC: *(int *)p = repair_metric_by_name(s); break;
			}
			break;
		}
	}
	if (!t.id || !repair_kind(t.action) || by_id(t.id))
		return;
	{
		struct receipt *r = slot_new();

		if (!r)
			return;
		*r = t;
		if (r->id >= next_id)
			next_id = r->id + 1;
	}
}

static void load_row(struct blob_attr *e)
{
	struct repair_row t;
	struct blob_attr *c, *x, *v;
	size_t rem, r2, r3;
	int i;

	memset(&t, 0, sizeof(t));
	blobmsg_for_each_attr(c, e, rem) {
		const char *k = blobmsg_name(c);

		if (!strcmp(k, "id")) t.id = (uint32_t)num(c);
		else if (!strcmp(k, "fault")) str(t.fault, sizeof(t.fault), c);
		else if (!strcmp(k, "action")) str(t.action, sizeof(t.action), c);
		else if (!strcmp(k, "days") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(x, c, r2) {
				double f[12] = { 0 };
				struct repair_day *s;
				int j = 0;

				blobmsg_for_each_attr(v, x, r3)
					if (j < 12)
						f[j++] = num(v);
				if (j < 12 || f[0] <= 0)
					continue;
				s = &t.d[((long)f[0] % REPAIR_DAYS + REPAIR_DAYS) % REPAIR_DAYS];
				s->day = (long)f[0];
				s->helped = (uint16_t)f[1];
				s->none = (uint16_t)f[2];
				s->worse = (uint16_t)f[3];
				s->unknown = (uint16_t)f[4];
				s->undone = (uint16_t)f[5];
				s->calls = (uint16_t)f[6];
				s->shadows = (uint16_t)f[7];
				s->needed = (uint16_t)f[8];
				s->not_needed = (uint16_t)f[9];
				s->gain_pct_sum = (float)f[10] / 10;
				s->shadow_gain_sum = (float)f[11] / 10;
			}
	}
	if (!t.id || !t.fault[0] || !t.action[0])
		return;
	for (i = 0; i < ROW_MAX; i++)
		if (!rows[i].id) {
			rows[i] = t;
			if (t.id >= next_row)
				next_row = t.id + 1;
			return;
		}
}

void repair_load(struct blob_attr *tbl)
{
	struct blob_attr *c, *e, *v;
	size_t rem, r2, r3;
	int nr = 0, nrow = 0, h = 0;

	memset(R, 0, sizeof(R));
	memset(rows, 0, sizeof(rows));
	blobmsg_for_each_attr(c, tbl, rem) {
		const char *k = blobmsg_name(c);

		if (!strcmp(k, "next")) next_id = (uint32_t)num(c);
		else if (!strcmp(k, "next_row")) next_row = (uint32_t)num(c);
		else if (!strcmp(k, "receipts") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(e, c, r2) {
				load_receipt(e);
				nr++;
			}
		else if (!strcmp(k, "rows") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(e, c, r2) {
				load_row(e);
				nrow++;
			}
		else if (!strcmp(k, "wifi_normal") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(e, c, r2) {
				struct care_norm *cn = h < 24 ? &wn_hour[h] : h == 24 ? &wn_all : NULL;
				double f[3] = { 0 };
				int j = 0;

				h++;
				if (!cn)
					continue;
				blobmsg_for_each_attr(v, e, r3)
					if (j < 3)
						f[j++] = num(v);
				cn->mean = (float)f[0];
				cn->dev = (float)f[1];
				cn->n = (unsigned)f[2];
			}
	}
	if (next_id < 1)
		next_id = 1;
	if (next_row < 1)
		next_row = 1;
	AG_INFO("proven repair: restored %d receipts and the evidence of %d faults", nr, nrow);
}
