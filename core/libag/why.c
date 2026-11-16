/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW "why was it slow?": a stretch of the home's timeline, explained (see
 * ag/why.h).
 *
 * Every sample of the stretch is put in one of a few states (the line down,
 * full, slow, lossy; DNS failing; an access point in trouble; the gateway not
 * recording), and each state that lasted at least two samples is a finding.
 * A blip is not. The verdict is the finding that explains most of the
 * stretch: its length, counted three times when nothing worked (the line
 * down, the gateway off, a device kept off), twice for a problem, once for a
 * warning. The story puts the findings and what happened around them in
 * order of time: a device that started a big download, AIGW changing a
 * channel, a device moving to 2.4 GHz.
 *
 * Slow is judged against the home's normal at those hours (proactive care's,
 * else the timeline's own), so a line that always answers in 40 ms isn't slow
 * at 40 ms, and one that answers in 8 ms is at 60.
 */
#define _GNU_SOURCE                     /* localtime_r */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/why.h"

#define MAX_SAMPLES  WHY_MAX_SAMPLES    /* the longest stretch judged (agd asks for no more) */
#define PRE_S        600                /* what happened this long before the stretch belongs to it */
#define CAUSE_S      300                /* an AIGW change this long before a problem may be its cause */
#define BUSY_PCT     80                 /* % of the line's speed that is full */
#define MIN_RUN      2                  /* samples: one is a blip */

enum {
	S_SLOW,                         /* the internet or the next router answered slowly (any cause) */
	S_GAP, S_NOREC, S_NOROUTE, S_LINE_DOWN, S_ISP_DOWN, S_FULL, S_LINE_SLOW, S_ISP_SLOW,
	S_N
};

struct cand {
	const char *code, *area;
	int sev, prio, weight;
	int samples;
	long since, until;
	char text[400];
	char brief[WHY_TEXT];           /* for the story */
};

struct judge {
	const struct why_in *in;
	struct why_result *out;
	int E;                          /* samples the stretch should have */
	const struct why_min *slot[MAX_SAMPLES];
	struct cand c[WHY_FINDINGS];
	int nc;
	int gw_known;                   /* the next router answered echo requests at some time in it */
};

/* nothing worked meanwhile (no internet, the gateway off, the device kept off): counted three times */
static int nothing_worked(const char *code)
{
	static const char *const c[] = { "NO_ROUTE", "LINE_DOWN", "ISP_DOWN", "GATEWAY_OFF", "DEVICE_BLOCKED",
					 "DEVICE_OFFLINE" };
	size_t i;

	for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		if (!strcmp(code, c[i]))
			return 1;
	return 0;
}

/* weight: how much of the stretch it explains (-1: from its length and severity) */
static void addf(struct judge *J, const char *code, const char *area, int sev, int prio, int samples, int weight,
		 long since, long until, const char *brief, const char *fmt, ...) __attribute__((format(printf, 11, 12)));

static void addf(struct judge *J, const char *code, const char *area, int sev, int prio, int samples, int weight,
		 long since, long until, const char *brief, const char *fmt, ...)
{
	struct cand *c;
	va_list ap;

	if (J->nc >= WHY_FINDINGS)
		return;
	c = &J->c[J->nc++];
	memset(c, 0, sizeof(*c));
	c->code = code;
	c->area = area;
	c->sev = sev;
	c->prio = prio;
	c->samples = samples;
	c->since = since;
	c->until = until;
	if (weight < 0)
		weight = sev < DS_WARN ? 0 : nothing_worked(code) ? samples * 3 : sev == DS_PROBLEM ? samples * 2 : samples;
	c->weight = sev < DS_WARN ? 0 : weight;
	snprintf(c->brief, sizeof(c->brief), "%s", brief ? brief : "");
	va_start(ap, fmt);
	vsnprintf(c->text, sizeof(c->text), fmt, ap);
	va_end(ap);
}

/* ---------- words ---------- */

void why_clock(long t, long now, char *buf, size_t n)
{
	time_t a = (time_t)t, b = (time_t)now;
	struct tm ta, tb;

	localtime_r(&a, &ta);
	localtime_r(&b, &tb);
	if (ta.tm_yday == tb.tm_yday && ta.tm_year == tb.tm_year)
		strftime(buf, n, "%H:%M", &ta);
	else
		strftime(buf, n, "%a %H:%M", &ta);
}

static void dur(long s, char *buf, size_t n)
{
	if (s < 120)
		snprintf(buf, n, "%ld s", s < 0 ? 0 : s);
	else if (s < 3600 * 2)
		snprintf(buf, n, "%ld min", (s + 30) / 60);
	else if (s % 3600 < 300)
		snprintf(buf, n, "%ld h", s / 3600);
	else
		snprintf(buf, n, "%ld h %ld min", s / 3600, (s % 3600) / 60);
}

static const char *nm(const struct why_in *in, const uint8_t mac[6])
{
	static char b[18];

	if (in->name) {
		const char *s = in->name(mac, in->ctx);

		if (s && *s)
			return s;
	}
	snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return b;
}

static int mac_zero(const uint8_t m[6])
{
	static const uint8_t z[6];

	return !memcmp(m, z, 6);
}

static const char *radio_name(const struct why_in *in, int r)
{
	return r >= 0 && r < WHY_RADIOS && in->radio[r] ? in->radio[r] : "an access point";
}

const char *why_kind_name(int kind)
{
	static const char *const n[] = {
		"start", "join", "leave", "roam", "steer", "weak", "signal-ok", "health", "channel", "decision",
		"care", "speed", "upgrade",
	};

	return kind >= 0 && kind < WE_N ? n[kind] : "?";
}

/* ---------- the normal ---------- */

