/* SPDX-License-Identifier: Apache-2.0 */
/* .agm runtime tests: the C feature extractor and model must reproduce the
 * Python trainer's numbers (golden file), and damaged model files must be
 * refused without crashing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/agm.h"
#include "ag/domfeat.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } \
} while (0)

static void test_golden(const struct ag_model *m, const char *golden)
{
	FILE *f = fopen(golden, "r");
	char line[4096];
	int dim, rows = 0;
	const float *bigram = ag_model_bigram(m, &dim);

	CHECK(f != NULL, "cannot open %s", golden);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *tok[2 + AG_DOMFEAT_N + 1] = { 0 }, *save = NULL, *p;
		char label[256];
		float x[AG_DOMFEAT_N];
		int n = 0, i, rc;

		if (line[0] == '#')
			continue;
		line[strcspn(line, "\r\n")] = '\0';
		for (p = strtok_r(line, "\t", &save); p && n < 2 + AG_DOMFEAT_N + 1; p = strtok_r(NULL, "\t", &save))
			tok[n++] = p;
		if (n < 2)
			continue;
		rows++;
		rc = ag_domain_label(tok[0], label, sizeof(label));
		if (n == 2 && !strcmp(tok[1], "-")) {
			CHECK(rc == -1, "%s: expected no label, got '%s'", tok[0], label);
			continue;
		}
		CHECK(n == 2 + AG_DOMFEAT_N + 1, "bad golden line for %s", tok[0]);
		if (n != 2 + AG_DOMFEAT_N + 1)
			continue;
		CHECK(rc == 0 && !strcmp(label, tok[1]), "%s: label '%s', want '%s'", tok[0], label, tok[1]);
		ag_domain_features(tok[0], label, bigram, dim, x);
		for (i = 0; i < AG_DOMFEAT_N; i++) {
			double want = atof(tok[2 + i]);

			CHECK(fabs(x[i] - want) <= 1e-5 * (1 + fabs(want)), "%s: feature %d = %.9g, want %.9g",
			      tok[0], i, x[i], want);
		}
		{
			double p = ag_model_prob(m, x), want = atof(tok[2 + AG_DOMFEAT_N]);

			CHECK(fabs(p - want) < 1e-6, "%s: score %.9f, want %.9f", tok[0], p, want);
		}
	}
	fclose(f);
	CHECK(rows >= 20, "golden file has only %d rows", rows);
}

static void test_damaged(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf, *copy;
	long sz;
	size_t i;
	char err[128];
	struct ag_model *m;

	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)sz);
	copy = malloc((size_t)sz);
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
		sz = 0;
	fclose(f);

	/* every truncation must be refused or load cleanly, never crash */
	for (i = 0; i < (size_t)sz; i += 97) {
		m = ag_model_parse(buf, i, err, sizeof(err));
		CHECK(m == NULL, "truncated model (%zu bytes) was accepted", i);
		ag_model_free(m);
	}
	/* flipped bytes in the header and tree structure */
	for (i = 0; i < (size_t)sz && i < 4096; i += 7) {
		memcpy(copy, buf, (size_t)sz);
		copy[i] ^= 0xa5;
		m = ag_model_parse(copy, (size_t)sz, err, sizeof(err));
		if (m) {
			float x[AG_DOMFEAT_N] = { 10, 3, 0, 0.3f, 3, 0, 2, -3, -6, 0, 0.8f, 0 };

			(void)ag_model_prob(m, x);   /* still evaluates safely */
			ag_model_free(m);
		}
	}
	free(buf);
	free(copy);
}

int main(int argc, char **argv)
{
	char err[128] = "";
	struct ag_model *m;

	if (argc < 3) {
		fprintf(stderr, "usage: test_agm model.agm golden.tsv\n");
		return 2;
	}
	m = ag_model_load(argv[1], err, sizeof(err));
	CHECK(m != NULL, "load %s: %s", argv[1], err);
	if (!m)
		return 1;
	CHECK(ag_model_schema(m) == ag_fnv1a(AG_DOMFEAT_SCHEMA), "schema mismatch");
	CHECK(ag_model_nfeatures(m) == AG_DOMFEAT_N, "feature count %d", ag_model_nfeatures(m));
	test_golden(m, argv[2]);
	test_damaged(argv[1]);
	CHECK(ag_model_load("/nonexistent.agm", err, sizeof(err)) == NULL, "missing file accepted");
	printf("model: %s, %d trees\n", ag_model_name(m), ag_model_ntrees(m));
	ag_model_free(m);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("agm runtime: all checks passed\n");
	return 0;
}
