/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW calls and games first: the rules (see ag/calls.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/calls.h"

void call_params_default(struct call_params *p)
{
	memset(p, 0, sizeof(*p));
	p->min_pps = 5;
	p->max_kbps = 10000;
	p->max_pkt = 1300;
	p->ratio = 25;
	p->steady = 2;
	p->spread_pct = 60;
	p->video_kbps = 300;
	p->smooth_ms = 150;
	p->smooth_jitter = 30;
	p->smooth_loss10 = 10;
}

/* ---------- hints: ports and names ---------- */

static const struct port_hint {
	uint16_t lo, hi;
	uint8_t kind;
	const char *app;
} ports[] = {
	{ 3478, 3481, CK_CALL, NULL },          /* STUN and TURN: Teams, WhatsApp, Meet, FaceTime, ... */
	{ 5349, 5349, CK_CALL, NULL },          /* TURN over DTLS */
	{ 8801, 8810, CK_CALL, "Zoom" },
	{ 19302, 19309, CK_CALL, "Google Meet" },
	{ 16384, 16387, CK_CALL, "FaceTime" },
	{ 16393, 16402, CK_CALL, "FaceTime" },
	{ 9000, 9000, CK_CALL, "Webex" },
	{ 3074, 3074, CK_GAME, "Xbox Live" },
	{ 3659, 3659, CK_GAME, "EA" },
	{ 27000, 27050, CK_GAME, "Steam" },
	{ 7000, 7500, CK_GAME, "Riot Games" },
	{ 19132, 19133, CK_GAME, "Minecraft" },
};

static const struct name_hint {
	const char *suffix;
	uint8_t kind;
	const char *app;
} names[] = {
	{ "zoom.us", CK_CALL, "Zoom" },
	{ "zoom.com", CK_CALL, "Zoom" },
	{ "teams.microsoft.com", CK_CALL, "Microsoft Teams" },
	{ "teams.live.com", CK_CALL, "Microsoft Teams" },
	{ "skype.com", CK_CALL, "Skype" },
	{ "lync.com", CK_CALL, "Microsoft Teams" },
	{ "meet.google.com", CK_CALL, "Google Meet" },
	{ "stun.l.google.com", CK_CALL, "Google Meet" },
	{ "whatsapp.net", CK_CALL, "WhatsApp" },
	{ "whatsapp.com", CK_CALL, "WhatsApp" },
	{ "facetime.apple.com", CK_CALL, "FaceTime" },
	{ "discord.gg", CK_CALL, "Discord" },
	{ "discord.media", CK_CALL, "Discord" },
	{ "webex.com", CK_CALL, "Webex" },
	{ "signal.org", CK_CALL, "Signal" },
	{ "xboxlive.com", CK_GAME, "Xbox Live" },
	{ "playstation.net", CK_GAME, "PlayStation" },
	{ "riotgames.com", CK_GAME, "Riot Games" },
	{ "steamserver.net", CK_GAME, "Steam" },
	{ "valve.net", CK_GAME, "Steam" },
	{ "epicgames.dev", CK_GAME, "Epic Games" },
	{ "epicgames.com", CK_GAME, "Epic Games" },
	{ "roblox.com", CK_GAME, "Roblox" },
	{ "minecraft.net", CK_GAME, "Minecraft" },
	{ "nintendo.net", CK_GAME, "Nintendo" },
	{ "battle.net", CK_GAME, "Battle.net" },
	{ "callofduty.com", CK_GAME, "Call of Duty" },
	{ "ea.com", CK_GAME, "EA" },
};

/* never a call: DNS, QUIC (the web), time, discovery, VPNs (a tunnel may carry a download) */
static int never(uint16_t port)
{
	static const uint16_t no[] = { 53, 67, 68, 123, 137, 138, 443, 500, 853, 1194, 1900, 4500, 5353, 51820 };
	size_t i;

	for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
		if (port == no[i])
			return 1;
	return 0;
}

/* name ends with suffix, on a label boundary ("eu.zoom.us", not "notzoom.us") */
static int ends(const char *name, size_t l, const char *suffix)
{
	size_t s = strlen(suffix);

	return l >= s && !strncmp(name + l - s, suffix, s) && (l == s || name[l - s - 1] == '.');
}

int call_hint(uint8_t proto, uint16_t rport, const char *name, int *by, const char **app)
{
	size_t i, l;

	*app = NULL;
	*by = CB_TRAITS;
	if (name && *name) {
		/* "x.zoom.us (nearby)": the name the home looked up for the same /24 */
		const char *sp = strchr(name, ' ');

		l = sp ? (size_t)(sp - name) : strlen(name);
		while (l && name[l - 1] == '.')
			l--;
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (ends(name, l, names[i].suffix)) {
				*by = CB_NAME;
				*app = names[i].app;
				return names[i].kind;
			}
	}
	if (proto != 17)
		return CK_NONE;
	for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
		if (rport >= ports[i].lo && rport <= ports[i].hi) {
			*by = CB_PORT;
			*app = ports[i].app;
			return ports[i].kind;
		}
	return CK_NONE;
}

const char *call_kind_name(int kind)
{
	return kind == CK_CALL ? "Call" : kind == CK_GAME ? "Game" : kind == CK_RT ? "Realtime" : "None";
}

const char *call_kind_words(int kind)
{
	return kind == CK_CALL ? "call" : kind == CK_GAME ? "game" : "call or game";
}

/* ---------- the traits ---------- */

static uint32_t per_s(uint32_t v, uint32_t ms)
{
	return ms ? (uint32_t)((uint64_t)v * 1000 / ms) : 0;
}

/* one interval that looks like a call or a game; its packets a second, both ways together */
static int looks(const struct call_iv *v, const struct call_params *p, uint32_t *pps, int *video)
{
	uint32_t up = per_s(v->up_pk, v->ms), down = per_s(v->down_pk, v->ms);
	uint32_t ukb, dkb, hi, lo;

	if (v->ms < 500 || up < (uint32_t)p->min_pps || down < (uint32_t)p->min_pps)
		return 0;
	if (v->up_b / v->up_pk > (uint32_t)p->max_pkt || v->down_b / v->down_pk > (uint32_t)p->max_pkt)
		return 0;
	ukb = (uint32_t)((uint64_t)v->up_b * 8 / v->ms);       /* bits per ms = kbit/s */
	dkb = (uint32_t)((uint64_t)v->down_b * 8 / v->ms);
	if (ukb > (uint32_t)p->max_kbps || dkb > (uint32_t)p->max_kbps)
		return 0;
	hi = up > down ? up : down;
	lo = up > down ? down : up;
	if (hi > lo * (uint32_t)p->ratio)
		return 0;                       /* one way: a stream, not a conversation */
	*pps = up + down;
	if (ukb > (uint32_t)p->video_kbps || dkb > (uint32_t)p->video_kbps || v->up_b / v->up_pk >= 400 ||
	    v->down_b / v->down_pk >= 400)
		*video = 1;
	return 1;
}

int call_classify(const struct call_flow *f, const struct call_params *p, struct call_class *out)
{
	uint32_t lo = 0, hi = 0, pps;
	int i, video = 0, hk, by;
	const char *app;

	memset(out, 0, sizeof(*out));
	if (f->proto != 17 || never(f->rport) || f->n < p->steady || p->steady < 1)
		return 0;
	for (i = f->n - p->steady; i < f->n; i++) {
		if (!looks(&f->iv[i], p, &pps, &video))
			return 0;
		if (i == f->n - p->steady || pps < lo)
			lo = pps;
		if (pps > hi)
			hi = pps;
	}
	if ((uint64_t)lo * 100 < (uint64_t)hi * (uint64_t)(100 - p->spread_pct))
		return 0;                       /* bursts: not a steady rate */
	hk = call_hint(f->proto, f->rport, f->name, &by, &app);
	if (hk != CK_NONE) {
		out->kind = hk;
		out->by = by;
		out->app = app;
	} else {
		out->kind = video ? CK_CALL : CK_RT;
		out->by = CB_TRAITS;
	}
	return 1;
}

/* ---------- how it went ---------- */

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y;
}

