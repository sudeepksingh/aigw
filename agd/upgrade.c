/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Firmware upgrades that check themselves (the verdict: core/libag/upgrade.c,
 * ag/upgrade.h). A firmware handler knows whether the new image boots; agd
 * knows whether the home works as well on it as before:
 *
 *   activate   (USP Activate(), TR-069 Download, agctl): the home is recorded
 *              (the devices connected, the week's time without a problem, the
 *              nightly speed, the internet) in upgrade.file, which survives the
 *              upgrade (/etc/aigw is kept over sysupgrade)
 *   boot       agd starts on the new version: watching. Devices coming back,
 *              every proactive care check (every 2 min for the first hour) and
 *              speed test are counted against before and the home's normal
 *   worse      (devices that don't come back, no internet, restarts at once;
 *              slower, a lower speed or score at the end of upgrade.watch):
 *              the ISP is told (UpgradeChecked!), and with upgrade.rollback the
 *              previous image boots again, when the handler still has it (A/B
 *              flash). Back on it, agd checks whether what went wrong is right
 *              again: the cause confirmed. The version is held off for
 *              upgrade.hold_days, so an ACS that retries doesn't install it again
 *   no worse   kept, and the ISP is told too
 *   never ran  the gateway booted, but on the old version: NotBooted
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>

#include "ag/care.h"
#include "ag/upgrade.h"
#include "agd.h"

#define QUICK_S      3600               /* the first hour of a watch: checks every QUICK_CHECK_S */
#define QUICK_CHECK  120
#define SAVE_EVERY   60

static struct {
	int state;
	struct up_before b;
	struct up_after a;
	struct up_verdict v;
	long ended;                     /* the verdict (wall s) */
	long rb_at;                     /* running the previous image again since */
	uint8_t rb_back[UP_DEVS];       /* ... and the devices missing on the new one that came back */
	int confirmed;                  /* back on it, what was wrong is right: 1, 0 not, -1 not known */
	char note[600];
	char held[64];                  /* a version rolled back: not installed again before held_until */
	long held_until;
	char held_why[900];
	char isp[4][24];                /* results not told to the ISP yet (UpgradeChecked!), told at isp_at */
	int isp_n;
	long isp_at;
} U;
static struct up_params P;
static char path[300];
static int dirty, start_pending, at_start;
static long saved_at, version_at;

/* "45 s", "30 min", "24 h" */
static const char *dur(long s, char *buf, size_t n)
{
	if (s < 120)
		snprintf(buf, n, "%ld s", s);
	else if (s < 7200)
		snprintf(buf, n, "%ld min", s / 60);
	else
		snprintf(buf, n, "%ld h", s / 3600);
	return buf;
}

/* ---------- the record, kept over the upgrade ---------- */

static void file_path(void)
{
	char *slash;

	if (cfg.upgrade_file[0]) {
		snprintf(path, sizeof(path), "%s", cfg.upgrade_file);
		return;
	}
	path[0] = '\0';
	if (!cfg.state_file[0])
		return;
	snprintf(path, sizeof(path), "%s", cfg.state_file);
	slash = strrchr(path, '/');
	snprintf(slash ? slash + 1 : path, sizeof(path) - (size_t)(slash ? slash + 1 - path : 0), "agd-upgrade.json");
}

const char *upgrade_file(void)
{
	return path;
}

static void add_f(struct blob_buf *b, const char *name, double v)
{
	blobmsg_add_double(b, name, v);
}

