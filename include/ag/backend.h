/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Model back-ends: who computes a model's scores.
 *
 * AIGW's models (.agm, include/ag/agm.h) are evaluated by a back-end. Two are
 * built in:
 *
 *   interp   the reference interpreter (agm.c): every other back-end is
 *            checked against it
 *   flat     the same trees packed into one array, thresholds as floats
 *            rounded so every decision is identical: same scores, faster
 *
 * A router with an NPU or a DSP (or a faster CPU library) adds its own as a
 * shared library in the back-end directory (/usr/lib/aigw/backends, files
 * owned by root and writable by nobody else, so only the package manager
 * puts one there). agd loads each one, and uses it only when it gives the
 * reference's verdicts on a set of probe inputs and is faster; otherwise the
 * built-in one stays. A back-end never sees traffic: only feature vectors.
 *
 * Writing one (C, any toolchain that builds for the router):
 *
 *     #include <ag/backend.h>
 *
 *     static void *prepare(const struct ag_model_view *m, char *err, size_t n) { ... }
 *     static int predict(void *h, const float *x, int count, double *raw) { ... }
 *     static void release(void *h) { ... }
 *
 *     static const struct ag_backend me = {
 *         .abi = AG_BACKEND_ABI, .name = "acme-npu", .prepare = prepare,
 *         .predict = predict, .release = release,
 *     };
 *     const struct ag_backend *ag_backend_entry(void) { return &me; }
 *
 * prepare() gets a read-only view of the model (it may keep pointers into it
 * only until release()) and returns the back-end's own handle, or NULL with a
 * reason when it can't take this model (kind, size). predict() writes the raw
 * score (log-odds, before the sigmoid) of each of `count` feature vectors,
 * stored one after another (count x n_features floats), and returns 0. It is
 * called from one thread only.
 */
#ifndef AG_BACKEND_H
#define AG_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define AG_BACKEND_ABI 1

struct ag_model;

/* one tree node: an inner node goes left when x[feature] <= value; a leaf has
 * feature -1 and its value is added to the score. Children always come after
 * their parent (left > index, right > index). */
struct ag_node {
	int32_t feature;
	double value;
	int32_t left, right;
};

struct ag_tree_view {
	uint32_t n_nodes;
	const struct ag_node *nodes;          /* node 0 is the root */
};

struct ag_model_view {
	uint32_t kind;                        /* AGM_KIND_GBDT */
	uint32_t n_features;
	uint32_t schema;                      /* feature schema hash */
	const char *name;
	double base;                          /* added to the sum of the trees */
	uint32_t n_trees;
	const struct ag_tree_view *trees;
};

struct ag_backend {
	int abi;                              /* AG_BACKEND_ABI */
	const char *name;
	void *(*prepare)(const struct ag_model_view *m, char *err, size_t n);
	int (*predict)(void *handle, const float *x, int count, double *raw);
	void (*release)(void *handle);
};

/* the entry point a back-end library exports */
typedef const struct ag_backend *(*ag_backend_entry_fn)(void);
#define AG_BACKEND_ENTRY "ag_backend_entry"

/* a read-only view of a parsed model (valid while the model lives) */
void ag_model_view(const struct ag_model *m, struct ag_model_view *v);

/* the built-in back-ends */
extern const struct ag_backend ag_backend_interp;
extern const struct ag_backend ag_backend_flat;

#endif