static int cmp_f(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

void why_baseline(const struct why_min *m, int n, long from, long to, float norm[WN_N], int have[WN_N])
{
	float *v[WN_N];
	int k[WN_N] = { 0 }, i, w;

	for (w = 0; w < WN_N; w++) {
		have[w] = 0;
		norm[w] = 0;
		v[w] = malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
	}
	for (i = 0; i < n; i++) {
		const struct why_min *s = &m[i];

		if (!s->t || ((long)s->t >= from && (long)s->t < to) || !s->route)
			continue;
		if (s->gw_sent && s->gw_lost < s->gw_sent && s->gw_ms != WHY_NONE && v[WN_GW])
			v[WN_GW][k[WN_GW]++] = s->gw_ms;
		if (s->in_sent && s->in_lost < s->in_sent) {
			if (v[WN_LOSS])
				v[WN_LOSS][k[WN_LOSS]++] = (float)s->in_lost * 100.0f / (float)s->in_sent;
			if (s->in_ms != WHY_NONE && v[WN_INET])
				v[WN_INET][k[WN_INET]++] = s->in_ms;
		}
		if (s->dns_q >= 3 && s->dns_ms != WHY_NONE && v[WN_DNS])
			v[WN_DNS][k[WN_DNS]++] = s->dns_ms;
	}
	for (w = 0; w < WN_N; w++) {
		if (!v[w])
			continue;
		if (k[w] >= 30) {               /* half an hour of samples, at least */
			qsort(v[w], (size_t)k[w], sizeof(float), cmp_f);
			if (w == WN_LOSS) {
				double s = 0;   /* loss: the mean (the median of a rare loss is 0) */

				for (i = 0; i < k[w]; i++)
					s += v[w][i];
				norm[w] = (float)(s / k[w]);
			} else {
				norm[w] = v[w][k[w] / 2];
			}
			have[w] = 1;
		}
		free(v[w]);
	}
}

/* slow from this delay: twice the normal and a real difference; without a normal, Diagnose's lines */
int why_slow_ms(float n, int have)
{
	int t;

	if (!have)
		return 150;
	t = (int)(n * 2 > n + 30 ? n * 2 : n + 30);
	return t < 40 ? 40 : t;
}

static int slow_inet(const struct why_in *in)
{
	return why_slow_ms(in->norm[WN_INET], in->have_norm[WN_INET]);
}

static int slow_gw(const struct why_in *in)
{
	float n = in->norm[WN_GW];
	int t;

	if (!in->have_norm[WN_GW])
		return 100;
	t = (int)(n * 2 > n + 20 ? n * 2 : n + 20);
	return t < 20 ? 20 : t;
}

/* ---------- runs: how long, and the longest stretch (a one-sample hole doesn't end it) ---------- */

static int runs(struct judge *J, const unsigned char *on, long *since, long *until)
{
	int i, total = 0, best = 0, cur = 0, start = -1, bs = -1, be = -1, hole = 0, last = -1;
	long step = J->in->step;

	for (i = 0; i < J->E; i++) {
		if (on[i]) {
			total++;
			if (start < 0 || hole > 1) {
				start = i;
				cur = 0;
			}
			cur++;
			hole = 0;
			last = i;
			if (cur > best) {
				best = cur;
				bs = start;
				be = last;
			}
		} else if (start >= 0) {
			hole++;
		}
	}
	*since = bs >= 0 ? J->in->from + bs * step : 0;
	*until = be >= 0 ? J->in->from + (be + 1) * step : 0;
	return total;
}

/* ---------- the stretch ---------- */

int why_full(const struct why_min *s, float down_mbps, float peak_down_mbps, float peak_up_mbps)
{
	if (down_mbps > 0)
		return s->down_kbps >= (uint32_t)(down_mbps * 1000.0f * BUSY_PCT / 100.0f);
	/* the line's speed isn't known: near the most this timeline has seen on it */
	if (peak_down_mbps >= 5 && s->down_kbps >= (uint32_t)(peak_down_mbps * 1000.0f * BUSY_PCT / 100.0f))
		return 1;
	return peak_up_mbps >= 2 && s->up_kbps >= (uint32_t)(peak_up_mbps * 1000.0f * BUSY_PCT / 100.0f);
}

static int busy(const struct why_in *in, const struct why_min *s)
{
	return why_full(s, in->down_mbps, in->peak_down_mbps, in->peak_up_mbps);
}

static int answered(int sent, int lost)
{
	return sent > 0 && lost < sent;
}

/* an AIGW change shortly before t, in words ("" none) */
static void aigw_before(const struct why_in *in, long t, char *buf, size_t n)
{
	int i;

	buf[0] = '\0';
	for (i = in->ne - 1; i >= 0; i--) {
		const struct why_ev *e = &in->e[i];
		char at[16];

		if ((long)e->t > t || (long)e->t < t - CAUSE_S)
			continue;
		if (e->kind == WE_CHANNEL || e->kind == WE_STEER ||
		    (e->kind == WE_DECISION && (e->flag & WF_WIFI) && !(e->flag & WF_ENDED))) {
			why_clock((long)e->t, in->to, at, sizeof(at));
			snprintf(buf, n, " It started %ld min after a change at %s: %s.", (t - (long)e->t + 59) / 60, at,
				 e->text);
			return;
		}
	}
}

static void line_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	char a[16], b[16], d[40], ch[320];
	long since, until;
	int k;

	k = runs(J, st[S_NOROUTE], &since, &until);
	if (k >= MIN_RUN) {
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		addf(J, "NO_ROUTE", "Internet", DS_PROBLEM, 20, k, -1, since, until, "no internet connection (no route)",
		     "The gateway had no way to the internet for %s (no default route; the longest from %s to %s): its "
		     "internet connection was down.", d, a, b);
	}
	k = runs(J, st[S_LINE_DOWN], &since, &until);
	if (k >= MIN_RUN) {
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		aigw_before(in, since, ch, sizeof(ch));
		addf(J, "LINE_DOWN", "Internet", DS_PROBLEM, 30, k, -1, since, until, "the internet was down",
		     "The internet didn't answer for %s (the longest from %s to %s)%s: the line between the gateway and "
		     "the ISP was down (modem, cable or fibre).%s", d, a, b,
		     J->gw_known ? ", nor did the next router" : "", ch);
	}
	k = runs(J, st[S_ISP_DOWN], &since, &until);
	if (k >= MIN_RUN) {
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		aigw_before(in, since, ch, sizeof(ch));
		addf(J, "ISP_DOWN", "Internet", DS_PROBLEM, 31, k, -1, since, until, "the internet was down beyond the home",
		     "The internet didn't answer for %s (the longest from %s to %s) while the next router did: the "
		     "problem was beyond the home, in the ISP's network.%s", d, a, b, ch);
	}
}

