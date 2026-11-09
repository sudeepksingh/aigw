/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-device behaviour baselines: what is normal for each device.
 *
 * Learned continuously, in fixed memory per device:
 *   - the sites it uses (registrable domains, in a 1 KB Bloom filter)
 *   - how many new sites it picks up per hour (novelty)
 *   - how much it uploads per hour (running mean and variance)
 *   - at which hours of the day it is active
 *
 * Once a device is steady (watched long enough, enough sites seen), changes
 * raise alerts with reasons:
 *   new-destination  a device with a stable pattern uses a site it never used
 *   upload-spike     far more upload in an hour than the device's usual
 *   unusual-hour     activity at an hour of day the device has never been active
 *
 * Phones and laptops pick up new sites all the time, so they never count as
 * "stable" and don't raise new-destination alerts; IoT devices do.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "agd.h"
#include "ag/agm.h"
#include "ag/domfeat.h"

#define ALPHA        0.05             /* EWMA weight of the newest bucket (~20-bucket memory) */
#define ACTIVE_BYTES (100 * 1024)     /* a bucket with less traffic doesn't count as active */
#define MAX_CATCHUP  (24 * 31)        /* buckets replayed after a device was away */

/* ---------- Bloom filter (k = 3) ---------- */

static void bloom_idx(const char *s, uint32_t idx[3])
{
	uint32_t h1 = ag_fnv1a(s), h2 = h1 * 0x9e3779b1u;
	int i;

	h2 ^= h2 >> 15;
	h2 |= 1;
	for (i = 0; i < 3; i++)
		idx[i] = (h1 + (uint32_t)i * h2) % (BLOOM_BYTES * 8);
}

static int bloom_has(const uint8_t *bf, const char *s)
{
	uint32_t idx[3];
	int i;

	bloom_idx(s, idx);
	for (i = 0; i < 3; i++)
		if (!(bf[idx[i] >> 3] & (1u << (idx[i] & 7))))
			return 0;
	return 1;
}

static void bloom_add(uint8_t *bf, const char *s)
{
	uint32_t idx[3];
	int i;

	bloom_idx(s, idx);
	for (i = 0; i < 3; i++)
		bf[idx[i] >> 3] |= (uint8_t)(1u << (idx[i] & 7));
}

/* ---------- buckets ---------- */

static int64_t bucket_start(int64_t t)
{
	return t - (t % cfg.bucket_seconds);
}

/* Hour of day of a bucket: local time for real hours; for the short buckets
 * the tests use, every 24 buckets make a "day". */
static int hour_of(int64_t bucket)
{
	if (cfg.bucket_seconds == 3600) {
		time_t tt = (time_t)bucket;
		struct tm tm;

		localtime_r(&tt, &tm);
		return tm.tm_hour;
	}
	return (int)((bucket / cfg.bucket_seconds) % 24);
}

static double per_day(double per_bucket)
{
	return per_bucket * 86400.0 / cfg.bucket_seconds;
}

static void finish_bucket(struct baseline *b)
{
	double x = (double)b->up_now, d = x - b->up_mean;

	b->up_mean += ALPHA * d;
	b->up_var = (1 - ALPHA) * (b->up_var + ALPHA * d * d);
	b->novelty = (1 - ALPHA) * b->novelty + ALPHA * b->new_now;
	if (b->up_now + b->down_now >= ACTIVE_BYTES) {
		int h = hour_of(b->bucket);

		if (b->active[h] < 0xffff)
			b->active[h]++;
	}
	b->buckets++;
	b->up_now = b->down_now = 0;
	b->new_now = 0;
	b->spike_alerted = b->unusual_alerted = 0;
}

static void roll(struct baseline *b, int64_t now)
{
	int64_t cur = bucket_start(now);
	int n = 0;

	if (!b->since)
		b->since = now;
	if (!b->bucket) {
		b->bucket = cur;
		return;
	}
	while (b->bucket < cur && n++ < MAX_CATCHUP) {
		finish_bucket(b);
		b->bucket += cfg.bucket_seconds;
	}
	if (b->bucket < cur)
		b->bucket = cur;
}