static void save(void)
{
	struct blob_buf b = { 0 };
	char tmp[320], *json;
	void *t, *arr, *row;
	FILE *f;
	int i, m, ok = 0;

	if (!path[0])
		return;
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "version", 1);
	blobmsg_add_u32(&b, "state", (uint32_t)U.state);
	blobmsg_add_u64(&b, "ended", (uint64_t)U.ended);
	blobmsg_add_u64(&b, "rb_at", (uint64_t)U.rb_at);
	blobmsg_add_u32(&b, "confirmed", (uint32_t)(U.confirmed + 1));
	blobmsg_add_string(&b, "note", U.note);
	blobmsg_add_string(&b, "held", U.held);
	blobmsg_add_u64(&b, "held_until", (uint64_t)U.held_until);
	blobmsg_add_string(&b, "held_why", U.held_why);
	arr = blobmsg_open_array(&b, "isp");
	for (i = 0; i < U.isp_n; i++)
		blobmsg_add_string(&b, NULL, U.isp[i]);
	blobmsg_close_array(&b, arr);
	blobmsg_add_u64(&b, "isp_at", (uint64_t)U.isp_at);

	t = blobmsg_open_table(&b, "before");
	blobmsg_add_u64(&b, "at", (uint64_t)U.b.at);
	blobmsg_add_string(&b, "from", U.b.from);
	blobmsg_add_string(&b, "to", U.b.to);
	blobmsg_add_u32(&b, "from_image", U.b.from_image);
	blobmsg_add_u32(&b, "to_image", U.b.to_image);
	blobmsg_add_u32(&b, "online", (uint32_t)(U.b.online + 1));
	blobmsg_add_u32(&b, "watched_s", U.b.watched_s);
	blobmsg_add_u32(&b, "ok_s", U.b.ok_s);
	add_f(&b, "speed_mbps", U.b.speed_mbps);
	arr = blobmsg_open_array(&b, "devices");     /* [mac, name, wifi, back, back_at, back after the rollback] */
	for (i = 0; i < U.b.ndev && i < UP_DEVS; i++) {
		row = blobmsg_open_array(&b, NULL);
		blobmsg_add_string(&b, NULL, U.b.dev[i]);
		blobmsg_add_string(&b, NULL, U.b.name[i]);
		blobmsg_add_u32(&b, NULL, U.b.wifi[i]);
		blobmsg_add_u32(&b, NULL, U.a.back[i]);
		blobmsg_add_u64(&b, NULL, (uint64_t)U.a.back_at[i]);
		blobmsg_add_u32(&b, NULL, U.rb_back[i]);
		blobmsg_close_array(&b, row);
	}
	blobmsg_close_array(&b, arr);
	blobmsg_close_table(&b, t);

	t = blobmsg_open_table(&b, "after");
	blobmsg_add_u64(&b, "booted", (uint64_t)U.a.booted);
	blobmsg_add_u32(&b, "starts", (uint32_t)U.a.starts);
	blobmsg_add_u64(&b, "last_check", (uint64_t)U.a.last_check);
	blobmsg_add_u32(&b, "watched_s", U.a.watched_s);
	blobmsg_add_u32(&b, "ok_s", U.a.ok_s);
	blobmsg_add_u32(&b, "checks", U.a.checks);
	blobmsg_add_u32(&b, "offline", U.a.offline);
	blobmsg_add_u64(&b, "online_at", (uint64_t)U.a.online_at);
	arr = blobmsg_open_array(&b, "metrics");     /* [sum measured, sum normal, n] */
	for (m = 0; m < UP_METRICS; m++) {
		row = blobmsg_open_array(&b, NULL);
		add_f(&b, NULL, U.a.v[m]);
		add_f(&b, NULL, U.a.norm[m]);
		blobmsg_add_u32(&b, NULL, U.a.n[m]);
		blobmsg_close_array(&b, row);
	}
	blobmsg_close_array(&b, arr);
	add_f(&b, "speed_sum", U.a.speed_sum);
	blobmsg_add_u32(&b, "speed_n", U.a.speed_n);
	blobmsg_close_table(&b, t);

	t = blobmsg_open_table(&b, "verdict");
	blobmsg_add_u32(&b, "result", (uint32_t)U.v.result);
	blobmsg_add_u32(&b, "why", U.v.why);
	blobmsg_add_u32(&b, "back", (uint32_t)U.v.back);
	blobmsg_add_u32(&b, "missing", (uint32_t)U.v.missing);
	blobmsg_add_u32(&b, "missing_wifi", (uint32_t)U.v.missing_wifi);
	blobmsg_add_u32(&b, "score_before", (uint32_t)(U.v.score_before + 1));
	blobmsg_add_u32(&b, "score_after", (uint32_t)(U.v.score_after + 1));
	blobmsg_add_string(&b, "text", U.v.text);
	blobmsg_close_table(&b, t);

	json = blobmsg_format_json(b.head, true);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (json && (f = fopen(tmp, "w"))) {
		fputs(json, f);
		fflush(f);
		/* it must be on flash before the gateway reboots into the new image */
		if (fsync(fileno(f)) == 0 && fclose(f) == 0 && rename(tmp, path) == 0)
			ok = 1;
	}
	free(json);
	blob_buf_free(&b);
	if (!ok)
		AG_WARN("upgrade: could not save %s: %s", path, strerror(errno));
	dirty = 0;
	saved_at = (long)ag_wall_s();
}

static double jd(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	case BLOBMSG_TYPE_INT64: return (double)(int64_t)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	default: return 0;
	}
}

static void js(struct blob_attr *a, char *out, size_t n)
{
	snprintf(out, n, "%s", blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "");
}

#define IS(k) (!strcmp(blobmsg_name(cur), k))

static void load_devices(struct blob_attr *arr)
{
	struct blob_attr *row, *f;
	size_t r1, r2;
	int i = 0, k;

	blobmsg_for_each_attr(row, arr, r1) {
		if (i >= UP_DEVS)
			break;
		k = 0;
		blobmsg_for_each_attr(f, row, r2) {
			switch (k++) {
			case 0: js(f, U.b.dev[i], sizeof(U.b.dev[i])); break;
			case 1: js(f, U.b.name[i], sizeof(U.b.name[i])); break;
			case 2: U.b.wifi[i] = (uint8_t)jd(f); break;
			case 3: U.a.back[i] = (uint8_t)jd(f); break;
			case 4: U.a.back_at[i] = (long)jd(f); break;
			case 5: U.rb_back[i] = (uint8_t)jd(f); break;
			}
		}
		if (U.b.dev[i][0])
			i++;
	}
	U.b.ndev = i;
}

