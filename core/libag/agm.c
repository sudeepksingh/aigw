/* SPDX-License-Identifier: Apache-2.0 */
/* .agm model loader and evaluator. Model files are treated as untrusted:
 * every size, index and child pointer is checked before use. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/agm.h"

#define AGM_MAX_FILE   (4 * 1024 * 1024)
#define AGM_MAX_TREES  4096
#define AGM_MAX_NODES  65536
#define AGM_NODE_BYTES 20   /* i32 feature, f64 value, i32 left, i32 right */

struct node {
	int32_t feature;
	double value;     /* threshold, or leaf value */
	int32_t left, right;
};

struct tree {
	uint32_t n;
	struct node *nodes;
};

struct ag_model {
	char name[33];
	uint32_t kind, n_features, schema;
	double base;
	uint32_t n_trees;
	struct tree *trees;
	uint32_t bigram_dim;
	float *bigram;
	char *meta;
};

uint32_t ag_fnv1a(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s) {
		h ^= (uint8_t)*s++;
		h *= 16777619u;
	}
	return h;
}

/* ---------- little-endian readers with bounds ---------- */

struct rd {
	const uint8_t *p;
	size_t len, off;
	int bad;
};

static int need(struct rd *r, size_t n)
{
	if (r->bad || r->len - r->off < n) {
		r->bad = 1;
		return 0;
	}
	return 1;
}