int baseline_steady(const struct device *d)
{
	return d->bl.since && ag_wall_s() - d->bl.since >= (int64_t)cfg.learn_minutes * 60 &&
	       d->bl.known >= (uint32_t)cfg.min_known;
}

int baseline_quiet_hour(const struct device *d)
{
	return d->bl.since && d->bl.buckets >= (uint32_t)cfg.unusual_min_days * 24 &&
	       d->bl.active[hour_of(bucket_start(ag_wall_s()))] == 0;
}

static const char *dev_label(const struct device *d)
{
	return d->hostname[0] ? d->hostname : d->mac[0] ? d->mac : d->ip;
}

static const char *human(double bytes, char *out, size_t n)
{
	if (bytes >= 1024.0 * 1024 * 1024)
		snprintf(out, n, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024.0 * 1024)
		snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024));
	else
		snprintf(out, n, "%.0f KB", bytes / 1024.0);
	return out;
}

/* ---------- learning + judging ---------- */

void baseline_domain(struct device *d, const char *qname)
{
	struct baseline *b = &d->bl;
	char site[256], reason[512];
	int steady;

	if (ag_domain_registrable(qname, site, sizeof(site)) < 0)
		return;
	roll(b, ag_wall_s());
	if (bloom_has(b->bloom, site))
		return;
	steady = baseline_steady(d);
	bloom_add(b->bloom, site);
	b->known++;
	b->new_now++;
	if (!steady || per_day(b->novelty) >= cfg.novelty_max)
		return;
	snprintf(reason, sizeof(reason),
		 "%s used %.120s for the first time. It normally sticks to %u known sites "
		 "(about %.1f new per day), so a new one can mean a firmware update, "
		 "a new feature, or a compromise.",
		 dev_label(d), site, b->known - 1, per_day(b->novelty));
	alerts_raise("new-destination", "warning", d->ip, site, 0, reason);
}

void baseline_traffic(struct device *d, uint64_t up, uint64_t down)
{
	struct baseline *b = &d->bl;
	char reason[512], h1[24], h2[24];
	int64_t now = ag_wall_s();
	double sd;

	roll(b, now);
	b->up_now += up;
	b->down_now += down;
	if (!baseline_steady(d) || b->buckets < 24)
		return;

	sd = sqrt(b->up_var > 0 ? b->up_var : 0);
	if (!b->spike_alerted && (double)b->up_now >= cfg.spike_min_mb * 1024 * 1024 &&
	    (double)b->up_now > b->up_mean + cfg.spike_factor * sd) {
		b->spike_alerted = 1;
		snprintf(reason, sizeof(reason),
			 "%s uploaded %s in the current %s, against a usual %s. "
			 "That can be a backup or video upload, or data being taken.",
			 dev_label(d), human((double)b->up_now, h1, sizeof(h1)),
			 cfg.bucket_seconds == 3600 ? "hour" : "period",
			 human(b->up_mean, h2, sizeof(h2)));
		alerts_raise("upload-spike", "warning", d->ip, "upload", (double)b->up_now, reason);
	}

	if (!b->unusual_alerted && b->buckets >= (uint32_t)cfg.unusual_min_days * 24 &&
	    b->up_now + b->down_now >= ACTIVE_BYTES && b->active[hour_of(b->bucket)] == 0) {
		char when[32];

		b->unusual_alerted = 1;
		if (cfg.bucket_seconds == 3600)
			snprintf(when, sizeof(when), "%02d:00", hour_of(b->bucket));
		else
			snprintf(when, sizeof(when), "slot %d", hour_of(b->bucket));
		snprintf(reason, sizeof(reason),
			 "%s is active at %s, which it hasn't been in the %u day%s AIGW has watched it.",
			 dev_label(d), when, b->buckets / 24, b->buckets / 24 == 1 ? "" : "s");
		alerts_raise("unusual-hour", "info", d->ip, when, 0, reason);
	}
}

