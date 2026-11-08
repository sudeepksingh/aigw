/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Models (phase 5, "outside"): the packaged models, signed updates that are
 * hot-loaded without a restart, and rollback.
 *
 *   offered ── by the bridge plug-in (from cortex) or by you (agctl model install)
 *      │
 *   check ──── signed by a trusted key (usign, /etc/aigw/keys.d) · parses ·
 *      │       reads this build's features · sane scores · newer than the one
 *      │       in use and than any version you rolled back
 *      ▼
 *   install ── written to models.dir (the previous update kept as .prev), then
 *              swapped in between two lookups; an ag.model event says so
 *
 * Signed lists travel the same way: "threat-intel" (intel.c), names other
 * homes confirmed as threats, has no packaged version and only comes from
 * cortex.
 *
 * The packaged model (model_dir, from the package) needs no signature and is
 * the fallback: a broken or untrusted update is refused and the running model
 * stays. `agctl model rollback` goes back one step and holds that version off.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>

#include "agd.h"
#include "ag/agm.h"
#include "ag/sign.h"

#define MODEL_MAX_BYTES (8 * 1024 * 1024)
#define KEY_MAX         32

/* what a slot holds: an .agm model, or a signed list (threat intelligence) */
struct ops {
	const char *ext;           /* file name extension in models.dir */
	const char *kind;          /* "model" or "list" */
	const char *size_word;     /* "trees" or "entries" */
	void *(*parse)(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en);
	int (*check)(const void *obj, char *err, size_t n);
	void (*use)(void *obj);
	void (*release)(void *obj);
	int (*version)(const void *obj, char *date, size_t n);
	int (*size)(const void *obj);
};

static int meta_int(const struct ag_model *m, const char *key, char *str, size_t sn);

static void *agm_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en)
{
	char why[160];
	struct ag_model *m = ag_model_parse(data, len, why, sizeof(why));

	if (!m) {
		snprintf(err, en, "not a usable .agm file: %s", why);
		return NULL;
	}
	snprintf(name, nn, "%s", ag_model_name(m));
	return m;
}
static int agm_check(const void *o, char *err, size_t n) { return alerts_model_check(o, err, n); }
static void agm_use(void *o) { alerts_model_use(o); }
static void agm_release(void *o) { ag_model_free(o); }
static int agm_version(const void *o, char *date, size_t n) { return meta_int(o, "version", date, n); }
static int agm_size(const void *o) { return ag_model_ntrees(o); }

static const struct ops agm_ops = {
	".agm", "model", "trees", agm_parse, agm_check, agm_use, agm_release, agm_version, agm_size,
};
static const struct ops list_ops = {
	".list", "list", "entries", intel_parse, intel_check, intel_use, intel_free, intel_version, intel_size,
};

static const struct ops *ops_for(const uint8_t *data, size_t len)
{
	if (len >= 4 && !memcmp(data, "AGM1", 4))
		return &agm_ops;
	if (len >= 12 && !memcmp(data, "# aigw-list ", 12))
		return &list_ops;
	return NULL;
}

struct slot {
	const char *name;
	const struct ops *ops;

	int loaded;
	int version;               /* from the model's META "version" / the list's header; packaged = 0 */
	int trees;                 /* trees, or entries of a list */
	char source[12];           /* "package" or "update" */
	char signer[17];           /* key fingerprint, "" for the package */
	char origin[64];           /* who offered it */
	char trained[24];
	int64_t installed;
	int hold;                  /* versions up to this one were rolled back: not again */
	char refused[200];         /* the last offer that was turned down, and why */
	int64_t refused_at;
};

static struct slot slots[] = {
	{ .name = "domain-risk", .ops = &agm_ops },
	{ .name = "threat-intel", .ops = &list_ops },     /* no packaged version: only from cortex */
};
#define NSLOTS ((int)(sizeof(slots) / sizeof(slots[0])))

static struct slot *slot_find(const char *name)
{
	int i;

	for (i = 0; i < NSLOTS; i++)
		if (!strcmp(slots[i].name, name))
			return &slots[i];
	return NULL;
}

