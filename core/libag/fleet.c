/* SPDX-License-Identifier: Apache-2.0 */
/* Fleet learning: buckets, the Bayesian blend, the choice, the priors file (see ag/fleet.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag/fleet.h"

#define MAX_PRIORS 4096
#define MAX_NORMS  512

/* which dimensions a level keeps: band, width, congestion, device, hour, line */
static const char masks[FL_LEVELS][FL_DIMS] = {
	{ 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 0, 1 }, { 1, 0, 1, 0, 0, 1 }, { 1, 0, 0, 0, 0, 0 }, { 0 },
};

int fl_hour_band(int hour)
{
	return hour < 0 || hour > 23 ? 0 : hour / 6;
}

const char *fl_hour_band_name(int band)
{
	static const char *const n[] = { "night", "morning", "afternoon", "evening" };

	return band >= 0 && band < 4 ? n[band] : "-";
}

const char *fl_result_name(int r)
{
	static const char *const n[] = { "helped", "no-change", "worse", "undone" };

	return r >= 0 && r < FL_RESULTS ? n[r] : "";
}

const char *fl_device_class(const char *type)
{
	static const struct { const char *type, *cls; } t[] = {
		{ "iPhone", "phone" }, { "iPad", "phone" }, { "Android", "phone" }, { "watch", "phone" },
		{ "Mac", "laptop" }, { "Windows", "laptop" }, { "Linux", "laptop" },
		{ "TV", "tv" }, { "media player", "tv" },
		{ "IoT", "iot" }, { "printer", "iot" }, { "camera", "iot" }, { "smart speaker", "iot" }, { "smart home", "iot" },
	};
	size_t i;

	if (!type || !*type)
		return "-";
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcasecmp(type, t[i].type))
			return t[i].cls;
	return "other";
}

void fl_key(const struct fl_ctx *c, int level, char *buf, size_t n)
{
	size_t len = 0;
	int d;

	if (level < 0 || level >= FL_LEVELS)
		level = FL_LEVELS - 1;
	buf[0] = '\0';
	for (d = 0; d < FL_DIMS && len < n; d++) {
		const char *v = masks[level][d] ? (c && c->f[d][0] ? c->f[d] : "-") : "*";
		int w = snprintf(buf + len, n - len, "%s%s", d ? "/" : "", v);

		if (w < 0)
			break;
		len += (size_t)w;
	}
}

int fl_word_ok(const char *s, size_t max)
{
	size_t i, n = s ? strlen(s) : 0;

	if (n < 1 || n >= max)
		return 0;
	for (i = 0; i < n; i++) {
		char ch = s[i];

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
		      strchr("_.+-*/", ch)))
			return 0;
	}
	return 1;
}

int fl_key_level(const char *key)
{
	char star[FL_DIMS];
	const char *p = key;
	int d = 0, l;

	if (!fl_word_ok(key, FL_KEY))
		return -1;
	for (;;) {
		size_t w = strcspn(p, "/");

		if (d >= FL_DIMS || w == 0)
			return -1;
		star[d++] = !(w == 1 && *p == '*');
		if (!p[w])
			break;
		p += w + 1;
	}
	if (d != FL_DIMS)
		return -1;
	for (l = 0; l < FL_LEVELS; l++)
		if (!memcmp(star, masks[l], FL_DIMS))
			return l;
	return -1;
}

void fl_count(struct fl_counts *c, int result, int sign)
{
	uint32_t *f = result == FL_HELPED ? &c->helped : result == FL_WORSE ? &c->worse :
		      result == FL_UNDONE ? &c->undone : NULL;

	if (sign > 0) {
		c->tried++;
		if (f)
			(*f)++;
		return;
	}
	if (c->tried)
		c->tried--;
	if (f && *f)
		(*f)--;
}

int fl_counts_ok(const struct fl_counts *c)
{
	return (uint64_t)c->helped + c->worse + c->undone <= c->tried;
}

/* ---------- the priors file ---------- */

void fl_priors_free(struct fl_priors *P)
{
	if (!P)
		return;
	free(P->p);
	free(P->nm);
	free(P);
}

struct fl_priors *fl_priors_parse(const char *text, size_t len, char *err, size_t en)
{
	struct fl_priors *P = calloc(1, sizeof(*P));
	char *buf = malloc(len + 1), *line, *save = NULL;
	int head = 0, ln = 0;