/* Close finished buckets for idle devices too, so quiet hours count. */
void baseline_tick(void)
{
	int64_t now = ag_wall_s();
	int i;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].bl.since)
			roll(&devs[i].bl, now);
}

/* ---------- representation ---------- */

void baseline_blob(struct blob_buf *b, const struct device *d, int full)
{
	const struct baseline *bl = &d->bl;
	void *t, *a;
	int i;

	t = blobmsg_open_table(b, "baseline");
	blobmsg_add_string(b, "state", !bl->since ? "new" : baseline_steady(d) ? "steady" : "learning");
	blobmsg_add_u32(b, "known_sites", bl->known);
	blobmsg_add_double(b, "new_sites_per_day", per_day(bl->novelty));
	blobmsg_add_u8(b, "stable", baseline_steady(d) && per_day(bl->novelty) < cfg.novelty_max);
	blobmsg_add_double(b, "upload_per_bucket", bl->up_mean);
	blobmsg_add_u32(b, "buckets", bl->buckets);
	a = blobmsg_open_array(b, "active_hours");
	for (i = 0; i < 24; i++)
		blobmsg_add_u32(b, NULL, bl->active[i]);
	blobmsg_close_array(b, a);
	if (full) {
		char hex[BLOOM_BYTES * 2 + 1];

		for (i = 0; i < BLOOM_BYTES; i++)
			snprintf(hex + i * 2, 3, "%02x", bl->bloom[i]);
		blobmsg_add_u64(b, "since", (uint64_t)bl->since);
		blobmsg_add_double(b, "novelty", bl->novelty);
		blobmsg_add_double(b, "up_var", bl->up_var);
		blobmsg_add_string(b, "sites", hex);
	}
	blobmsg_close_table(b, t);
}

static double dnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	case BLOBMSG_TYPE_INT64: return (double)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8: return (double)blobmsg_get_u8(a);
	default: return 0;
	}
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void baseline_load(struct device *d, struct blob_attr *tbl)
{
	struct baseline *bl = &d->bl;
	struct blob_attr *cur, *e;
	size_t rem, r2;

	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "known_sites")) bl->known = (uint32_t)dnum(cur);
		else if (!strcmp(k, "since")) bl->since = (int64_t)dnum(cur);
		else if (!strcmp(k, "buckets")) bl->buckets = (uint32_t)dnum(cur);
		else if (!strcmp(k, "novelty")) bl->novelty = dnum(cur);
		else if (!strcmp(k, "upload_per_bucket")) bl->up_mean = dnum(cur);
		else if (!strcmp(k, "up_var")) bl->up_var = dnum(cur);
		else if (!strcmp(k, "active_hours") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			int i = 0;

			blobmsg_for_each_attr(e, cur, r2)
				if (i < 24)
					bl->active[i++] = (uint16_t)dnum(e);
		} else if (!strcmp(k, "sites") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			const char *h = blobmsg_get_string(cur);
			int i;

			if (strlen(h) != BLOOM_BYTES * 2)
				continue;
			for (i = 0; i < BLOOM_BYTES; i++) {
				int hi = hexval(h[2 * i]), lo = hexval(h[2 * i + 1]);

				if (hi < 0 || lo < 0) {
					memset(bl->bloom, 0, sizeof(bl->bloom));
					bl->known = 0;
					break;
				}
				bl->bloom[i] = (uint8_t)(hi << 4 | lo);
			}
		}
	}
	if (!isfinite(bl->novelty) || bl->novelty < 0) bl->novelty = 0;
	if (!isfinite(bl->up_mean) || bl->up_mean < 0) bl->up_mean = 0;
	if (!isfinite(bl->up_var) || bl->up_var < 0) bl->up_var = 0;
}
