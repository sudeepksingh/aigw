/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW proactive care, before the call: which faults are worth acting on,
 * one ticket per fault, flaps, the day's limit, and the customer's note in
 * plain words (see ag/proactive.h). Pure: no I/O.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ag/brief.h"
#include "ag/diag.h"
#include "ag/proactive.h"

void pro_params_default(struct pro_params *p)
{
	p->threshold = 3;
	p->min_s = 180;
	p->long1 = 600;
	p->long2 = 1800;
	p->clear_s = 600;
	p->cooldown_s = 21600;
	p->max_per_day = 3;
	p->wifi_grace = 1800;
	p->score_poor = 90;
	p->bad_loss_pct = 25;
	p->bad_ms = 1000;
}

const char *pro_status_name(int st)
{
	static const char *const n[PS_N] = { "Watching", "Fixing", "Proposed", "Raised", "Suppressed", "Fixed", "Resolved",
					     "Passed" };

	return st >= 0 && st < PS_N ? n[st] : "Watching";
}

static uint32_t hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (s && *s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h ? h : 1;
}

static void cat(char *buf, size_t n, const char *s)
{
	size_t l = strlen(buf);

	if (l < n)
		snprintf(buf + l, n - l, "%s%s", l ? ", " : "", s);
}

int pro_points(const struct pro_signal *s, long now, int repeats, int score_week, const struct pro_params *p,
	       char *why, size_t n)
{
	long lasted = s->since && now > s->since ? now - s->since : 0;
	char w[64];
	int pts = 0;

	if (why && n)
		why[0] = '\0';
	if (s->sev >= DS_PROBLEM) {
		pts += 2;
		if (why)
			cat(why, n, "a problem +2");
	} else if (s->sev >= DS_WARN) {
		pts += 1;
		if (why)
			cat(why, n, "a warning +1");
	} else
		return 0;               /* below a warning: never */
	if ((p->bad_loss_pct > 0 && s->loss_pct >= p->bad_loss_pct) || (p->bad_ms > 0 && s->delay_ms >= p->bad_ms)) {
		/* noticed at once: a call breaks up, a page stalls, however short or few it is */
		pts += 1;
		if (s->loss_pct >= p->bad_loss_pct && p->bad_loss_pct > 0)
			snprintf(w, sizeof(w), "%d%% lost +1", s->loss_pct);
		else
			snprintf(w, sizeof(w), "%d ms replies +1", s->delay_ms);
		if (why)
			cat(why, n, w);
	}
	if (lasted>= p->long2 || lasted >= p->long1) {
		int k = lasted >= p->long2 ? 2 : 1;

		pts += k;
		snprintf(w, sizeof(w), "for %ld min +%d", lasted / 60, k);
		if (why)
			cat(why, n, w);
	}
	if (s->devices >= 2) {
		int k = s->devices >= 4 ? 2 : 1;

		pts += k;
		snprintf(w, sizeof(w), "%d devices +%d", s->devices, k);
		if (why)
			cat(why, n, w);
	}
	if (repeats >= 2) {
		int k = repeats >= 4 ? 2 : 1;

		pts += k;
		snprintf(w, sizeof(w), "%d times today +%d", repeats, k);
		if (why)
			cat(why, n, w);
	}
	if (score_week >= 0 && score_week < p->score_poor) {
		pts += 1;
		snprintf(w, sizeof(w), "a %d%% week +1", score_week);
		if (why)
			cat(why, n, w);
	}
	return pts;
}

int pro_worth(const struct pro_signal *s, int points, long now, const struct pro_params *p)
{
	long lasted = s->since && now > s->since ? now - s->since : 0;

	if (!s->code || s->sev < DS_WARN || points < p->threshold || lasted < p->min_s)
		return 0;
	if (!strcmp(s->code, "LINE_FULL"))
		return s->rough_calls > 0;      /* the home used its whole line: noticed only when calls suffered */
	return s->fault == BF_LINE || s->fault == BF_HOME_WIFI || s->fault == BF_DEVICE;
}

int pro_repeats(const struct pro_state *st, const char *key, long now)
{
	uint32_t h = hash(key);
	int i, n = 0;

	for (i = 0; i < PRO_EPISODES; i++)
		if (st->ep[i].h == h && st->ep[i].t && st->ep[i].t > now - 86400)
			n++;
	return n;
}

