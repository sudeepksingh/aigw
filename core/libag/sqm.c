/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW self-tuning smart queueing: the rules (see ag/sqm.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/sqm.h"

void sqm_params_default(struct sqm_params *p)
{
	memset(p, 0, sizeof(*p));
	p->window = 60;
	p->detect = 10;
	p->slow_pct = 50;
	p->trial = 30;
	p->grace = 2;
	p->keep_pct = 60;
	p->keep_ms = 30;
	p->speed_pct = 75;
	p->line_pct = 90;
	p->peak_pct = 85;
}

static int full(const struct why_min *s, const struct sqm_line *l)
{
	return s->t && s->route && why_full(s, l->down_mbps, l->peak_down_mbps, l->peak_up_mbps);
}

static int answered_ms(const struct why_min *s)
{
	return s->in_sent && s->in_lost < s->in_sent && s->in_ms != WHY_NONE ? s->in_ms : -1;
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

void sqm_stats_of(const struct why_min *m, int n, long from, long to, const struct sqm_line *l, struct sqm_stats *out)
{
	int *ms = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	uint32_t *kb = malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));
	int i, k = 0, a = 0;

	memset(out, 0, sizeof(*out));
	out->ms = -1;
	if (!ms || !kb) {
		free(ms);
		free(kb);
		return;
	}
	for (i = 0; i < n; i++) {
		const struct why_min *s = &m[i];
		int d;

		if ((long)s->t < from || (long)s->t >= to || !full(s, l))
			continue;
		kb[k++] = s->down_kbps;
		if ((d = answered_ms(s)) >= 0)
			ms[a++] = d;
	}
	out->n = k;
	if (k) {
		qsort(kb, (size_t)k, sizeof(*kb), cmp_u32);
		out->kbps = kb[k / 2];
	}
	if (a) {
		qsort(ms, (size_t)a, sizeof(*ms), cmp_int);
		out->ms = ms[a / 2];
	}
	free(ms);
	free(kb);
}

int sqm_bloated(const struct why_min *m, int n, long now, int step, const struct sqm_line *l, const struct sqm_params *p,
		char *why, size_t wn)
{
	int i, nfull = 0, nslow = 0, slow = why_slow_ms(l->normal_ms, l->have_normal);
	long from = now - (long)p->window * step;
	struct sqm_stats st;

	if (why && wn)
		why[0] = '\0';
	for (i = 0; i < n; i++) {
		const struct why_min *s = &m[i];
		int d;

		if ((long)s->t < from || (long)s->t >= now || !full(s, l))
			continue;
		nfull++;
		if ((d = answered_ms(s)) >= 0 && d >= slow)
			nslow++;
	}
	if (nslow < p->detect || nslow * 100 < nfull * p->slow_pct)
		return 0;
	sqm_stats_of(m, n, from, now, l, &st);
	if (why && wn) {
		char nrm[40] = "";

		if (l->have_normal)
			snprintf(nrm, sizeof(nrm), " (normally %.0f ms)", l->normal_ms);
		snprintf(why, wn, "Bufferbloat: in the last %d samples the line was full %d times, and %d of them the internet "
			 "answered slowly, typically in %d ms%s: everything else waits behind the downloads. Smart queueing "
			 "keeps the queue short and fair.", p->window, nfull, nslow, st.ms, nrm);
	}
	return 1;
}

int sqm_rates(const struct sqm_line *l, const struct sqm_params *p, uint32_t *down_kbps, uint32_t *up_kbps)
{
	*down_kbps = *up_kbps = 0;
	if (l->down_mbps > 0)
		*down_kbps = (uint32_t)(l->down_mbps * 1000.0f * (float)p->line_pct / 100.0f);
	else if (l->peak_down_mbps >= 2)
		*down_kbps = (uint32_t)(l->peak_down_mbps * 1000.0f * (float)p->peak_pct / 100.0f);
	/* the upload's speed is never sold or tested here: the most seen, when there is enough of it */
	if (l->peak_up_mbps >= 0.5f)
		*up_kbps = (uint32_t)(l->peak_up_mbps * 1000.0f * (float)p->peak_pct / 100.0f);
	return *down_kbps ? 0 : -1;
}

int sqm_judge(const struct sqm_stats *before, const struct sqm_stats *after, const struct sqm_line *l,
	      const struct sqm_params *p, char *why, size_t wn)
{
	int fine = l->have_normal ? (int)l->normal_ms + p->keep_ms : 40 + p->keep_ms;
	int lower, speed_ok;

	if (why && wn)
		why[0] = '\0';
	if (after->n < p->trial || after->ms < 0)
		return SQ_WAIT;
	lower = before->ms > 0 ? after->ms * 100 <= before->ms * p->keep_pct || after->ms <= fine : after->ms <= fine;
	if (before->ms > 0 && after->ms >= before->ms)
		lower = 0;                      /* never "better" when it isn't lower at all */
	speed_ok = !before->kbps || (uint64_t)after->kbps * 100 >= (uint64_t)before->kbps * (uint64_t)p->speed_pct;
	if (lower && speed_ok) {
		if (before->ms > 0)
			snprintf(why, wn, "kept: while the line was full the internet answered in %d ms (before %d ms), downloads "
				 "%.1f Mbit/s (before %.1f)", after->ms, before->ms, after->kbps / 1000.0, before->kbps / 1000.0);
		else
			snprintf(why, wn, "kept: while the line was full the internet answered in %d ms, downloads %.1f Mbit/s "
				 "(before: not measured)", after->ms, after->kbps / 1000.0);
		return SQ_KEEP;
	}
	if (!lower)
		snprintf(why, wn, "tried: while the line was full the internet still answered in %d ms (before %d ms): no "
			 "better; back as it was", after->ms, before->ms);
	else
		snprintf(why, wn, "tried: the delay fell to %d ms (before %d ms), but downloads slowed to %.1f Mbit/s (before "
			 "%.1f): too much; back as it was", after->ms, before->ms, after->kbps / 1000.0, before->kbps / 1000.0);
	return SQ_UNDO;
}