/* who downloaded most while the line was full */
static void full_finding(struct judge *J, unsigned char *full, unsigned char *slow)
{
	const struct why_in *in = J->in;
	struct { uint8_t mac[6]; double kb; int n; } who[8];
	int nw = 0, i, j, k, ks, best = -1;
	double down = 0, peak = 0, inet = 0;
	int ninet = 0;
	long since, until;
	char a[16], b[16], d[40], whos[160] = "", slows[160] = "", brief[WHY_TEXT];

	memset(who, 0, sizeof(who));
	k = runs(J, full, &since, &until);
	if (k < MIN_RUN)
		return;
	for (i = 0; i < J->E; i++) {
		const struct why_min *s = J->slot[i];

		if (!full[i] || !s)
			continue;
		down += s->down_kbps;
		if (s->down_kbps > peak)
			peak = s->down_kbps;
		if (s->in_ms != WHY_NONE && answered(s->in_sent, s->in_lost))
			inet += s->in_ms, ninet++;
		for (j = 0; j < WHY_TOP; j++) {
			int w;

			if (!s->top[j].kbps || mac_zero(s->top[j].mac))
				continue;
			for (w = 0; w < nw && memcmp(who[w].mac, s->top[j].mac, 6); w++)
				;
			if (w == nw) {
				if (nw >= 8)
					continue;
				memcpy(who[nw++].mac, s->top[j].mac, 6);
			}
			who[w].kb += s->top[j].kbps;
			who[w].n++;
		}
	}
	for (i = 0; i < nw; i++)
		if (best < 0 || who[i].kb > who[best].kb)
			best = i;
	ks = 0;
	for (i = 0; i < J->E; i++)
		ks += full[i] && slow[i];
	why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
	dur((long)k * in->step, d, sizeof(d));
	if (best >= 0 && who[best].kb >= down * 0.3) {
		double mb = who[best].kb / k / 1000.0;

		snprintf(whos, sizeof(whos), ", most of it by %s (%.0f Mbit/s on average)", nm(in, who[best].mac), mb);
	}
	if (ks >= MIN_RUN && ninet) {
		char nrm[48] = "";

		if (in->have_norm[WN_INET])
			snprintf(nrm, sizeof(nrm), ", normally %.0f ms", in->norm[WN_INET]);
		snprintf(slows, sizeof(slows), " Everything else was slower meanwhile: the internet answered in %.0f ms%s.",
			 inet / ninet, nrm);
	} else {
		snprintf(slows, sizeof(slows), " Other downloads shared what was left.");
	}
	snprintf(brief, sizeof(brief), "the line was full%s%s", best >= 0 ? ": " : "", best >= 0 ? nm(in, who[best].mac) : "");
	if (in->down_mbps > 0)
		addf(J, "LINE_FULL", "Internet", DS_WARN, 65, k, -1, since, until, brief,
		     "The line was used to its speed for %s (the longest from %s to %s): %.0f of %.0f Mbit/s%s. Nothing was "
		     "broken: the home used all of it.%s", d, a, b, peak / 1000.0, in->down_mbps, whos, slows);
	else
		addf(J, "LINE_FULL", "Internet", DS_WARN, 65, k, -1, since, until, brief,
		     "The line was used to its limit for %s (the longest from %s to %s): up to %.0f Mbit/s, the most this "
		     "gateway has seen on it%s. Nothing was broken: the home used all of it.%s", d, a, b, peak / 1000.0,
		     whos, slows);
}

static void slow_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	long since, until;
	char a[16], b[16], d[40], nrm[64], ch[320], brief[WHY_TEXT];
	double sum = 0;
	int k, i, cnt = 0;

	k = runs(J, st[S_LINE_SLOW], &since, &until);
	if (k >= MIN_RUN) {
		for (i = 0; i < J->E; i++)
			if (st[S_LINE_SLOW][i] && J->slot[i] && J->slot[i]->gw_ms != WHY_NONE)
				sum += J->slot[i]->gw_ms, cnt++;
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		nrm[0] = '\0';
		if (in->have_norm[WN_GW])
			snprintf(nrm, sizeof(nrm), " (normally %.0f ms)", in->norm[WN_GW]);
		aigw_before(in, since, ch, sizeof(ch));
		snprintf(brief, sizeof(brief), "the next router answered slowly (%.0f ms)", cnt ? sum / cnt : 0);
		addf(J, "LINE_SLOW", "Internet", DS_WARN, 72, k, -1, since, until, brief,
		     "For %s (the longest from %s to %s) already the next router answered slowly: %.0f ms%s. The line to "
		     "the ISP (modem, cable, fibre) was slow, and the gateway didn't see the home filling it.%s", d, a, b,
		     cnt ? sum / cnt : 0, nrm, ch);
	}
	sum = 0, cnt = 0;
	k = runs(J, st[S_ISP_SLOW], &since, &until);
	if (k >= MIN_RUN) {
		for (i = 0; i < J->E; i++)
			if (st[S_ISP_SLOW][i] && J->slot[i] && J->slot[i]->in_ms != WHY_NONE)
				sum += J->slot[i]->in_ms, cnt++;
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		nrm[0] = '\0';
		if (in->have_norm[WN_INET])
			snprintf(nrm, sizeof(nrm), " (normally %.0f ms)", in->norm[WN_INET]);
		aigw_before(in, since, ch, sizeof(ch));
		snprintf(brief, sizeof(brief), "the internet answered slowly (%.0f ms)", cnt ? sum / cnt : 0);
		addf(J, "ISP_SLOW", "Internet", DS_WARN, 73, k, -1, since, until, brief,
		     "For %s (the longest from %s to %s) the internet answered slowly: %.0f ms%s, while the next router "
		     "was fine and the home wasn't filling the line: slow beyond the home, in the ISP's network.%s", d, a,
		     b, cnt ? sum / cnt : 0, nrm, ch);
	}
}