/* a key seen now: the episode it belongs to (a new one after clear_s away), by its index */
static int episode(struct pro_state *st, const char *key, long now, long since, const struct pro_params *p)
{
	uint32_t h = hash(key);
	int i, best = -1;

	for (i = 0; i < PRO_EPISODES; i++)
		if (st->ep[i].h == h && st->ep[i].last >= now - p->clear_s && (best < 0 || st->ep[i].t > st->ep[best].t))
			best = i;
	if (best >= 0) {
		st->ep[best].last = now;
		return best;
	}
	i = st->ep_head;
	st->ep_head = (st->ep_head + 1) % PRO_EPISODES;
	st->ep[i].h = h;
	st->ep[i].t = since && since < now ? since : now;
	st->ep[i].last = now;
	st->ep[i].held = 0;
	return i;
}

static struct pro_ticket *by_key(struct pro_state *st, const char *key, int open)
{
	int i;

	for (i = 0; i < PRO_TICKETS; i++)
		if (st->t[i].used && st->t[i].open == open && !strcmp(st->t[i].key, key))
			return &st->t[i];
	return NULL;
}

/* a slot: unused, else the closed one closed longest ago; NULL all open */
static struct pro_ticket *slot(struct pro_state *st)
{
	struct pro_ticket *best = NULL;
	int i;

	for (i = 0; i < PRO_TICKETS; i++) {
		if (!st->t[i].used)
			return &st->t[i];
		if (!st->t[i].open && (!best || st->t[i].closed < best->closed))
			best = &st->t[i];
	}
	return best;
}

static long hold(const struct pro_ticket *t, const struct pro_params *p)
{
	long h = p->clear_s;
	int i;

	for (i = 0; i < t->flaps && h < p->cooldown_s; i++)
		h *= 2;
	return h < p->cooldown_s ? h : p->cooldown_s;
}

static void add(struct pro_change *ch, int *k, int max, int kind, struct pro_ticket *t)
{
	if (*k >= max)
		return;
	ch[*k].kind = kind;
	ch[*k].t = t;
	(*k)++;
}

static void fill(struct pro_ticket *t, const struct pro_signal *s, long now)
{
	snprintf(t->code, sizeof(t->code), "%s", s->code);
	snprintf(t->target, sizeof(t->target), "%s", s->target ? s->target : "");
	snprintf(t->source, sizeof(t->source), "%s", s->source ? s->source : "");
	t->fault = s->fault;
	t->sev = s->sev;
	if (s->sev > t->sev_max)
		t->sev_max = s->sev;
	t->devices = s->devices;
	t->last_seen = now;
}

int pro_step(struct pro_state *st, const struct pro_signal *sig, int n, long now, int score_week,
	     const struct pro_params *p, struct pro_change *ch, int max)
{
	unsigned char seen[PRO_TICKETS] = { 0 };
	int i, j, k = 0;

	if (!st->c.since)
		st->c.since = now;
	for (i = 0; i < n; i++) {
		struct pro_signal x;
		const struct pro_signal *s = &x;
		struct pro_ticket *t;
		char why[160];
		int pts, rep, e;
		long es;

		if (!sig[i].code || !sig[i].key[0])
			continue;
		for (j = 0; j < i; j++)
			if (!strcmp(sig[j].key, sig[i].key))
				break;
		if (j < i) {
			st->c.deduped++;        /* two sources, one fault: one ticket */
			continue;
		}
		/* how long: what the source says, or since this episode began, whichever is longer (a source
		   may only see the last minutes; the looks see it all) */
		x = sig[i];
		e = episode(st, x.key, now, x.since, p);
		es = st->ep[e].t;
		if (es && (!x.since || es < x.since))
			x.since = es;
		if ((t = by_key(st, s->key, 1))) {
			fill(t, s, now);
			seen[t - st->t] = 1;
			if (!st->area_hold && (t->status == PS_WATCHING || t->status == PS_FIXING || t->quiet) &&
			    t->watch_until && now >= t->watch_until) {
				t->watch_until = 0;
				add(ch, &k, max, PC_GRACE_OVER, t);
			}
			continue;
		}
		rep = pro_repeats(st, s->key, now);
		pts = pro_points(s, now, rep, score_week, p, why, sizeof(why));
		if (!pro_worth(s, pts, now, p))
			continue;
		if (st->area_hold) {
			/* a known area outage: the ISP knows and tells its customers; no ticket, no note */
			if (!st->ep[e].held) {
				st->ep[e].held = 1;
				st->c.area_held++;
			}
			continue;
		}
		if ((t = by_key(st, s->key, 0)) && t->closed && now - t->closed < p->cooldown_s) {
			/* back soon after it cleared: the same ticket */
			t->open = 1;
			t->closed = 0;
			t->flaps++;
			t->episodes++;
			t->points = pts > t->points ? pts : t->points;
			snprintf(t->why, sizeof(t->why), "%s", why);
			fill(t, s, now);
			st->c.flaps++;
			seen[t - st->t] = 1;
			add(ch, &k, max, PC_REOPENED, t);
			continue;
		}
		if (!(t = slot(st)))
			continue;               /* all open: they come first */
		memset(t, 0, sizeof(*t));
		t->used = 1;
		t->open = 1;
		t->id = ++st->next_id;
		memcpy(t->key, s->key, sizeof(t->key));
		t->key[sizeof(t->key) - 1] = '\0';
		t->points = pts;
		snprintf(t->why, sizeof(t->why), "%s", why);
		t->since = s->since ? s->since : now;
		t->opened = now;
		t->episodes = rep > 0 ? rep : 1;
		t->status = PS_WATCHING;
		fill(t, s, now);
		st->c.tickets++;
		seen[t - st->t] = 1;
		add(ch, &k, max, PC_NEW, t);
	}
	for (i = 0; i < PRO_TICKETS; i++) {
		struct pro_ticket *t = &st->t[i];

		if (t->used && t->open && !seen[i] && now - t->last_seen >= hold(t, p))
			add(ch, &k, max, PC_CLEARED, t);
	}
	return k;
}

