/* SPDX-License-Identifier: Apache-2.0 */
/*
 * aigw-demo: the simulated home of the AIGW OS demo image (docs/try-it.md).
 *
 * It writes what agd keeps about a home, as if agd had run there for three
 * days, so agctl, LuCI, the data model and cortex show real-looking content
 * in a virtual machine with no devices and no internet:
 *
 *   the timeline   why.file: a sample every minute for 72 h and what happened
 *                  (devices joining, speed tests, an upgrade, calls, AIGW's
 *                  decisions). Yesterday from 20:00 the slow evening: the TV in
 *                  4K and a big game download fill the line, the internet's
 *                  delay goes from 12 ms to about 190 ms, a work call stutters.
 *                  AIGW finds bufferbloat, proposes smart queueing, a person
 *                  approves it at 20:38: the delay falls to about 22 ms, the
 *                  next call is smooth, and the trial keeps it
 *   agd's state    state_file: nine devices, the alert and the decision,
 *                  smart queueing's trial, calls first's report, two weeks of
 *                  proactive care's days and the nightly speed test
 *   the upgrade    upgrade.file: an upgrade to this version two days ago,
 *                  watched for 24 h and kept
 *
 * The verdicts and their words come from AIGW's own rules (core/libag): smart
 * queueing's judgement, the calls' quality, the upgrade check. agd then
 * explains the timeline itself when asked (agctl why, agctl brief).
 *
 *   aigw-demo check                 is this a QEMU virtual machine? (exit 0: yes)
 *   aigw-demo seed --state F --why F --upgrade F [--now T] [--replace] [--test]
 *
 * seed refuses outside QEMU (--test: in the test suite, with files of its
 * own), and won't overwrite an existing state file without --replace. The
 * demo image's init script (platform/openwrt/aigw-demo) is the only caller;
 * this program is in no package a router installs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>

#include "ag/ag.h"
#include "ag/calls.h"
#include "ag/sqm.h"
#include "ag/upgrade.h"
#include "ag/why.h"

#define STEP   60
#define HOURS  72
#define NMIN   (HOURS * 3600 / STEP)
#define LINE_MBPS 94.2f                 /* the nightly speed test */
#define NORMAL_MS 12
#define WAN    "eth1"                   /* the demo VM's WAN (OpenWRT x86: the second NIC) */
#define FROM_VERSION "0.40.0"

static const char *const DMI_VENDOR = "/sys/class/dmi/id/sys_vendor";

/* ---------- the home ---------- */

enum { TV, MAYA, DAD, MUM, WORK, XBOX, ECHO, NEST, CAM, ND };

static const struct dev {
	const char *mac, *ip, *host, *type, *vc;
	int wifi;                       /* on Wi-Fi (5 or 2 GHz), 0 wired */
	const char *doms[3];
} D[ND] = {
	[TV]   = { "02:a1:6e:00:00:11", "192.168.1.110", "Living-room-TV", "TV", "", 5,
		   { "netflix.com", "nflxvideo.net", "youtube.com" } },
	[MAYA] = { "02:a1:6e:00:00:12", "192.168.1.112", "Mayas-MacBook", "Mac", "", 5,
		   { "steampowered.com", "steamcontent.com", "discord.com" } },
	[DAD]  = { "02:a1:6e:00:00:13", "192.168.1.113", "Dads-Pixel", "Android", "android-dhcp-14", 5,
		   { "whatsapp.net", "googleapis.com", "gstatic.com" } },
	[MUM]  = { "02:a1:6e:00:00:14", "192.168.1.114", "Mums-iPhone", "iPhone", "", 5,
		   { "icloud.com", "apple.com", "instagram.com" } },
	[WORK] = { "02:a1:6e:00:00:15", "192.168.1.115", "LAPTOP-WORK7", "Windows", "MSFT 5.0", 5,
		   { "teams.microsoft.com", "office.com", "sharepoint.com" } },
	[XBOX] = { "02:a1:6e:00:00:16", "192.168.1.116", "Xbox-Series-X", "game console", "", 0,
		   { "xboxlive.com", "xbox.com", "microsoft.com" } },
	[ECHO] = { "02:a1:6e:00:00:17", "192.168.1.117", "Kitchen-Echo", "smart speaker", "", 2,
		   { "amazon.com", "amazonalexa.com", "a2z.com" } },
	[NEST] = { "02:a1:6e:00:00:18", "192.168.1.118", "Nest-Thermostat", "smart home", "", 2,
		   { "nest.com", "googleapis.com", "" } },
	[CAM]  = { "02:a1:6e:00:00:19", "192.168.1.119", "Doorbell-camera", "camera", "udhcp 1.36.1", 2,
		   { "ring.com", "amazonaws.com", "" } },
};

/* the times of the story (wall s), from "now" */
static struct {
	long now, first, last;          /* the timeline: first and last sample */
	long e;                         /* yesterday 20:00, local time */
	long tv0, tv1;                  /* the TV in 4K */
	long big0, big1;                /* the game download */
	long prop, on;                  /* smart queueing proposed, approved (in effect) */
	long call1a, call1b;            /* a work call on the full line, before */
	long call2a, call2b;            /* a call on the full line, with smart queueing */
	long game0, game1;              /* the evening before: a game on the Xbox */
	long up;                        /* the upgrade: activated */
	long boot;                      /* ... the new image running */
} T;

static struct why_min ring[NMIN];
static int nring;
static struct why_ev evs[WHY_EV_MAX];
static int nev;

