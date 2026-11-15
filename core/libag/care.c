/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW proactive care: when a problem starts and ends, who hears of it, the score (see ag/care.h) */
#include <stdio.h>
#include <string.h>

#include "ag/care.h"
#include "ag/diag.h"

static const char *const areas[CARE_AREAS] = { "Internet", "DNS", "WiFi", "Device", "AIGW" };

void care_params_default(struct care_params *p)
{
	p->confirm = 2;
	p->clear = 2;
	p->wifi_grace = 1800;
	p->max_gap = 1800;
}

int care_area_index(const char *area)
{
	int i;

	for (i = 0; area && i < CARE_AREAS; i++)
		if (!strcmp(area, areas[i]))
			return i;
	return -1;
}

const char *care_area_name(int idx)
{
	return idx >= 0 && idx < CARE_AREAS ? areas[idx] : "";
}

int care_bad(const struct care_verdict *v)
{
	return v && v->code && v->sev >= DS_WARN;
}

int care_isp_owned(const char *area)
{
	int a = care_area_index(area);

	return a == CA_INTERNET || a == CA_DNS;
}

static void take(struct care_state *s, const struct care_verdict *v)
{
	snprintf(s->code, sizeof(s->code), "%s", v->code);
	snprintf(s->area, sizeof(s->area), "%s", v->area ? v->area : "");
	s->sev = v->sev;
	snprintf(s->text, sizeof(s->text), "%s", v->text ? v->text : "");
}

/* the ISP must hear of it now: its own side at once, Wi-Fi once AIGW had its time */
static int due(const struct care_state *s, long now, const struct care_params *p)
{
	int a = care_area_index(s->area);

	if (a == CA_INTERNET || a == CA_DNS)
		return 1;
	return a == CA_WIFI && now - s->since >= p->wifi_grace;
}

int care_step(struct care_state *s, struct care_report *r, const struct care_verdict *v, long now,
	      const struct care_params *p)
{
	struct care_day *d = care_report_day(r, now);
	int bad = care_bad(v), ev = 0, a = bad ? care_area_index(v->area) : -1;

	long gap = 0;

	d->checks++;
	if (!bad)
		d->good++;
	else if (a >= 0)
		d->bad[a]++;
	/* the time since the last check was watched (a gap while agd was down
	   wasn't): problem time if one was open, else fine, though it is problem
	   time after all if the bad run before it turns into one */
	if (s->last_check && now > s->last_check && now - s->last_check <= p->max_gap)
		gap = now - s->last_check;
	d->watched_secs += (unsigned)gap;
	if (s->active)
		d->problem_secs += (unsigned)gap;
	else {
		d->ok_secs += (unsigned)gap;
		if (s->bad_run)
			s->pending += gap;
	}

	if (bad) {
		s->ok_run = 0;
		if (s->bad_run < 1000)
			s->bad_run++;
		if (!s->active) {
			if (s->bad_run == 1) {
				s->since = now;         /* the first bad check: when it started, if it lasts */
				s->pending = 0;
			}
			if (s->bad_run >= p->confirm) {
				s->active = 1;
				s->reported = 0;
				take(s, v);
				d->problems++;
				/* it was a problem since the first bad check */
				d->problem_secs += (unsigned)s->pending;
				d->ok_secs -= (unsigned)s->pending < d->ok_secs ? (unsigned)s->pending : d->ok_secs;
				s->pending = 0;
				ev |= CARE_STARTED;
			}
		} else if (strcmp(s->code, v->code)) {
			take(s, v);             /* another cause now (Wi-Fi, then the line): the newest */
			ev |= CARE_CHANGED;
		}
		if (s->active && !s->reported && due(s, now, p)) {
			s->reported = 1;
			d->reported++;
			ev |= CARE_REPORT;
		}
	} else {
		s->bad_run = 0;
		s->pending = 0;
		if (s->active && ++s->ok_run >= p->clear) {
			s->duration = now - s->since;
			if (!s->reported && care_area_index(s->area) == CA_WIFI) {
				if (s->acted_at && s->acted_at >= s->since - CARE_ACTED_BEFORE)
					d->fixed++;
				else
					d->cleared++;
			}
			s->active = 0;
			s->ok_run = 0;
			ev |= CARE_CLEARED;
		} else if (!s->active)
			s->ok_run = 0;
	}
	s->last_check = now;
	return ev;
}

void care_acted(struct care_state *s, long now)
{
	s->acted_at = now;
}

struct care_day *care_report_day(struct care_report *r, long wall_s)
{
	long day = wall_s / 86400;
	struct care_day *d = &r->d[day % CARE_DAYS];

	if (d->day != day) {
		memset(d, 0, sizeof(*d));
		d->day = day;
	}
	return d;
}

void care_report_sum(const struct care_report *r, long wall_s, int days, struct care_day *out)
{
	long today = wall_s / 86400;
	int i, k;

	memset(out, 0, sizeof(*out));
	out->day = today;
	for (i = 0; i < CARE_DAYS; i++) {
		const struct care_day *d = &r->d[i];

		if (!d->day || d->day > today || d->day <= today - days)
			continue;
		out->checks += d->checks;
		out->good += d->good;
		for (k = 0; k < CARE_AREAS; k++)
			out->bad[k] += d->bad[k];
		out->problems += d->problems;
		out->reported += d->reported;
		out->fixed += d->fixed;
		out->problem_secs += d->problem_secs;
		out->warnings += d->warnings;
		out->warned += d->warned;
		out->lead_secs += d->lead_secs;
		if (d->speed_tests && (!out->speed_tests || d->speed_kbps_min < out->speed_kbps_min))
			out->speed_kbps_min = d->speed_kbps_min;
		out->speed_tests += d->speed_tests;
		out->speed_kbps_sum += d->speed_kbps_sum;
		out->speed_low += d->speed_low;
		out->watched_secs += d->watched_secs;
		out->ok_secs += d->ok_secs;
		out->cleared += d->cleared;
	}
}

int care_score(const struct care_day *d)
{
	if (d->watched_secs)
		return (int)((unsigned long long)(d->ok_secs < d->watched_secs ? d->ok_secs : d->watched_secs) * 100 /
			     d->watched_secs);
	return d->checks ? (int)(d->good * 100 / d->checks) : -1;
}

int care_main_cause(const struct care_day *d)
{
	int k, best = -1;

	for (k = 0; k < CARE_AREAS; k++)
		if (d->bad[k] && (best < 0 || d->bad[k] > d->bad[best]))
			best = k;
	return best;
}
