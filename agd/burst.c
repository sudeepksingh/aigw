/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Lookup bursts: behaviour, not names.
 *
 * Malware that generates domain names tries many of them until one answers.
 * Most get "no such name" (NXDOMAIN), and none was ever used in the home. The
 * name model (alerts.c) misses generators whose names look like words; this
 * signal doesn't read the names at all.
 *
 *   home memory  every registrable domain that resolved for any device, in two
 *                Bloom filter generations: when the newer one fills, the older
 *                is dropped, so sites nobody uses any more are forgotten.
 *                Saved with the state.
 *   per device   its recent lookups of names the home never used: distinct
 *                registrable domains, and whether each got NXDOMAIN.
 *
 *   dga-burst    within alerts.burst_window, at least alerts.burst_min such
 *                names got NXDOMAIN, and they are at least alerts.burst_share
 *                of the device's never-used names then. A never-used name that
 *                answers right after a failed one is named as the likely
 *                control server (in the alert, or in a second one when it comes
 *                later).
 *
 * Not counted: names AIGW blocked, answers of 0.0.0.0 (filtering resolvers),
 * empty answers and server failures, the same name again, local names.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "agd.h"
#include "ag/agm.h"
#include "ag/domfeat.h"

#define HOME_BYTES   8192             /* 65536 bits per generation */
#define HOME_GEN_MAX 4000             /* sites per generation: ~0.5% false "used" each */
#define HIT_GAP      15               /* seconds: an answer this soon after a failure is the hit */

static uint8_t home[2][HOME_BYTES];   /* [0] newer, [1] older */
static uint32_t home_n[2];
static uint64_t bursts_total, hits_total;

static void idx3(const char *s, uint32_t idx[3])
{
	uint32_t h1 = ag_fnv1a(s), h2 = h1 * 0x85ebca6bu;
	int i;

	h2 ^= h2 >> 13;
	h2 |= 1;
	for (i = 0; i < 3; i++)
		idx[i] = (h1 + (uint32_t)i * h2) % (HOME_BYTES * 8);
}

static int gen_has(const uint8_t *bf, const char *s)
{
	uint32_t idx[3];
	int i;

	idx3(s, idx);
	for (i = 0; i < 3; i++)
		if (!(bf[idx[i] >> 3] & (1u << (idx[i] & 7))))
			return 0;
	return 1;
}

static void gen_add(uint8_t *bf, const char *s)
{
	uint32_t idx[3];
	int i;

	idx3(s, idx);
	for (i = 0; i < 3; i++)
		bf[idx[i] >> 3] |= (uint8_t)(1u << (idx[i] & 7));
}

/* a site that works: kept in the newer generation, so one in use is never forgotten */
static void home_learn(const char *site)
{
	if (gen_has(home[0], site))
		return;
	if (home_n[0] >= HOME_GEN_MAX) {
		memcpy(home[1], home[0], HOME_BYTES);
		home_n[1] = home_n[0];
		memset(home[0], 0, HOME_BYTES);
		home_n[0] = 0;
		AG_INFO("burst: home memory turned over (%u sites kept as the older generation)", home_n[1]);
	}
	gen_add(home[0], site);
	home_n[0]++;
}

static int home_has(const char *site)
{
	return gen_has(home[0], site) || gen_has(home[1], site);
}

static const char *dev_label(const struct device *d)
{
	return d->hostname[0] ? d->hostname : d->mac[0] ? d->mac : d->ip;
}

static void raise_hit(struct device *d, const char *site, unsigned nx)
{
	char reason[512];

	hits_total++;
	snprintf(reason, sizeof(reason),
		 "%.63s reached %.80s right after trying %u names that don't exist: likely the control server "
		 "it was looking for. Block it for the whole home with 'agctl block %.80s'.",
		 dev_label(d), site, nx, site);
	alerts_raise("dga-burst", "warning", d->ip, site, 1.0, reason);
}

