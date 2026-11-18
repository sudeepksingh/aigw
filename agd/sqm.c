/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW self-tuning smart queueing (the rules: core/libag/sqm.c).
 *
 *   found     every sample of the timeline (agd/why.c): in the last hour, the
 *             line full and the internet slow meanwhile, most of the full
 *             time (sqm.detect samples): alert "bufferbloat", with the WAN and
 *             the rates (90% of the line's sold or tested speed, else 85% of
 *             the most seen; the upload from the most seen)
 *   decided   the bufferbloat policy makes it an "sqm" decision: proposed in
 *             advise mode, applied in enforce (act.c, the guardrails)
 *   applied   scripts/aigw-sqm (sqm.handler): cake, else htb with fq_codel;
 *             downloads through ifb; an SQM AIGW didn't set up is left alone
 *   judged    once the line has been full sqm.trial samples since: the
 *             internet's delay then, and the download speed, against the day
 *             before. Clearly lower, and downloads keeping 75%: kept (no end;
 *             restored after a restart, put back when the WAN comes back).
 *             Otherwise undone ("tried"), and nothing proposed for sqm.hold
 *   by hand   agctl sqm --on / --off: a person's own, never judged or undone
 *
 * The verdict and the before and after are kept with agd's state. While it is
 * on, the ISP signal SMART_QUEUE isn't raised: AIGW already did it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>

#include "ag/sqm.h"
#include "ag/why.h"
#include "agd.h"

#define BEFORE_S   86400                /* "before": the day before it went on */
#define ALERT_GAP  3600                 /* bloat told at most this often */
#define CHECK_S    600                  /* a kept one is checked (and put back) this often */
#define HANDLER_MS 10000

static struct {
	uint32_t id;                    /* the trial's decision */
	long on_at;                     /* it took effect */
	struct sqm_stats before, after;
	int verdict;                    /* SQ_WAIT (running), SQ_KEEP, SQ_UNDO */
	long judged_at;
	char summary[400];
	char target[64];
} Q;
static long last_alert, last_check;
static char unavailable[200];

static const char *handler(void)
{
	static const char *const dirs[] = { "/usr/sbin/aigw-sqm", "/usr/local/sbin/aigw-sqm", NULL };
	int i;

	if (cfg.sqm_handler[0])
		return !access(cfg.sqm_handler, X_OK) ? cfg.sqm_handler : NULL;
	for (i = 0; dirs[i]; i++)
		if (!access(dirs[i], X_OK))
			return dirs[i];
	return NULL;
}

/* "eth0:45000:8500" -> its parts; 0 when it is one AIGW could apply (words for people) */
int sqm_target_ok(const char *t, char *words, size_t wn)
{
	char ifc[32];
	unsigned long d, u;
	int i, k = 0;

	if (!t || sscanf(t, "%31[A-Za-z0-9._@-]:%lu:%lu%n", ifc, &d, &u, &k) != 3 || t[k] || !d || d > 10000000 ||
	    u > 10000000)
		return -1;
	for (i = 0; ifc[i]; i++)
		if (!isalnum((unsigned char)ifc[i]) && !strchr("._@-", ifc[i]))
			return -1;
	if (strlen(ifc) > 15)
		return -1;
	if (words && wn) {
		if (u)
			snprintf(words, wn, "the line (%s): smart queueing at %.1f / %.1f Mbit/s", ifc, d / 1000.0, u / 1000.0);
		else
			snprintf(words, wn, "the line (%s): smart queueing at %.1f Mbit/s down", ifc, d / 1000.0);
	}
	return 0;
}

