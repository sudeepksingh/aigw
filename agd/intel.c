/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Shared threat intelligence (phase 5): a signed list of names that people
 * confirmed as threats, or that several homes flagged, published by cortex
 * and delivered like a model update (models.c checks the signature and the
 * version, and can roll it back). A lookup of a listed name, or of any name
 * under it, raises a "known-threat" alert. No raw data travels: the list is
 * names and a short reason each.
 *
 *   # aigw-list threat-intel
 *   # version 3
 *   xkqjzvbwptmr.com<TAB>confirmed by people in 2 homes
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agd.h"

#define INTEL_MAX   20000
#define REASON_MAX  100

struct entry {
	char *name;
	char *reason;
};

struct intel {
	char name[32];
	int version;
	char published[24];
	int n;
	struct entry *e;
	char *blob;                  /* all strings live here */
};

static struct intel *active;

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static int name_ok(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > 253 || !strchr(s, '.') || s[0] == '.' || s[n - 1] == '.')
		return 0;
	for (; *s; s++)
		if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '.' || *s == '-' || *s == '_'))
			return 0;
	return 1;
}

void intel_free(void *obj)
{
	struct intel *in = obj;

	if (!in)
		return;
	free(in->e);
	free(in->blob);
	free(in);
}

/* the list's name, or NULL when data is not an aigw list */
void *intel_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en)
{
	struct intel *in;
	char *line, *save = NULL;
	int cap = 0;

	if (len < 13 || memcmp(data, "# aigw-list ", 12)) {
		snprintf(err, en, "not an aigw list");
		return NULL;
	}
	in = calloc(1, sizeof(*in));
	if (!in || !(in->blob = malloc(len + 1))) {
		free(in);
		snprintf(err, en, "out of memory");
		return NULL;
	}
	memcpy(in->blob, data, len);
	in->blob[len] = '\0';
	for (line = strtok_r(in->blob, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *tab, *r;

		line[strcspn(line, "\r")] = '\0';
		if (line[0] == '#') {
			if (!strncmp(line, "# aigw-list ", 12))
				snprintf(in->name, sizeof(in->name), "%.31s", line + 12);
			else if (!strncmp(line, "# version ", 10))
				in->version = atoi(line + 10);
			else if (!strncmp(line, "# published ", 12))
				snprintf(in->published, sizeof(in->published), "%.23s", line + 12);
			continue;
		}
		if (!*line)
			continue;
		tab = strchr(line, '\t');
		r = "";
		if (tab) {
			*tab = '\0';
			r = tab + 1;
			if (strlen(r) > REASON_MAX)
				r[REASON_MAX] = '\0';
		}
		for (char *c = line; *c; c++)
			*c = (char)tolower((unsigned char)*c);
		if (!name_ok(line)) {
			snprintf(err, en, "bad name '%.60s'", line);
			goto bad;
		}
		for (char *c = r; *c; c++)
			if ((unsigned char)*c < 0x20)
				*c = ' ';
		if (in->n >= INTEL_MAX) {
			snprintf(err, en, "more than %d names", INTEL_MAX);
			goto bad;
		}
		if (in->n >= cap) {
			struct entry *ne = realloc(in->e, (size_t)(cap = cap ? cap * 2 : 256) * sizeof(*ne));

			if (!ne)
				goto bad;
			in->e = ne;
		}
		in->e[in->n].name = line;
		in->e[in->n].reason = r;
		in->n++;
	}
	if (!in->name[0]) {
		snprintf(err, en, "list has no name");
		goto bad;
	}
	qsort(in->e, (size_t)in->n, sizeof(*in->e), cmp_entry);
	snprintf(name, nn, "%s", in->name);
	return in;
bad:
	intel_free(in);
	return NULL;
}

int intel_check(const void *obj, char *err, size_t n)
{
	const struct intel *in = obj;

	if (in->version <= 0) {
		snprintf(err, n, "list has no version");
		return -1;
	}
	return 0;
}

void intel_use(void *obj)
{
	struct intel *old = active;

	active = obj;
	if (old && old != active)
		intel_free(old);
	if (active)
		AG_INFO("threat intelligence in use: %d names (version %d)", active->n, active->version);
}

int intel_version(const void *obj, char *published, size_t n)
{
	const struct intel *in = obj;

	if (published && n)
		snprintf(published, n, "%s", in->published);
	return in->version;
}

int intel_size(const void *obj)
{
	return ((const struct intel *)obj)->n;
}

/* qname or a parent of it on the list (never a bare TLD): 1, with the listed name and reason */
int intel_match(const char *qname, char *hit, size_t hn, char *why, size_t wn)
{
	char q[NAME_LEN];
	const char *p;
	size_t i;

	if (!active || !active->n)
		return 0;
	for (i = 0; qname[i] && i < sizeof(q) - 1; i++)
		q[i] = (char)tolower((unsigned char)qname[i]);
	q[i] = '\0';
	if (i && q[i - 1] == '.')
		q[i - 1] = '\0';
	for (p = q; p && strchr(p, '.'); p = strchr(p, '.') ? strchr(p, '.') + 1 : NULL) {
		struct entry key = { (char *)p, NULL }, *e;

		e = bsearch(&key, active->e, (size_t)active->n, sizeof(key), cmp_entry);
		if (e) {
			snprintf(hit, hn, "%s", e->name);
			snprintf(why, wn, "%s", *e->reason ? e->reason : "on the shared threat list");
			return 1;
		}
	}
	return 0;
}