static void load_metrics(struct blob_attr *arr)
{
	struct blob_attr *row, *f;
	size_t r1, r2;
	int m = 0, k;

	blobmsg_for_each_attr(row, arr, r1) {
		if (m >= UP_METRICS)
			break;
		k = 0;
		blobmsg_for_each_attr(f, row, r2) {
			if (k == 0) U.a.v[m] = jd(f);
			else if (k == 1) U.a.norm[m] = jd(f);
			else if (k == 2) U.a.n[m] = (unsigned)jd(f);
			k++;
		}
		m++;
	}
}

static void load(void)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur, *sub;
	size_t rem, r2;

	memset(&U, 0, sizeof(U));
	U.confirmed = -1;
	U.v.score_before = U.v.score_after = -1;
	if (!path[0] || access(path, R_OK))
		return;
	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_file(&b, path)) {
		AG_WARN("upgrade: ignoring unreadable %s", path);
		blob_buf_free(&b);
		return;
	}
	blobmsg_for_each_attr(cur, b.head, rem) {
		if (IS("state")) U.state = (int)jd(cur);
		else if (IS("ended")) U.ended = (long)jd(cur);
		else if (IS("rb_at")) U.rb_at = (long)jd(cur);
		else if (IS("confirmed")) U.confirmed = (int)jd(cur) - 1;
		else if (IS("note")) js(cur, U.note, sizeof(U.note));
		else if (IS("held")) js(cur, U.held, sizeof(U.held));
		else if (IS("held_until")) U.held_until = (long)jd(cur);
		else if (IS("held_why")) js(cur, U.held_why, sizeof(U.held_why));
		else if (IS("isp")) {
			blobmsg_for_each_attr(sub, cur, r2)
				if (U.isp_n < 4)
					js(sub, U.isp[U.isp_n++], sizeof(U.isp[0]));
		}
		else if (IS("isp_at")) U.isp_at = (long)jd(cur);
		else if (IS("before")) {
			blobmsg_for_each_attr(sub, cur, r2) {
				const char *k = blobmsg_name(sub);

				if (!strcmp(k, "at")) U.b.at = (long)jd(sub);
				else if (!strcmp(k, "from")) js(sub, U.b.from, sizeof(U.b.from));
				else if (!strcmp(k, "to")) js(sub, U.b.to, sizeof(U.b.to));
				else if (!strcmp(k, "from_image")) U.b.from_image = (uint32_t)jd(sub);
				else if (!strcmp(k, "to_image")) U.b.to_image = (uint32_t)jd(sub);
				else if (!strcmp(k, "online")) U.b.online = (int)jd(sub) - 1;
				else if (!strcmp(k, "watched_s")) U.b.watched_s = (unsigned)jd(sub);
				else if (!strcmp(k, "ok_s")) U.b.ok_s = (unsigned)jd(sub);
				else if (!strcmp(k, "speed_mbps")) U.b.speed_mbps = (float)jd(sub);
				else if (!strcmp(k, "devices")) load_devices(sub);
			}
		} else if (IS("after")) {
			blobmsg_for_each_attr(sub, cur, r2) {
				const char *k = blobmsg_name(sub);

				if (!strcmp(k, "booted")) U.a.booted = (long)jd(sub);
				else if (!strcmp(k, "starts")) U.a.starts = (int)jd(sub);
				else if (!strcmp(k, "last_check")) U.a.last_check = (long)jd(sub);
				else if (!strcmp(k, "watched_s")) U.a.watched_s = (unsigned)jd(sub);
				else if (!strcmp(k, "ok_s")) U.a.ok_s = (unsigned)jd(sub);
				else if (!strcmp(k, "checks")) U.a.checks = (unsigned)jd(sub);
				else if (!strcmp(k, "offline")) U.a.offline = (unsigned)jd(sub);
				else if (!strcmp(k, "online_at")) U.a.online_at = (long)jd(sub);
				else if (!strcmp(k, "metrics")) load_metrics(sub);
				else if (!strcmp(k, "speed_sum")) U.a.speed_sum = jd(sub);
				else if (!strcmp(k, "speed_n")) U.a.speed_n = (unsigned)jd(sub);
			}
		} else if (IS("verdict")) {
			blobmsg_for_each_attr(sub, cur, r2) {
				const char *k = blobmsg_name(sub);

				if (!strcmp(k, "result")) U.v.result = (int)jd(sub);
				else if (!strcmp(k, "why")) U.v.why = (unsigned)jd(sub);
				else if (!strcmp(k, "back")) U.v.back = (int)jd(sub);
				else if (!strcmp(k, "missing")) U.v.missing = (int)jd(sub);
				else if (!strcmp(k, "missing_wifi")) U.v.missing_wifi = (int)jd(sub);
				else if (!strcmp(k, "score_before")) U.v.score_before = (int)jd(sub) - 1;
				else if (!strcmp(k, "score_after")) U.v.score_after = (int)jd(sub) - 1;
				else if (!strcmp(k, "text")) js(sub, U.v.text, sizeof(U.v.text));
			}
		}
	}
	blob_buf_free(&b);
}

/* ---------- telling ---------- */

