/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW firmware upgrades that check themselves: the verdict (see ag/upgrade.h) */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ag/upgrade.h"

void up_params_default(struct up_params *p)
{
	memset(p, 0, sizeof(*p));
	p->watch_s = 86400;
	p->reconnect_s = 1800;
	p->offline_s = 900;
	p->max_starts = 3;
	p->missing_min = 2;
	p->missing_pct = 25;
	p->min_samples = 6;
	p->slower_x = 1.5f;
	p->slower_ms[CM_GW_MS] = 10;
	p->slower_ms[CM_INET_MS] = 20;
	p->slower_ms[CM_LOSS] = 2;
	p->slower_ms[CM_DNS_MS] = 50;
	p->speed_pct = 80;
	p->score_drop = 10;
	p->score_min_s = 4 * 3600;
}

const char *up_state_name(int state)
{
	static const char *const n[] = { "None", "Activated", "Watching", "Kept", "Worse", "RollingBack", "RolledBack",
					 "NotBooted" };

	return state >= 0 && state < (int)(sizeof(n) / sizeof(n[0])) ? n[state] : "None";
}

static const char *const why_names[UPW_N] = {
	"DevicesMissing", "NoInternet", "Restarts", "NextHopLatency", "InternetLatency", "InternetLoss", "DNSLatency",
	"DownloadSpeed", "ExperienceScore",
};

const char *up_why_name(unsigned bit)
{
	int i;

	for (i = 0; i < UPW_N; i++)
		if (bit == 1u << i)
			return why_names[i];
	return "";
}

void up_why_names(unsigned why, char *buf, size_t n)
{
	size_t len = 0;
	int i;

	if (n)
		buf[0] = '\0';
	for (i = 0; i < UPW_N && len < n; i++)
		if (why & (1u << i))
			len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? "," : "", why_names[i]);
}

void up_after_start(struct up_after *a, long now)
{
	memset(a, 0, sizeof(*a));
	a->booted = now;
	a->starts = 1;
}

int up_after_seen(const struct up_before *b, struct up_after *a, const char *mac, long now)
{
	int i;

	for (i = 0; i < b->ndev && i < UP_DEVS; i++)
		if (!strcasecmp(b->dev[i], mac)) {
			if (a->back[i])
				return -1;
			a->back[i] = 1;
			a->back_at[i] = now;
			return i;
		}
	return -1;
}

void up_after_check(struct up_after *a, long now, int problem, int online, const struct care_sample *s,
		    const struct care_sample *normal, long max_gap)
{
	int m;

	/* the time between two checks, as proactive care counts it (gaps while agd was off left out) */
	if (a->last_check && now > a->last_check && now - a->last_check <= max_gap) {
		a->watched_s += (unsigned)(now - a->last_check);
		if (!problem)
			a->ok_s += (unsigned)(now - a->last_check);
	}
	a->last_check = now;
	a->checks++;
	if (!online)
		a->offline++;
	else if (!a->online_at)
		a->online_at = now;
	for (m = 0; m < UP_METRICS; m++)
		if (s && normal && s->have[m] && normal->have[m]) {
			a->v[m] += s->v[m];
			a->norm[m] += normal->v[m];
			a->n[m]++;
		}
}

void up_after_speed(struct up_after *a, float mbps)
{
	a->speed_sum += mbps;
	a->speed_n++;
}

int up_score(unsigned watched_s, unsigned ok_s)
{
	return watched_s ? (int)((unsigned long long)ok_s * 100 / watched_s) : -1;
}

static void say(char *buf, size_t n, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;

	if (len + 1 >= n)
		return;
	if (len)
		buf[len++] = ' ', buf[len] = '\0';
	va_start(ap, fmt);
	vsnprintf(buf + len, n - len, fmt, ap);
	va_end(ap);
}

/* a duration in words: "45 s", "30 min", "24 h" */
static const char *dur(long s, char *buf, size_t n)
{
	if (s < 120)
		snprintf(buf, n, "%ld s", s);
	else if (s < 7200)
		snprintf(buf, n, "%ld min", s / 60);
	else
		snprintf(buf, n, "%ld h", s / 3600);
	return buf;
}

static const char *const metric_words[UP_METRICS] = {
	"The next hop (modem, ISP's router) answers in", "The internet answers in", "Internet loss is",
	"DNS answers in",
};

int up_judge(const struct up_before *b, const struct up_after *a, long now, const struct up_params *p,
	     struct up_verdict *v)
{
	long el = now - a->booted;
	int i, m, over = el >= p->watch_s;
	unsigned why = 0;