static int run(const char *a1, const char *a2, const char *a3, const char *a4, char *out, size_t n)
{
	const char *h = handler();
	char *argv[6];
	int rc;

	if (!h) {
		snprintf(out, n, "no smart queueing program (aigw-sqm; sqm.handler)");
		return -1;
	}
	argv[0] = (char *)h;
	argv[1] = (char *)a1;
	argv[2] = (char *)a2;
	argv[3] = (char *)a3;
	argv[4] = (char *)a4;
	argv[5] = NULL;
	rc = handler_run(h, argv, out, n, HANDLER_MS);
	{
		/* the last line is the message (tc may have said something before it) */
		size_t l = strlen(out);
		char *nl;

		while (l && (out[l - 1] == '\n' || out[l - 1] == '\r'))
			out[--l] = '\0';
		if ((nl = strrchr(out, '\n')))
			memmove(out, nl + 1, strlen(nl + 1) + 1);
	}
	return rc;
}

int sqm_apply(const char *target, char *err, size_t en)
{
	char ifc[32], d[16], u[16], out[512], want[64];

	if (sqm_target_ok(target, NULL, 0) < 0) {
		snprintf(err, en, "not a smart queueing target: %.40s", target);
		return -1;
	}
	sscanf(target, "%31[^:]:%15[0-9]:%15[0-9]", ifc, d, u);
	/* already so (the guardrails put actions back every minute): nothing to do */
	if (run("status", ifc, NULL, NULL, out, sizeof(out)) == 0 && !strncmp(out, "on ", 3)) {
		const char *sp = strchr(out + 3, ' ');

		snprintf(want, sizeof(want), " %s %s", d, u);
		if (sp && !strcmp(sp, want))
			return 0;
	}
	if (run("apply", ifc, d, u, out, sizeof(out)) != 0) {
		snprintf(err, en, "%s", out[0] ? out : "aigw-sqm failed");
		snprintf(unavailable, sizeof(unavailable), "%s", err);
		return -1;
	}
	unavailable[0] = '\0';
	AG_INFO("sqm: %s", out);
	calls_sqm_changed();            /* shaped anew: calls first again, inside it */
	return 0;
}

void sqm_remove(const char *target)
{
	char ifc[32], out[512];

	if (sqm_target_ok(target, NULL, 0) < 0 || sscanf(target, "%31[^:]", ifc) != 1)
		return;
	if (run("remove", ifc, NULL, NULL, out, sizeof(out)) != 0)
		AG_WARN("sqm: remove %s: %s", ifc, out);
	else
		AG_INFO("sqm: %s", out);
	calls_sqm_changed();
}

/* calls and games first: the WAN AIGW's smart queueing shapes now (0), else -1 */
int sqm_shaping(char *ifc, size_t n)
{
	uint32_t id;
	int st;
	char target[64];

	if (act_find_action("sqm", &id, &st, target, sizeof(target), NULL, NULL, 0) < 0 || st <= 0 ||
	    sqm_target_ok(target, NULL, 0) < 0)
		return -1;
	snprintf(ifc, n, "%.*s", (int)strcspn(target, ":"), target);
	return 0;
}

/* calls and games first: these far ends ("udp,<ip>,<port>") go first in the shaping on ifc; none: no one */
int sqm_prio(const char *ifc, const char *const *eps, int n, char *out, size_t on)
{
	const char *h = handler();
	char *argv[40];
	int i, k = 0, rc;

	if (!h) {
		snprintf(out, on, "no smart queueing program (aigw-sqm; sqm.handler)");
		return -1;
	}
	argv[k++] = (char *)h;
	argv[k++] = (char *)"prio";
	argv[k++] = (char *)ifc;
	for (i = 0; i < n && k < 38; i++)
		argv[k++] = (char *)eps[i];
	argv[k] = NULL;
	rc = handler_run(h, argv, out, on, HANDLER_MS);
	{
		size_t l = strlen(out);
		char *nl;

		while (l && (out[l - 1] == '\n' || out[l - 1] == '\r'))
			out[--l] = '\0';
		if ((nl = strrchr(out, '\n')))
			memmove(out, nl + 1, strlen(nl + 1) + 1);
	}
	return rc;
}

/* in effect now (on trial, kept, or a person's own) */
int sqm_on(void)
{
	uint32_t id;
	int st;

	return act_find_action("sqm", &id, &st, NULL, 0, NULL, NULL, 0) == 0 && st > 0;
}