static int full(const struct why_min *s, const struct sqm_line *l)
{
	return s->t && s->route && why_full(s, l->down_mbps, l->peak_down_mbps, l->peak_up_mbps);
}

static int by_time(const void *a, const void *b)
{
	const struct why_min *x = *(const struct why_min *const *)a, *y = *(const struct why_min *const *)b;

	return x->t < y->t ? -1 : x->t > y->t;
}

void call_quality_of(const struct why_min *m, int n, long from, long to, const struct sqm_line *l,
		     struct call_quality *q)
{
	const struct why_min **s = malloc(sizeof(*s) * (size_t)(n > 0 ? n : 1));
	int *ms = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int *kb = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int i, k = 0, a = 0, nk = 0, only_full, jn = 0, prev = -1;
	long sent = 0, lost = 0, jsum = 0;

	memset(q, 0, sizeof(*q));
	q->ms = q->jitter_ms = -1;
	if (!s || !ms || !kb)
		goto out;
	for (i = 0; i < n; i++)
		if (m[i].t && (long)m[i].t >= from && (long)m[i].t < to) {
			s[k++] = &m[i];
			if (full(&m[i], l))
				q->full++;
		}
	q->samples = k;
	qsort(s, (size_t)k, sizeof(*s), by_time);
	only_full = q->full > 0;
	for (i = 0; i < k; i++) {
		const struct why_min *x = s[i];
		int ok = x->in_sent && x->in_lost < x->in_sent && x->in_ms != WHY_NONE;

		if (only_full && !full(x, l))
			continue;
		q->judged++;
		sent += x->in_sent;
		lost += x->in_lost;
		if (only_full)
			kb[nk++] = (int)x->down_kbps;
		if (!ok) {
			prev = -1;
			continue;
		}
		ms[a++] = x->in_ms;
		if (prev >= 0) {
			jsum += x->in_ms > prev ? x->in_ms - prev : prev - x->in_ms;
			jn++;
		}
		prev = x->in_ms;
	}
	if (a) {
		qsort(ms, (size_t)a, sizeof(int), cmp_int);
		q->ms = ms[a / 2];
		q->jitter_ms = jn ? (int)((jsum + jn / 2) / jn) : 0;
	}
	if (sent)
		q->loss10 = (int)((lost * 1000 + sent / 2) / sent);
	if (nk) {
		qsort(kb, (size_t)nk, sizeof(int), cmp_int);
		q->line_kbps = (uint32_t)kb[nk / 2];
	}
out:
	free(s);
	free(ms);
	free(kb);
}