static void tell(const char *event, const char *summary)
{
	struct blob_buf b = { 0 };
	char why[200];

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", event);
	blobmsg_add_string(&b, "from", U.b.from);
	blobmsg_add_string(&b, "to", U.b.to);
	blobmsg_add_string(&b, "state", up_state_name(U.state));
	up_why_names(U.v.why, why, sizeof(why));
	blobmsg_add_string(&b, "reasons", why);
	blobmsg_add_string(&b, "summary", summary ? summary : "");
	agd_emit(AG_EV_UPGRADE, &b);
	blob_buf_free(&b);
}

static void isp_now(void)
{
	char why[200], text[1600];
	int i;

	up_why_names(U.v.why, why, sizeof(why));
	snprintf(text, sizeof(text), "%s%s%s", U.v.text, U.note[0] && U.v.text[0] ? " " : "", U.note);
	for (i = 0; i < U.isp_n; i++)
		dm_on_upgrade(U.isp[i], U.b.from, U.b.to, why, text);
	U.isp_n = 0;
	U.isp_at = 0;
	save();
}

/* the result, for the ISP: Kept, RolledBack, Worse, NotBooted, RollbackConfirmed, RollbackNotConfirmed.
   Kept in the record until told: found at start, it waits for the protocol clients, and what
   follows soon after waits with it, in order */
static void verdict_to_isp(const char *result)
{
	long now = (long)ag_wall_s();

	if (U.isp_n == 4)
		isp_now();
	snprintf(U.isp[U.isp_n++], sizeof(U.isp[0]), "%s", result);
	if (U.isp_n == 1)
		U.isp_at = now + (at_start ? cfg.upgrade_report_delay : 0);
	if (now >= U.isp_at && agd_connected())
		isp_now();
	else
		save();
}

/* ---------- before ---------- */

static long booted_at(void)
{
	FILE *f = fopen("/proc/uptime", "r");
	double up = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0;
	fclose(f);
	return up > 0 ? (long)ag_wall_s() - (long)up : 0;
}

void upgrade_on_activated(uint32_t to_id, const char *to_ver, uint32_t from_id, const char *from_ver, const char *who)
{
	long now = (long)ag_wall_s();
	struct care_day d;
	char d1[24];
	int i;

	if (!cfg.upgrade_check || !path[0] || !strcmp(who, UPGRADE_WHO))
		return;
	if (to_id == from_id || !to_ver || !*to_ver || (from_ver && !strcmp(to_ver, from_ver)))
		return;
	memset(&U.b, 0, sizeof(U.b));
	memset(&U.a, 0, sizeof(U.a));
	memset(&U.v, 0, sizeof(U.v));
	memset(U.rb_back, 0, sizeof(U.rb_back));
	U.v.score_before = U.v.score_after = -1;
	U.b.at = now;
	snprintf(U.b.from, sizeof(U.b.from), "%s", from_ver ? from_ver : "");
	snprintf(U.b.to, sizeof(U.b.to), "%s", to_ver);
	U.b.from_image = from_id;
	U.b.to_image = to_id;
	U.b.online = care_online();
	/* the devices connected now: on Wi-Fi, or active in the last 10 minutes */
	for (i = 0; i < DEV_MAX && U.b.ndev < UP_DEVS; i++) {
		struct device *dv = &devs[i];
		int k = U.b.ndev;

		if (!dv->used || !dv->mac[0] || !(dv->wifi_connected || (dv->last_seen && now - dv->last_seen <= 600)))
			continue;
		snprintf(U.b.dev[k], sizeof(U.b.dev[k]), "%s", dv->mac);
		snprintf(U.b.name[k], sizeof(U.b.name[k]), "%s", dv->hostname);
		U.b.wifi[k] = dv->wifi_connected ? 1 : 0;
		U.b.ndev++;
	}
	if (care_report_of(7, &d) == 0) {
		U.b.watched_s = d.watched_secs;
		U.b.ok_s = d.ok_secs;
		if (d.speed_tests)
			U.b.speed_mbps = (float)d.speed_kbps_sum / (float)d.speed_tests / 1000.0f;
	}
	U.state = UP_ACTIVATED;
	U.ended = U.rb_at = 0;
	U.confirmed = -1;
	U.note[0] = '\0';
	AG_INFO("upgrade: %s -> %s activated (asked by %s): %d devices connected, %s; it will be checked for %s after "
		"the boot", U.b.from, U.b.to, who, U.b.ndev,
		U.b.online == 1 ? "the internet answers" : U.b.online == 0 ? "no internet" : "the internet not checked yet",
		dur(cfg.upgrade_watch, d1, sizeof(d1)));
	save();
	dev_state_save();               /* the week's care data, too, before the reboot */
	tell("activated", NULL);
}

void upgrade_on_activate_failed(const char *who)
{
	if (U.state != UP_ACTIVATED || !strcmp(who, UPGRADE_WHO))
		return;
	AG_INFO("upgrade: %s was not activated after all: nothing to check", U.b.to);
	U.state = UP_NONE;
	save();
}

int upgrade_refused(const char *version, char *err, size_t n)
{
	long now = (long)ag_wall_s();
	char until[32];
	struct tm tm;
	time_t t;

	if (!version || !*version || !U.held[0] || strcmp(version, U.held) || now >= U.held_until)
		return 0;
	t = (time_t)U.held_until;
	gmtime_r(&t, &tm);
	strftime(until, sizeof(until), "%Y-%m-%d %H:%M UTC", &tm);
	snprintf(err, n, "AIGW rolled %s back: %s It is not installed again before %s (agctl upgrade --forget, or "
		 "Device.%s.Upgrade.Forget())", U.held, U.held_why, until, cfg.mgmt_prefix[0] ? cfg.mgmt_prefix : "X_AIGW_");
	return 1;
}