static struct sqm_params params(void)
{
	struct sqm_params p;

	sqm_params_default(&p);
	p.detect = cfg.sqm_detect;
	p.trial = cfg.sqm_trial;
	return p;
}

/* the target it would use now ("eth0:45000:8500"); -1 with words */
static int target_now(char *buf, size_t n, char *err, size_t en)
{
	struct sqm_line l;
	struct sqm_params p = params();
	uint32_t d, u;
	const char *wan = why_wan();

	why_line_now(&l);
	if (!wan || !*wan) {
		snprintf(err, en, "no WAN interface (no default route)");
		return -1;
	}
	if (sqm_rates(&l, &p, &d, &u) < 0) {
		snprintf(err, en, "the line's speed isn't known yet (care.speed_mbps, the nightly speed test, or a busy evening "
			 "on the timeline)");
		return -1;
	}
	snprintf(buf, n, "%.31s:%u:%u", wan, d, u);
	return 0;
}

static void trial(uint32_t id, long on_at, const char *target)
{
	struct sqm_params p = params();
	struct sqm_line l;
	struct why_min *m;
	long now = (long)ag_wall_s(), start = on_at + (long)p.grace * cfg.why_interval;
	int n, max = 4 * 86400 / (cfg.why_interval > 0 ? cfg.why_interval : 60) + 16;
	char why[400];

	if (max > 20000)
		max = 20000;
	m = malloc(sizeof(*m) * (size_t)max);
	if (!m)
		return;
	why_line_now(&l);
	if (Q.id != id) {               /* a new trial: what the line was like the day before */
		memset(&Q, 0, sizeof(Q));
		Q.id = id;
		Q.on_at = on_at;
		snprintf(Q.target, sizeof(Q.target), "%s", target);
		n = why_samples(on_at - BEFORE_S, on_at, m, max);
		sqm_stats_of(m, n, on_at - BEFORE_S, on_at, &l, &Q.before);
		AG_INFO("sqm: trial of %s: before, the line was full %d times, the internet then %d ms, downloads %.1f Mbit/s",
			target, Q.before.n, Q.before.ms, Q.before.kbps / 1000.0);
	}
	n = why_samples(start, now + 1, m, max);
	sqm_stats_of(m, n, start, now + 1, &l, &Q.after);
	free(m);
	{
		unsigned dk = 0;

		if (sscanf(target, "%*[^:]:%u", &dk) != 1)
			dk = 0;
		Q.verdict = sqm_judge(&Q.before, &Q.after, &l, &p, dk, why, sizeof(why));
	}
	if (Q.verdict == SQ_WAIT)
		return;
	Q.judged_at = now;
	snprintf(Q.summary, sizeof(Q.summary), "%s", why);
	AG_INFO("sqm: %s %s", target, why);
	if (Q.verdict == SQ_KEEP)
		act_keep(id, why);
	else
		act_undo_because(id, why);
}