static uint32_t h32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/* a number in [0, n) that is the same for the same time and salt */
static int noise(long t, int salt, int n)
{
	return n > 0 ? (int)(h32((uint32_t)t * 2654435761U ^ (uint32_t)salt * 40503U) % (uint32_t)n) : 0;
}

static int in(long t, long a, long b)
{
	return t >= a && t < b;
}

static void local_tm(long t, struct tm *tm)
{
	time_t tt = (time_t)t;

	localtime_r(&tt, tm);
}

static void mac_bytes(const char *s, uint8_t m[6])
{
	unsigned v[6];
	int i;

	memset(m, 0, 6);
	if (s && sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
		for (i = 0; i < 6; i++)
			m[i] = (uint8_t)v[i];
}

/* ---------- what happened ---------- */

static void ev(long t, int kind, int dev, int flag, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void ev(long t, int kind, int dev, int flag, const char *fmt, ...)
{
	struct why_ev *e;
	va_list ap;

	if (nev >= WHY_EV_MAX || t < T.first || t > T.now)
		return;
	e = &evs[nev++];
	memset(e, 0, sizeof(*e));
	e->t = (uint32_t)t;
	e->kind = (uint8_t)kind;
	e->flag = (uint8_t)flag;
	if (dev >= 0)
		mac_bytes(D[dev].mac, e->mac);
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
}

static int by_ev_time(const void *a, const void *b)
{
	const struct why_ev *x = a, *y = b;

	return x->t < y->t ? -1 : x->t > y->t;
}

/* ---------- a sample ---------- */

/* kbit/s each device downloads at t */
static void downloads(long t, uint32_t kb[ND])
{
	struct tm tm;
	int h, evening, weekday;

	local_tm(t, &tm);
	h = tm.tm_hour;
	evening = h >= 18 && h < 23;
	weekday = tm.tm_wday >= 1 && tm.tm_wday <= 5;
	memset(kb, 0, sizeof(uint32_t) * ND);
	/* the TV: HD in the evening, 4K during the slow evening */
	if (in(t, T.tv0, T.tv1))
		kb[TV] = 23500 + (uint32_t)noise(t, 1, 1500);
	else if ((h == 18 && tm.tm_min >= 30) || (evening && h >= 19) || (h >= 21 && h < 23))
		kb[TV] = 6500 + (uint32_t)noise(t, 1, 2500);
	/* Maya's laptop: browsing after school; the game download */
	if (in(t, T.big0, T.on))
		kb[MAYA] = 64500 + (uint32_t)noise(t, 2, 2500);
	else if (in(t, T.on, T.big1))
		kb[MAYA] = 54000 + (uint32_t)noise(t, 2, 2000);
	else if (h >= 16 && h < 22)
		kb[MAYA] = 900 + (uint32_t)noise(t, 2, 3000);
	/* the work laptop: video calls in office hours */
	if (in(t, T.call1a, T.call1b))
		kb[WORK] = 2200 + (uint32_t)noise(t, 3, 400);
	else if (weekday && h >= 9 && h < 17)
		kb[WORK] = 1500 + (uint32_t)noise(t, 3, 1800);
	/* the phones */
	if (in(t, T.call2a, T.call2b))
		kb[DAD] = 900 + (uint32_t)noise(t, 4, 300);
	else if (evening || (h >= 7 && h < 9))
		kb[DAD] = 200 + (uint32_t)noise(t, 4, 1500);
	if (evening || (h >= 7 && h < 9) || (!weekday && h >= 10 && h < 18))
		kb[MUM] = 300 + (uint32_t)noise(t, 5, 1800);
	/* the Xbox: a game the evening before */
	if (in(t, T.game0, T.game1))
		kb[XBOX] = 700 + (uint32_t)noise(t, 6, 600);
	/* the rest of the home, all day */
	kb[ECHO] = 20 + (uint32_t)noise(t, 7, 60) + (h >= 7 && h < 9 ? 300 : 0);
	kb[NEST] = 5 + (uint32_t)noise(t, 8, 10);
	kb[CAM] = 60 + (uint32_t)noise(t, 9, 120) + (noise(t, 10, 40) == 0 ? 2500 : 0);
}

static void sample(long t, struct why_min *m)
{
	uint32_t kb[ND], sum = 0, up = 0;
	struct tm tm;
	int i, k;

	local_tm(t, &tm);
	memset(m, 0, sizeof(*m));
	m->t = (uint32_t)t;
	m->route = 1;
	m->gw_sent = m->in_sent = 3;
	m->gw_ms = (uint16_t)(1 + noise(t, 20, 2));
	m->in_ms = (uint16_t)(NORMAL_MS - 1 + noise(t, 21, 4));
	m->dns_ms = (uint16_t)(4 + noise(t, 22, 5));
	m->dns_q = (uint16_t)((tm.tm_hour >= 1 && tm.tm_hour < 6 ? 2 : tm.tm_hour >= 17 && tm.tm_hour < 23 ? 30 : 12) +
			      noise(t, 23, 10));
	downloads(t, kb);
	for (i = 0; i < ND; i++)
		sum += kb[i];
	/* the slow evening: the line is full and the queue in the modem makes everything wait */
	if (in(t, T.big0, T.on)) {
		m->in_ms = (uint16_t)(165 + noise(t, 24, 60));
		m->gw_ms = (uint16_t)(2 + noise(t, 25, 3));
		m->dns_ms = (uint16_t)(150 + noise(t, 27, 80));
	} else if (in(t, T.on, T.big1)) {
		m->in_ms = (uint16_t)(18 + noise(t, 24, 8));       /* smart queueing: full, and still quick */
		m->dns_ms = (uint16_t)(6 + noise(t, 27, 6));
	}
	up = sum / 12 + 300 + (uint32_t)noise(t, 28, 200);
	if (in(t, T.call1a, T.call1b) || in(t, T.call2a, T.call2b))
		up += 1500;
	m->down_kbps = sum;
	m->up_kbps = up;
	/* the two devices that downloaded most */
	for (k = 0; k < WHY_TOP; k++) {
		int best = -1;

		for (i = 0; i < ND; i++)
			if (kb[i] && (best < 0 || kb[i] > kb[best]))
				best = i;
		if (best < 0)
			break;
		mac_bytes(D[best].mac, m->top[k].mac);
		m->top[k].kbps = kb[best];
		kb[best] = 0;                   /* the next one is another device */
	}
}

static void timeline(void)
{
	long t;

	nring = 0;
	for (t = T.first; t <= T.last && nring < NMIN; t += STEP) {
		/* the minute the gateway rebooted into the new image: no sample */
		if (t >= T.up && t < T.boot)
			continue;
		sample(t, &ring[nring++]);
	}
}

/* ---------- the story ---------- */

static struct sqm_line line(void)
{
	struct sqm_line l;
	int i;

	memset(&l, 0, sizeof(l));
	l.down_mbps = LINE_MBPS;
	for (i = 0; i < nring; i++) {
		if (ring[i].down_kbps / 1000.0f > l.peak_down_mbps)
			l.peak_down_mbps = ring[i].down_kbps / 1000.0f;
		if (ring[i].up_kbps / 1000.0f > l.peak_up_mbps)
			l.peak_up_mbps = ring[i].up_kbps / 1000.0f;
	}
	l.normal_ms = NORMAL_MS;
	l.have_normal = 1;
	return l;
}

static struct {
	char target[64];
	uint32_t down, up;
	char alert_why[400];
	struct sqm_stats before, after;
	int verdict;
	long judged;
	char summary[400];
} Q;

static struct {
	struct call_report r[3];
	char home[3][600], isp[3][400];
	int n;
} C;

static void smart_queueing(void)
{
	struct sqm_line l = line();
	struct sqm_params p;
	long t;

	sqm_params_default(&p);
	sqm_rates(&l, &p, &Q.down, &Q.up);
	snprintf(Q.target, sizeof(Q.target), "%s:%u:%u", WAN, Q.down, Q.up);
	if (!sqm_bloated(ring, nring, T.prop, STEP, &l, &p, Q.alert_why, sizeof(Q.alert_why)))
		snprintf(Q.alert_why, sizeof(Q.alert_why), "The line was full and the internet slow meanwhile.");
	sqm_stats_of(ring, nring, T.on - 86400, T.on, &l, &Q.before);
	for (t = T.on + STEP; t < T.now; t += STEP) {
		sqm_stats_of(ring, nring, T.on + (long)p.grace * STEP, t + 1, &l, &Q.after);
		Q.verdict = sqm_judge(&Q.before, &Q.after, &l, &p, Q.down, Q.summary, sizeof(Q.summary));
		if (Q.verdict != SQ_WAIT) {
			Q.judged = t;
			break;
		}
	}
}

static int top_of(long a, long b)
{
	uint32_t sum[ND] = { 0 };
	int i, k, best = -1;

	for (i = 0; i < nring; i++) {
		if (!in((long)ring[i].t, a, b))
			continue;
		for (k = 0; k < WHY_TOP; k++) {
			uint8_t m[6];
			int d;

			for (d = 0; d < ND; d++) {
				mac_bytes(D[d].mac, m);
				if (!memcmp(m, ring[i].top[k].mac, 6))
					sum[d] += ring[i].top[k].kbps;
			}
		}
	}
	for (i = 0; i < ND; i++)
		if (sum[i] && (best < 0 || sum[i] > sum[best]))
			best = i;
	return best;
}

static void call(int dev, int kind, const char *app, long a, long b, int prio, int sqm_on)
{
	struct sqm_line l = line();
	struct call_params p;
	struct call_report *r = &C.r[C.n];
	struct call_report anon;
	int top;

	call_params_default(&p);
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->app = app;
	r->device = D[dev].host;
	r->start = a;
	r->end = b;
	r->prioritized = prio;
	r->sqm_on = sqm_on;
	call_quality_of(ring, nring, a, b, &l, &r->q);
	r->verdict = call_verdict(&r->q, &p);
	top = top_of(a, b);
	r->top = top >= 0 && top != dev ? D[top].host : NULL;
	call_words(r, C.home[C.n], sizeof(C.home[0]));
	anon = *r;
	anon.app = anon.device = anon.top = NULL;
	call_words(&anon, C.isp[C.n], sizeof(C.isp[0]));
	ev(b, WE_CALL, dev, r->verdict, "%s", C.home[C.n]);
	C.n++;
}

static void story(void)
{
	long day, t;
	struct tm tm;
	int i;

	/* every night: the speed test; every evening the phones come home, every morning they go
	   (local hours, from the midnight before the timeline starts) */
	local_tm(T.first, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	for (day = (long)mktime(&tm); day < T.now; day += 3600) {
		local_tm(day, &tm);
		t = day;
		if (tm.tm_hour == 3 && tm.tm_min == 0)
			ev(t + 120, WE_SPEED, -1, 0, "Speed test: %.1f Mbit/s", LINE_MBPS - 1.5 + noise(t, 30, 30) / 10.0);
		if (tm.tm_hour == 8 && tm.tm_min == 0) {
			ev(t + 600 + noise(t, 31, 900), WE_LEAVE, DAD, 0, "%s left Wi-Fi", D[DAD].host);
			ev(t + 1500 + noise(t, 32, 900), WE_LEAVE, MUM, 0, "%s left Wi-Fi", D[MUM].host);
		}
		if (tm.tm_hour == 17 && tm.tm_min == 0) {
			ev(t + 1800 + noise(t, 33, 1500), WE_JOIN, MUM, 5, "%s joined Wi-Fi on 5 GHz (-%d dBm)", D[MUM].host,
			   58 + noise(t, 34, 6));
			ev(t + 3000 + noise(t, 35, 1500), WE_JOIN, DAD, 5, "%s joined Wi-Fi on 5 GHz (-%d dBm)", D[DAD].host,
			   55 + noise(t, 36, 6));
		}
	}
	/* the upgrade to this version, two days ago: watched for a day, kept */
	ev(T.up, WE_UPGRADE, -1, 0, "Firmware %s to %s: activated", FROM_VERSION, AG_VERSION);
	ev(T.boot + 5, WE_START, -1, 1, "The gateway started (it was off for %ld min)", (T.boot - T.up + 30) / 60);
	ev(T.boot + 86400 + 60, WE_UPGRADE, -1, 0, "Firmware %s to %s: kept", FROM_VERSION, AG_VERSION);
	/* the evening before: a game, smooth */
	call(XBOX, CK_GAME, "Xbox Live", T.game0, T.game1, 0, 0);
	/* the slow evening */
	ev(T.tv0, WE_JOIN, MAYA, 5, "%s joined Wi-Fi on 5 GHz (-%d dBm)", D[MAYA].host, 52);
	call(WORK, CK_CALL, "Microsoft Teams", T.call1a, T.call1b, 0, 0);
	ev(T.on, WE_DECISION, -1, 4 /* WF_WIFI */, "AIGW acted: sqm %s, by policy bufferbloat, approved by you", Q.target);
	call(DAD, CK_CALL, "WhatsApp", T.call2a, T.call2b, 1, 1);
	qsort(evs, (size_t)nev, sizeof(evs[0]), by_ev_time);
	for (i = 0; i < nev; i++)
		if ((long)evs[i].t > T.now)
			nev = i;
}

/* ---------- writing ---------- */

static int write_file(const char *path, const void *data, size_t n)
{
	char tmp[512], dir[512], *sl;
	FILE *f;
	int ok;

	snprintf(dir, sizeof(dir), "%s", path);
	if ((sl = strrchr(dir, '/')) && sl != dir) {
		*sl = '\0';
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen(tmp, "wb"))) {
		fprintf(stderr, "aigw-demo: %s: %s\n", tmp, strerror(errno));
		return -1;
	}
	ok = fwrite(data, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) < 0) {
		fprintf(stderr, "aigw-demo: %s: %s\n", path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int write_why(const char *path)
{
	struct why_file_hdr h;
	size_t n = sizeof(h) + sizeof(ring[0]) * (size_t)nring + sizeof(evs);
	char *buf = calloc(1, n);
	int rc;

	if (!buf)
		return -1;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, WHY_FILE_MAGIC, 8);
	h.min_size = sizeof(struct why_min);
	h.ev_size = sizeof(struct why_ev);
	h.ring = (uint32_t)nring;
	h.interval = STEP;
	h.ev_n = (uint32_t)nev;
	h.ev_head = (uint32_t)nev % WHY_EV_MAX;
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), ring, sizeof(ring[0]) * (size_t)nring);
	memcpy(buf + sizeof(h) + sizeof(ring[0]) * (size_t)nring, evs, sizeof(evs));
	rc = write_file(path, buf, n);
	free(buf);
	return rc;
}

static int write_json(const char *path, struct blob_buf *b)
{
	char *json = blobmsg_format_json(b->head, true);
	int rc;

	if (!json)
		return -1;
	rc = write_file(path, json, strlen(json));
	free(json);
	return rc;
}

static void top_list(struct blob_buf *b, const char *name, const char *count, const struct dev *d, uint64_t base)
{
	void *arr = blobmsg_open_array(b, name), *t;
	int i;

	for (i = 0; i < 3; i++) {
		if (!d->doms[i] || !d->doms[i][0])
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", d->doms[i]);
		blobmsg_add_u64(b, count, base >> i);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

static void devices_json(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "devices"), *t;
	int i;

	for (i = 0; i < ND; i++) {
		const struct dev *d = &D[i];
		uint64_t down = 0;
		int k;

		for (k = 0; k < nring; k++) {
			int j;
			uint8_t m[6];

			mac_bytes(d->mac, m);
			for (j = 0; j < WHY_TOP; j++)
				if (!memcmp(m, ring[k].top[j].mac, 6))
					down += (uint64_t)ring[k].top[j].kbps * 1000 / 8 * STEP;
		}
		if (!down)
			down = 20000000ULL + (uint64_t)noise(i, 40, 50000000);
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "ip", d->ip);
		blobmsg_add_string(b, "mac", d->mac);
		blobmsg_add_string(b, "hostname", d->host);
		blobmsg_add_string(b, "type", d->type);
		if (d->vc[0])
			blobmsg_add_string(b, "vendor_class", d->vc);
		blobmsg_add_u64(b, "first_seen", (uint64_t)(T.now - 86400 * (20 + i)));
		blobmsg_add_u64(b, "last_seen", (uint64_t)(T.now - 30 - noise(i, 41, 200)));
		blobmsg_add_u64(b, "queries", (uint64_t)(400 + noise(i, 42, 4000)));
		blobmsg_add_u64(b, "flows", (uint64_t)(200 + noise(i, 43, 2000)));
		blobmsg_add_u64(b, "bytes_down", down);
		blobmsg_add_u64(b, "bytes_up", down / 14);
		if (i == ECHO || i == NEST || i == CAM)
			blobmsg_add_string(b, "zone", "iot");
		top_list(b, "top_domains", "lookups", d, 2000 + (uint64_t)noise(i, 44, 3000));
		top_list(b, "top_destinations", "bytes", d, down / 2);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

static void alerts_json(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "alerts"), *t;
	char subject[64];
	struct tm tm;

	local_tm(T.prop, &tm);
	snprintf(subject, sizeof(subject), "%s, %02d:%02d:%02d", WAN, tm.tm_hour, tm.tm_min, tm.tm_sec);
	t = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "id", 1);
	blobmsg_add_string(b, "kind", "bufferbloat");
	blobmsg_add_string(b, "severity", "info");
	blobmsg_add_u64(b, "first", (uint64_t)T.prop);
	blobmsg_add_u64(b, "last", (uint64_t)T.prop);
	blobmsg_add_u32(b, "count", 1);
	blobmsg_add_string(b, "device", Q.target);
	blobmsg_add_string(b, "ip", "");
	blobmsg_add_string(b, "mac", "");
	blobmsg_add_string(b, "subject", subject);
	blobmsg_add_double(b, "score", 1.0);
	blobmsg_add_string(b, "reason", Q.alert_why);
	blobmsg_close_table(b, t);
	blobmsg_close_array(b, arr);
}

static void decisions_json(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "decisions"), *t;
	char reason[640];

	snprintf(reason, sizeof(reason), "%s A trial at %u:%u (kbit/s down:up): kept if the delay while the line is full "
		 "clearly falls and downloads keep their speed, else undone.", Q.alert_why, Q.down, Q.up);
	t = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "id", 1);
	blobmsg_add_string(b, "state", Q.verdict == SQ_KEEP ? "kept" : "active");
	blobmsg_add_string(b, "action", "sqm");
	blobmsg_add_string(b, "target", Q.target);
	blobmsg_add_string(b, "scope", "");
	blobmsg_add_u32(b, "rate", 0);
	blobmsg_add_string(b, "device", "");
	blobmsg_add_string(b, "origin", "policy bufferbloat, approved by you");
	blobmsg_add_u32(b, "alert", 1);
	blobmsg_add_u32(b, "ttl", 86400);
	blobmsg_add_u8(b, "automatic", 0);
	blobmsg_add_u64(b, "created", (uint64_t)T.prop);
	blobmsg_add_u64(b, "decided", (uint64_t)T.on);
	blobmsg_add_u64(b, "applied", (uint64_t)T.on);
	blobmsg_add_u64(b, "expires", (uint64_t)(Q.verdict == SQ_KEEP ? 0 : T.on + 86400));
	blobmsg_add_string(b, "reason", reason);
	blobmsg_add_string(b, "note", Q.summary);
	blobmsg_close_table(b, t);
	blobmsg_close_array(b, arr);
}