	if (!P || !buf) {
		snprintf(err, en, "out of memory");
		goto bad;
	}
	memcpy(buf, text, len);
	buf[len] = '\0';
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char a[FL_KEY], b[FL_KEY], c[FL_KEY];
		unsigned h, t, he, w, u;
		float mean, dev;
		int m, band;

		ln++;
		line[strcspn(line, "\r")] = '\0';
		if (!*line)
			continue;
		if (!strncmp(line, "# aigw-priors ", 14)) {
			if (strcmp(line + 14, "fleet-priors")) {
				snprintf(err, en, "not the fleet's priors (%.40s)", line + 14);
				goto bad;
			}
			head = 1;
		} else if (!strncmp(line, "# version ", 10)) {
			P->version = atoi(line + 10);
		} else if (!strncmp(line, "# published ", 12)) {
			snprintf(P->published, sizeof(P->published), "%.*s", (int)strcspn(line + 12, " "), line + 12);
		} else if (!strncmp(line, "# k ", 4)) {
			P->k = atoi(line + 4);
		} else if (!strncmp(line, "# homes ", 8)) {
			P->homes = (uint32_t)strtoul(line + 8, NULL, 10);
		} else if (line[0] == '#') {
			continue;
		} else if (line[0] == 'o' && line[1] == ' ') {
			struct fl_prior *p;

			if (sscanf(line + 2, "%23s %15s %47s %u %u %u %u %u", a, b, c, &h, &t, &he, &w, &u) != 8 ||
			    !fl_word_ok(a, FL_CODE) || !fl_word_ok(b, FL_ACTION) || fl_key_level(c) < 0 || !h) {
				snprintf(err, en, "line %d: not an outcome bucket", ln);
				goto bad;
			}
			if (P->n >= MAX_PRIORS) {
				snprintf(err, en, "more than %d buckets", MAX_PRIORS);
				goto bad;
			}
			if (!(P->n & 63)) {
				void *x = realloc(P->p, sizeof(*P->p) * (size_t)(P->n + 64));

				if (!x) {
					snprintf(err, en, "out of memory");
					goto bad;
				}
				P->p = x;
			}
			p = &P->p[P->n++];
			snprintf(p->code, sizeof(p->code), "%s", a);
			snprintf(p->action, sizeof(p->action), "%s", b);
			snprintf(p->key, sizeof(p->key), "%s", c);
			p->homes = h;
			p->c.tried = t;
			p->c.helped = he;
			p->c.worse = w;
			p->c.undone = u;
			if (!fl_counts_ok(&p->c)) {
				snprintf(err, en, "line %d: helped, worse and undone add up to more than tried", ln);
				goto bad;
			}
		} else if (line[0] == 'n' && line[1] == ' ') {
			struct fl_norm *q;

			if (sscanf(line + 2, "%9s %d %d %u %f %f", a, &m, &band, &h, &mean, &dev) != 6 || !fl_word_ok(a, 10) ||
			    m < 0 || m > 7 || band < 0 || band > 3 || !h || !(mean >= 0 && mean < 1e6) || !(dev >= 0 && dev < 1e6)) {
				snprintf(err, en, "line %d: not a normal", ln);
				goto bad;
			}
			if (P->nn >= MAX_NORMS) {
				snprintf(err, en, "more than %d normals", MAX_NORMS);
				goto bad;
			}
			if (!(P->nn & 31)) {
				void *x = realloc(P->nm, sizeof(*P->nm) * (size_t)(P->nn + 32));

				if (!x) {
					snprintf(err, en, "out of memory");
					goto bad;
				}
				P->nm = x;
			}
			q = &P->nm[P->nn++];
			snprintf(q->line, sizeof(q->line), "%s", a);
			q->metric = m;
			q->band = band;
			q->homes = h;
			q->mean = mean;
			q->dev = dev;
		} else {
			snprintf(err, en, "line %d: unknown", ln);
			goto bad;
		}
	}
	if (!head || P->version < 1 || P->k < 2) {
		snprintf(err, en, "%s", !head ? "not a priors file (# aigw-priors fleet-priors)" :
			 P->version < 1 ? "no version" : "k must be 2 or more (one home's counts are never a prior)");
		goto bad;
	}
	free(buf);
	return P;
