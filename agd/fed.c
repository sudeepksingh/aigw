/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Federated learning (phase 7): every home's answers improve the domain-risk
 * judgement for all homes, and the names never leave the home.
 *
 *   your answers ── a dga alert you rejected or undid (false alarm), one you
 *        │          approved (right), or agctl label <name> safe|threat
 *        ▼
 *   examples ────── kept here only: the name and your verdict (at most 200)
 *        │
 *   a round ─────── cortex sends the shared layer (14 numbers), the bridge hands
 *        │          it over ({"fed": …}, capability fed.train)
 *        ▼
 *   train here ──── a few steps of logistic regression on your examples plus a
 *        │          fixed set of public anchor names, starting from the shared layer
 *        ▼
 *   privacy ─────── the change is clipped to length C and Gaussian noise of
 *        │          σ·C is added to each number (σ at least alerts.fed_noise)
 *        ▼
 *   leaves ──────── 14 noisy numbers and how many examples: no names, no features
 *
 * The layer ("domain-adapter") sits on top of the trees:
 *
 *   score = sigmoid( w0·logit(tree score) + Σ wi·(feature_i − center_i)/scale_i + b )
 *
 * w0 = 1 and the rest 0 is the trees' own score. Cortex averages the homes'
 * changes and publishes the new layer as a signed file that arrives like a
 * model update (models.c); before it is used it must still score the anchors'
 * machine-generated names at or above the alert threshold and the popular
 * sites below it.
 *
 * Privacy accounting: each round is a Gaussian mechanism over the whole
 * home's data with noise multiplier σ; rounds add up as zero-concentrated
 * DP (ρ = Σ 1/(2σ²)), reported as ε at δ = 1e-5: about 5.3 for one round at
 * σ = 1, 20 after ten. alerts.fed_budget stops taking part before ε would
 * pass it. The formal guarantee weakens with every round; what never
 * weakens is that names, features and verdicts stay in the home.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>

#include "agd.h"
#include "ag/domfeat.h"

#define EX_MAX     200
#define FED_DELTA  1e-5
#define THREAT_WEIGHT 2.0           /* a missed threat costs more than a false alarm */

/* ---------- the adapter (a signed "# aigw-adapter" file) ---------- */

struct adapter {
	int version;
	char published[24];
	double center[AG_DOMFEAT_N], scale[AG_DOMFEAT_N];
	double w[FED_W];                    /* w0 (tree logit), w1..w12 (features), bias */
};

static struct adapter *in_use;

static int parse_nums(const char *s, double *out, int n)
{
	char *end;
	int i;

	for (i = 0; i < n; i++) {
		out[i] = strtod(s, &end);
		if (end == s || !isfinite(out[i]))
			return -1;
		s = end;
	}
	while (*s == ' ' || *s == '\t')
		s++;
	return *s && *s != '\n' && *s != '\r' ? -1 : 0;
}

void *fed_adapter_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en)
{
	struct adapter *a = calloc(1, sizeof(*a));
	char *text = strndup((const char *)data, len), *line, *save = NULL;
	int have = 0, i;

	if (!a || !text) {
		snprintf(err, en, "out of memory");
		goto bad;
	}
	for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "# aigw-adapter ", 15)) {
			snprintf(name, nn, "%.*s", (int)strcspn(line + 15, " \r"), line + 15);
			have |= 1;
		} else if (!strncmp(line, "# version ", 10)) {
			a->version = atoi(line + 10);
		} else if (!strncmp(line, "# published ", 12)) {
			snprintf(a->published, sizeof(a->published), "%.*s", (int)strcspn(line + 12, " \r"), line + 12);
		} else if (!strncmp(line, "center ", 7)) {
			if (parse_nums(line + 7, a->center, AG_DOMFEAT_N))
				break;
			have |= 2;
		} else if (!strncmp(line, "scale ", 6)) {
			if (parse_nums(line + 6, a->scale, AG_DOMFEAT_N))
				break;
			have |= 4;
		} else if (!strncmp(line, "weights ", 8)) {
			if (parse_nums(line + 8, a->w, FED_W))
				break;
			have |= 8;
		}
	}
	if (have != 15) {
		snprintf(err, en, "not a complete adapter file (needs center, scale and %d weights)", FED_W);
		goto bad;
	}
	for (i = 0; i < AG_DOMFEAT_N; i++)
		if (!(a->scale[i] > 1e-6)) {
			snprintf(err, en, "scale %d must be positive", i + 1);
			goto bad;
		}
	free(text);
	return a;