static void loss_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	unsigned char on[MAX_SAMPLES], gon[MAX_SAMPLES];
	int i, sent = 0, lost = 0, gsent = 0, glost = 0, k;
	long since, until;
	char a[16], b[16], brief[WHY_TEXT];

	memset(on, 0, sizeof(on));
	memset(gon, 0, sizeof(gon));
	for (i = 0; i < J->E; i++) {
		const struct why_min *s = J->slot[i];

		if (!s || st[S_LINE_DOWN][i] || st[S_ISP_DOWN][i] || st[S_NOROUTE][i])
			continue;
		sent += s->in_sent, lost += s->in_lost;
		if (s->in_lost)
			on[i] = 1;
		if (s->gw_sent && answered(s->gw_sent, s->gw_lost)) {
			gsent += s->gw_sent, glost += s->gw_lost;
			if (s->gw_lost)
				gon[i] = 1;
		}
	}
	if (sent < 9 || lost * 100 < sent * 5)
		return;
	if (gsent >= 9 && glost * 100 >= gsent * 5) {
		k = runs(J, gon, &since, &until);
		if (k < MIN_RUN)
			return;
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		snprintf(brief, sizeof(brief), "packets lost to the next router (%d%%)", glost * 100 / gsent);
		addf(J, "LINE_LOSS", "Internet", glost * 100 >= gsent * 20 ? DS_PROBLEM : DS_WARN, 70, k, -1, since, until, brief,
		     "%d%% of packets to the next router were lost (most from %s to %s): the line to the ISP (modem, "
		     "cable, fibre) was unreliable.", glost * 100 / gsent, a, b);
		return;
	}
	k = runs(J, on, &since, &until);
	if (k < MIN_RUN)
		return;
	why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
	snprintf(brief, sizeof(brief), "packets lost on the internet (%d%%)", lost * 100 / sent);
	addf(J, "ISP_LOSS", "Internet", lost * 100 >= sent * 20 ? DS_PROBLEM : DS_WARN, 71, k, -1, since, until, brief,
	     "%d%% of packets to the internet were lost (most from %s to %s)%s: in the ISP's network.", lost * 100 / sent,
	     a, b, gsent ? " while the next router answered" : "");
}

static void dns_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	unsigned char fail[MAX_SAMPLES], slow[MAX_SAMPLES];
	int i, k;
	unsigned q = 0, f = 0;
	double ms = 0;
	int nms = 0;
	long since, until;
	char a[16], b[16], d[40];

	memset(fail, 0, sizeof(fail));
	memset(slow, 0, sizeof(slow));
	for (i = 0; i < J->E; i++) {
		const struct why_min *s = J->slot[i];

		if (!s || s->dns_q < 3 || st[S_LINE_DOWN][i] || st[S_ISP_DOWN][i] || st[S_NOROUTE][i])
			continue;
		q += s->dns_q, f += s->dns_fail;
		if (s->dns_fail * 2 >= s->dns_q)
			fail[i] = 1;
		else if (s->dns_ms != WHY_NONE && s->dns_ms >= 300)
			slow[i] = 1;
		if (s->dns_ms != WHY_NONE)
			ms += s->dns_ms, nms++;
	}
	k = runs(J, fail, &since, &until);
	if (k >= MIN_RUN) {
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		addf(J, "DNS_FAILING", "DNS", DS_PROBLEM, 40, k, -1, since, until, "names didn't resolve",
		     "For %s (the longest from %s to %s) names didn't resolve: %u of %u lookups failed while the internet "
		     "answered. Websites and apps failed although the line worked.", d, a, b, f, q);
	}
	k = runs(J, slow, &since, &until);
	if (k >= MIN_RUN) {
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		addf(J, "DNS_SLOW", "DNS", DS_WARN, 90, k, -1, since, until, "names resolved slowly",
		     "For %s (the longest from %s to %s) names took 300 ms or more to resolve: pages started slowly.", d, a,
		     b);
	}
}

static const char *cause_code(int cause)
{
	switch (cause) {
	case HC_CONGESTION: return "WIFI_CONGESTION";
	case HC_COVERAGE: return "WIFI_COVERAGE";
	case HC_HOPPING: return "WIFI_HOPPING";
	case HC_DEVICE: return "WIFI_DEVICE";
	case HC_RADIO: return "WIFI_RADIO";
	}
	return "WIFI_PROBLEM";
}

static const char *cause_words(int cause)
{
	switch (cause) {
	case HC_CONGESTION: return "its channel was busy (neighbouring networks, interference)";
	case HC_COVERAGE: return "devices far from it lost traffic (weak signal)";
	case HC_HOPPING: return "a device kept switching bands at the edge of 5 GHz";
	case HC_DEVICE: return "one device lost traffic while the others were fine";
	case HC_RADIO: return "no device could use it (the radio itself)";
	}
	return "devices lost traffic";
}