int upgrade_forget(void)
{
	int had = U.held[0] != 0;

	if (had)
		AG_INFO("upgrade: %s may be installed again (the hold is lifted)", U.held);
	U.held[0] = '\0';
	U.held_until = 0;
	U.held_why[0] = '\0';
	save();
	return had;
}

/* ---------- the watch ---------- */

static void params(void)
{
	up_params_default(&P);
	P.watch_s = cfg.upgrade_watch;
	P.reconnect_s = cfg.upgrade_reconnect;
	if (P.offline_s > P.reconnect_s)
		P.offline_s = P.reconnect_s;
}

static void start_watch(long now)
{
	char d1[24], d2[24];

	up_after_start(&U.a, now);
	U.state = UP_WATCHING;
	AG_INFO("upgrade: running %s (was %s): checking it for %s -- %d devices to come back within %s",
		U.b.to, U.b.from, dur(P.watch_s, d1, sizeof(d1)), U.b.ndev, dur(P.reconnect_s, d2, sizeof(d2)));
	save();
	tell("watching", NULL);
	care_check_now();
}

static void rolled_back_done(void *ctx, int ok, const char *msg)
{
	(void)ctx;
	if (ok)
		return;
	U.state = UP_WORSE;
	snprintf(U.note, sizeof(U.note), "Going back to %s failed: %s", U.b.from, msg && *msg ? msg : "the firmware handler refused");
	AG_WARN("upgrade: %s", U.note);
	save();
	tell("worse", U.note);
	verdict_to_isp("Worse");
}

static void worse(long now)
{
	struct fw_image all[FW_MAX];
	char err[200];
	int i, n, id = -1;

	U.ended = now;
	AG_WARN("upgrade: %s is worse than %s: %s", U.b.to, U.b.from, U.v.text);
	if (!cfg.upgrade_rollback) {
		U.state = UP_WORSE;
		snprintf(U.note, sizeof(U.note), "Not rolled back: upgrade.rollback is off.");
	} else {
		n = fw_list_now(all, FW_MAX);       /* the kept copy's disk may have come up just now */
		for (i = 0; i < n; i++)
			if (!all[i].active && !strcmp(all[i].version, U.b.from) &&
			    (!strcmp(all[i].status, "Available") || !strcmp(all[i].status, "Active")))
				id = (int)all[i].id;
		if (id < 0) {
			char how[400];

			fw_rollback(how, sizeof(how));
			U.state = UP_WORSE;
			snprintf(U.note, sizeof(U.note), "There is no copy of %s on this gateway to go back to (%.300s): it "
				 "stays on %s. Better not to roll %s out further.", U.b.from,
				 how[0] ? how : "its firmware handler lists none", U.b.to, U.b.to);
		} else {
			U.state = UP_ROLLING_BACK;
			snprintf(U.held, sizeof(U.held), "%s", U.b.to);
			U.held_until = now + (long)cfg.upgrade_hold_days * 86400;
			snprintf(U.held_why, sizeof(U.held_why), "%s", U.v.text);
			snprintf(U.note, sizeof(U.note), "Rolled back to %s (image %d).", U.b.from, id);
			save();
			if (fw_activate((uint32_t)id, 1, UPGRADE_WHO, rolled_back_done, NULL, err, sizeof(err)) < 0) {
				U.state = UP_WORSE;
				snprintf(U.note, sizeof(U.note), "Going back to %s failed: %s", U.b.from, err);
			} else {
				AG_WARN("upgrade: rolling back to %s (image %d); %s is held off for %d days", U.b.from, id,
					U.b.to, cfg.upgrade_hold_days);
			}
		}
	}
	if (U.state == UP_WORSE)
		AG_WARN("upgrade: %s", U.note);
	save();
	tell(U.state == UP_ROLLING_BACK ? "rolling-back" : "worse", U.v.text);
	if (U.state == UP_WORSE)
		verdict_to_isp("Worse");
}

static void kept(long now)
{
	U.state = UP_KEPT;
	U.ended = now;
	AG_INFO("upgrade: %s kept: %s", U.b.to, U.v.text);
	save();
	tell("kept", U.v.text);
	verdict_to_isp("Kept");
}

static void rolled_back(long now)
{
	U.state = UP_ROLLED_BACK;
	U.rb_at = now;
	U.confirmed = -1;
	memset(U.rb_back, 0, sizeof(U.rb_back));
	AG_WARN("upgrade: back on %s; checking that what went wrong on %s is right again", U.b.from, U.b.to);
	save();
	tell("rolled-back", U.v.text);
	verdict_to_isp("RolledBack");
	care_check_now();
}