static void sqm_json(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "sqm");

	blobmsg_add_u32(b, "id", 1);
	blobmsg_add_u64(b, "on_at", (uint64_t)T.on);
	blobmsg_add_u32(b, "before_n", (uint32_t)Q.before.n);
	blobmsg_add_u32(b, "before_ms", (uint32_t)(Q.before.ms < 0 ? 0 : Q.before.ms));
	blobmsg_add_u32(b, "before_kbps", Q.before.kbps);
	blobmsg_add_u32(b, "verdict", (uint32_t)Q.verdict);
	blobmsg_add_u64(b, "judged_at", (uint64_t)Q.judged);
	blobmsg_add_u32(b, "after_n", (uint32_t)Q.after.n);
	blobmsg_add_u32(b, "after_ms", (uint32_t)(Q.after.ms < 0 ? 0 : Q.after.ms));
	blobmsg_add_u32(b, "after_kbps", Q.after.kbps);
	blobmsg_add_string(b, "summary", Q.summary);
	blobmsg_add_string(b, "target", Q.target);
	blobmsg_close_table(b, t);
}

static void calls_json(struct blob_buf *b)
{
	const struct call_report *r = &C.r[C.n - 1];        /* the last one */
	void *t = blobmsg_open_table(b, "calls");
	uint32_t on_full = 0, smooth_full = 0, prio = 0;
	int i;

	for (i = 0; i < C.n; i++) {
		if (C.r[i].q.full) {
			on_full++;
			smooth_full += C.r[i].verdict == CV_SMOOTH;
		}
		prio += (uint32_t)C.r[i].prioritized;
	}
	/* the week's calls: these three and the ones on a quiet line before them */
	blobmsg_add_u32(b, "total", 14);
	blobmsg_add_u32(b, "calls", 11);
	blobmsg_add_u32(b, "games", 3);
	blobmsg_add_u32(b, "on_full", on_full);
	blobmsg_add_u32(b, "smooth_full", smooth_full);
	blobmsg_add_u32(b, "prioritized", prio);
	blobmsg_add_u32(b, "kind", (uint32_t)r->kind);
	blobmsg_add_u32(b, "verdict", (uint32_t)r->verdict);
	blobmsg_add_u32(b, "last_prio", (uint32_t)r->prioritized);
	blobmsg_add_u32(b, "last_sqm", (uint32_t)r->sqm_on);
	blobmsg_add_u64(b, "start", (uint64_t)r->start);
	blobmsg_add_u64(b, "end", (uint64_t)r->end);
	blobmsg_add_u64(b, "full_s", (uint64_t)r->q.full * STEP);
	blobmsg_add_u32(b, "samples", (uint32_t)r->q.samples);
	blobmsg_add_u32(b, "full", (uint32_t)r->q.full);
	blobmsg_add_u32(b, "judged", (uint32_t)r->q.judged);
	blobmsg_add_u32(b, "ms", (uint32_t)(r->q.ms + 1));
	blobmsg_add_u32(b, "jitter", (uint32_t)(r->q.jitter_ms + 1));
	blobmsg_add_u32(b, "loss10", (uint32_t)r->q.loss10);
	blobmsg_add_u32(b, "line_kbps", r->q.line_kbps);
	blobmsg_add_string(b, "mac", D[DAD].mac);
	blobmsg_add_string(b, "app", r->app ? r->app : "");
	blobmsg_add_string(b, "home", C.home[C.n - 1]);
	blobmsg_add_string(b, "isp", C.isp[C.n - 1]);
	blobmsg_close_table(b, t);
}