static void wifi_findings(struct judge *J)
{
	const struct why_in *in = J->in;
	int r, i, k;

	for (r = 0; r < WHY_RADIOS; r++) {
		unsigned char on[MAX_SAMPLES];
		int causes[8] = { 0 }, cause = 0, poor = 0, loss = 0, nl = 0;
		long since, until;
		char a[16], b[16], d[40], brief[WHY_TEXT];

		memset(on, 0, sizeof(on));
		for (i = 0; i < J->E; i++) {
			const struct why_min *s = J->slot[i];

			if (!s || !s->r[r].used || (s->r[r].status != HEAL_DEGRADED && s->r[r].status != HEAL_POOR))
				continue;
			on[i] = 1;
			poor += s->r[r].status == HEAL_POOR;
			if (s->r[r].cause < 8)
				causes[s->r[r].cause]++;
			loss += s->r[r].loss, nl++;
		}
		k = runs(J, on, &since, &until);
		if (k < MIN_RUN)
			continue;
		for (i = 1; i < 8; i++)
			if (causes[i] > causes[cause])
				cause = i;
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		snprintf(brief, sizeof(brief), "Wi-Fi trouble on %s", radio_name(in, r));
		addf(J, cause_code(cause), "WiFi", poor * 2 >= k ? DS_PROBLEM : DS_WARN, in->host ? 86 : 85, k, -1, since, until,
		     brief, "Wi-Fi on %s was %s for %s (the longest from %s to %s): %s; %d%% of its probes were lost.",
		     radio_name(in, r), poor * 2 >= k ? "poor" : "degraded", d, a, b, cause_words(cause), nl ? loss / nl : 0);
	}
}

/* the device asked about: its events */
static void host_findings(struct judge *J)
{
	const struct why_in *in = J->in;
	const char *h = in->host_name && *in->host_name ? in->host_name : nm(in, in->host);
	long off_since = 0, off_total = 0, weak_since = 0, weak_total = 0, blk_since = 0, blk_total = 0, lim_since = 0,
	     lim_total = 0, low_at = 0, off_first = 0, weak_first = 0, blk_first = 0, lim_first = 0;
	int connected = -1, leaves = 0, i, weak_dbm = 0, on24 = 0, known = 0, len;
	char drops[120] = "", a[16], d[40];
	size_t dl = 0;

	for (i = 0; i < in->ne; i++) {
		const struct why_ev *e = &in->e[i];
		long t = (long)e->t < in->from ? in->from : (long)e->t;

		if (memcmp(e->mac, in->host, 6) || (long)e->t >= in->to)
			continue;
		switch (e->kind) {
		case WE_JOIN:
		case WE_ROAM:
			known = 1;
			if (connected == 0 && off_since)
				off_total += t - off_since, off_since = 0;
			connected = 1;
			on24 = e->flag == 2;
			if (on24 && (long)e->t >= in->from && !low_at)
				low_at = (long)e->t;
			break;
		case WE_LEAVE:
			known = 1;
			connected = 0;
			off_since = t;
			if (!off_first)
				off_first = t;
			if ((long)e->t >= in->from) {
				leaves++;
				if (leaves <= 4) {
					why_clock((long)e->t, in->to, a, sizeof(a));
					dl += (size_t)snprintf(drops + dl, sizeof(drops) - dl, "%s%s", leaves > 1 ? ", " : "", a);
					if (dl >= sizeof(drops))
						dl = sizeof(drops) - 1;
				}
			}
			break;
		case WE_WEAK:
			if (!weak_since)        /* still weak (seen again after a restart): from the first */
				weak_since = t;
			if (!weak_first)
				weak_first = t;
			weak_dbm = -(int)e->flag;
			break;
		case WE_SIGNAL_OK:
			if (weak_since)
				weak_total += t - weak_since, weak_since = 0;
			break;
		case WE_DECISION:
			if (e->flag & WF_BLOCKS) {
				if (e->flag & WF_ENDED) {
					if (blk_since)
						blk_total += t - blk_since, blk_since = 0;
				} else if (!blk_since) {
					blk_since = t;
					if (!blk_first)
						blk_first = t;
				}
			}
			if (e->flag & WF_LIMITS) {
				if (e->flag & WF_ENDED) {
					if (lim_since)
						lim_total += t - lim_since, lim_since = 0;
				} else if (!lim_since) {
					lim_since = t;
					if (!lim_first)
						lim_first = t;
				}
			}
			break;
		}
	}
	if (off_since && connected == 0)
		off_total += in->to - off_since;
	if (weak_since)
		weak_total += in->to - weak_since;
	if (blk_since)
		blk_total += in->to - blk_since;
	if (lim_since)
		lim_total += in->to - lim_since;
	len = (int)(in->to - in->from);
	if (blk_total >= 2L * in->step) {
		dur(blk_total, d, sizeof(d));
		addf(J, "DEVICE_BLOCKED", "AIGW", DS_PROBLEM, 10, (int)(blk_total / in->step), -1, blk_first, in->to,
		     NULL, "AIGW itself kept %s off the network for %s of it (a decision). Undo it if that "
		     "isn't wanted.", h, d);
	}
	if (lim_total >= 2L * in->step) {
		dur(lim_total, d, sizeof(d));
		addf(J, "DEVICE_LIMITED", "AIGW", DS_WARN, 60, (int)(lim_total / in->step), -1, lim_first, in->to,
		     NULL, "AIGW slowed %s on purpose for %s of it (a decision).", h, d);
	}
	if (known && off_total * 2 >= len && !blk_total) {
		dur(off_total, d, sizeof(d));
		addf(J, "DEVICE_OFFLINE", "Device", DS_PROBLEM, 51, (int)(off_total / in->step), -1, off_first, in->to,
		     NULL, "%s wasn't on Wi-Fi for %s of it: off, out of range, or on another network.", h,
		     d);
	} else if (leaves >= 2) {
		addf(J, "WIFI_DROPS", "WiFi", DS_WARN, 79, leaves, leaves * 3, in->from, in->to, NULL,
		     "%s dropped off Wi-Fi %d times (%s%s): every drop stops what it was doing for a few seconds.", h, leaves,
		     drops, leaves > 4 ? ", ..." : "");
	}
	if (weak_total >= 2L * in->step) {
		dur(weak_total, d, sizeof(d));
		addf(J, "WIFI_COVERAGE", "WiFi", DS_WARN, 80, (int)(weak_total / in->step), -1, weak_first, in->to,
		     NULL, "%s's Wi-Fi signal was weak for %s of it (%d dBm): too far from the access point, "
		     "or walls in the way.", h, d, weak_dbm);
	}
	if (low_at) {
		int k = on24 ? (int)((in->to - low_at) / in->step) : 0;

		why_clock(low_at, in->to, a, sizeof(a));
		addf(J, "DEVICE_ON_24GHZ", "WiFi", on24 && in->to - low_at >= len / 2 ? DS_WARN : DS_INFO, 95, k, k / 2,
		     low_at, in->to, NULL,
		     "%s moved to 2.4 GHz at %s%s: slower than 5 GHz, and shared with more devices.", h, a,
		     on24 ? "" : ", and back to 5 GHz later");
	}
}

