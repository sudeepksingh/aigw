/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Model back-ends (phase 7): who computes the domain-risk model's scores.
 *
 *   built in ── interp (the reference) and flat (same scores, faster)
 *   loaded ──── each library in models.backend_dir (an NPU, a DSP, a faster
 *               CPU library), if it and the directory belong to root and
 *               nobody else may write them (include/ag/backend.h)
 *        │
 *   checked ─── whenever a model is loaded: every back-end scores 300 probe
 *        │      inputs (the anchor names, and generated names of every kind);
 *        │      it must give the reference's verdict on each (alert or not,
 *        │      at the alert threshold) within 0.005 of its score
 *        ▼
 *   chosen ──── models.backend: "auto" takes the fastest that passed; a name
 *               takes that one if it passed; otherwise the reference
 *
 * A back-end that fails the check is never used for that model, and the
 * reason is shown (agctl models).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "agd.h"
#include "ag/agm.h"
#include "ag/backend.h"
#include "ag/domfeat.h"

#define BACKEND_MAX 8
#define PROBES      300
#define BENCH_REPS  20

struct be {
	const struct ag_backend *ops;
	char source[300];                    /* "built in" or the library's path */
	int ok;                              /* passed the check for the model in use */
	char why[160];
	double ns;                           /* per score, measured at the check */
	void *handle;                        /* prepared for the model in use */
};

static struct be bes[BACKEND_MAX];
static int n_bes;
static struct be *cur;
static char load_notes[400];
static int loaded_dir;

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void note(const char *fmt, const char *a, const char *b)
{
	size_t l = strlen(load_notes);

	if (l < sizeof(load_notes) - 2)
		snprintf(load_notes + l, sizeof(load_notes) - l, "%s", l ? "; " : "");
	l = strlen(load_notes);
	snprintf(load_notes + l, sizeof(load_notes) - l, fmt, a, b);
}