/* every sample of the timeline */
void sqm_on_sample(void)
{
	uint32_t id;
	int st;
	int64_t applied = 0;
	char target[64], origin[48], err[200], why[400];
	long now = (long)ag_wall_s();

	if (act_find_action("sqm", &id, &st, target, sizeof(target), &applied, origin, sizeof(origin)) == 0) {
		/* a trial isn't judged while the area's line is down or slow: that says nothing about SQM */
		if (st == 1 && strstr(origin, "bufferbloat") && !area_hold(AH_SQM_JUDGE))
			trial(id, applied ? (long)applied : now, target);
		else if (st == 2 && now - last_check >= CHECK_S) {
			last_check = now;               /* kept: still there? (a WAN that came back has lost it) */
			if (sqm_apply(target, err, sizeof(err)) < 0)
				AG_WARN("sqm: putting back %s: %s", target, err);
		}
		return;
	}
	if (!cfg.sqm || now - last_alert < ALERT_GAP || act_recent("sqm", now - cfg.sqm_hold))
		return;
	{
		struct sqm_params p = params();
		struct sqm_line l;
		struct why_min *m = malloc(sizeof(*m) * (size_t)(p.window + 4));
		int n;

		if (!m)
			return;
		why_line_now(&l);
		n = why_samples(now - (long)p.window * cfg.why_interval, now + 1, m, p.window + 4);
		if (!sqm_bloated(m, n, now + 1, cfg.why_interval, &l, &p, why, sizeof(why))) {
			free(m);
			return;
		}
		free(m);
	}
	last_alert = now;
	if (target_now(target, sizeof(target), err, sizeof(err)) < 0) {
		AG_INFO("sqm: bufferbloat, but %s", err);
		return;
	}
	if (!handler()) {
		snprintf(unavailable, sizeof(unavailable), "no smart queueing program (aigw-sqm)");
		return;
	}
	{
		char reason[640];

		snprintf(reason, sizeof(reason), "%s A trial at %s (kbit/s down:up): kept if the delay while the line is full "
			 "clearly falls and downloads keep their speed, else undone.", why, strchr(target, ':') + 1);
		char subject[64];
		time_t tt = (time_t)now;
		struct tm tm;

		/* each time it is seen is its own alert (a repeat of one alert would not be decided on again) */
		localtime_r(&tt, &tm);
		snprintf(subject, sizeof(subject), "%.31s, %02d:%02d:%02d", why_wan(), tm.tm_hour, tm.tm_min, tm.tm_sec);
		AG_INFO("sqm: %s", why);
		alerts_raise("bufferbloat", "info", target, subject, 1.0, reason);
	}
}

/* ---------- by hand ---------- */

int sqm_request(int on, uint32_t *id, char *err, size_t en)
{
	char target[64], st_target[64], origin[48];
	uint32_t cur;
	int st;

	if (!on) {
		if (act_find_action("sqm", &cur, &st, st_target, sizeof(st_target), NULL, origin, sizeof(origin)) < 0 || !st) {
			snprintf(err, en, "smart queueing isn't on");
			return -1;
		}
		*id = cur;
		return act_revert(cur, err, en);
	}
	if (act_find_action("sqm", &cur, &st, NULL, 0, NULL, NULL, 0) == 0 && st) {
		snprintf(err, en, "smart queueing is already on (decision %u)", cur);
		return -1;
	}
	if (target_now(target, sizeof(target), err, en) < 0)
		return -1;
	return act_request("sqm", target, cfg.act_max_ttl, NULL, "Smart queueing on the line, asked for by you.", id, err,
			   en);
}

/* the help desk's brief: smart queueing as a proposal that waits for a yes */
int sqm_propose(const char *origin, const char *reason, uint32_t *id, char *err, size_t en)
{
	char target[64];

	if (!cfg.sqm) {
		snprintf(err, en, "smart queueing is off (sqm.enabled)");
		return -1;
	}
	if (target_now(target, sizeof(target), err, en) < 0)
		return -1;
	return act_propose("sqm", target, cfg.act_max_ttl, origin, reason, id, err, en);
}

/* proactive care: smart queueing on the line as it is now, as act.mode allows (enforce applies, advise proposes) */
int sqm_auto(const char *origin, const char *reason, uint32_t *id, int *applied, char *err, size_t en)
{
	char target[64];

	*applied = 0;
	if (!cfg.sqm) {
		snprintf(err, en, "smart queueing is off (sqm.enabled)");
		return -1;
	}
	if (target_now(target, sizeof(target), err, en) < 0)
		return -1;
	return act_auto("sqm", target, cfg.act_max_ttl, origin, reason, id, applied, err, en);
}

/* ---------- what it is doing ---------- */

static const char *status(char *target, size_t tn, uint32_t *id)
{
	char origin[48];
	int st;

	if (act_find_action("sqm", id, &st, target, tn, NULL, origin, sizeof(origin)) == 0)
		return st == 0 ? "Proposed" : st == 2 ? "On" : strstr(origin, "bufferbloat") ? "Trial" : "On";
	*id = 0;
	snprintf(target, tn, "%s", Q.target);
	if (unavailable[0] && cfg.sqm)
		return "Unavailable";
	return Q.verdict == SQ_UNDO && Q.judged_at ? "Tried" : "Off";
}