bad:
	free(buf);
	fl_priors_free(P);
	return NULL;
}

const struct fl_prior *fl_prior_find(const struct fl_priors *P, const char *code, const char *action,
				     const struct fl_ctx *c, int k, int *level)
{
	char key[FL_KEY];
	int l, i;

	*level = -1;
	if (!P)
		return NULL;
	for (l = 0; l < FL_LEVELS; l++) {
		fl_key(c, l, key, sizeof(key));
		for (i = 0; i < P->n; i++) {
			const struct fl_prior *p = &P->p[i];

			/* k-anonymity, also here: a bucket fewer than k homes add up to is never used */
			if (p->homes >= (uint32_t)k && p->homes >= (uint32_t)P->k && !strcmp(p->code, code) &&
			    !strcmp(p->action, action) && !strcmp(p->key, key)) {
				*level = l;
				return p;
			}
		}
	}
	return NULL;
}

const struct fl_norm *fl_norm_find(const struct fl_priors *P, const char *line, int metric, int band, int k)
{
	const struct fl_norm *any = NULL;
	int i;

	for (i = 0; P && i < P->nn; i++) {
		const struct fl_norm *q = &P->nm[i];

		if (q->metric != metric || q->band != band || q->homes < (uint32_t)k || q->homes < (uint32_t)P->k)
			continue;
		if (!strcmp(q->line, line && *line ? line : "-"))
			return q;
		if (!strcmp(q->line, "*"))
			any = q;                /* every line type together */
	}
	return any;
}

/* ---------- the blend and the choice ---------- */

void fl_blend(const struct fl_counts *fleet, uint32_t homes, int level, const struct fl_counts *local,
	      double strength, struct fl_blend *o)
{
	double s = 0, ft = 1, ah = 1, ab = 1, an = 1, lt = local ? local->tried : 0, tot;

	memset(o, 0, sizeof(*o));
	o->level = -1;
	if (fleet && fleet->tried) {
		s = fleet->tried < strength ? fleet->tried : strength;
		ft = fleet->tried;
		ah += s * fleet->helped / ft;
		ab += s * (fleet->worse + fleet->undone) / ft;
		an += s * (fleet->tried - fleet->helped - fleet->worse - fleet->undone) / ft;
		o->fleet_tried = fleet->tried;
		o->fleet_helped = fleet->helped;
		o->fleet_homes = homes;
		o->level = level;
	}
	if (local && local->tried) {
		ah += local->helped;
		ab += local->worse + local->undone;
		an += local->tried - local->helped - local->worse - local->undone;
		o->local_tried = local->tried;
		o->local_helped = local->helped;
	}
	tot = ah + ab + an;
	o->helped = ah / tot;
	o->bad = ab / tot;
	o->score = o->helped - o->bad;
	o->fleet_share = s + lt > 0 ? s / (s + lt) : 0;
}

int fl_choose(const struct fl_cand *c, int n, double margin, unsigned min_evidence)
{
	int i, best = 0;

	if (n < 2 || c[0].b.fleet_tried + c[0].b.local_tried < min_evidence)
		return 0;
	for (i = 1; i < n; i++)
		if (c[i].b.fleet_tried + c[i].b.local_tried >= min_evidence && c[i].b.score > c[best].b.score)
			best = i;
	return best && c[best].b.score - c[0].b.score >= margin ? best : 0;
}

void fl_blend_words(const struct fl_blend *b, char *buf, size_t n)
{
	char here[48];

	if (b->local_tried)
		snprintf(here, sizeof(here), "here %u of %u", b->local_helped, b->local_tried);
	else
		snprintf(here, sizeof(here), "never tried here");
	if (b->level >= 0)
		snprintf(buf, n, "helps %.0f%%, worse %.0f%% (fleet: %u of %u tries in %u homes; %s)", 100 * b->helped,
			 100 * b->bad, b->fleet_helped, b->fleet_tried, b->fleet_homes, here);
	else
		snprintf(buf, n, "helps %.0f%%, worse %.0f%% (nothing from the fleet; %s)", 100 * b->helped, 100 * b->bad, here);
}