bad:
	free(text);
	free(a);
	return NULL;
}

void fed_adapter_free(void *o) { free(o); }
int fed_adapter_version(const void *o, char *date, size_t n)
{
	const struct adapter *a = o;

	if (date && n)
		snprintf(date, n, "%s", a->published);
	return a->version;
}
int fed_adapter_size(const void *o) { (void)o; return FED_W; }

/* inputs of the layer: logit of the tree score, then the scaled features */
static void inputs(double p, const float *x, const double *center, const double *scale, double *in)
{
	int i;

	if (p < 1e-6)
		p = 1e-6;
	if (p > 1 - 1e-6)
		p = 1 - 1e-6;
	in[0] = log(p / (1 - p));
	for (i = 0; i < AG_DOMFEAT_N; i++)
		in[i + 1] = ((double)x[i] - center[i]) / scale[i];
}

static double layer(const double *w, const double *in)
{
	double z = w[FED_W - 1];
	int i;

	for (i = 0; i < FED_W - 1; i++)
		z += w[i] * in[i];
	if (z > 40)
		z = 40;
	if (z < -40)
		z = -40;
	return 1 / (1 + exp(-z));
}

double fed_adjust(double p, const float *x)
{
	double in[FED_W];

	if (!in_use || p < 0)
		return p;
	inputs(p, x, in_use->center, in_use->scale, in);
	return layer(in_use->w, in);
}

/* public names whose verdict is not in doubt: they keep each home's update
 * (and the published layer) from simply turning every score down or up */
static const char *const anchor_bad[] = {
	"xkqjzvbwptmr.com", "qwmxzptkrbvn.net", "wpqzkfjxvbnm.com", "zxcvbnmasdfg.biz",
	"lkjhgfdsapoiu.org", "mnbvcxzqwerty.ru", "a8f3k2l9q0zx.info", "x7k2m9q4z8w1.com",
	/* the same kind under a subdomain or a two-part suffix: a layer must not learn
	 * "more labels = safe", which a "www." in front would get around */
	"www.xkqjzvbwptmr.com", "cdn.qwmxzptkrbvn.net", "api.a8f3k2l9q0zx.info", "mail.wpqzkfjxvbnm.com",
	"xkqjzvbwptmr.co.uk", NULL
};
static const char *const anchor_good[] = {
	"google.com", "wikipedia.org", "netflix.com", "stackoverflow.com", "microsoftonline.com",
	"cloudflare.com", "instagram.com", "whatsapp.net", "www.google.com", "en.wikipedia.org",
	"bbc.co.uk", "www.netflix.com", NULL
};

/* tree score and features of a name; -1 when it isn't judged */
static double tree(const char *name, float *x)
{
	char label[256];

	return alerts_tree_score(name, label, sizeof(label), x);
}

static int own_threats_kept(const struct adapter *a, char *err, size_t n);

int fed_adapter_check(const void *o, char *err, size_t n)
{
	const struct adapter *a = o;
	double in[FED_W], s;
	float x[AG_DOMFEAT_N];
	int i;

	if (!alerts_model_trees()) {
		snprintf(err, n, "can't be checked: no domain-risk model loaded");
		return -1;
	}
	if (!(a->w[0] >= 0.2 && a->w[0] <= 5)) {
		snprintf(err, n, "weight of the trees' score is %.2f, outside 0.2 .. 5", a->w[0]);
		return -1;
	}
	for (i = 0; anchor_bad[i]; i++) {
		double t = tree(anchor_bad[i], x);

		if (t < 0)
			continue;
		inputs(t, x, a->center, a->scale, in);
		s = layer(a->w, in);
		if (t >= cfg.dga_threshold && s < cfg.dga_threshold) {
			snprintf(err, n, "would no longer flag %s (score %.2f, alerts at %.2f)", anchor_bad[i], s,
				 cfg.dga_threshold);
			return -1;
		}
		if (s < t - 0.05) {             /* the trees miss it already: no worse */
			snprintf(err, n, "would lower the score of %s from %.2f to %.2f", anchor_bad[i], t, s);
			return -1;
		}
	}
	for (i = 0; anchor_good[i]; i++) {
		if ((s = tree(anchor_good[i], x)) < 0)
			continue;
		inputs(s, x, a->center, a->scale, in);
		if ((s = layer(a->w, in)) >= cfg.dga_threshold) {
			snprintf(err, n, "would flag %s (score %.2f)", anchor_good[i], s);
			return -1;
		}
	}
	return own_threats_kept(a, err, n);
}