int pro_budget(const struct pro_state *st, long now, const struct pro_params *p)
{
	int i, n = 0;

	for (i = 0; i < PRO_SENT; i++)
		if (st->sent[i] && st->sent[i] > now - 86400)
			n++;
	return n >= p->max_per_day ? 0 : p->max_per_day - n;
}

void pro_notified(struct pro_state *st, struct pro_ticket *t, long now)
{
	if (t->notified)
		return;
	t->notified = 1;
	t->notified_at = now;
	st->sent[st->sent_head] = now;
	st->sent_head = (st->sent_head + 1) % PRO_SENT;
	st->c.raised++;
}

int pro_close(struct pro_state *st, struct pro_ticket *t, long now)
{
	int quiet = t->quiet;

	t->open = 0;
	t->closed = now;
	t->watch_until = 0;
	t->quiet = 0;
	if (quiet) {
		/* a flap: over again, as the ISP heard already; counted once, the first time */
		t->status = t->status == PS_FIXING ? PS_FIXED : t->status == PS_RAISED || t->status == PS_PROPOSED ?
			    PS_RESOLVED : t->status;
		return t->status;
	}
	if (t->status == PS_SUPPRESSED)
		;                       /* kept at home: stays so */
	else if (t->acted || t->status == PS_FIXING) {
		t->status = PS_FIXED;
		st->c.fixed++;
	} else if (t->notified)
		t->status = PS_RESOLVED;
	else
		t->status = PS_PASSED;
	if (!t->called && (t->status == PS_FIXED || t->notified))
		st->c.avoided++;        /* fixed, or the customer heard first, and nobody called */
	return t->status;
}

void pro_on_called(struct pro_state *st)
{
	int i;

	for (i = 0; i < PRO_TICKETS; i++)
		if (st->t[i].used && st->t[i].open && !st->t[i].called) {
			st->t[i].called = 1;
			st->c.called++;
		}
}

struct pro_ticket *pro_find(struct pro_state *st, uint32_t id)
{
	int i;

	for (i = 0; i < PRO_TICKETS; i++)
		if (st->t[i].used && st->t[i].id == id)
			return &st->t[i];
	return NULL;
}

int pro_open_count(const struct pro_state *st)
{
	int i, n = 0;

	for (i = 0; i < PRO_TICKETS; i++)
		n += st->t[i].used && st->t[i].open;
	return n;
}

/* ---------- the customer's note ---------- */

static const char *when_words(int hour)
{
	return hour >= 5 && hour < 12 ? "this morning" : hour >= 12 && hour < 17 ? "this afternoon" :
	       hour >= 17 && hour < 22 ? "this evening" : "tonight";
}

static int is(const char *code, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(code, *list))
			return 1;
	return 0;
}