static long day_of(long t)
{
	struct tm tm;

	local_tm(t, &tm);
	return (t + tm.tm_gmtoff) / 86400;
}

static void care_json(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "care"), *s, *arr, *row;
	long today = day_of(T.now), yday = day_of(T.e), d;
	int k;

	blobmsg_add_u64(b, "last_check", (uint64_t)(T.now - 120));
	s = blobmsg_open_table(b, "speed");
	{
		struct tm tm;
		long at;

		local_tm(T.now, &tm);                   /* the last night's test, 03:02 */
		tm.tm_hour = 3;
		tm.tm_min = 2;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		at = (long)mktime(&tm);
		if (at > T.now)
			at -= 86400;
		blobmsg_add_u64(b, "at", (uint64_t)at);
		blobmsg_add_double(b, "mbps", LINE_MBPS);
		blobmsg_add_u32(b, "status", 1);
		blobmsg_add_u64(b, "day_done", (uint64_t)day_of(at));
	}
	blobmsg_close_table(b, s);
	arr = blobmsg_open_array(b, "days");
	for (k = 13; k >= 0; k--) {
		unsigned checks = 144, good = 144, bad0 = 0, problems = 0, fixed = 0, psecs = 0, watched = 86400;
		unsigned speed = (unsigned)(LINE_MBPS * 1000) - 1500 + (unsigned)noise(k, 50, 3000);

		d = today - k;
		if (d == today) {
			struct tm tm;

			local_tm(T.now, &tm);
			watched = (unsigned)(tm.tm_hour * 3600 + tm.tm_min * 60);
			checks = good = watched / 600;
			if (!checks)
				continue;
		}
		if (d == yday) {                /* the slow evening: the line full and slow, until smart queueing fixed it */
			bad0 = (unsigned)((T.on - T.big0) / 600);
			good = checks - bad0;
			problems = fixed = 1;
			psecs = (unsigned)(T.on - T.big0);
		}
		if (d == day_of(T.up)) {        /* the upgrade's reboot */
			good = checks - 1;
			watched -= 120;
		}
		row = blobmsg_open_array(b, NULL);
		blobmsg_add_u64(b, NULL, (uint64_t)d);
		blobmsg_add_u32(b, NULL, checks);
		blobmsg_add_u32(b, NULL, good);
		blobmsg_add_u32(b, NULL, bad0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, problems);
		blobmsg_add_u32(b, NULL, 0);            /* reported: smart queueing was AIGW's to do */
		blobmsg_add_u32(b, NULL, fixed);
		blobmsg_add_u32(b, NULL, psecs);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, d == today && watched < 4 * 3600 ? 0 : 1);
		blobmsg_add_u32(b, NULL, d == today && watched < 4 * 3600 ? 0 : speed);
		blobmsg_add_u32(b, NULL, d == today && watched < 4 * 3600 ? 0 : speed);
		blobmsg_add_u32(b, NULL, 0);
		blobmsg_add_u32(b, NULL, watched);
		blobmsg_add_u32(b, NULL, watched - psecs);
		blobmsg_add_u32(b, NULL, 0);            /* cleared by itself */
		blobmsg_close_array(b, row);
	}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