static uint8_t *read_file(const char *path, size_t max, size_t *len, char *err, size_t n)
{
	struct stat st;
	uint8_t *buf;
	FILE *f = fopen(path, "rb");

	if (!f) {
		snprintf(err, n, "%s: %s", path, strerror(errno));
		return NULL;
	}
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max) {
		snprintf(err, n, "%s: not a regular file of at most %zu bytes", path, max);
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		snprintf(err, n, "%s: read failed", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[st.st_size] = 0;
	*len = (size_t)st.st_size;
	fclose(f);
	return buf;
}

/* 0 when sigfile is a trusted key's signature of data; the key's fingerprint in signer */
int models_verify(const uint8_t *data, size_t len, const char *sigfile, char signer[17], char *err, size_t n)
{
	uint8_t fp[8], sig[64], kfp[8], pk[32];
	char *text, kpath[512];
	size_t tl;
	DIR *d;
	struct dirent *e;
	int found = 0, ok = -1;

	if (!sigfile || !*sigfile || access(sigfile, F_OK)) {
		snprintf(err, n, "not signed (no .sig file)");
		return -1;
	}
	text = (char *)read_file(sigfile, 4096, &tl, err, n);
	if (!text)
		return -1;
	if (ag_usign_sig(text, fp, sig)) {
		snprintf(err, n, "the signature file is not in usign format");
		free(text);
		return -1;
	}
	free(text);
	ag_fingerprint_hex(fp, signer);
	d = opendir(cfg.trusted_keys);
	while (d && (e = readdir(d)) && ok) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(kpath, sizeof(kpath), "%s/%s", cfg.trusted_keys, e->d_name);
		text = (char *)read_file(kpath, 1024, &tl, err, n);
		if (!text)
			continue;
		if (!ag_usign_pubkey(text, kfp, pk) && !memcmp(kfp, fp, 8)) {
			found = 1;
			ok = ag_ed25519_verify(sig, data, len, pk);
		}
		free(text);
	}
	if (d)
		closedir(d);
	if (!found)
		snprintf(err, n, "signed by key %s, which this gateway doesn't trust (agctl trust <key.pub>)", signer);
	else if (ok)
		snprintf(err, n, "signature by key %s does not match: the file was changed", signer);
	return ok;
}

static int meta_int(const struct ag_model *m, const char *key, char *str, size_t sn)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	size_t rem;
	int v = 0;

	if (str && sn)
		str[0] = '\0';
	blob_buf_init(&b, 0);
	if (blobmsg_add_json_from_string(&b, ag_model_meta(m))) {
		blob_for_each_attr(cur, b.head, rem) {
			if (!strcmp(blobmsg_name(cur), key)) {
				switch (blobmsg_type(cur)) {
				case BLOBMSG_TYPE_INT32: v = (int)blobmsg_get_u32(cur); break;
				case BLOBMSG_TYPE_INT64: v = (int)blobmsg_get_u64(cur); break;
				case BLOBMSG_TYPE_DOUBLE: v = (int)blobmsg_get_double(cur); break;
				}
			} else if (str && !strcmp(blobmsg_name(cur), "trained") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
				snprintf(str, sn, "%s", blobmsg_get_string(cur));
			}
		}
	}
	blob_buf_free(&b);
	return v;
}

static void paths(const struct slot *s, const char *suffix, char *out, size_t n)
{
	snprintf(out, n, "%s/%s%s%s", cfg.models_dir, s->name, s->ops->ext, suffix);
}

/* the slot a file is for, from its name inside (NULL if none) */
static struct slot *slot_of(const uint8_t *data, size_t len)
{
	const struct ops *o = ops_for(data, len);
	char name[64], err[160];
	struct slot *s;
	void *obj;

	if (!o || !(obj = o->parse(data, len, name, sizeof(name), err, sizeof(err))))
		return NULL;
	o->release(obj);
	s = slot_find(name);
	return s && s->ops == o ? s : NULL;
}

/* verify + parse + check; the object on success */
static void *load(struct slot *want, const uint8_t *data, size_t len, const char *sigfile, int need_sig,
		  struct slot **which, char signer[17], char *err, size_t n)
{
	const struct ops *o;
	struct slot *s;
	char why[200], name[64];
	void *obj;