static void confirm(int yes, const char *text)
{
	U.confirmed = yes;
	snprintf(U.note + strlen(U.note), sizeof(U.note) - strlen(U.note), "%s%s", U.note[0] ? " " : "", text);
	AG_INFO("upgrade: %s", text);
	save();
	tell("confirmed", text);
	verdict_to_isp(yes ? "RollbackConfirmed" : "RollbackNotConfirmed");
}

/* back on the previous image: is what went wrong right again? */
static void check_confirmed(long now)
{
	int i, missing = 0, back = 0;
	char text[300];

	if (U.confirmed >= 0)
		return;
	if (U.v.why & UPW_DEVICES) {
		for (i = 0; i < U.b.ndev; i++) {
			struct device *dv;

			if (U.a.back[i])
				continue;
			missing++;
			if (!U.rb_back[i] && (dv = dev_find_mac(U.b.dev[i], 0)) && dv->last_seen >= U.rb_at)
				U.rb_back[i] = 1, dirty = 1;
			back += U.rb_back[i];
		}
		if (missing && (back == missing || now - U.rb_at >= P.reconnect_s)) {
			int yes = back * 2 >= missing;

			snprintf(text, sizeof(text), yes ? "Confirmed: back on %s, %d of the %d devices that stayed away "
				 "on %s came back." : "Not confirmed: back on %s, %d of the %d devices that stayed away on %s "
				 "came back: probably not the firmware.", U.b.from, back, missing, U.b.to);
			confirm(yes, text);
		}
		return;
	}
	if (U.v.why & UPW_INTERNET) {
		if (care_online() == 1 && care_now()->last_check >= U.rb_at) {
			snprintf(text, sizeof(text), "Confirmed: back on %s, the internet answers again.", U.b.from);
			confirm(1, text);
		} else if (now - U.rb_at >= 2 * P.offline_s) {
			snprintf(text, sizeof(text), "Not confirmed: back on %s, still no internet: probably not the "
				 "firmware (the line?).", U.b.from);
			confirm(0, text);
		}
	}
}

static void watch(long now)
{
	int i, r;

	for (i = 0; i < U.b.ndev; i++) {
		struct device *dv;

		if (U.a.back[i] || !(dv = dev_find_mac(U.b.dev[i], 0)) || dv->last_seen < U.a.booted)
			continue;
		if (up_after_seen(&U.b, &U.a, U.b.dev[i], now) >= 0) {
			AG_DEBUG("upgrade: %s (%s) is back, %lds after the boot", U.b.name[i][0] ? U.b.name[i] : U.b.dev[i],
				 U.b.dev[i], now - U.a.booted);
			dirty = 1;
		}
	}
	r = up_judge(&U.b, &U.a, now, &P, &U.v);
	if (r == UPV_WORSE)
		worse(now);
	else if (r == UPV_KEEP)
		kept(now);
}

/* what the firmware handler runs now (asked again at most every 10 s here) */
static const char *running_version(long now, int fresh)
{
	static char v[96];

	if (fresh || now - version_at >= 10 || !v[0]) {
		fw_version(v, sizeof(v));
		version_at = now;
	}
	return v;
}

/* where the record stands, against the version running now (at start, and while agd runs:
   a handler may switch images without stopping agd) */
static void follow_version(long now, int at_start)
{
	const char *v = running_version(now, at_start);
	long boot = booted_at();

	if (!v[0])
		return;
	if (U.state == UP_ACTIVATED) {
		if (!strcmp(v, U.b.to)) {
			start_watch(now);
		} else if (at_start && strcmp(v, U.b.from) && boot > U.b.at + 5) {
			/* rebooted, and not on the old version: the new one, whose image named it otherwise
			   (OpenWRT: the image's metadata against /etc/openwrt_release) */
			AG_INFO("upgrade: the new image calls itself %s (it said %s before): watching it as such", v, U.b.to);
			snprintf(U.b.to, sizeof(U.b.to), "%.63s", v);
			start_watch(now);
		} else if (at_start && !strcmp(v, U.b.from) && boot > U.b.at + 5) {
			U.state = UP_NOT_BOOTED;
			U.ended = now;
			snprintf(U.note, sizeof(U.note), "The gateway restarted, but on %s: %s did not start (the boot loader "
				 "went back, or the image was not written).", U.b.from, U.b.to);
			AG_WARN("upgrade: %s", U.note);
			save();
			tell("not-booted", U.note);
			verdict_to_isp("NotBooted");
		}
	} else if (U.state == UP_ROLLING_BACK) {
		if (!strcmp(v, U.b.from)) {
			rolled_back(now);
		} else if (at_start && !strcmp(v, U.b.to) && boot > U.ended + 5) {
			U.state = UP_WORSE;
			snprintf(U.note, sizeof(U.note), "Going back to %s did not work: the gateway restarted on %s.",
				 U.b.from, U.b.to);
			AG_WARN("upgrade: %s", U.note);
			save();
			tell("worse", U.note);
			verdict_to_isp("Worse");
		}
	} else if (U.state == UP_WATCHING && at_start) {
		if (!strcmp(v, U.b.to)) {
			U.a.starts++;
			AG_INFO("upgrade: still checking %s (start %d on it, %ld min since the boot)", U.b.to, U.a.starts,
				(now - U.a.booted) / 60);
			save();
		} else {
			AG_INFO("upgrade: the gateway runs %s now, not %s: the check of %s ends", v, U.b.to, U.b.to);
			U.state = UP_NONE;
			save();
		}
	}
}

