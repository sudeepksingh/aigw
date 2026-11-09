/* SPDX-License-Identifier: Apache-2.0 */
/*
 * "flat": the built-in fast back-end for gradient-boosted trees.
 *
 * Every tree is laid out depth-first in one shared array of 12-byte nodes, so
 * an inner node's left child is the next node and only the right child needs
 * an offset; walking a tree reads memory forwards, which suits the small
 * caches of router CPUs. Thresholds are stored as floats rounded down from
 * the model's doubles: for a float input x, x <= t holds exactly when
 * x <= round_down(t), so every decision, and therefore every score, is the
 * same as the reference interpreter's. Leaf values stay doubles, added in the
 * same order.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ag/agm.h"
#include "ag/backend.h"

struct fnode {
	float thr;                 /* inner: threshold (rounded down); leaf: unused */
	int32_t feature;           /* -1: leaf, its value in leaf[] at index `right` */
	int32_t right;             /* inner: index of the right child */
};

struct flat {
	uint32_t n_features, n_trees;
	double base;
	struct fnode *nodes;
	uint32_t *root;            /* each tree's first node */
	double *leaf;
	uint32_t n_nodes, n_leaves, cap;
};

/* the largest float that is <= d */
static float round_down(double d)
{
	float f = (float)d;

	if ((double)f > d)
		f = nextafterf(f, -INFINITY);
	return f;
}

/* depth-first copy of tree t's subtree at node i; returns 0, or -1 when too deep */
static int lay(struct flat *fl, const struct ag_tree_view *t, int32_t i, int depth)
{
	const struct ag_node *nd = &t->nodes[i];
	uint32_t at;

	/* a node reached twice (two parents) would make the copy grow: stop at the cap */
	if (depth > 256 || fl->n_nodes >= fl->cap)
		return -1;
	at = fl->n_nodes++;
	if (nd->feature < 0) {
		fl->nodes[at].feature = -1;
		fl->nodes[at].thr = 0;
		fl->nodes[at].right = (int32_t)fl->n_leaves;
		fl->leaf[fl->n_leaves++] = nd->value;
		return 0;
	}
	fl->nodes[at].feature = nd->feature;
	fl->nodes[at].thr = round_down(nd->value);
	if (lay(fl, t, nd->left, depth + 1))
		return -1;
	fl->nodes[at].right = (int32_t)fl->n_nodes;
	return lay(fl, t, nd->right, depth + 1);
}

static void flat_release(void *h)
{
	struct flat *fl = h;

	if (!fl)
		return;
	free(fl->nodes);
	free(fl->root);
	free(fl->leaf);
	free(fl);
}

static void *flat_prepare(const struct ag_model_view *v, char *err, size_t n)
{
	struct flat *fl;
	uint64_t total = 0;
	uint32_t t;

	if (v->kind != AGM_KIND_GBDT) {
		if (err && n)
			strncpy(err, "only gradient-boosted trees", n - 1);
		return NULL;
	}
	for (t = 0; t < v->n_trees; t++)
		total += v->trees[t].n_nodes;
	fl = calloc(1, sizeof(*fl));
	if (!fl || total > (1u << 24))
		goto fail;
	fl->n_features = v->n_features;
	fl->n_trees = v->n_trees;
	fl->base = v->base;
	fl->nodes = calloc((size_t)total, sizeof(*fl->nodes));
	fl->leaf = calloc((size_t)total, sizeof(*fl->leaf));
	fl->root = calloc(v->n_trees, sizeof(*fl->root));
	if (!fl->nodes || !fl->leaf || !fl->root)
		goto fail;
	fl->cap = (uint32_t)total;
	for (t = 0; t < v->n_trees; t++) {
		uint32_t before = fl->n_nodes;

		fl->root[t] = fl->n_nodes;
		if (lay(fl, &v->trees[t], 0, 0) || fl->n_nodes - before > v->trees[t].n_nodes)
			goto fail;
	}
	return fl;
fail:
	if (err && n)
		strncpy(err, "can't lay out this model's trees", n - 1);
	flat_release(fl);
	return NULL;
}

static int flat_predict(void *h, const float *x, int count, double *raw)
{
	const struct flat *fl = h;
	int k;

	for (k = 0; k < count; k++, x += fl->n_features) {
		double sum = fl->base;
		uint32_t t;

		for (t = 0; t < fl->n_trees; t++) {
			const struct fnode *nd = fl->nodes + fl->root[t];

			while (nd->feature >= 0)
				nd = x[nd->feature] <= nd->thr ? nd + 1 : fl->nodes + nd->right;
			sum += fl->leaf[nd->right];
		}
		raw[k] = sum;
	}
	return 0;
}

const struct ag_backend ag_backend_flat = {
	.abi = AG_BACKEND_ABI, .name = "flat",
	.prepare = flat_prepare, .predict = flat_predict, .release = flat_release,
};