	signer[0] = '\0';
	if (need_sig || (sigfile && *sigfile && !access(sigfile, F_OK))) {
		if (models_verify(data, len, sigfile, signer, err, n))
			return NULL;
	}
	if (!(o = ops_for(data, len))) {
		snprintf(err, n, "neither an .agm model nor an aigw list");
		return NULL;
	}
	if (!(obj = o->parse(data, len, name, sizeof(name), why, sizeof(why)))) {
		snprintf(err, n, "%s", why);
		return NULL;
	}
	s = slot_find(name);
	if (!s || s->ops != o || (want && s != want)) {
		snprintf(err, n, "%s '%s' is not one this gateway uses", o->kind, name);
		o->release(obj);
		return NULL;
	}
	if (o->check(obj, why, sizeof(why))) {
		snprintf(err, n, "%s %s", s->name, why);
		o->release(obj);
		return NULL;
	}
	*which = s;
	return obj;
}

static void use(struct slot *s, void *obj, const char *source, const char *signer, const char *origin)
{
	s->version = s->ops->version(obj, s->trained, sizeof(s->trained));
	s->trees = s->ops->size(obj);
	s->loaded = 1;
	snprintf(s->source, sizeof(s->source), "%s", source);
	snprintf(s->signer, sizeof(s->signer), "%s", signer ? signer : "");
	snprintf(s->origin, sizeof(s->origin), "%s", origin ? origin : "");
	s->installed = ag_wall_s();
	s->ops->use(obj);
}

static void event(const struct slot *s, const char *what, const char *why)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "model", s->name);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_u32(&b, "version", (uint32_t)s->version);
	blobmsg_add_string(&b, "source", s->source);
	blobmsg_add_string(&b, "origin", s->origin);
	if (why)
		blobmsg_add_string(&b, "why", why);
	agd_emit(AG_EV_MODEL, &b);
	blob_buf_free(&b);
}

static int load_package(struct slot *s, char *err, size_t n)
{
	char path[512], signer[17];
	struct slot *w;
	void *m;
	uint8_t *data;
	size_t len;

	snprintf(path, sizeof(path), "%s/%s%s", cfg.model_dir, s->name, s->ops->ext);
	data = read_file(path, MODEL_MAX_BYTES, &len, err, n);
	if (!data)
		return -1;
	m = load(s, data, len, NULL, 0, &w, signer, err, n);
	free(data);
	if (!m)
		return -1;
	use(s, m, "package", "", "package");
	return 0;
}

static int read_hold(const struct slot *s)
{
	char path[512], buf[32] = "";
	FILE *f;

	paths(s, ".hold", path, sizeof(path));
	if (!(f = fopen(path, "r")))
		return 0;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	return atoi(buf);
}

static void write_hold(const struct slot *s)
{
	char path[512];
	FILE *f;

	paths(s, ".hold", path, sizeof(path));
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%d\n", s->hold);
		fclose(f);
	}
}

void models_init(void)
{
	int i;

	if (!cfg.alerts_enabled)
		return;
	for (i = 0; i < NSLOTS; i++) {
		struct slot *s = &slots[i], *w;
		char path[512], sig[512], err[256], signer[17];
		void *m = NULL;
		uint8_t *data;
		size_t len;

		s->hold = read_hold(s);
		paths(s, "", path, sizeof(path));
		paths(s, ".sig", sig, sizeof(sig));
		if (!access(path, F_OK)) {
			data = read_file(path, MODEL_MAX_BYTES, &len, err, sizeof(err));
			if (data)
				m = load(s, data, len, sig, cfg.models_require_sig, &w, signer, err, sizeof(err));
			free(data);
			if (m) {
				use(s, m, "update", signer, "installed earlier");
				AG_INFO("model %s: update version %d (signed by %s)", s->name, s->version,
					signer[0] ? signer : "nobody");
				continue;
			}
			AG_WARN("%s %s: installed update not used: %s", s->ops->kind, s->name, err);
		}
		if (s->ops == &list_ops)
			continue;                    /* lists only come as updates */
		if (load_package(s, err, sizeof(err)))
			AG_WARN("model %s not loaded (%s); other alerts still work", s->name, err);
	}
}