static int write_state(const char *path)
{
	struct blob_buf b = { 0 };
	int rc;

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "version", 1);
	blobmsg_add_u64(&b, "saved", (uint64_t)T.now);
	devices_json(&b);
	alerts_json(&b);
	decisions_json(&b);
	care_json(&b);
	sqm_json(&b);
	calls_json(&b);
	rc = write_json(path, &b);
	blob_buf_free(&b);
	return rc;
}

static int write_upgrade(const char *path)
{
	struct up_before ub;
	struct up_after ua;
	struct up_params p;
	struct up_verdict v;
	struct blob_buf b = { 0 };
	void *t, *arr, *row;
	long end = T.boot + 86400 + 60;
	int i, m, rc;

	memset(&ub, 0, sizeof(ub));
	memset(&ua, 0, sizeof(ua));
	up_params_default(&p);
	ub.at = T.up;
	snprintf(ub.from, sizeof(ub.from), "%s", FROM_VERSION);
	snprintf(ub.to, sizeof(ub.to), "%s", AG_VERSION);
	ub.from_image = 1;
	ub.to_image = 2;
	ub.online = 1;
	ub.watched_s = 7 * 86400;
	ub.ok_s = 7 * 86400 - 1800;
	ub.speed_mbps = LINE_MBPS - 0.4f;
	for (i = 0; i < ND; i++) {
		snprintf(ub.dev[i], sizeof(ub.dev[i]), "%s", D[i].mac);
		snprintf(ub.name[i], sizeof(ub.name[i]), "%s", D[i].host);
		ub.wifi[i] = D[i].wifi ? 1 : 0;
	}
	ub.ndev = ND;
	up_after_start(&ua, T.boot);
	for (i = 0; i < ND; i++)
		up_after_seen(&ub, &ua, D[i].mac, T.boot + 20 + 25 * i);
	ua.watched_s = 86400;
	ua.ok_s = 86400;
	ua.checks = 144 + 30;           /* every 2 min in the first hour, then every 10 */
	ua.online_at = T.boot + 15;
	ua.last_check = end - 60;
	for (m = 0; m < UP_METRICS; m++) {
		static const float meas[UP_METRICS] = { 1.4f, 12.3f, 0.0f, 5.1f };
		static const float norm[UP_METRICS] = { 1.5f, 12.6f, 0.1f, 5.4f };

		ua.n[m] = ua.checks;
		ua.v[m] = meas[m] * ua.checks;
		ua.norm[m] = norm[m] * ua.checks;
	}
	ua.speed_sum = LINE_MBPS;
	ua.speed_n = 1;
	up_judge(&ub, &ua, end, &p, &v);

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "version", 1);
	blobmsg_add_u32(&b, "state", (uint32_t)(v.result == UPV_KEEP ? UP_KEPT : UP_WATCHING));
	blobmsg_add_u64(&b, "ended", (uint64_t)end);
	blobmsg_add_u64(&b, "rb_at", 0);
	blobmsg_add_u32(&b, "confirmed", 0);
	blobmsg_add_string(&b, "note", "");
	blobmsg_add_string(&b, "held", "");
	blobmsg_add_u64(&b, "held_until", 0);
	blobmsg_add_string(&b, "held_why", "");
	arr = blobmsg_open_array(&b, "isp");
	blobmsg_close_array(&b, arr);
	blobmsg_add_u64(&b, "isp_at", 0);
	t = blobmsg_open_table(&b, "before");
	blobmsg_add_u64(&b, "at", (uint64_t)ub.at);
	blobmsg_add_string(&b, "from", ub.from);
	blobmsg_add_string(&b, "to", ub.to);
	blobmsg_add_u32(&b, "from_image", ub.from_image);
	blobmsg_add_u32(&b, "to_image", ub.to_image);
	blobmsg_add_u32(&b, "online", (uint32_t)(ub.online + 1));
	blobmsg_add_u32(&b, "watched_s", ub.watched_s);
	blobmsg_add_u32(&b, "ok_s", ub.ok_s);
	blobmsg_add_double(&b, "speed_mbps", ub.speed_mbps);
	arr = blobmsg_open_array(&b, "devices");
	for (i = 0; i < ub.ndev; i++) {
		row = blobmsg_open_array(&b, NULL);
		blobmsg_add_string(&b, NULL, ub.dev[i]);
		blobmsg_add_string(&b, NULL, ub.name[i]);
		blobmsg_add_u32(&b, NULL, ub.wifi[i]);
		blobmsg_add_u32(&b, NULL, ua.back[i]);
		blobmsg_add_u64(&b, NULL, (uint64_t)ua.back_at[i]);
		blobmsg_add_u32(&b, NULL, 0);
		blobmsg_close_array(&b, row);
	}
	blobmsg_close_array(&b, arr);
	blobmsg_close_table(&b, t);
	t = blobmsg_open_table(&b, "after");
	blobmsg_add_u64(&b, "booted", (uint64_t)ua.booted);
	blobmsg_add_u32(&b, "starts", (uint32_t)ua.starts);
	blobmsg_add_u64(&b, "last_check", (uint64_t)ua.last_check);
	blobmsg_add_u32(&b, "watched_s", ua.watched_s);
	blobmsg_add_u32(&b, "ok_s", ua.ok_s);
	blobmsg_add_u32(&b, "checks", ua.checks);
	blobmsg_add_u32(&b, "offline", ua.offline);
	blobmsg_add_u64(&b, "online_at", (uint64_t)ua.online_at);
	arr = blobmsg_open_array(&b, "metrics");
	for (m = 0; m < UP_METRICS; m++) {
		row = blobmsg_open_array(&b, NULL);
		blobmsg_add_double(&b, NULL, ua.v[m]);
		blobmsg_add_double(&b, NULL, ua.norm[m]);
		blobmsg_add_u32(&b, NULL, ua.n[m]);
		blobmsg_close_array(&b, row);
	}
	blobmsg_close_array(&b, arr);
	blobmsg_add_double(&b, "speed_sum", ua.speed_sum);
	blobmsg_add_u32(&b, "speed_n", ua.speed_n);
	blobmsg_close_table(&b, t);
	t = blobmsg_open_table(&b, "verdict");
	blobmsg_add_u32(&b, "result", (uint32_t)v.result);
	blobmsg_add_u32(&b, "why", v.why);
	blobmsg_add_u32(&b, "back", (uint32_t)v.back);
	blobmsg_add_u32(&b, "missing", (uint32_t)v.missing);
	blobmsg_add_u32(&b, "missing_wifi", (uint32_t)v.missing_wifi);
	blobmsg_add_u32(&b, "score_before", (uint32_t)(v.score_before + 1));
	blobmsg_add_u32(&b, "score_after", (uint32_t)(v.score_after + 1));
	blobmsg_add_string(&b, "text", v.text);
	blobmsg_close_table(&b, t);
	rc = write_json(path, &b);
	blob_buf_free(&b);
	return rc;
}

