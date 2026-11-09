/* SPDX-License-Identifier: Apache-2.0 */
/* .agm runtime tests: the C feature extractor and model must reproduce the
 * Python trainer's numbers (golden file), and damaged model files must be
 * refused without crashing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ag/agm.h"
#include "ag/backend.h"
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

/* the built-in back-ends: interp equals ag_model_raw, flat equals interp bit
 * for bit (thresholds rounded down to floats), also on inputs that sit exactly
 * on a threshold or next to it; and how much faster flat is */
static uint32_t rng = 12345;
static float rnd(float lo, float hi)
{
	rng = rng * 1103515245u + 12345u;
	return lo + (hi - lo) * (float)((rng >> 8) & 0xffffff) / 16777216.0f;
}

static double secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void test_backends(const struct ag_model *m)
{
	static const float lo[AG_DOMFEAT_N] = { 1, 0, 0, 0, 0, 0, 2, -9, -12, 0, 0.2f, 0 };
	static const float hi[AG_DOMFEAT_N] = { 40, 5, 1, 1, 20, 4, 6, -1, -1, 20, 1, 1 };
	enum { N = 20000 };
	struct ag_model_view v;
	float *x = malloc(sizeof(float) * AG_DOMFEAT_N * N);
	double *a = malloc(sizeof(double) * N), *b = malloc(sizeof(double) * N), t0, ti, tf;
	char err[128] = "";
	void *hi_ = NULL, *hf = NULL;
	int i, j, same = 0;

	ag_model_view(m, &v);
	CHECK(v.n_trees == (uint32_t)ag_model_ntrees(m) && v.n_features == AG_DOMFEAT_N, "model view");
	for (i = 0; i < N; i++)
		for (j = 0; j < AG_DOMFEAT_N; j++)
			x[i * AG_DOMFEAT_N + j] = rnd(lo[j], hi[j]);
	/* inputs on the thresholds themselves, and the floats either side of them */
	for (i = 0; i < 3000 && i < N; i++) {
		const struct ag_tree_view *t = &v.trees[i % v.n_trees];
		const struct ag_node *nd = &t->nodes[0];

		if (nd->feature >= 0) {
			float f = (float)nd->value;

			x[i * AG_DOMFEAT_N + nd->feature] = i % 3 == 0 ? f : i % 3 == 1 ? nextafterf(f, -1e30f) :
							    nextafterf(f, 1e30f);
		}
	}
	hi_ = ag_backend_interp.prepare(&v, err, sizeof(err));
	hf = ag_backend_flat.prepare(&v, err, sizeof(err));
	CHECK(hi_ && hf, "prepare: %s", err);
	if (!hi_ || !hf)
		goto out;
	t0 = secs();
	ag_backend_interp.predict(hi_, x, N, a);
	ti = secs() - t0;
	t0 = secs();
	ag_backend_flat.predict(hf, x, N, b);
	tf = secs() - t0;
	for (i = 0; i < N; i++) {
		same += a[i] == b[i];
		if (i < 500)
			CHECK(a[i] == ag_model_raw(m, x + i * AG_DOMFEAT_N), "interp back-end differs from ag_model_raw at %d", i);
	}
	CHECK(same == N, "flat differs from the reference on %d of %d inputs", N - same, N);
	printf("back-ends: flat equals interp on %d inputs; %.2f us vs %.2f us per score (%.1fx)\n", N,
	       tf * 1e6 / N, ti * 1e6 / N, ti / tf);
out:
	if (hi_)
		ag_backend_interp.release(hi_);
	if (hf)
		ag_backend_flat.release(hf);
	free(x);
	free(a);
	free(b);
}

static void test_registrable(void)
{
	static const char *const cases[][2] = {
		{ "www.example.com", "example.com" },
		{ "a.b.c.example.co.uk.", "example.co.uk" },
		{ "Mail.Google.COM", "google.com" },
		{ "example.example.com", "example.com" },
		{ "uk.co.uk", "uk.co.uk" },
		{ "r3---sn-x.googlevideo.com", "googlevideo.com" },
		{ "user.github.io", "user.github.io" },
		{ "printer.local", NULL },
		{ "10.0.0.1", NULL },
		{ "localhost", NULL },
	};
	char out[256];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int rc = ag_domain_registrable(cases[i][0], out, sizeof(out));

		if (!cases[i][1])
			CHECK(rc == -1, "%s: expected none, got %s", cases[i][0], out);
		else
			CHECK(rc == 0 && !strcmp(out, cases[i][1]), "%s: got %s, want %s",
			      cases[i][0], rc ? "(none)" : out, cases[i][1]);
	}
}

int main(int argc, char **argv)
{
	char err[128] = "";
	struct ag_model *m;

	test_registrable();

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
	test_backends(m);
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