	memset(v, 0, sizeof(*v));
	v->score_before = b->watched_s >= (unsigned)p->score_min_s ? up_score(b->watched_s, b->ok_s) : -1;
	v->score_after = a->watched_s >= (unsigned)p->score_min_s ? up_score(a->watched_s, a->ok_s) : -1;
	for (i = 0; i < b->ndev && i < UP_DEVS; i++) {
		if (a->back[i]) {
			v->back++;
		} else {
			v->missing++;
			if (b->wifi[i])
				v->missing_wifi++;
		}
	}

	/* at once */
	if (a->starts > p->max_starts) {
		why |= UPW_RESTARTS;
		say(v->text, sizeof(v->text), "AIGW started %d times on the new firmware: it, or the gateway, keeps "
		    "restarting.", a->starts);
	}
	if (b->online == 1 && !a->online_at && a->checks >= 3 && el >= p->offline_s) {
		why |= UPW_INTERNET;
		char d[24];

		say(v->text, sizeof(v->text), "No internet since the upgrade (%s, %u checks); it worked before.",
		    dur(el, d, sizeof(d)), a->checks);
	}
	if ((el >= p->reconnect_s || !v->missing) && v->missing >= p->missing_min &&
	    v->missing * 100 >= p->missing_pct * b->ndev) {
		char kind[32], d[24];

		why |= UPW_DEVICES;
		if (v->missing_wifi == v->missing)
			snprintf(kind, sizeof(kind), "all on Wi-Fi");
		else if (!v->missing_wifi)
			snprintf(kind, sizeof(kind), "all wired");
		else
			snprintf(kind, sizeof(kind), "%d on Wi-Fi", v->missing_wifi);
		say(v->text, sizeof(v->text), "%d of the %d devices connected before the upgrade did not come back "
		    "within %s (%s).", v->missing, b->ndev, dur(p->reconnect_s, d, sizeof(d)), kind);
	}
	if (why) {
		v->result = UPV_WORSE;
		v->why = why;
		return v->result;
	}
	if (!over) {
		v->result = UPV_WATCHING;
		return v->result;
	}

	/* at the end of the watch */
	for (m = 0; m < UP_METRICS; m++) {
		double mv, mn;
		int worse;

		if (a->n[m] < p->min_samples)
			continue;
		mv = a->v[m] / a->n[m];
		mn = a->norm[m] / a->n[m];
		worse = m == CM_LOSS ? mv - mn >= p->slower_ms[m] : mv >= mn * p->slower_x && mv - mn >= p->slower_ms[m];
		if (!worse)
			continue;
		why |= m == CM_GW_MS ? UPW_NEXTHOP : m == CM_INET_MS ? UPW_LATENCY : m == CM_LOSS ? UPW_LOSS : UPW_DNS;
		if (m == CM_LOSS)
			say(v->text, sizeof(v->text), "%s %.1f%%, normally %.1f%% at these hours.", metric_words[m], mv, mn);
		else
			say(v->text, sizeof(v->text), "%s %.0f ms, normally %.0f ms at these hours.", metric_words[m], mv, mn);
	}
	if (b->speed_mbps > 0 && a->speed_n) {
		double sp = a->speed_sum / a->speed_n;

		if (sp * 100 < (double)b->speed_mbps * p->speed_pct) {
			why |= UPW_SPEED;
			say(v->text, sizeof(v->text), "Downloads reach %.0f Mbit/s, %.0f Mbit/s in the week before.", sp,
			    (double)b->speed_mbps);
		}
	}
	if (v->score_before >= 0 && v->score_after >= 0 && v->score_before - v->score_after >= p->score_drop) {
		why |= UPW_SCORE;
		say(v->text, sizeof(v->text), "The home was without a problem %d%% of the time, %d%% in the week before.",
		    v->score_after, v->score_before);
	}
	v->why = why;
	if (why) {
		v->result = UPV_WORSE;
		return v->result;
	}
	v->result = UPV_KEEP;
	{
		char devs[64] = "", score[96] = "", d[24];

		if (b->ndev)
			snprintf(devs, sizeof(devs), "%d of %d devices back, ", v->back, b->ndev);
		if (v->score_after >= 0 && v->score_before >= 0)
			snprintf(score, sizeof(score), "; without a problem %d%% of the time (%d%% the week before)",
				 v->score_after, v->score_before);
		else if (v->score_after >= 0)
			snprintf(score, sizeof(score), "; without a problem %d%% of the time", v->score_after);
		snprintf(v->text, sizeof(v->text), "No worse after %s: %sdelays, loss and DNS as usual for these hours%s.",
			 dur(el, d, sizeof(d)), devs, score);
	}
	return v->result;
}
