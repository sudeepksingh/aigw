/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Wi-Fi sensing: links, robust features, sticky HMMs, the day's activity
 * (see ag/sense.h).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ag/sense.h"

void sn_params_default(struct sn_params *p)
{
	p->window = 6;
	p->look_s = 5;
	p->min_fresh = 3;
	p->k_spread = 2.0f;
	p->min_db = 1.0f;
	p->cp_db = 4;
	p->learn = 20;
	p->stay_zone = 0.95f;
	p->night_from = 23;
	p->night_to = 7;
	p->stay_home = 0.995f;
	p->learn_days = 7;
	p->checkin_min = 45;
}

void sn_init(struct sn_state *s)
{
	int k;

	memset(s, 0, sizeof(*s));
	s->p_home = 0.5f;
	s->home = 1;
	for (k = 0; k < SN_LINKS; k++)
		s->l[k].csi = 255;
}

int sn_zone(struct sn_state *s, const char *name)
{
	int k;

	if (!name || !*name)
		name = "home";
	for (k = 0; k < s->nz; k++)
		if (!strcasecmp(s->z[k].name, name))
			return k;
	if (s->nz >= SN_ZONES)
		return -1;
	memset(&s->z[s->nz], 0, sizeof(s->z[0]));
	snprintf(s->z[s->nz].name, sizeof(s->z[0].name), "%s", name);
	s->z[s->nz].p = 0.1f;
	return s->nz++;
}

static struct sn_link *link_of(struct sn_state *s, const char *mac, int band, int create)
{
	struct sn_link *free_l = NULL, *old = NULL;
	int k;

	for (k = 0; k < SN_LINKS; k++) {
		struct sn_link *l = &s->l[k];

		if (l->mac[0] && l->band == band && !strcasecmp(l->mac, mac))
			return l;
		if (!l->mac[0] && !free_l)
			free_l = l;
		if (!old || l->last < old->last)
			old = l;
	}
	if (!create)
		return NULL;
	if (!free_l)
		free_l = old;           /* the link not seen for longest gives way */
	memset(free_l, 0, sizeof(*free_l));
	snprintf(free_l->mac, sizeof(free_l->mac), "%s", mac);
	free_l->band = (uint8_t)band;
	free_l->csi = 255;
	return free_l;
}

void sn_look(struct sn_state *s, const char *mac, int band, int zone, int personal, int dbm, uint32_t rx, long t)
{
	struct sn_link *l;

	if (!mac || !*mac || !dbm || dbm < -110 || dbm > 0)
		return;
	if (!(l = link_of(s, mac, band, 1)))
		return;
	l->zone = (uint8_t)(zone >= 0 && zone < SN_ZONES ? zone : 0);
	l->personal = personal ? 1 : 0;
	s->personal_known |= l->personal;
	l->present = 1;
	l->looks++;
	/* only a reading from frames sent since the last look says something about now */
	if (rx != l->rx && l->rx && l->nbuf < SN_WIN)
		l->buf[l->nbuf++] = (int8_t)dbm;
	l->rx = rx;
	l->last = t;
}

void sn_csi(struct sn_state *s, const char *mac, int score)
{
	int k;

	for (k = 0; k < SN_LINKS; k++)
		if (s->l[k].mac[0] && !strcasecmp(s->l[k].mac, mac))
			s->l[k].csi = (uint8_t)(score < 0 ? 0 : score > 100 ? 100 : score);
}