static int write_atomic(const char *path, const uint8_t *data, size_t len)
{
	char tmp[600];
	int fd, ok;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, path)) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void refuse(struct slot *s, const char *origin, const char *err)
{
	if (!s)
		return;
	snprintf(s->refused, sizeof(s->refused), "%.60s: %.130s", origin, err);
	s->refused_at = ag_wall_s();
	event(s, "refused", err);
}

int models_offer(const char *file, const char *sig, const char *origin, int force, int *version,
		 char *err, size_t errlen)
{
	char dst[512], dsig[512], prev[512], psig[512], signer[17];
	struct slot *s = NULL;
	void *m;
	uint8_t *data, *sdata = NULL;
	size_t len, slen = 0;
	int v;

	if (!cfg.alerts_enabled) {
		snprintf(err, errlen, "alerts are off, so no model is used (alerts.enabled)");
		return -1;
	}
	data = read_file(file, MODEL_MAX_BYTES, &len, err, errlen);
	if (!data)
		return -1;
	m = load(NULL, data, len, sig, cfg.models_require_sig, &s, signer, err, errlen);
	if (!m) {
		/* name the slot in the refusal when the file at least says what it is */
		refuse(slot_of(data, len), origin, err);
		AG_WARN("update offered by %s refused: %s", origin, err);
		free(data);
		return -1;
	}
	v = s->ops->version(m, NULL, 0);
	if (!force && s->loaded && v <= s->version) {
		snprintf(err, errlen, "%s version %d is not newer than the one in use (%d)", s->name, v, s->version);
		goto refused;
	}
	if (!force && v <= s->hold) {
		snprintf(err, errlen, "%s version %d: you rolled back to before version %d (agctl model install --force)",
			 s->name, v, s->hold);
		goto refused;
	}
	if (sig && *sig && !access(sig, F_OK))
		sdata = read_file(sig, 4096, &slen, err, errlen);
	if (mkdir(cfg.models_dir, 0755) && errno != EEXIST) {
		snprintf(err, errlen, "cannot create %s: %s", cfg.models_dir, strerror(errno));
		goto fail;
	}
	paths(s, "", dst, sizeof(dst));
	paths(s, ".sig", dsig, sizeof(dsig));
	paths(s, ".prev", prev, sizeof(prev));
	paths(s, ".prev.sig", psig, sizeof(psig));
	if (!strcmp(s->source, "update")) {     /* keep the one in use for rollback */
		rename(dst, prev);
		if (rename(dsig, psig))
			unlink(psig);
	}
	if (write_atomic(dst, data, len) || (sdata ? write_atomic(dsig, sdata, slen) : (unlink(dsig), 0))) {
		snprintf(err, errlen, "cannot write %s: %s", dst, strerror(errno));
		goto fail;
	}
	use(s, m, "update", signer, origin);
	if (force && v <= s->hold) {
		s->hold = 0;
		write_hold(s);
	}
	s->refused[0] = '\0';
	AG_INFO("model %s: version %d from %s now in use (signed by %s), no restart needed", s->name, v, origin,
		signer[0] ? signer : "nobody");
	event(s, "loaded", NULL);
	if (version)
		*version = v;
	free(data);
	free(sdata);
	return 0;
refused:
	refuse(s, origin, err);
	AG_INFO("model offered by %s refused: %s", origin, err);
fail:
	s->ops->release(m);
	free(data);
	free(sdata);
	return -1;
}