void burst_dns(struct device *d, const char *qname, int outcome, int flagged)
{
	struct burst *b = &d->burst;
	struct burst_item *it, *prev;
	char site[NAME_LEN], reason[640], extra[200];
	unsigned i, n = 0, nx = 0, fl = 0;
	int64_t now = ag_wall_s(), first_nx = 0;
	uint32_t h;
	int known, is_cand = 0;

	if (!cfg.alerts_enabled || !cfg.burst_enabled || outcome == BURST_IGNORE)
		return;
	if (ag_domain_registrable(qname, site, sizeof(site)) < 0)
		return;
	known = home_has(site);
	if (outcome == BURST_OK)
		home_learn(site);
	if (known)
		return;

	/* the same name again: a retry, not another name */
	h = ag_fnv1a(site);
	for (i = 0; i < BURST_RING; i++) {
		it = &b->it[i];
		if (it->ts && it->h == h && now - it->ts < cfg.burst_window) {
			if (outcome == BURST_OK)
				it->nx = 0;
			return;
		}
	}

	prev = &b->it[(b->head + BURST_RING - 1) % BURST_RING];
	if (outcome == BURST_OK && prev->ts && prev->nx && now - prev->ts <= HIT_GAP) {
		snprintf(b->cand, sizeof(b->cand), "%s", site);
		b->cand_ts = now;
		is_cand = 1;
	}
	it = &b->it[b->head];
	b->head = (b->head + 1) % BURST_RING;
	it->ts = now;
	it->h = h;
	it->nx = outcome == BURST_NX;
	it->flagged = flagged != 0;

	for (i = 0; i < BURST_RING; i++) {
		const struct burst_item *e = &b->it[i];

		if (!e->ts || now - e->ts >= cfg.burst_window)
			continue;
		n++;
		if (e->nx) {
			nx++;
			fl += e->flagged;
			if (!first_nx || e->ts < first_nx)
				first_nx = e->ts;
		}
	}

	if (outcome == BURST_OK) {
		/* after a burst: the first never-used name that answers right after a failure */
		if (now < b->hit_until && !b->hit_named && is_cand) {
			b->hit_named = 1;
			raise_hit(d, site, nx);
		}
		return;
	}
	if (nx < (unsigned)cfg.burst_min || nx < cfg.burst_share * n ||
	    (b->alerted && now - b->alerted < cfg.burst_window))
		return;

	bursts_total++;
	b->alerted = now;
	b->hit_until = now + cfg.burst_window;
	b->hit_named = 0;
	extra[0] = '\0';
	if (b->cand_ts && b->cand_ts >= first_nx) {
		b->hit_named = 1;
		hits_total++;
		snprintf(extra, sizeof(extra), " One answered: %.60s, likely its control server ('agctl block %.60s').",
			 b->cand, b->cand);
	}
	snprintf(reason, sizeof(reason),
		 "%.63s looked up %u names in %d min that don't exist and that no device here ever used "
		 "(the name model flagged %u of them)%s. Malware that generates names tries many until one answers, "
		 "to find its control servers.%s",
		 dev_label(d), nx, (int)((now - first_nx) / 60) + 1, fl,
		 baseline_quiet_hour(d) ? ", at an hour it is normally idle" : "", extra);
	alerts_raise("dga-burst", "warning", d->ip, b->hit_named ? b->cand : "failed lookups",
		     (double)nx / n, reason);
}

void burst_blob(struct blob_buf *bb)
{
	void *t = blobmsg_open_table(bb, "bursts");

	blobmsg_add_u8(bb, "enabled", (uint8_t)(cfg.alerts_enabled && cfg.burst_enabled));
	blobmsg_add_u32(bb, "home_sites", home_n[0] + home_n[1]);
	blobmsg_add_u64(bb, "raised", bursts_total);
	blobmsg_add_u64(bb, "servers_named", hits_total);
	blobmsg_close_table(bb, t);
}

/* ---------- state ---------- */

static void hex_out(struct blob_buf *bb, const char *name, const uint8_t *p, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	static char out[HOME_BYTES * 2 + 1];
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hx[p[i] >> 4];
		out[2 * i + 1] = hx[p[i] & 15];
	}
	out[2 * n] = '\0';
	blobmsg_add_string(bb, name, out);
}

static int hex_in(const char *s, uint8_t *p, size_t n)
{
	size_t i;

	if (strlen(s) != n * 2)
		return -1;
	for (i = 0; i < 2 * n; i++) {
		char c = s[i];
		int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

		if (v < 0)
			return -1;
		if (i & 1)
			p[i / 2] = (uint8_t)(p[i / 2] | v);
		else
			p[i / 2] = (uint8_t)(v << 4);
	}
	return 0;
}

void burst_save(struct blob_buf *bb)
{
	void *t = blobmsg_open_table(bb, "home_sites");

	blobmsg_add_u32(bb, "n0", home_n[0]);
	blobmsg_add_u32(bb, "n1", home_n[1]);
	hex_out(bb, "gen0", home[0], HOME_BYTES);
	hex_out(bb, "gen1", home[1], HOME_BYTES);
	blobmsg_close_table(bb, t);
}

void burst_load(struct blob_attr *tbl)
{
	static uint8_t g[2][HOME_BYTES];
	uint32_t n[2] = { 0, 0 };
	int ok = 0;
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *k = blobmsg_name(cur);
		int t = blobmsg_type(cur);

		if (!strcmp(k, "n0") && t == BLOBMSG_TYPE_INT32)
			n[0] = blobmsg_get_u32(cur);
		else if (!strcmp(k, "n1") && t == BLOBMSG_TYPE_INT32)
			n[1] = blobmsg_get_u32(cur);
		else if (!strcmp(k, "gen0") && t == BLOBMSG_TYPE_STRING)
			ok |= hex_in(blobmsg_get_string(cur), g[0], HOME_BYTES) ? 0 : 1;
		else if (!strcmp(k, "gen1") && t == BLOBMSG_TYPE_STRING)
			ok |= hex_in(blobmsg_get_string(cur), g[1], HOME_BYTES) ? 0 : 2;
	}
	if (ok != 3) {
		AG_WARN("burst: ignoring an unreadable home memory in the state file");
		return;
	}
	memcpy(home, g, sizeof(home));
	home_n[0] = n[0] > HOME_GEN_MAX ? HOME_GEN_MAX : n[0];
	home_n[1] = n[1] > HOME_GEN_MAX ? HOME_GEN_MAX : n[1];
	AG_INFO("burst: restored the home's memory of %u sites", home_n[0] + home_n[1]);
}
