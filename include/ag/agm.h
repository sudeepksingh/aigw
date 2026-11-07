/* SPDX-License-Identifier: Apache-2.0 */
/*
 * .agm: AIGW's on-device model format and runtime.
 *
 * A small, portable interpreter instead of a general ML framework: no C++,
 * a few KB of code, and models can be replaced without recompiling.
 * All fields are little-endian, so one file works on every router CPU.
 *
 *   0   char  magic[4]      "AGM1"
 *   4   u16   version       1
 *   6   u16   kind          2 = gradient-boosted trees, binary (sigmoid output)
 *   8   u32   n_features
 *   12  u32   schema_hash   FNV-1a of the feature schema name (e.g. "domain-v1")
 *   16  char  name[32]
 *   48  u32   n_sections
 *   52  sections: { u32 type, u32 len, u8 data[len] }
 *
 *   section 1  GBDT    f64 base, u32 n_trees, per tree: u32 n_nodes,
 *                      n_nodes x { i32 feature (-1 = leaf), f64 threshold
 *                      (leaf: value), i32 left, i32 right }   go left if x <= threshold
 *   section 2  BIGRAM  u32 dim, f32 logp[dim*dim]   (feature-extractor table)
 *   section 3  META    UTF-8 JSON: training data, metrics, date
 *
 * A model whose schema hash doesn't match the feature extractor is refused,
 * so a model update can never silently feed the wrong features.
 */
#ifndef AG_AGM_H
#define AG_AGM_H

#include <stddef.h>
#include <stdint.h>

#define AGM_KIND_GBDT 2

struct ag_model;

uint32_t ag_fnv1a(const char *s);

struct ag_model *ag_model_load(const char *path, char *err, size_t errlen);
struct ag_model *ag_model_parse(const uint8_t *buf, size_t len, char *err, size_t errlen);
void ag_model_free(struct ag_model *m);

const char *ag_model_name(const struct ag_model *m);
uint32_t ag_model_schema(const struct ag_model *m);
int ag_model_nfeatures(const struct ag_model *m);
int ag_model_ntrees(const struct ag_model *m);
const char *ag_model_meta(const struct ag_model *m);
const float *ag_model_bigram(const struct ag_model *m, int *dim);

/* Raw score (log-odds) and probability for one feature vector. */
double ag_model_raw(const struct ag_model *m, const float *x);
double ag_model_prob(const struct ag_model *m, const float *x);

#endif