void sqm_blob(struct blob_buf *b)
{
	char target[64], ifc[32] = "", out[256] = "";
	uint32_t id, d = 0, u = 0;
	const char *s = status(target, sizeof(target), &id);
	void *t;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.sqm);
	blobmsg_add_string(b, "status", s);
	if (id)
		blobmsg_add_u32(b, "decision", id);
	if (target[0] && sscanf(target, "%31[^:]:%u:%u", ifc, &d, &u) >= 1) {
		blobmsg_add_string(b, "interface", ifc);
		blobmsg_add_u32(b, "down_kbps", d);
		blobmsg_add_u32(b, "up_kbps", u);
	}
	if (ifc[0] && run("status", ifc, NULL, NULL, out, sizeof(out)) == 0)
		blobmsg_add_string(b, "shaping", out);
	if (unavailable[0])
		blobmsg_add_string(b, "unavailable", unavailable);
	if (Q.id) {
		t = blobmsg_open_table(b, "trial");
		blobmsg_add_u32(b, "decision", Q.id);
		blobmsg_add_u64(b, "on_at", (uint64_t)Q.on_at);
		blobmsg_add_u32(b, "before_full", (uint32_t)Q.before.n);
		blobmsg_add_u32(b, "before_ms", (uint32_t)(Q.before.ms < 0 ? 0 : Q.before.ms));
		blobmsg_add_u32(b, "before_kbps", Q.before.kbps);
		blobmsg_add_u32(b, "after_full", (uint32_t)Q.after.n);
		blobmsg_add_u32(b, "after_ms", (uint32_t)(Q.after.ms < 0 ? 0 : Q.after.ms));
		blobmsg_add_u32(b, "after_kbps", Q.after.kbps);
		blobmsg_add_u32(b, "needed", (uint32_t)cfg.sqm_trial);
		blobmsg_add_string(b, "verdict", Q.verdict == SQ_KEEP ? "kept" : Q.verdict == SQ_UNDO ? "tried" : "running");
		if (Q.summary[0])
			blobmsg_add_string(b, "summary", Q.summary);
		blobmsg_close_table(b, t);
	}
}

/* Device.X_AIGW_.SQM.: Enable, Status, Interface, DownloadKbps, UploadKbps, Since, LatencyBeforeMs,
   LatencyAfterMs, Summary */
int sqm_field(const char *f, char *buf, size_t n)
{
	char target[64], ifc[32] = "";
	uint32_t id, d = 0, u = 0;
	const char *s = status(target, sizeof(target), &id);
	int on = !strcmp(s, "On") || !strcmp(s, "Trial") || !strcmp(s, "Proposed");

	if (target[0])
		sscanf(target, "%31[^:]:%u:%u", ifc, &d, &u);
	if (!strcmp(f, "Enable"))
		snprintf(buf, n, "%s", cfg.sqm ? "true" : "false");
	else if (!strcmp(f, "Status"))
		snprintf(buf, n, "%s", s);
	else if (!strcmp(f, "Interface"))
		snprintf(buf, n, "%s", on ? ifc : "");
	else if (!strcmp(f, "DownloadKbps"))
		snprintf(buf, n, "%u", on ? d : 0);
	else if (!strcmp(f, "UploadKbps"))
		snprintf(buf, n, "%u", on ? u : 0);
	else if (!strcmp(f, "Since"))
		snprintf(buf, n, "%ld", Q.id && Q.id == id ? Q.on_at : 0L);
	else if (!strcmp(f, "LatencyBeforeMs"))
		snprintf(buf, n, "%d", Q.id && Q.before.ms > 0 ? Q.before.ms : 0);
	else if (!strcmp(f, "LatencyAfterMs"))
		snprintf(buf, n, "%d", Q.id && Q.judged_at && Q.after.ms > 0 ? Q.after.ms : 0);
	else if (!strcmp(f, "Summary"))
		snprintf(buf, n, "%s", Q.summary);
	else
		return -1;
	return 0;
}