void upgrade_init(void)
{
	char d1[24];

	params();
	file_path();
	load();
	if (!cfg.upgrade_check) {
		AG_INFO("upgrade: firmware upgrades are not checked (upgrade.check 0)");
		return;
	}
	if (!path[0]) {
		AG_INFO("upgrade: no state file: firmware upgrades are not checked");
		return;
	}
	/* where it stands is worked out on the first tick on ubus: what it finds is told then */
	start_pending = U.state == UP_ACTIVATED || U.state == UP_ROLLING_BACK || U.state == UP_WATCHING;
	AG_INFO("upgrade: firmware upgrades are checked for %s after the boot%s%s%s", dur(cfg.upgrade_watch, d1, sizeof(d1)),
		cfg.upgrade_rollback ? ", and rolled back when worse" : ", not rolled back (upgrade.rollback 0)",
		U.held[0] ? "; held off: " : "", U.held);
}

void upgrade_tick(void)
{
	long now = (long)ag_wall_s();

	if (!cfg.upgrade_check || !path[0] || !agd_connected())
		return;
	if (start_pending) {
		start_pending = 0;
		at_start = 1;
		follow_version(now, 1);
		at_start = 0;
	}
	if (U.isp_n && now >= U.isp_at)
		isp_now();
	switch (U.state) {
	case UP_ACTIVATED:
	case UP_ROLLING_BACK:
		follow_version(now, 0);
		break;
	case UP_WATCHING:
		watch(now);
		break;
	case UP_ROLLED_BACK:
		check_confirmed(now);
		break;
	}
	if (dirty && now - saved_at >= SAVE_EVERY)
		save();
}

/* care.c: one check */
void upgrade_on_check(const struct care_sample *s, const struct care_sample *normal, int online, int problem, long max_gap)
{
	if (U.state != UP_WATCHING)
		return;
	up_after_check(&U.a, (long)ag_wall_s(), problem, online, s, normal, max_gap);
	dirty = 1;
}

void upgrade_on_speed(float mbps)
{
	if (U.state != UP_WATCHING)
		return;
	up_after_speed(&U.a, mbps);
	dirty = 1;
}

/* care.c: check more often early in a watch (and just after a rollback) */
int upgrade_quick(void)
{
	long now = (long)ag_wall_s();

	if (U.state == UP_WATCHING && now - U.a.booted < QUICK_S)
		return QUICK_CHECK;
	if (U.state == UP_ROLLED_BACK && U.confirmed < 0 && now - U.rb_at < QUICK_S)
		return QUICK_CHECK;
	return 0;
}

/* ---------- agctl, the data model ---------- */

int upgrade_field(const char *f, char *buf, size_t n)
{
	char why[200];

	if (!strcmp(f, "Status")) snprintf(buf, n, "%s", up_state_name(U.state));
	else if (!strcmp(f, "FromVersion")) snprintf(buf, n, "%s", U.b.from);
	else if (!strcmp(f, "ToVersion")) snprintf(buf, n, "%s", U.b.to);
	else if (!strcmp(f, "Activated")) snprintf(buf, n, "%ld", U.state ? U.b.at : 0);
	else if (!strcmp(f, "Ended")) snprintf(buf, n, "%ld", U.state ? U.ended : 0);
	else if (!strcmp(f, "DevicesBefore")) snprintf(buf, n, "%d", U.state ? U.b.ndev : 0);
	else if (!strcmp(f, "DevicesBack")) {
		int i, k = 0;

		for (i = 0; i < U.b.ndev; i++)
			k += U.a.back[i];
		snprintf(buf, n, "%d", U.state >= UP_WATCHING ? k : 0);
	} else if (!strcmp(f, "Reasons")) {
		up_why_names(U.v.why, why, sizeof(why));
		snprintf(buf, n, "%s", why);
	} else if (!strcmp(f, "Summary")) {
		snprintf(buf, n, "%s%s%s", U.v.text, U.note[0] && U.v.text[0] ? " " : "", U.note);
	} else if (!strcmp(f, "ScoreBefore")) snprintf(buf, n, "%d", U.v.score_before);
	else if (!strcmp(f, "ScoreAfter")) snprintf(buf, n, "%d", U.v.score_after);
	else if (!strcmp(f, "RollbackConfirmed"))
		snprintf(buf, n, "%s", U.state != UP_ROLLED_BACK || U.confirmed < 0 ? "Unknown" : U.confirmed ? "Yes" : "No");
	else if (!strcmp(f, "HeldVersion")) snprintf(buf, n, "%s", U.held_until > (long)ag_wall_s() ? U.held : "");
	else if (!strcmp(f, "HeldUntil")) snprintf(buf, n, "%ld", U.held_until > (long)ag_wall_s() ? U.held_until : 0);
	else if (!strcmp(f, "RollbackMethod")) snprintf(buf, n, "%s", fw_rollback(NULL, 0));
	else if (!strcmp(f, "RollbackNote")) fw_rollback(buf, n);
	else return -1;
	return 0;
}