void fed_adapter_use(void *o)
{
	struct adapter *old = in_use;

	in_use = o;
	if (old && old != o)
		free(old);
	if (in_use)
		AG_INFO("domain-adapter version %d in use (shared layer learned by homes together)", in_use->version);
}

/* ---------- examples: your answers, kept here ---------- */

struct example {
	char name[NAME_LEN];
	int label;                          /* 0 = safe (false alarm), 1 = threat */
	char source[12];                    /* "decision" or "you" */
	int64_t at;
};

static struct example ex[EX_MAX];
static int n_ex;

/* the privacy budget spent so far */
static struct {
	uint32_t rounds;                    /* rounds this home took part in */
	uint32_t last_round;
	double rho;                         /* zCDP, sum over rounds of 1/(2σ²) */
	int noiseless;                      /* a round without noise: no guarantee */
	int64_t last_at;
	char last_note[160];
} spent;

static int name_ok(const char *s)
{
	size_t i, n = strlen(s);

	if (n < 3 || n >= NAME_LEN || !strchr(s, '.'))
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '-' && s[i] != '_')
			return 0;
	return 1;
}

int fed_label(const char *name, int label, const char *source, char *err, size_t n)
{
	char low[NAME_LEN];
	int i;

	if (!name || !name_ok(name)) {
		snprintf(err, n, "'%s' is not a DNS name", name ? name : "");
		return -1;
	}
	for (i = 0; name[i] && i < NAME_LEN - 1; i++)
		low[i] = (char)tolower((unsigned char)name[i]);
	low[i] = '\0';
	for (i = 0; i < n_ex; i++)
		if (!strcmp(ex[i].name, low))
			break;
	if (i == n_ex) {
		if (n_ex == EX_MAX) {           /* the oldest goes */
			memmove(ex, ex + 1, sizeof(ex[0]) * (EX_MAX - 1));
			n_ex--;
		}
		i = n_ex++;
	}
	snprintf(ex[i].name, sizeof(ex[i].name), "%s", low);
	ex[i].label = label ? 1 : 0;
	snprintf(ex[i].source, sizeof(ex[i].source), "%s", source);
	ex[i].at = ag_wall_s();
	AG_INFO("learning example: %s is %s (%s)", low, label ? "a threat" : "safe", source);
	return 0;
}

int fed_forget(const char *name)
{
	int i, n = 0;

	for (i = 0; i < n_ex; i++) {
		if (name && *name && strcasecmp(ex[i].name, name))
			continue;
		memmove(ex + i, ex + i + 1, sizeof(ex[0]) * (size_t)(n_ex - i - 1));
		n_ex--;
		i--;
		n++;
	}
	return n;
}

/* names you said are threats, which the trees flag, must stay flagged here */
static int own_threats_kept(const struct adapter *a, char *err, size_t n)
{
	double in[FED_W], t, s;
	float x[AG_DOMFEAT_N];
	int i;

	for (i = 0; i < n_ex; i++) {
		if (!ex[i].label || (t = tree(ex[i].name, x)) < cfg.dga_threshold)
			continue;
		inputs(t, x, a->center, a->scale, in);
		if ((s = layer(a->w, in)) < cfg.dga_threshold) {
			/* the reason is reported (the bridge sends model status to cortex): no name in it */
			AG_INFO("domain-adapter version %d would no longer flag %s (score %.2f), which you said is a threat",
				a->version, ex[i].name, s);
			snprintf(err, n, "would no longer flag a name you said is a threat (score %.2f; agctl fed lists them)", s);
			return -1;
		}
	}
	return 0;
}