/* what AIGW itself changed in the stretch, and a firmware upgrade */
static void aigw_findings(struct judge *J)
{
	const struct why_in *in = J->in;
	char list[300] = "";
	size_t l = 0;
	int i, n = 0;

	for (i = 0; i < in->ne; i++) {
		const struct why_ev *e = &in->e[i];

		if ((long)e->t < in->from || (long)e->t >= in->to)
			continue;
		if (e->kind == WE_UPGRADE)
			addf(J, "FIRMWARE_CHANGED", "AIGW", DS_INFO, 96, 0, -1, (long)e->t, (long)e->t, NULL, "%s", e->text);
		if (e->kind == WE_CHANNEL || e->kind == WE_STEER || (e->kind == WE_DECISION && !(e->flag & WF_ENDED))) {
			if (n++ < 3 && l < sizeof(list)) {
				l += (size_t)snprintf(list + l, sizeof(list) - l, "%s%s", n > 1 ? "; " : "", e->text);
				if (l >= sizeof(list))
					l = sizeof(list) - 1;
			}
		}
	}
	if (n)
		addf(J, "AIGW_CHANGED", "AIGW", DS_INFO, 97, 0, -1, in->from, in->to, NULL, "AIGW changed %d thing%s meanwhile: %s%s.",
		     n, n == 1 ? "" : "s", list, n > 3 ? "; ..." : "");
}

static void gap_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	long since, until;
	char a[16], b[16], d[40];
	int k, i, reboot = 0;

	k = runs(J, st[S_GAP], &since, &until);
	if (k >= MIN_RUN) {
		for (i = 0; i < in->ne; i++)
			if (in->e[i].kind == WE_START && (long)in->e[i].t >= since && (long)in->e[i].t <= until + 2L * in->step)
				reboot |= in->e[i].flag ? 2 : 1;
		why_clock(since, in->to, a, sizeof(a)), why_clock(until, in->to, b, sizeof(b));
		dur((long)k * in->step, d, sizeof(d));
		if (reboot & 2)
			addf(J, "GATEWAY_OFF", "All", DS_PROBLEM, 15, k, -1, since, until, "the gateway was off",
			     "The gateway was off or restarting for %s (from %s to %s, then it started again): no internet "
			     "for anyone meanwhile. A power cut, someone switching it off, or a crash.", d, a, b);
		else
			addf(J, "NOT_RECORDED", "AIGW", DS_INFO, 98, 0, -1, since, until, "AIGW wasn't recording",
			     "AIGW wasn't recording for %s (from %s to %s)%s: what happened then isn't known.", d, a, b,
			     reboot ? ", it was restarted" : "");
	}
	if (in->from < in->recorded_since || !in->recorded_since) {
		if (!in->recorded_since)
			addf(J, "NOT_RECORDED", "AIGW", DS_INFO, 99, 0, -1, in->from, in->to, NULL,
			     "The gateway has no timeline yet: nothing is known about that time.");
		else {
			why_clock(in->recorded_since, in->to, a, sizeof(a));
			addf(J, "NOT_RECORDED", "AIGW", DS_INFO, 99, 0, -1, in->from, in->recorded_since, NULL,
			     "The timeline starts at %s: what happened before isn't known.", a);
		}
	}
}