/* ---------- saved with agd's state ---------- */

void sqm_save(struct blob_buf *b)
{
	void *t;

	if (!Q.id)
		return;
	t = blobmsg_open_table(b, "sqm");
	blobmsg_add_u32(b, "id", Q.id);
	blobmsg_add_u64(b, "on_at", (uint64_t)Q.on_at);
	blobmsg_add_u32(b, "before_n", (uint32_t)Q.before.n);
	blobmsg_add_u32(b, "before_ms", (uint32_t)(Q.before.ms < 0 ? 0 : Q.before.ms));
	blobmsg_add_u32(b, "before_kbps", Q.before.kbps);
	blobmsg_add_u32(b, "verdict", (uint32_t)Q.verdict);
	blobmsg_add_u64(b, "judged_at", (uint64_t)Q.judged_at);
	blobmsg_add_u32(b, "after_n", (uint32_t)Q.after.n);
	blobmsg_add_u32(b, "after_ms", (uint32_t)(Q.after.ms < 0 ? 0 : Q.after.ms));
	blobmsg_add_u32(b, "after_kbps", Q.after.kbps);
	blobmsg_add_string(b, "summary", Q.summary);
	blobmsg_add_string(b, "target", Q.target);
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
	}
	return 0;
}

void sqm_load(struct blob_attr *t)
{
	struct blob_attr *c;
	size_t rem;

	memset(&Q, 0, sizeof(Q));
	blobmsg_for_each_attr(c, t, rem) {
		const char *k = blobmsg_name(c);

		if (!strcmp(k, "id")) Q.id = (uint32_t)jnum(c);
		else if (!strcmp(k, "on_at")) Q.on_at = jnum(c);
		else if (!strcmp(k, "before_n")) Q.before.n = (int)jnum(c);
		else if (!strcmp(k, "before_ms")) Q.before.ms = jnum(c) ? (int)jnum(c) : -1;
		else if (!strcmp(k, "before_kbps")) Q.before.kbps = (uint32_t)jnum(c);
		else if (!strcmp(k, "verdict")) Q.verdict = (int)jnum(c);
		else if (!strcmp(k, "judged_at")) Q.judged_at = jnum(c);
		else if (!strcmp(k, "after_n")) Q.after.n = (int)jnum(c);
		else if (!strcmp(k, "after_ms")) Q.after.ms = (int)jnum(c);
		else if (!strcmp(k, "after_kbps")) Q.after.kbps = (uint32_t)jnum(c);
		else if (!strcmp(k, "summary") && blobmsg_type(c) == BLOBMSG_TYPE_STRING)
			snprintf(Q.summary, sizeof(Q.summary), "%s", blobmsg_get_string(c));
		else if (!strcmp(k, "target") && blobmsg_type(c) == BLOBMSG_TYPE_STRING)
			snprintf(Q.target, sizeof(Q.target), "%s", blobmsg_get_string(c));
	}
}

void sqm_init(void)
{
	char out[64] = "";

	if (!cfg.sqm)
		return;
	if (!handler())
		snprintf(unavailable, sizeof(unavailable), "no smart queueing program (aigw-sqm)");
	else if (run("check", NULL, NULL, NULL, out, sizeof(out)) == 0 && !strcmp(out, "none"))
		snprintf(unavailable, sizeof(unavailable), "this kernel has neither cake nor htb with fq_codel");
	AG_INFO("sqm: smart queueing on bufferbloat%s%s%s", out[0] && strcmp(out, "none") ? " (" : "",
		out[0] && strcmp(out, "none") ? out : "", out[0] && strcmp(out, "none") ? ")" : "");
	if (unavailable[0])
		AG_INFO("sqm: %s", unavailable);
}