/* a person's answer to a decision taken on a dga alert */
void fed_on_answer(uint32_t alert_id, int label)
{
	char kind[16], subject[NAME_LEN], err[128];

	if (!cfg.fed_enabled || !alert_id || alerts_subject(alert_id, kind, sizeof(kind), subject, sizeof(subject)))
		return;
	if (strcmp(kind, "dga"))
		return;
	fed_label(subject, label, "decision", err, sizeof(err));
}

/* ---------- a training round ---------- */

static double gauss(void)
{
	uint64_t r[2];
	double u1, u2;

	if (getrandom(r, sizeof(r), 0) != sizeof(r))
		return 0;
	u1 = ((double)(r[0] >> 11) + 1) / 9007199254740993.0;       /* (0, 1] */
	u2 = (double)(r[1] >> 11) / 9007199254740992.0;
	return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

double fed_epsilon(double rho)
{
	return rho + 2 * sqrt(rho * log(1 / FED_DELTA));
}

int fed_train(const struct fed_round *r, double *delta, int *n_used, double *noise, char *err, size_t n)
{
	static double in[EX_MAX + 16][FED_W];
	static double y[EX_MAX + 16], wt[EX_MAX + 16];
	double w[FED_W], g[FED_W], norm = 0, sigma;
	float x[AG_DOMFEAT_N];
	int m = 0, mine = 0, i, j, e;

	if (!cfg.fed_enabled) {
		snprintf(err, n, "federated learning is off here (alerts.fed)");
		return -1;
	}
	if (!alerts_model_trees()) {
		snprintf(err, n, "no domain-risk model loaded");
		return -1;
	}
	if (r->round && r->round == spent.last_round) {
		snprintf(err, n, "already took part in round %u", r->round);
		return -1;
	}
	for (i = 0; i < AG_DOMFEAT_N; i++)
		if (!(r->scale[i] > 1e-6) || !isfinite(r->center[i])) {
			snprintf(err, n, "round has an unusable scale");
			return -1;
		}
	for (i = 0; i < FED_W; i++)
		if (!isfinite(r->w[i]) || fabs(r->w[i]) > 50) {
			snprintf(err, n, "round has unusable weights");
			return -1;
		}
	for (i = 0; i < n_ex; i++) {
		double s = tree(ex[i].name, x);

		if (s < 0)
			continue;
		inputs(s, x, r->center, r->scale, in[m]);
		y[m] = ex[i].label;
		wt[m] = ex[i].label ? THREAT_WEIGHT : 1;
		m++;
		mine++;
	}
	if (mine < cfg.fed_min_examples) {
		snprintf(err, n, "%d answered example%s here, a round needs %d", mine, mine == 1 ? "" : "s",
			 cfg.fed_min_examples);
		return -1;
	}
	/* anchors: together as heavy as this home's own examples */
	{
		int na = 0;

		for (i = 0; anchor_bad[i]; i++)
			na++;
		for (i = 0; anchor_good[i]; i++)
			na++;
		for (j = 0; j < 2; j++)
			for (i = 0; (j ? anchor_good : anchor_bad)[i]; i++) {
				double s = tree((j ? anchor_good : anchor_bad)[i], x);

				if (s < 0)
					continue;
				inputs(s, x, r->center, r->scale, in[m]);
				y[m] = j ? 0 : 1;
				wt[m] = (double)mine / na * (j ? 1 : THREAT_WEIGHT);
				m++;
			}
	}
	memcpy(w, r->w, sizeof(w));
	for (e = 0; e < r->epochs; e++) {
		double tw = 0;

		memset(g, 0, sizeof(g));
		for (i = 0; i < m; i++) {
			double d = (layer(w, in[i]) - y[i]) * wt[i];

			for (j = 0; j < FED_W - 1; j++)
				g[j] += d * in[i][j];
			g[FED_W - 1] += d;
			tw += wt[i];
		}
		for (j = 0; j < FED_W; j++)
			w[j] -= r->lr * g[j] / tw;
	}
	for (j = 0; j < FED_W; j++) {
		delta[j] = w[j] - r->w[j];
		norm += delta[j] * delta[j];
	}
	norm = sqrt(norm);
	if (norm > r->clip)
		for (j = 0; j < FED_W; j++)
			delta[j] *= r->clip / norm;
	sigma = r->noise > cfg.fed_noise ? r->noise : cfg.fed_noise;
	if (cfg.fed_budget > 0 && (sigma <= 0 || fed_epsilon(spent.rho + 1 / (2 * sigma * sigma)) > cfg.fed_budget)) {
		snprintf(err, n, "privacy budget reached: another round at noise %.2f would take ε past %.1f "
			 "(alerts.fed_budget)", sigma, cfg.fed_budget);
		return -1;
	}
	for (j = 0; j < FED_W; j++)
		delta[j] += sigma * r->clip * gauss();
	*n_used = mine;
	*noise = sigma;

	spent.rounds++;
	spent.last_round = r->round;
	spent.last_at = ag_wall_s();
	if (sigma > 0)
		spent.rho += 1 / (2 * sigma * sigma);
	else
		spent.noiseless = 1;
	snprintf(spent.last_note, sizeof(spent.last_note), "round %u: %d example%s, change %.3f clipped to %.2f, noise %.2f",
		 r->round, mine, mine == 1 ? "" : "s", norm, r->clip, sigma);
	AG_INFO("federated learning %s", spent.last_note);
	return 0;
}

/* ---------- status, state ---------- */

void fed_blob(struct blob_buf *b, int names)
{
	void *arr, *t;
	int i, safe = 0;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.fed_enabled);
	blobmsg_add_double(b, "min_noise", cfg.fed_noise);
	blobmsg_add_double(b, "budget", cfg.fed_budget);
	blobmsg_add_u32(b, "min_examples", (uint32_t)cfg.fed_min_examples);
	for (i = 0; i < n_ex; i++)
		safe += !ex[i].label;
	blobmsg_add_u32(b, "examples", (uint32_t)n_ex);
	blobmsg_add_u32(b, "safe", (uint32_t)safe);
	blobmsg_add_u32(b, "threat", (uint32_t)(n_ex - safe));
	blobmsg_add_u32(b, "rounds", spent.rounds);
	blobmsg_add_u32(b, "last_round", spent.last_round);
	blobmsg_add_u64(b, "last_at", (uint64_t)spent.last_at);
	blobmsg_add_string(b, "last", spent.last_note);
	blobmsg_add_double(b, "rho", spent.rho);
	if (spent.noiseless)
		blobmsg_add_string(b, "epsilon", "none (a round without noise)");
	else
		blobmsg_add_double(b, "epsilon", spent.rounds ? fed_epsilon(spent.rho) : 0);
	blobmsg_add_double(b, "delta", FED_DELTA);
	t = blobmsg_open_table(b, "adapter");
	blobmsg_add_u8(b, "loaded", in_use != NULL);
	if (in_use) {
		void *a;

		blobmsg_add_u32(b, "version", (uint32_t)in_use->version);
		a = blobmsg_open_array(b, "weights");
		for (i = 0; i < FED_W; i++)
			blobmsg_add_double(b, NULL, in_use->w[i]);
		blobmsg_close_array(b, a);
	}
	blobmsg_close_table(b, t);
	if (!names)
		return;
	arr = blobmsg_open_array(b, "list");
	for (i = n_ex - 1; i >= 0; i--) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", ex[i].name);
		blobmsg_add_string(b, "label", ex[i].label ? "threat" : "safe");
		blobmsg_add_string(b, "source", ex[i].source);
		blobmsg_add_u64(b, "at", (uint64_t)ex[i].at);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

void fed_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "fed"), *arr, *e;
	int i;

	blobmsg_add_u32(b, "rounds", spent.rounds);
	blobmsg_add_u32(b, "last_round", spent.last_round);
	blobmsg_add_double(b, "rho", spent.rho);
	blobmsg_add_u8(b, "noiseless", (uint8_t)spent.noiseless);
	blobmsg_add_u64(b, "last_at", (uint64_t)spent.last_at);
	blobmsg_add_string(b, "last", spent.last_note);
	arr = blobmsg_open_array(b, "examples");
	for (i = 0; i < n_ex; i++) {
		e = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", ex[i].name);
		blobmsg_add_u32(b, "label", (uint32_t)ex[i].label);
		blobmsg_add_string(b, "source", ex[i].source);
		blobmsg_add_u64(b, "at", (uint64_t)ex[i].at);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

static double anum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	case BLOBMSG_TYPE_INT64: return (double)(int64_t)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (double)(int16_t)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (double)blobmsg_get_u8(a);
	case BLOBMSG_TYPE_STRING: return atof(blobmsg_get_string(a));
	default: return NAN;
	}
}