/* what was fine */
static void fine_findings(struct judge *J, unsigned char st[S_N][MAX_SAMPLES])
{
	const struct why_in *in = J->in;
	int i, r, n = 0, sent = 0, lost = 0, dq = 0, df = 0, good = 0, bad = 0;
	float v[MAX_SAMPLES];
	char nrm[48] = "";

	for (i = 0; i < J->E; i++) {
		const struct why_min *s = J->slot[i];

		if (!s || !s->route)
			continue;
		sent += s->in_sent, lost += s->in_lost;
		if (s->in_ms != WHY_NONE && answered(s->in_sent, s->in_lost))
			v[n++] = s->in_ms;
		dq += s->dns_q, df += s->dns_fail;
		for (r = 0; r < WHY_RADIOS; r++)
			if (s->r[r].used)
				good += s->r[r].status == HEAL_GOOD, bad += s->r[r].status == HEAL_DEGRADED || s->r[r].status == HEAL_POOR;
	}
	(void)st;
	for (r = 0; r < J->nc; r++)             /* not "fine" where something was found */
		if (J->c[r].sev >= DS_WARN) {
			if (!strcmp(J->c[r].area, "Internet"))
				n = 0;
			if (!strcmp(J->c[r].area, "DNS"))
				dq = 0;
		}
	if (n) {
		qsort(v, (size_t)n, sizeof(float), cmp_f);
		if (in->have_norm[WN_INET])
			snprintf(nrm, sizeof(nrm), " (normally %.0f ms)", in->norm[WN_INET]);
		addf(J, "INTERNET_OK", "Internet", DS_OK, 910, 0, -1, in->from, in->to, NULL,
		     "The internet answered in %.0f ms%s, %d%% lost.", v[n / 2], nrm, sent ? lost * 100 / sent : 0);
	}
	if (dq >= 3)
		addf(J, "DNS_OK", "DNS", DS_OK, 920, 0, -1, in->from, in->to, NULL, "%d lookups, %d failed.", dq, df);
	if (good && !bad)
		addf(J, "WIFI_OK", "WiFi", DS_OK, 930, 0, -1, in->from, in->to, NULL, "Wi-Fi was fine on every access point.");
}

/* ---------- the story ---------- */

static void story_add(struct why_result *o, long t, const char *text, int keep)
{
	int i, worst = -1;

	if (o->nstory >= WHY_STORY) {
		if (!keep)
			return;
		/* full: drop the first line that isn't a finding (tagged by a leading space) */
		for (i = 0; i < o->nstory; i++)
			if (o->story[i].text[0] == ' ') {
				worst = i;
				break;
			}
		if (worst < 0)
			return;
		memmove(&o->story[worst], &o->story[worst + 1], sizeof(o->story[0]) * (size_t)(o->nstory - worst - 1));
		o->nstory--;
	}
	o->story[o->nstory].t = t;
	o->story[o->nstory].text[0] = ' ';
	{
		size_t l = strlen(text), max = sizeof(o->story[0].text) - 2;

		if (l > max)
			l = max;
		memcpy(o->story[o->nstory].text + !keep, text, l);
		o->story[o->nstory].text[!keep + l] = '\0';
	}
	o->nstory++;
}

static int ev_in_story(const struct why_in *in, const struct why_ev *e)
{
	if ((long)e->t >= in->to || (long)e->t < in->from - PRE_S)
		return 0;
	switch (e->kind) {
	case WE_JOIN:
	case WE_LEAVE:
	case WE_ROAM:
	case WE_WEAK:
	case WE_SIGNAL_OK:
		return in->host && !memcmp(e->mac, in->host, 6);
	case WE_DECISION:
		return !in->host || mac_zero(e->mac) || !memcmp(e->mac, in->host, 6) || (e->flag & WF_WIFI);
	}
	return 1;
}

static void build_story(struct judge *J)
{
	const struct why_in *in = J->in;
	struct why_result *o = J->out;
	uint8_t prev[6] = { 0 };
	int i, starts = 0;
	char line[WHY_TEXT + 64], b[16];
	uint32_t big = in->down_mbps > 0 ? (uint32_t)(in->down_mbps * 250.0f) : 5000;   /* a quarter of the line */

	if (big < 5000)
		big = 5000;
	/* the findings that lasted, first: they are kept when the story is full */
	for (i = 0; i < o->n; i++) {
		const struct cand *c = &J->c[i];

		if (c->sev < DS_WARN || !c->brief[0] || !c->since)
			continue;
		why_clock(c->until, in->to, b, sizeof(b));
		snprintf(line, sizeof(line), "%s, until %s", c->brief, b);
		story_add(o, c->since, line, 1);
	}
	for (i = 0; i < in->ne; i++)
		if (ev_in_story(in, &in->e[i]))
			story_add(o, (long)in->e[i].t, in->e[i].text, 0);
	/* a device starting a big download */
	for (i = 0; i < J->E && starts < 4; i++) {
		const struct why_min *s = J->slot[i];

		if (!s)
			continue;
		if (s->top[0].kbps >= big && memcmp(prev, s->top[0].mac, 6)) {
			snprintf(line, sizeof(line), "%s started downloading (%.0f Mbit/s)", nm(in, s->top[0].mac),
				 s->top[0].kbps / 1000.0);
			story_add(o, (long)s->t, line, 0);
			starts++;
		}
		if (s->top[0].kbps >= big)
			memcpy(prev, s->top[0].mac, 6);
		else
			memset(prev, 0, 6);
	}
	/* in order of time (a stable insertion sort), and the tags off */
	for (i = 1; i < o->nstory; i++) {
		int j;

		for (j = i; j > 0 && o->story[j].t < o->story[j - 1].t; j--) {
			struct why_line t = o->story[j];

			o->story[j] = o->story[j - 1];
			o->story[j - 1] = t;
		}
	}
	for (i = 0; i < o->nstory; i++)
		if (o->story[i].text[0] == ' ')
			memmove(o->story[i].text, o->story[i].text + 1, strlen(o->story[i].text));
}

/* ---------- the verdict ---------- */