static void sort8(int8_t *v, int n)
{
	int i, j;

	for (i = 1; i < n; i++) {
		int8_t x = v[i];

		for (j = i; j > 0 && v[j - 1] > x; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

/* this link's window: 1 moving, 0 still, -1 nothing to judge */
static int judge_link(struct sn_state *s, struct sn_link *l, const struct sn_params *p)
{
	int8_t v[SN_WIN];
	int med, k, moving = 0, cp = 0, dev, top = 0, sum = 0;
	float spread;

	if (l->csi != 255 && l->csi >= 50)
		moving = 2;             /* the vendor's CSI says so */
	if (l->nbuf < p->min_fresh)
		return moving ? 1 : -1;
	memcpy(v, l->buf, l->nbuf);
	sort8(v, l->nbuf);
	med = l->nbuf & 1 ? v[l->nbuf / 2] : (v[l->nbuf / 2 - 1] + v[l->nbuf / 2]) / 2;
	/* the spread around the median, the largest deviation left out: one stray frame is not movement */
	for (k = 0; k < l->nbuf; k++) {
		dev = l->buf[k] > med ? l->buf[k] - med : med - l->buf[k];
		sum += dev;
		top = dev > top ? dev : top;
	}
	spread = l->nbuf >= 4 ? (float)(sum - top) / (float)(l->nbuf - 1) : (float)sum / (float)l->nbuf;
	if (l->last_med) {
		cp = med - l->last_med;
		cp = cp < 0 ? -cp : cp;
	}
	if (l->quiet_n >= p->learn &&
	    ((spread >= l->quiet * p->k_spread && spread >= l->quiet + p->min_db) || cp >= p->cp_db))
		moving = 1;
	/* the quiet spread, from quiet windows only (a long average once learned) */
	if (!moving && !s->frozen) {
		float a = l->quiet_n < p->learn ? 1.0f / (float)(l->quiet_n + 1) : 0.05f;

		/* while learning, a window far above the rest is not taken as quiet */
		if (l->quiet_n < 3 || spread <= 3 * l->quiet + 1) {
			l->quiet += (spread - l->quiet) * a;
			if (l->quiet < 0.3f)
				l->quiet = 0.3f;
			if (l->quiet_n < 65535)
				l->quiet_n++;
		}
	}
	l->last_med = (int8_t)med;
	return moving ? 1 : 0;
}

/* one step of a sticky two-state filter: P(state 1) given the likelihood ratio of this window's evidence */
static float step(float p, float stay, float lr)
{
	float prior = p * stay + (1 - p) * (1 - stay), post = prior * lr / (prior * lr + (1 - prior));

	return post < 0.002f ? 0.002f : post > 0.998f ? 0.998f : post;
}

/* 1 on Saturdays and Sundays (day 0, 1970-01-01, was a Thursday) */
static int weekend(long day)
{
	return (day + 3) % 7 >= 5;
}

/* the profile for that kind of day: its own once learned on 2 days, else the other kind's */
static const uint16_t *profile(const struct sn_state *s, long day)
{
	int k = weekend(day);

	if (s->days_of[k] < 2 && s->days_of[!k] >= 2)
		k = !k;
	return s->days_of[k] >= 2 ? s->usual[k] : NULL;
}

static float usual_p(const struct sn_state *s, int hour, const struct sn_params *p)
{
	const uint16_t *u = profile(s, s->day);
	float per_hour = 3600.0f / (float)(p->window * p->look_s);

	return u ? (float)u[hour % 24] / 10.0f / per_hour : 0.1f;
}

static void new_day(struct sn_state *s, long day)
{
	int h;

	if (s->day && day != s->day && !s->frozen) {
		int judged = 0, k = weekend(s->day);

		for (h = 0; h < 24; h++)
			judged += s->judged[h];
		if (judged >= 24) {     /* a day with something to judge (not one with the gateway off) */
			int w = s->days_of[k] < 13 ? s->days_of[k] + 1 : 14;

			for (h = 0; h < 24; h++) {
				int x = s->seen[h] * 10;

				if (!s->judged[h])
					continue;
				s->usual[k][h] = (uint16_t)(s->days_of[k] ? s->usual[k][h] + (x - s->usual[k][h]) / w : x);
			}
			if (s->days_of[k] < 65535)
				s->days_of[k]++;
			if (s->days < 65535)
				s->days++;
		}
	}
	if (day != s->day) {
		memset(s->seen, 0, sizeof(s->seen));
		memset(s->judged, 0, sizeof(s->judged));
		s->day = day;
	}
}

int sn_window(struct sn_state *s, long t, int hour, long day, const struct sn_params *p)
{
	int zev[SN_ZONES], k, ev = 0, any = 0, judged = 0, personal = 0, was = s->home;
	float lr = 1, u;

	hour = hour < 0 || hour > 23 ? 0 : hour;
	new_day(s, day);
	s->windows++;
	for (k = 0; k < SN_ZONES; k++)
		zev[k] = -1;
	for (k = 0; k < SN_LINKS; k++) {
		struct sn_link *l = &s->l[k];
		int m;

		if (!l->mac[0])
			continue;
		if (l->present && l->personal)
			personal++;
		m = l->present ? judge_link(s, l, p) : -1;
		l->motion = m == 1;
		if (m >= 0 && l->zone < SN_ZONES && zev[l->zone] < m)
			zev[l->zone] = m;
		l->nbuf = 0;
		l->looks = 0;
		l->present = 0;
		l->csi = 255;
	}

	/* each zone: still or moving, sticky */
	for (k = 0; k < s->nz; k++) {
		struct sn_zone *z = &s->z[k];
		float r = zev[k] == 1 ? 0.8f / 0.04f : zev[k] == 0 ? 0.2f / 0.96f : 1.0f;
		int was_z = z->moving;

		z->p = step(z->p, p->stay_zone, r);
		z->moving = z->p >= 0.5f;
		if (z->moving != was_z)
			z->since = t;
		if (z->moving) {
			z->last_motion = t;
			z->motion_windows++;
			any = 1;
		}
		judged |= zev[k] >= 0;
	}

	/* someone home: movement, their own devices, and how usual stillness is at this hour */
	u = usual_p(s, hour, p);
	if (any)
		lr *= 20, snprintf(s->why, sizeof(s->why), "movement");
	else if (judged && !s->personal_known && !(hour >= p->night_from || hour < p->night_to))
		lr *= u > 0.05f ? (1 - (u > 0.9f ? 0.9f : u)) / 0.98f : 0.95f;  /* stillness by day: slowly, nobody home */
	if (s->personal_known)
		lr *= personal ? 8.0f : 0.3f;
	if (personal && !any)
		snprintf(s->why, sizeof(s->why), "a personal device is here");
	s->p_home = step(s->p_home, p->stay_home, lr);
	s->home = s->p_home >= 0.5f;
	if (s->home != was) {
		ev |= SN_EV_PRESENCE;
		s->home_since = t;
		if (!s->home)
			snprintf(s->why, sizeof(s->why), "%s", s->personal_known && !personal ?
				 "no movement, and the personal devices left" : "no movement for a long time");
	}

	/* the day's activity, and the check-in */
	if (judged || any) {
		s->judged[hour]++;
		if (any && s->seen[hour] < 65535)
			s->seen[hour]++;
	}
	s->run = any ? (uint8_t)(s->run < 255 ? s->run + 1 : 255) : 0;
	/* for the check-in, a minute of movement (one stray window is not someone); two to end an alarm */
	if (s->run >= (s->inactive ? 4 : 2)) {
		s->last_motion = t;
		s->expected = 0;
		if (s->inactive) {
			s->inactive = 0;
			ev |= SN_EV_ACTIVE_AGAIN;
		}
	} else if (!any && (judged || s->last_motion)) {
		/* away by its own devices: not inactivity */
		if (s->personal_known && !personal)
			s->expected = 0;
		else
			s->expected += usual_p(s, hour, p);
		if (!s->inactive && s->days >= p->learn_days && s->last_motion &&
		    s->expected * (float)(p->window * p->look_s) / 60.0f >= (float)p->checkin_min) {
			s->inactive = 1;
			s->inactive_since = s->last_motion;
			ev |= SN_EV_INACTIVE;
		}
	}
	return ev;
}

void sn_activity_minutes(const struct sn_state *s, int hour, const struct sn_params *p, int *today, int *usual)
{
	const uint16_t *pu = profile(s, s->day);
	float w = (float)(p->window * p->look_s) / 60.0f, u = 0;
	int h, t = 0;

	for (h = 0; h <= hour && h < 24; h++) {
		t += s->seen[h];
		u += pu ? (float)pu[h] / 10.0f : 0;
	}
	*today = (int)((float)t * w + 0.5f);
	*usual = s->days >= p->learn_days && pu ? (int)(u * w + 0.5f) : -1;
}

const char *sn_activity(const struct sn_state *s, int hour, const struct sn_params *p)
{
	int today, usual;

	if (s->inactive)
		return "Inactive";
	sn_activity_minutes(s, hour, p, &today, &usual);
	if (usual < 0)
		return "Learning";
	if (usual < 10 || today * 2 >= usual)
		return "AsUsual";
	return today ? "Less" : "None";
}

void sn_activity_words(const struct sn_state *s, int hour, const struct sn_params *p, char *buf, size_t n)
{
	const char *a = sn_activity(s, hour, p);
	int today, usual;

	sn_activity_minutes(s, hour, p, &today, &usual);
	if (usual < 0)
		snprintf(buf, n, "Movement seen for %d min today by %02d:00; AIGW is still learning what is usual (%d of %d "
			 "days).", today, hour + 1 > 23 ? 0 : hour + 1, s->days, p->learn_days);
	else if (!strcmp(a, "Inactive"))
		snprintf(buf, n, "No movement seen for a while, when there usually is: %d min today by %02d:00, usually "
			 "about %d by now.", today, hour + 1 > 23 ? 0 : hour + 1, usual);
	else
		snprintf(buf, n, "Movement seen for %d min today by %02d:00, usually about %d by now: %s.", today,
			 hour + 1 > 23 ? 0 : hour + 1, usual, !strcmp(a, "AsUsual") ? "as usual" :
			 !strcmp(a, "Less") ? "less than usual" : "none yet");
}