void pro_note(const char *code, int fixed, int hour, char *buf, size_t n)
{
	static const char *const down[] = { "LINE_DOWN", "NO_ROUTE", "ISP_DOWN", NULL };
	static const char *const slow[] = { "LINE_LOSS", "ISP_LOSS", "LINE_SLOW", "ISP_SLOW", "LINE_SLOW_SPEED", NULL };
	static const char *const names[] = { "DNS_FAILING", "DNS_SLOW", NULL };
	static const char *const device[] = { "WIFI_DEVICE", "WIFI_DROPS", "DEVICE_OFFLINE", NULL };
	const char *w = when_words(hour), *c = code ? code : "";

	if (is(c, down))
		snprintf(buf, n, fixed ? "We noticed your internet connection stopped working %s. It is working again now; "
			 "there is nothing you need to do." :
			 "We noticed your internet connection stopped working %s. Our team is already on it, so there is no "
			 "need to call us. We will let you know when it is back to normal.", w);
	else if (is(c, slow))
		snprintf(buf, n, fixed ? "We noticed your internet was slower than it should be %s. It is back to its usual "
			 "speed now; there is nothing you need to do." :
			 "We noticed your internet has been slower than it should be %s. Our team is already looking into it, "
			 "so there is no need to call us.", w);
	else if (is(c, names))
		snprintf(buf, n, fixed ? "We noticed some websites were slow to open %s. That is sorted now; there is nothing "
			 "you need to do." :
			 "We noticed some websites were slow to open or did not open %s. Our team is already on it, so there "
			 "is no need to call us.", w);
	else if (!strcmp(c, "WIFI_CONGESTION"))
		snprintf(buf, n, fixed ? "We noticed nearby networks were crowding your home Wi-Fi %s, so your gateway moved "
			 "to a quieter setting. It should be smoother now; there is nothing you need to do." :
			 "We noticed nearby networks are crowding your home Wi-Fi %s. Your gateway is working on it and we "
			 "are keeping an eye on it.", w);
	else if (!strcmp(c, "WIFI_HOPPING"))
		snprintf(buf, n, fixed ? "We noticed one of your devices kept losing its Wi-Fi connection %s, so your gateway "
			 "set it to use a steadier connection. There is nothing you need to do." :
			 "We noticed one of your devices keeps losing its Wi-Fi connection %s. We can set it to use a steadier "
			 "connection; we will check with you first.", w);
	else if (!strcmp(c, "WIFI_COVERAGE"))
		snprintf(buf, n, fixed ? "We noticed the Wi-Fi was weak in part of your home %s. It is better now; there is "
			 "nothing you need to do." :
			 "We noticed the Wi-Fi is weak in part of your home %s. Moving the device a little closer to the "
			 "gateway helps; we will be in touch about ways to improve it.", w);
	else if (!strcmp(c, "WIFI_RADIO"))
		snprintf(buf, n, fixed ? "We noticed your Wi-Fi stopped working %s. It is working again now; there is nothing "
			 "you need to do." :
			 "We noticed your Wi-Fi stopped working %s. We are on it and may ask you to restart your gateway.", w);
	else if (is(c, device))
		snprintf(buf, n, fixed ? "We noticed one of your devices kept dropping off Wi-Fi %s. It is staying connected "
			 "now; there is nothing you need to do." :
			 "We noticed one of your devices keeps dropping off Wi-Fi %s. Turning its Wi-Fi off and on again "
			 "usually helps, and we are keeping an eye on it.", w);
	else if (!strcmp(c, "LINE_FULL"))
		snprintf(buf, n, fixed ? "We noticed calls and games were choppy %s while your connection was busy, so your "
			 "gateway now keeps them smooth. There is nothing you need to do." :
			 "We noticed calls and games were choppy %s while your connection was busy. Your gateway can keep "
			 "them smooth; we will check with you first.", w);
	else
		snprintf(buf, n, fixed ? "We noticed a problem with your connection %s and fixed it. There is nothing you "
			 "need to do." :
			 "We noticed a problem with your connection %s. Our team is already on it, so there is no need to "
			 "call us.", w);
}

int pro_plain(const char *s)
{
	static const char *const acr[] = { "DNS", "SQM", "MAC", "ISP", "AIGW", "SSID", "USP", "ACS", "TR", "IP", "GHz",
					   "dBm", "ms", "Mbit", "Mbps", "kbit", "LAN", "WAN", "QoS", "CPE", NULL };
	static const char *const words[] = { "packet", "latency", "jitter", "bufferbloat", "channel", "router",
					     "decision", "queue", "band", "radio", "protocol", "bandwidth", "modem",
					     "firmware", "ticket", NULL };
	const char *p;
	int i, up = 0;

	if (!s)
		return 1;
	for (p = s; *p; p++) {
		if (*p == '_')
			return 0;
		up = isupper((unsigned char)*p) ? up + 1 : 0;
		if (up >= 3)
			return 0;       /* a code */
	}
	for (i = 0; acr[i]; i++) {
		size_t l = strlen(acr[i]);

		for (p = strstr(s, acr[i]); p; p = strstr(p + 1, acr[i]))
			if ((p == s || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[l]))
				return 0;
	}
	for (i = 0; words[i]; i++) {
		size_t l = strlen(words[i]);

		for (p = s; *p; p++)
			if (!strncasecmp(p, words[i], l))
				return 0;
	}
	return 1;
}