int call_verdict(const struct call_quality *q, const struct call_params *p)
{
	if (!q->judged || q->ms < 0)
		return CV_UNKNOWN;
	if (q->ms <= p->smooth_ms && q->jitter_ms <= p->smooth_jitter && q->loss10 <= p->smooth_loss10)
		return CV_SMOOTH;
	return CV_ROUGH;
}

const char *call_verdict_name(int v)
{
	return v == CV_SMOOTH ? "Smooth" : v == CV_ROUGH ? "Rough" : "Unknown";
}

/* ---------- in words ---------- */

void call_duration(long s, char *buf, size_t n)
{
	if (s < 0)
		s = 0;
	if (s < 90)
		snprintf(buf, n, "%ld s", s);
	else if (s < 3600)
		snprintf(buf, n, "%ld min", (s + 30) / 60);
	else
		snprintf(buf, n, "%ld h %02ld min", s / 3600, (s % 3600 + 30) / 60);
}

void call_words(const struct call_report *r, char *buf, size_t n)
{
	const struct call_quality *q = &r->q;
	char d[24], fd[24], who[120], line[120] = "", loss[24];
	size_t l;
	long len = r->end - r->start;

	call_duration(len, d, sizeof(d));
	if (r->device && *r->device)
		snprintf(who, sizeof(who), "The %s on %s%s%s%s", call_kind_words(r->kind), r->device, r->app ? " (" : "",
			 r->app ? r->app : "", r->app ? ")" : "");
	else
		snprintf(who, sizeof(who), "A %s", call_kind_words(r->kind));
	if (q->full) {
		/* samples are a minute (why.interval): the share of the call's length */
		long fs = q->samples ? len * q->full / q->samples : 0;

		call_duration(fs, fd, sizeof(fd));
		if (r->top && *r->top)
			snprintf(line, sizeof(line), " while the line was full for %s (%s downloading, %.1f Mbit/s)", fd, r->top,
				 q->line_kbps / 1000.0);
		else
			snprintf(line, sizeof(line), " while the line was full for %s (%.1f Mbit/s)", fd, q->line_kbps / 1000.0);
	}
	if (q->loss10 == 0)
		snprintf(loss, sizeof(loss), "no loss");
	else
		snprintf(loss, sizeof(loss), "%d.%d%% lost", q->loss10 / 10, q->loss10 % 10);
	if (r->verdict == CV_UNKNOWN)
		l = (size_t)snprintf(buf, n, "%s lasted %s%s; the internet's delay wasn't measured meanwhile.", who, d, line);
	else
		l = (size_t)snprintf(buf, n, "%s %s for %s%s%s: the internet answered in %d ms, jitter %d ms, %s.", who,
				     r->verdict == CV_SMOOTH ? "stayed smooth" : "was rough", d, line,
				     q->full ? "" : "; the line wasn't full meanwhile", q->ms, q->jitter_ms < 0 ? 0 : q->jitter_ms,
				     loss);
	if (l >= n)
		return;
	if (r->prioritized)
		snprintf(buf + l, n - l, " It went first in smart queueing.");
	else if (q->full && !r->sqm_on)
		snprintf(buf + l, n - l, " Smart queueing was off, so it waited behind the download.");
}