/* ---------- the times ---------- */

static void times(long now)
{
	struct tm tm;
	long midnight;

	memset(&T, 0, sizeof(T));
	T.now = now;
	T.last = now - now % STEP - STEP;
	T.first = T.last - (long)(NMIN - 1) * STEP;
	local_tm(now, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	midnight = (long)mktime(&tm);
	T.e = midnight - 4 * 3600;              /* yesterday 20:00 */
	T.tv0 = T.e - 10 * 60;
	T.tv1 = T.e + 85 * 60;
	T.big0 = T.e + 2 * 60;
	T.big1 = T.e + 85 * 60;
	T.prop = T.e + 12 * 60;
	T.on = T.e + 38 * 60;
	T.call1a = T.e + 5 * 60;
	T.call1b = T.e + 35 * 60;
	T.call2a = T.e + 46 * 60;
	T.call2b = T.e + 68 * 60;
	T.game0 = T.e - 86400 + 60 * 60;
	T.game1 = T.e - 86400 + 125 * 60;
	/* the upgrade: activated about 50 h ago, at a quiet hour (04:10), so it was watched for a day before now */
	T.up = T.e - 86400 + 8 * 3600 + 10 * 60;    /* the day before yesterday, 04:10 */
	T.up -= T.up % STEP;
	T.boot = T.up + 2 * STEP;
}

/* ---------- the gate ---------- */

static int in_qemu(char *why, size_t n)
{
	char v[64] = "";
	FILE *f = fopen(DMI_VENDOR, "r");

	if (f) {
		if (!fgets(v, sizeof(v), f))
			v[0] = '\0';
		fclose(f);
	}
	v[strcspn(v, "\r\n")] = '\0';
	if (!strcmp(v, "QEMU")) {
		snprintf(why, n, "a QEMU virtual machine");
		return 1;
	}
	snprintf(why, n, "not a QEMU virtual machine (%s: %s)", DMI_VENDOR, v[0] ? v : "none");
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: aigw-demo check\n"
		"       aigw-demo seed --state FILE --why FILE --upgrade FILE [--now SECONDS] [--replace] [--test]\n"
		"The simulated home of the AIGW OS demo image (docs/try-it.md). Only in a QEMU virtual machine.\n");
}