void fed_load(struct blob_attr *tbl)
{
	struct blob_attr *cur, *e, *f;
	size_t rem, r2, r3;

	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "rounds")) spent.rounds = (uint32_t)anum(cur);
		else if (!strcmp(k, "last_round")) spent.last_round = (uint32_t)anum(cur);
		else if (!strcmp(k, "rho")) spent.rho = anum(cur);
		else if (!strcmp(k, "noiseless")) spent.noiseless = anum(cur) != 0;
		else if (!strcmp(k, "last_at")) spent.last_at = (int64_t)anum(cur);
		else if (!strcmp(k, "last") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			snprintf(spent.last_note, sizeof(spent.last_note), "%s", blobmsg_get_string(cur));
		else if (!strcmp(k, "examples") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(e, cur, r2) {
				struct example x = { 0 };

				blobmsg_for_each_attr(f, e, r3) {
					const char *fk = blobmsg_name(f);

					if (!strcmp(fk, "name") && blobmsg_type(f) == BLOBMSG_TYPE_STRING)
						snprintf(x.name, sizeof(x.name), "%s", blobmsg_get_string(f));
					else if (!strcmp(fk, "label")) x.label = anum(f) != 0;
					else if (!strcmp(fk, "source") && blobmsg_type(f) == BLOBMSG_TYPE_STRING)
						snprintf(x.source, sizeof(x.source), "%s", blobmsg_get_string(f));
					else if (!strcmp(fk, "at")) x.at = (int64_t)anum(f);
				}
				if (name_ok(x.name) && n_ex < EX_MAX)
					ex[n_ex++] = x;
			}
	}
	if (spent.rho != spent.rho || spent.rho < 0)
		spent.rho = 0;
	AG_INFO("restored %d learning example%s, %u federated round%s", n_ex, n_ex == 1 ? "" : "s", spent.rounds,
		spent.rounds == 1 ? "" : "s");
}