/* devices 0: without the devices' addresses and names (for plug-ins: cortex's staged rollouts) */
void upgrade_blob(struct blob_buf *b, int devices)
{
	long now = (long)ag_wall_s();
	char why[200], text[1600], how[400];
	void *arr, *t;
	int i, m;

	blobmsg_add_u8(b, "check", cfg.upgrade_check != 0);
	blobmsg_add_u8(b, "rollback", cfg.upgrade_rollback != 0);
	blobmsg_add_u32(b, "watch_s", (uint32_t)cfg.upgrade_watch);
	blobmsg_add_u32(b, "reconnect_s", (uint32_t)cfg.upgrade_reconnect);
	blobmsg_add_string(b, "state", up_state_name(U.state));
	blobmsg_add_string(b, "rollback_method", fw_rollback(how, sizeof(how)));
	blobmsg_add_string(b, "rollback_note", how);
	if (U.held[0] && U.held_until > now) {
		blobmsg_add_string(b, "held", U.held);
		blobmsg_add_u64(b, "held_until", (uint64_t)U.held_until);
		blobmsg_add_string(b, "held_why", U.held_why);
	}
	if (!U.state)
		return;
	blobmsg_add_string(b, "from", U.b.from);
	blobmsg_add_string(b, "to", U.b.to);
	blobmsg_add_u64(b, "activated", (uint64_t)U.b.at);
	if (U.a.booted)
		blobmsg_add_u64(b, "booted", (uint64_t)U.a.booted);
	if (U.state == UP_WATCHING) {
		blobmsg_add_u32(b, "watched_for_s", (uint32_t)(now - U.a.booted));
		blobmsg_add_u32(b, "left_s", (uint32_t)(now - U.a.booted < P.watch_s ? P.watch_s - (now - U.a.booted) : 0));
	}
	if (U.ended)
		blobmsg_add_u64(b, "ended", (uint64_t)U.ended);
	blobmsg_add_u32(b, "starts", (uint32_t)U.a.starts);
	blobmsg_add_u32(b, "devices_before", (uint32_t)U.b.ndev);
	arr = blobmsg_open_array(b, "devices");
	for (i = 0; devices && i < U.b.ndev; i++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "mac", U.b.dev[i]);
		blobmsg_add_string(b, "name", U.b.name[i]);
		blobmsg_add_u8(b, "wifi", U.b.wifi[i]);
		blobmsg_add_u8(b, "back", U.a.back[i]);
		if (U.a.back[i])
			blobmsg_add_u32(b, "back_after_s", (uint32_t)(U.a.back_at[i] - U.a.booted));
		if (U.state == UP_ROLLED_BACK && !U.a.back[i])
			blobmsg_add_u8(b, "back_after_rollback", U.rb_back[i]);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
	t = blobmsg_open_table(b, "checks");
	blobmsg_add_u32(b, "n", U.a.checks);
	blobmsg_add_u32(b, "without_internet", U.a.offline);
	blobmsg_add_u32(b, "watched_s", U.a.watched_s);
	blobmsg_add_u32(b, "ok_s", U.a.ok_s);
	for (m = 0; m < UP_METRICS; m++) {
		void *mt;

		if (!U.a.n[m])
			continue;
		mt = blobmsg_open_table(b, care_metric_name(m));
		blobmsg_add_double(b, "measured", U.a.v[m] / U.a.n[m]);
		blobmsg_add_double(b, "normal", U.a.norm[m] / U.a.n[m]);
		blobmsg_add_u32(b, "n", U.a.n[m]);
		blobmsg_close_table(b, mt);
	}
	if (U.a.speed_n) {
		blobmsg_add_double(b, "speed_mbps", U.a.speed_sum / U.a.speed_n);
	}
	blobmsg_close_table(b, t);
	t = blobmsg_open_table(b, "before");
	blobmsg_add_u32(b, "watched_s", U.b.watched_s);
	blobmsg_add_u32(b, "ok_s", U.b.ok_s);
	if (U.b.speed_mbps > 0)
		blobmsg_add_double(b, "speed_mbps", U.b.speed_mbps);
	blobmsg_add_string(b, "internet", U.b.online == 1 ? "yes" : U.b.online == 0 ? "no" : "unknown");
	blobmsg_close_table(b, t);
	up_why_names(U.v.why, why, sizeof(why));
	blobmsg_add_string(b, "reasons", why);
	snprintf(text, sizeof(text), "%s%s%s", U.v.text, U.note[0] && U.v.text[0] ? " " : "", U.note);
	blobmsg_add_string(b, "summary", text);
	blobmsg_add_u32(b, "devices_back", (uint32_t)U.v.back);
	blobmsg_add_u32(b, "devices_missing", (uint32_t)U.v.missing);
	if (U.v.score_before >= 0)
		blobmsg_add_u32(b, "score_before", (uint32_t)U.v.score_before);
	if (U.v.score_after >= 0)
		blobmsg_add_u32(b, "score_after", (uint32_t)U.v.score_after);
	if (U.state == UP_ROLLED_BACK)
		blobmsg_add_string(b, "confirmed", U.confirmed < 0 ? "unknown" : U.confirmed ? "yes" : "no");
}