static uint32_t u32(struct rd *r)
{
	const uint8_t *p;

	if (!need(r, 4))
		return 0;
	p = r->p + r->off;
	r->off += 4;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t u16(struct rd *r)
{
	const uint8_t *p;

	if (!need(r, 2))
		return 0;
	p = r->p + r->off;
	r->off += 2;
	return (uint16_t)(p[0] | (p[1] << 8));
}

static double f64(struct rd *r)
{
	uint64_t v = 0;
	double d;
	int i;

	if (!need(r, 8))
		return 0;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | r->p[r->off + (size_t)i];
	r->off += 8;
	memcpy(&d, &v, sizeof(d));
	return d;
}

static float f32(struct rd *r)
{
	uint32_t v = u32(r);
	float f;

	memcpy(&f, &v, sizeof(f));
	return f;
}

static void seterr(char *err, size_t n, const char *msg)
{
	if (err && n)
		snprintf(err, n, "%s", msg);
}

void ag_model_free(struct ag_model *m)
{
	uint32_t i;

	if (!m)
		return;
	for (i = 0; m->trees && i < m->n_trees; i++)
		free(m->trees[i].nodes);
	free(m->trees);
	free(m->bigram);
	free(m->meta);
	free(m);
}

static int parse_gbdt(struct ag_model *m, struct rd *r, char *err, size_t errlen)
{
	uint32_t t, i;

	uint32_t n_trees;

	m->base = f64(r);
	n_trees = u32(r);
	if (r->bad || n_trees == 0 || n_trees > AGM_MAX_TREES || !isfinite(m->base)) {
		seterr(err, errlen, "bad tree count");
		return -1;
	}
	m->trees = calloc(n_trees, sizeof(*m->trees));
	if (!m->trees)
		return -1;
	m->n_trees = n_trees;   /* only once the array exists: ag_model_free relies on it */
	for (t = 0; t < m->n_trees; t++) {
		struct tree *tr = &m->trees[t];

		tr->n = u32(r);
		if (r->bad || tr->n == 0 || tr->n > AGM_MAX_NODES || !need(r, (size_t)tr->n * AGM_NODE_BYTES)) {
			seterr(err, errlen, "bad tree size");
			return -1;
		}
		tr->nodes = calloc(tr->n, sizeof(*tr->nodes));
		if (!tr->nodes)
			return -1;
		for (i = 0; i < tr->n; i++) {
			struct node *nd = &tr->nodes[i];

			nd->feature = (int32_t)u32(r);
			nd->value = f64(r);
			nd->left = (int32_t)u32(r);
			nd->right = (int32_t)u32(r);
			if (nd->feature >= 0) {
				/* children must point forward: guarantees evaluation terminates */
				if ((uint32_t)nd->feature >= m->n_features ||
				    nd->left <= (int32_t)i || nd->right <= (int32_t)i ||
				    (uint32_t)nd->left >= tr->n || (uint32_t)nd->right >= tr->n) {
					seterr(err, errlen, "bad tree node");
					return -1;
				}
			} else if (nd->feature != -1 || !isfinite(nd->value)) {
				seterr(err, errlen, "bad leaf");
				return -1;
			}
		}
	}
	return r->bad ? -1 : 0;
}

struct ag_model *ag_model_parse(const uint8_t *buf, size_t len, char *err, size_t errlen)
{
	struct rd r = { buf, len, 0, 0 };
	struct ag_model *m;
	uint32_t n_sec, s;

	if (len < 52 || memcmp(buf, "AGM1", 4)) {
		seterr(err, errlen, "not an .agm file");
		return NULL;
	}
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	r.off = 4;
	if (u16(&r) != 1) {
		seterr(err, errlen, "unsupported .agm version");
		goto fail;
	}
	m->kind = u16(&r);
	m->n_features = u32(&r);
	m->schema = u32(&r);
	memcpy(m->name, buf + 16, 32);
	m->name[32] = '\0';
	r.off = 48;
	n_sec = u32(&r);
	if (m->kind != AGM_KIND_GBDT || m->n_features == 0 || m->n_features > 1024 || n_sec > 16) {
		seterr(err, errlen, "unsupported model kind or size");
		goto fail;
	}
	for (s = 0; s < n_sec; s++) {
		uint32_t type = u32(&r), slen = u32(&r);
		struct rd sr;

		if (!need(&r, slen)) {
			seterr(err, errlen, "truncated section");
			goto fail;
		}
		sr = (struct rd){ buf + r.off, slen, 0, 0 };
		r.off += slen;
		if (type == 1) {
			if (m->trees || parse_gbdt(m, &sr, err, errlen) < 0)
				goto fail;
		} else if (type == 2) {
			uint32_t dim = u32(&sr), i;

			if (m->bigram || dim == 0 || dim > 256 || !need(&sr, (size_t)dim * dim * 4)) {
				seterr(err, errlen, "bad bigram table");
				goto fail;
			}
			m->bigram = malloc((size_t)dim * dim * sizeof(float));
			if (!m->bigram)
				goto fail;
			for (i = 0; i < dim * dim; i++)
				m->bigram[i] = f32(&sr);
			m->bigram_dim = dim;
		} else if (type == 3) {
			free(m->meta);
			m->meta = malloc(slen + 1);
			if (!m->meta)
				goto fail;
			memcpy(m->meta, sr.p, slen);
			m->meta[slen] = '\0';
		} /* unknown sections are skipped: newer files stay loadable */
	}
	if (!m->trees) {
		seterr(err, errlen, "model has no trees");
		goto fail;
	}
	return m;
fail:
	if (err && errlen && !*err)
		seterr(err, errlen, "malformed model");
	ag_model_free(m);
	return NULL;
}

struct ag_model *ag_model_load(const char *path, char *err, size_t errlen)
{
	struct ag_model *m = NULL;
	uint8_t *buf;
	long sz;
	FILE *f;

	if (err && errlen)
		*err = '\0';
	f = fopen(path, "rb");
	if (!f) {
		seterr(err, errlen, "cannot open model file");
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) <= 0 || sz > AGM_MAX_FILE || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		seterr(err, errlen, "bad model file size");
		return NULL;
	}
	buf = malloc((size_t)sz);
	if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
		m = ag_model_parse(buf, (size_t)sz, err, errlen);
	free(buf);
	fclose(f);
	return m;
}

const char *ag_model_name(const struct ag_model *m) { return m->name; }
uint32_t ag_model_schema(const struct ag_model *m) { return m->schema; }
int ag_model_nfeatures(const struct ag_model *m) { return (int)m->n_features; }
int ag_model_ntrees(const struct ag_model *m) { return (int)m->n_trees; }
const char *ag_model_meta(const struct ag_model *m) { return m->meta ? m->meta : "{}"; }

const float *ag_model_bigram(const struct ag_model *m, int *dim)
{
	*dim = (int)m->bigram_dim;
	return m->bigram;
}

double ag_model_raw(const struct ag_model *m, const float *x)
{
	double sum = m->base;
	uint32_t t;

	for (t = 0; t < m->n_trees; t++) {
		const struct node *nodes = m->trees[t].nodes;
		int32_t i = 0;

		while (nodes[i].feature >= 0)
			i = ((double)x[nodes[i].feature] <= nodes[i].value) ? nodes[i].left : nodes[i].right;
		sum += nodes[i].value;
	}
	return sum;
}

double ag_model_prob(const struct ag_model *m, const float *x)
{
	return 1.0 / (1.0 + exp(-ag_model_raw(m, x)));
}