int main(int argc, char **argv)
{
	const char *state = NULL, *why = NULL, *upgrade = NULL;
	long now = (long)time(NULL);
	int replace = 0, test = 0, c;
	char w[200];
	static const struct option opts[] = {
		{ "state", required_argument, NULL, 's' }, { "why", required_argument, NULL, 'w' },
		{ "upgrade", required_argument, NULL, 'u' }, { "now", required_argument, NULL, 'n' },
		{ "replace", no_argument, NULL, 'r' }, { "test", no_argument, NULL, 't' }, { NULL, 0, NULL, 0 },
	};

	if (argc < 2) {
		usage();
		return 2;
	}
	if (!strcmp(argv[1], "check")) {
		int ok = in_qemu(w, sizeof(w));

		printf("%s\n", w);
		return ok ? 0 : 1;
	}
	if (strcmp(argv[1], "seed")) {
		usage();
		return 2;
	}
	optind = 2;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 's': state = optarg; break;
		case 'w': why = optarg; break;
		case 'u': upgrade = optarg; break;
		case 'n': now = strtol(optarg, NULL, 10); break;
		case 'r': replace = 1; break;
		case 't': test = 1; break;
		default: usage(); return 2;
		}
	}
	if (!state || !why || !upgrade) {
		usage();
		return 2;
	}
	if (!test && !in_qemu(w, sizeof(w))) {
		fprintf(stderr, "aigw-demo: %s: the simulated home is only for the demo image in QEMU\n", w);
		return 1;
	}
	if (!replace && !access(state, F_OK)) {
		fprintf(stderr, "aigw-demo: %s exists (a real home's state?): not replaced without --replace\n", state);
		return 1;
	}
	times(now);
	timeline();
	smart_queueing();
	story();
	if (write_why(why) || write_state(state) || write_upgrade(upgrade))
		return 1;
	printf("aigw-demo: a simulated home in %s, %s, %s: %d samples from %ld, %d events; smart queueing %s; "
	       "%d calls\n", state, why, upgrade, nring, T.first, nev,
	       Q.verdict == SQ_KEEP ? "kept" : Q.verdict == SQ_UNDO ? "tried" : "on trial", C.n);
	return 0;
}