int models_rollback(const char *name, char *err, size_t errlen)
{
	struct slot *s = slot_find(name && *name ? name : "domain-risk"), *w;
	char dst[512], dsig[512], prev[512], psig[512], signer[17], why[256];
	void *m = NULL;
	uint8_t *data;
	size_t len;
	int from;

	if (!s) {
		snprintf(err, errlen, "no model or list '%s' (domain-risk, threat-intel)", name);
		return -1;
	}
	if (strcmp(s->source, "update")) {
		snprintf(err, errlen, s->ops == &list_ops ? "%s has no update to roll back" :
			 "%s is already the packaged model", s->name);
		return -1;
	}
	from = s->version;
	paths(s, "", dst, sizeof(dst));
	paths(s, ".sig", dsig, sizeof(dsig));
	paths(s, ".prev", prev, sizeof(prev));
	paths(s, ".prev.sig", psig, sizeof(psig));
	if (!access(prev, F_OK) && (data = read_file(prev, MODEL_MAX_BYTES, &len, why, sizeof(why)))) {
		m = load(s, data, len, psig, cfg.models_require_sig, &w, signer, why, sizeof(why));
		free(data);
	}
	if (m) {
		if (rename(prev, dst) || (rename(psig, dsig) && errno != ENOENT)) {
			snprintf(err, errlen, "cannot restore %s: %s", prev, strerror(errno));
			s->ops->release(m);
			return -1;
		}
		use(s, m, "update", signer, "rollback");
	} else {
		unlink(dst);
		unlink(dsig);
		unlink(prev);
		unlink(psig);
		if (s->ops == &list_ops) {          /* nothing before it: the list is gone */
			s->ops->use(NULL);
			s->loaded = s->version = s->trees = 0;
			s->source[0] = s->signer[0] = s->trained[0] = '\0';
		} else if (load_package(s, err, errlen)) {
			return -1;
		}
		snprintf(s->origin, sizeof(s->origin), "rollback");
	}
	s->hold = from > s->hold ? from : s->hold;
	write_hold(s);
	AG_INFO("model %s: rolled back from version %d to %d (%s); version %d and older are held off",
		s->name, from, s->version, s->source, s->hold);
	event(s, "rolled back", NULL);
	return 0;
}

int models_version(const char *name)
{
	struct slot *s = slot_find(name);

	return s && s->loaded ? s->version : -1;
}

void models_blob(struct blob_buf *b, const char *name)
{
	void *arr = blobmsg_open_array(b, name), *t;
	DIR *d;
	struct dirent *e;
	int i;

	for (i = 0; i < NSLOTS; i++) {
		const struct slot *s = &slots[i];

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", s->name);
		blobmsg_add_string(b, "kind", s->ops->kind);
		blobmsg_add_u8(b, "loaded", (uint8_t)s->loaded);
		blobmsg_add_u32(b, "version", (uint32_t)s->version);
		blobmsg_add_u32(b, s->ops->size_word, (uint32_t)s->trees);
		blobmsg_add_string(b, "source", s->source);
		blobmsg_add_string(b, "signer", s->signer);
		blobmsg_add_string(b, "origin", s->origin);
		blobmsg_add_string(b, "trained", s->trained);
		blobmsg_add_u64(b, "installed", (uint64_t)s->installed);
		blobmsg_add_u32(b, "held_back", (uint32_t)s->hold);
		blobmsg_add_string(b, "refused", s->refused);
		blobmsg_add_u64(b, "refused_at", (uint64_t)s->refused_at);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);

	arr = blobmsg_open_array(b, "trusted_keys");
	d = opendir(cfg.trusted_keys);
	while (d && (e = readdir(d))) {
		char path[512], *text, err[64], fph[17];
		uint8_t fp[8], pk[32];
		size_t len;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", cfg.trusted_keys, e->d_name);
		text = (char *)read_file(path, 1024, &len, err, sizeof(err));
		if (text && !ag_usign_pubkey(text, fp, pk)) {
			char *nl = strchr(text, '\n');

			t = blobmsg_open_table(b, NULL);
			ag_fingerprint_hex(fp, fph);
			blobmsg_add_string(b, "fingerprint", fph);
			if (nl)
				*nl = '\0';
			blobmsg_add_string(b, "comment", text + (strncmp(text, "untrusted comment: ", 19) ? 0 : 19));
			blobmsg_add_string(b, "file", path);
			blobmsg_close_table(b, t);
		}
		free(text);
	}
	if (d)
		closedir(d);
	blobmsg_close_array(b, arr);
}