void why_judge(const struct why_in *in, struct why_result *out)
{
	static struct judge J;
	static unsigned char st[S_N][MAX_SAMPLES];
	int i, j, gw_known = 0, worst = DS_OK, slow_i = slow_inet(in), slow_g = slow_gw(in);
	long step = in->step > 0 ? in->step : 60;

	memset(out, 0, sizeof(*out));
	memset(&J, 0, sizeof(J));
	memset(st, 0, sizeof(st));
	J.in = in;
	J.out = out;
	J.E = (int)((in->to - in->from + step - 1) / step);
	if (J.E > MAX_SAMPLES)
		J.E = MAX_SAMPLES;
	if (J.E < 0)
		J.E = 0;
	out->expected = J.E;
	for (i = 0; i < in->n; i++) {
		long k = ((long)in->m[i].t - in->from) / step;

		if (in->m[i].t && k >= 0 && k < J.E && !J.slot[k]) {
			J.slot[k] = &in->m[i];
			out->recorded++;
			if (in->m[i].gw_sent && answered(in->m[i].gw_sent, in->m[i].gw_lost))
				gw_known = 1;
		}
	}

	/* each sample's state */
	for (i = 0; i < J.E; i++) {
		const struct why_min *s = J.slot[i];
		long t = in->from + i * step;
		int inet_down, slow, gslow;

		if (!s) {
			st[t >= in->recorded_since && in->recorded_since ? S_GAP : S_NOREC][i] = 1;
			continue;
		}
		if (!s->route) {
			st[S_NOROUTE][i] = 1;
			continue;
		}
		inet_down = s->in_sent && s->in_lost == s->in_sent;
		if (inet_down) {
			/* the next router answered: beyond the home; it didn't (and does at other times): the line */
			if (answered(s->gw_sent, s->gw_lost))
				st[S_ISP_DOWN][i] = 1;
			else
				st[S_LINE_DOWN][i] = 1;
			continue;
		}
		slow = s->in_ms != WHY_NONE && answered(s->in_sent, s->in_lost) && s->in_ms >= slow_i;
		gslow = gw_known && s->gw_ms != WHY_NONE && answered(s->gw_sent, s->gw_lost) && s->gw_ms >= slow_g;
		if (busy(in, s))
			st[S_FULL][i] = 1;              /* the home's own use: whatever else is slow follows from it */
		else if (gslow)
			st[S_LINE_SLOW][i] = 1;
		else if (slow)
			st[S_ISP_SLOW][i] = 1;
		st[S_SLOW][i] = slow || gslow;
	}
	J.gw_known = gw_known;
	gap_findings(&J, st);
	line_findings(&J, st);
	full_finding(&J, st[S_FULL], st[S_SLOW]);
	slow_findings(&J, st);
	loss_findings(&J, st);
	dns_findings(&J, st);
	wifi_findings(&J);
	if (in->host)
		host_findings(&J);
	aigw_findings(&J);
	fine_findings(&J, st);

	/* the verdict: what explains most of the stretch (then Diagnose's order); the rest by order */
	for (i = 1; i < J.nc; i++)
		for (j = i; j > 0; j--) {
			struct cand *x = &J.c[j], *y = &J.c[j - 1];
			int before = x->weight > y->weight || (x->weight == y->weight && x->prio < y->prio);

			if (!before)
				break;
			{
				struct cand t = *x;

				*x = *y;
				*y = t;
			}
		}
	for (i = 0; i < J.nc; i++)
		if (J.c[i].sev > worst)
			worst = J.c[i].sev;
	if (worst <= DS_INFO && J.nc < WHY_FINDINGS) {
		char a[16], b[16];

		memmove(&J.c[1], &J.c[0], sizeof(J.c[0]) * (size_t)J.nc);
		J.nc++;
		memset(&J.c[0], 0, sizeof(J.c[0]));
		why_clock(in->from, in->to, a, sizeof(a));
		why_clock(in->to, in->to, b, sizeof(b));
		if (!out->recorded) {
			J.c[0].code = "NO_DATA";
			J.c[0].area = "All";
			J.c[0].sev = DS_INFO;
			snprintf(J.c[0].text, sizeof(J.c[0].text),
				 "The gateway has no record of %s to %s: it can't say what happened then.", a, b);
		} else {
			J.c[0].code = "OK";
			J.c[0].area = "All";
			J.c[0].sev = DS_OK;
			snprintf(J.c[0].text, sizeof(J.c[0].text),
				 in->host ? "Everything the gateway measured from %s to %s was normal, for %s too: the problem "
					    "was likely the service or website used, or the device itself." :
					    "Everything the gateway measured from %s to %s was normal%.0s: the problem was "
					    "likely the service or website used, or one device.",
				 a, b, in->host_name && *in->host_name ? in->host_name : "it");
		}
	}
	for (i = 0; i < J.nc; i++) {
		struct why_finding *f = &out->f[out->n++];
		const struct cand *c = &J.c[i];

		f->code = c->code;
		f->area = c->area;
		f->sev = c->sev;
		f->samples = c->samples;
		f->since = c->since;
		f->until = c->until;
		snprintf(f->text, sizeof(f->text), "%s", c->text);
	}
	build_story(&J);
}

const char *why_advice(const char *code)
{
	static const struct { const char *code, *advice; } t[] = {
		{ "LINE_FULL", "Nothing to repair: the home used the whole line. Smart queueing (SQM) keeps calls and games "
			       "responsive while others download; a faster plan if it happens often." },
		{ "GATEWAY_OFF", "Check the gateway's power supply and where it is plugged in; if it restarted by itself, "
				 "look at its logs from that time." },
		{ "NOT_RECORDED", "" },
		{ "NO_DATA", "Ask again about a time the gateway was recording (agctl why shows since when)." },
		{ "WIFI_DROPS", "Restart the device's Wi-Fi; turn off its power saving; move it closer, or update it." },
		{ "FIRMWARE_CHANGED", "" },
		{ "AIGW_CHANGED", "" },
	};
	size_t i;

	for (i = 0; code && i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].code, code))
			return t[i].advice;
	if (code && !strcmp(code, "DEVICE_ON_24GHZ"))
		return "Move the device closer to the access point, so it stays on 5 GHz.";
	return diag_advice(code);
}