/* root's, and writable by nobody else: only the package manager can put code here */
static int safe_path(const char *path, int dir)
{
	struct stat st;

	return !lstat(path, &st) && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH)) &&
	       (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

static void load_dir(void)
{
	DIR *d;
	struct dirent *e;

	loaded_dir = 1;
	if (!cfg.backend_dir[0] || access(cfg.backend_dir, F_OK))
		return;
	if (!safe_path(cfg.backend_dir, 1)) {
		note("%s%s: not root's, or writable by others: no back-ends loaded from it", cfg.backend_dir, "");
		return;
	}
	if (!(d = opendir(cfg.backend_dir)))
		return;
	while ((e = readdir(d)) && n_bes < BACKEND_MAX) {
		char path[300];
		const struct ag_backend *b;
		ag_backend_entry_fn entry;
		void *lib;
		size_t l = strlen(e->d_name);
		int i, dup = 0;

		if (l < 4 || strcmp(e->d_name + l - 3, ".so"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", cfg.backend_dir, e->d_name);
		if (!safe_path(path, 0)) {
			note("%s%s: not root's, or writable by others: skipped", e->d_name, "");
			continue;
		}
		if (!(lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))) {
			note("%s: %s", e->d_name, dlerror());
			continue;
		}
		entry = (ag_backend_entry_fn)dlsym(lib, AG_BACKEND_ENTRY);
		b = entry ? entry() : NULL;
		if (!b || b->abi != AG_BACKEND_ABI || !b->name || !*b->name || strlen(b->name) > 31 ||
		    !b->prepare || !b->predict || !b->release) {
			note("%s%s: not an AIGW back-end (ag_backend_entry, ABI 1)", e->d_name, "");
			dlclose(lib);
			continue;
		}
		for (i = 0; i < n_bes; i++)
			dup |= !strcmp(bes[i].ops->name, b->name);
		if (dup) {
			note("%s: a back-end named %s is already loaded", e->d_name, b->name);
			dlclose(lib);
			continue;
		}
		bes[n_bes].ops = b;
		snprintf(bes[n_bes].source, sizeof(bes[n_bes].source), "%s", path);
		n_bes++;
		AG_INFO("model back-end %s loaded from %s", b->name, path);
	}
	closedir(d);
}

static void init_list(void)
{
	if (n_bes)
		return;
	bes[0].ops = &ag_backend_interp;
	snprintf(bes[0].source, sizeof(bes[0].source), "built in (reference)");
	bes[1].ops = &ag_backend_flat;
	snprintf(bes[1].source, sizeof(bes[1].source), "built in");
	n_bes = 2;
	if (!loaded_dir)
		load_dir();
}

/* probe inputs: names of every kind, as the model's feature extractor sees them */
static int probes(const struct ag_model *m, float *x)
{
	static const char *const names[] = {
		"google.com", "wikipedia.org", "netflix.com", "bbc.co.uk", "www.google.com", "stackoverflow.com",
		"xkqjzvbwptmr.com", "qwmxzptkrbvn.net", "a8f3k2l9q0zx.info", "x7k2m9q4z8w1.com", "www.xkqjzvbwptmr.com",
		"bydgoszcz.pl", "kofamuveliza.top", "d2xqm7kjv0bh4f.cloudfront.net", NULL
	};
	static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789-";
	static const char *const tlds[] = { "com", "net", "org", "io", "co.uk", "ru", "top", "info" };
	const float *bg;
	char name[80], label[80];
	uint32_t seed = 2166136261u;
	int dim, n = 0, i, j;

	bg = ag_model_bigram(m, &dim);
	for (i = 0; names[i] && n < PROBES; i++)
		if (ag_domain_label(names[i], label, sizeof(label)) == 0)
			ag_domain_features(names[i], label, bg, dim, x + (size_t)n++ * AG_DOMFEAT_N);
	/* generated names: random or pronounceable labels, sometimes under www., various TLDs */
	while (n < PROBES) {
		char lab[24];
		int len, vowelish, www;

		seed = seed * 1103515245u + 12345u;
		len = 4 + (int)((seed >> 16) % 18);
		vowelish = (seed >> 8) & 1;
		www = !((seed >> 4) & 3);
		for (j = 0; j < len; j++) {
			seed = seed * 1103515245u + 12345u;
			lab[j] = vowelish && j % 2 ? "aeiou"[(seed >> 16) % 5] :
				 alpha[(seed >> 16) % (j && j < len - 1 ? 37 : 36)];      /* no '-' at either end */
		}
		lab[len] = '\0';
		seed = seed * 1103515245u + 12345u;
		snprintf(name, sizeof(name), "%s%s.%s", www ? "www." : "", lab, tlds[(seed >> 16) % 8]);
		if (ag_domain_label(name, label, sizeof(label)) == 0)
			ag_domain_features(name, label, bg, dim, x + (size_t)n++ * AG_DOMFEAT_N);
	}
	return n;
}

static double sig(double r) { return 1 / (1 + exp(-r)); }

/* check one back-end against the reference scores; fills ok, why, ns */
static void check(struct be *b, const struct ag_model_view *v, const float *x, int n, const double *ref)
{
	double *raw = calloc((size_t)n, sizeof(double));
	char err[160] = "";
	int64_t t0;
	int i, r;

	b->ok = 0;
	b->ns = 0;
	if (b->handle) {
		b->ops->release(b->handle);
		b->handle = NULL;
	}
	if (!raw)
		return;
	if (!(b->handle = b->ops->prepare(v, err, sizeof(err)))) {
		snprintf(b->why, sizeof(b->why), "can't take this model: %.120s", err[0] ? err : "no reason given");
		goto out;
	}
	if (b->ops->predict(b->handle, x, n, raw)) {
		snprintf(b->why, sizeof(b->why), "failed to score");
		goto out;
	}
	for (i = 0; i < n; i++) {
		double p = sig(raw[i]), q = sig(ref[i]);

		if (!isfinite(raw[i]) || fabs(p - q) > 0.005 || (p >= cfg.dga_threshold) != (q >= cfg.dga_threshold)) {
			snprintf(b->why, sizeof(b->why), "disagrees with the reference on probe %d (%.4f, reference %.4f)",
				 i + 1, p, q);
			goto out;
		}
	}
	t0 = now_ns();
	for (r = 0; r < BENCH_REPS; r++)
		b->ops->predict(b->handle, x, n, raw);
	b->ns = (double)(now_ns() - t0) / ((double)BENCH_REPS * n);
	b->ok = 1;
	b->why[0] = '\0';
out:
	if (!b->ok && b->handle) {
		b->ops->release(b->handle);
		b->handle = NULL;
	}
	free(raw);
}

/* a model is about to be used: check every back-end, choose one */
void backend_select(const struct ag_model *m)
{
	struct ag_model_view v;
	float *x;
	double *ref;
	void *h;
	int n, i;
	char err[160];
	struct be *pick = NULL;

	init_list();
	cur = NULL;
	if (!m) {                                   /* no model: nothing may hold on to the old one */
		for (i = 0; i < n_bes; i++)
			if (bes[i].handle) {
				bes[i].ops->release(bes[i].handle);
				bes[i].handle = NULL;
				bes[i].ok = 0;
			}
		return;
	}
	x = calloc(PROBES, sizeof(float) * AG_DOMFEAT_N);
	ref = calloc(PROBES, sizeof(double));
	if (!x || !ref)
		goto out;
	ag_model_view(m, &v);
	n = probes(m, x);
	h = ag_backend_interp.prepare(&v, err, sizeof(err));
	if (!h)
		goto out;
	ag_backend_interp.predict(h, x, n, ref);
	ag_backend_interp.release(h);
	for (i = 0; i < n_bes; i++)
		check(&bes[i], &v, x, n, ref);
	for (i = 0; i < n_bes; i++) {
		struct be *b = &bes[i];

		if (!b->ok)
			continue;
		if (strcmp(cfg.model_backend, "auto") ? !strcmp(cfg.model_backend, b->ops->name) :
		    (!pick || b->ns < pick->ns))
			pick = b;
	}
	if (!pick) {
		pick = &bes[0];                         /* the reference always passes against itself */
		if (strcmp(cfg.model_backend, "auto"))
			AG_WARN("model back-end %s not usable (%s); using the reference", cfg.model_backend,
				"not loaded, or failed its check: agctl models");
	}
	cur = pick->ok ? pick : NULL;
	for (i = 0; i < n_bes; i++)
		AG_INFO("model back-end %s: %s%s%s", bes[i].ops->name, bes[i].ok ? "passed" : "not usable: ",
			bes[i].ok ? "" : bes[i].why, &bes[i] == cur ? ", in use" : "");
out:
	free(x);
	free(ref);
}

/* the model's probability for one feature vector, by the chosen back-end */
double backend_prob(const struct ag_model *m, const float *x)
{
	double raw;

	if (cur && cur->handle && cur->ops->predict(cur->handle, x, 1, &raw) == 0 && isfinite(raw))
		return sig(raw);
	return ag_model_prob(m, x);
}

const char *backend_name(void)
{
	return cur ? cur->ops->name : "interp";
}

void backend_blob(struct blob_buf *b)
{
	void *arr, *t;
	int i;

	init_list();
	blobmsg_add_string(b, "backend_choice", cfg.model_backend);
	blobmsg_add_string(b, "backend_dir", cfg.backend_dir);
	if (load_notes[0])
		blobmsg_add_string(b, "backend_notes", load_notes);
	arr = blobmsg_open_array(b, "backends");
	for (i = 0; i < n_bes; i++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", bes[i].ops->name);
		blobmsg_add_string(b, "source", bes[i].source);
		blobmsg_add_u8(b, "ok", (uint8_t)bes[i].ok);
		blobmsg_add_u8(b, "in_use", &bes[i] == cur);
		blobmsg_add_double(b, "ns_per_score", bes[i].ns);
		if (bes[i].why[0])
			blobmsg_add_string(b, "why", bes[i].why);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}
