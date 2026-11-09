/* SPDX-License-Identifier: Apache-2.0 */
/*
 * An example model back-end (include/ag/backend.h): what a router maker
 * writes to run AIGW's models on its NPU or DSP. This one computes the trees
 * in 32-bit floats on the CPU, the way an accelerator with float units might,
 * so its scores differ from the reference interpreter in the last digits, but
 * never its verdicts; agd checks exactly that before using it.
 *
 * Built with -DEXAMPLE_BROKEN it adds 3 to every score: agd must refuse it.
 *
 *   cc -shared -fPIC -I include -o example.so plugins/examples/backend/example_backend.c
 *   install -m 0644 -o root example.so /usr/lib/aigw/backends/
 */
#include <stdlib.h>
#include <string.h>

#include "ag/backend.h"

#define KIND_GBDT 2

struct qnode { float thr; float leaf; int32_t feature, left, right; };

struct model {
	uint32_t n_features, n_trees;
	float base;
	uint32_t *start;
	struct qnode *nodes;
};

static void release(void *h)
{
	struct model *m = h;

	if (m) {
		free(m->start);
		free(m->nodes);
		free(m);
	}
}

/* "compile" the model for the accelerator: here, a float copy of every node */
static void *prepare(const struct ag_model_view *v, char *err, size_t n)
{
	struct model *m;
	uint32_t t, i, total = 0, at = 0;

	if (v->kind != KIND_GBDT) {
		strncpy(err, "example back-end: trees only", n - 1);
		return NULL;
	}
	for (t = 0; t < v->n_trees; t++)
		total += v->trees[t].n_nodes;
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->n_features = v->n_features;
	m->n_trees = v->n_trees;
	m->base = (float)v->base;
	m->start = calloc(v->n_trees, sizeof(*m->start));
	m->nodes = calloc(total, sizeof(*m->nodes));
	if (!m->start || !m->nodes) {
		release(m);
		return NULL;
	}
	for (t = 0; t < v->n_trees; t++) {
		m->start[t] = at;
		for (i = 0; i < v->trees[t].n_nodes; i++, at++) {
			const struct ag_node *s = &v->trees[t].nodes[i];
			struct qnode *q = &m->nodes[at];

			q->feature = s->feature;
			q->thr = (float)s->value;
			q->leaf = (float)s->value;
			q->left = s->left;
			q->right = s->right;
		}
	}
	return m;
}

static int predict(void *h, const float *x, int count, double *raw)
{
	const struct model *m = h;
	int k;

	for (k = 0; k < count; k++, x += m->n_features) {
		float sum = m->base;
		uint32_t t;

		for (t = 0; t < m->n_trees; t++) {
			const struct qnode *nodes = m->nodes + m->start[t];
			int32_t i = 0;

			while (nodes[i].feature >= 0)
				i = x[nodes[i].feature] <= nodes[i].thr ? nodes[i].left : nodes[i].right;
			sum += nodes[i].leaf;
		}
#ifdef EXAMPLE_BROKEN
		sum += 3;
#endif
		raw[k] = sum;
	}
	return 0;
}

static const struct ag_backend me = {
	.abi = AG_BACKEND_ABI,
#ifdef EXAMPLE_BROKEN
	.name = "example-broken",
#else
	.name = "example-npu",
#endif
	.prepare = prepare, .predict = predict, .release = release,
};

const struct ag_backend *ag_backend_entry(void)
{
	return &me;
}