/* a round from cortex, as the bridge hands it over */
int fed_round_parse(struct blob_attr *tbl, struct fed_round *r, char *err, size_t n)
{
	struct blob_attr *cur, *e;
	size_t rem, r2;
	int have = 0, k;

	memset(r, 0, sizeof(*r));
	r->lr = 0.5;
	r->epochs = 50;
	r->clip = 1;
	r->noise = 1;
	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *key = blobmsg_name(cur);
		double *arr = NULL;
		int want = 0;

		if (!strcmp(key, "round")) r->round = (uint32_t)anum(cur);
		else if (!strcmp(key, "lr")) r->lr = anum(cur);
		else if (!strcmp(key, "epochs")) r->epochs = (int)anum(cur);
		else if (!strcmp(key, "clip")) r->clip = anum(cur);
		else if (!strcmp(key, "noise")) r->noise = anum(cur);
		else if (!strcmp(key, "center")) { arr = r->center; want = AG_DOMFEAT_N; }
		else if (!strcmp(key, "scale")) { arr = r->scale; want = AG_DOMFEAT_N; }
		else if (!strcmp(key, "weights")) { arr = r->w; want = FED_W; }
		if (!arr)
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_ARRAY) {
			snprintf(err, n, "%s should be a list", key);
			return -1;
		}
		k = 0;
		blobmsg_for_each_attr(e, cur, r2) {
			if (k < want)
				arr[k] = anum(e);
			k++;
		}
		if (k != want) {
			snprintf(err, n, "%s needs %d numbers, has %d", key, want, k);
			return -1;
		}
		have |= arr == r->center ? 1 : arr == r->scale ? 2 : 4;
	}
	if (have != 7 || !r->round) {
		snprintf(err, n, "a round needs round, center, scale and weights");
		return -1;
	}
	if (!(r->lr > 0 && r->lr <= 10) || r->epochs < 1 || r->epochs > 1000 || !(r->clip > 0 && r->clip <= 10) ||
	    !(r->noise >= 0 && r->noise <= 100)) {
		snprintf(err, n, "round settings out of range (lr, epochs, clip, noise)");
		return -1;
	}
	return 0;
}
