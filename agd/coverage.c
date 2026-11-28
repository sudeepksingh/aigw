/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW coverage placement advice (the rules: core/libag/coverage.c,
 * ag/coverage.h).
 *
 * Every coverage.interval seconds (60) agd samples each Wi-Fi device from what
 * it already measures:
 *   - the gateway's own access points (hostapd): the signal, the band, the
 *     rate to the device
 *   - the mesh, when there is one (agd/mesh.c, EasyMesh Data Elements): the
 *     access point it is on, its signal there, resends, and what the other
 *     access points hear of it
 *   - Self-Healing Wi-Fi's probes: the loss it feels
 * and the local hour. Drops and roams come from the Wi-Fi events. The rules
 * keep each device's history (its usual signal, its weak hours and spots),
 * group the spots of all devices into the home's, and say what to do.
 *
 * The advice replaces the generic "move it closer" line wherever it was shown:
 * Diagnose, the timeline, the help-desk brief, proactive tickets and their
 * customer notes, the data model (Device.X_AIGW_.Coverage.), agctl coverage.
 * The ISP sees device kinds unless brief.names; cortex gets the counts and the
 * verdict (a home that needs an extender is a sales lead), never a name.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/coverage.h"
#include "ag/why.h"
#include "agd.h"

static struct cov_dev D[COV_DEVS];
static struct cov_result R;
static struct cov_params P;
static char ears[COV_EARS][18];         /* the mesh's access points (AL MAC); 0 the gateway */
static int nears = 1;
static long judged_at;
static struct uloop_timeout timer;

static void params(void)
{
	cov_params_default(&P);
	P.weak_dbm = cfg.heal_weak;
	P.learn = cfg.cov_learn;
	P.spot_min = P.learn / 6 < 3 ? 3 : P.learn / 6 > 10 ? 10 : P.learn / 6;
	P.decay = (uint32_t)(7 * 86400 / (cfg.cov_interval > 0 ? cfg.cov_interval : 60));
}

/* local time; a test may set the hour (and day) in a file */
static void local_time(long now, int *hour, long *day)
{
	const char *f = getenv("AIGW_TEST_HOUR_FILE");
	time_t t = (time_t)now;
	struct tm tm;
	FILE *fp;

	localtime_r(&t, &tm);
	*hour = tm.tm_hour;
	*day = (now + tm.tm_gmtoff) / 86400;
	if (f && *f && (fp = fopen(f, "r"))) {
		int h = -1, k = fscanf(fp, "%d %ld", &h, day);

		if (k >= 1 && h >= 0 && h < 24)
			*hour = h;
		fclose(fp);
	}
}

static struct cov_dev *slot(const char *mac, int create)
{
	struct cov_dev *free_one = NULL, *oldest = NULL;
	int i;

	for (i = 0; i < COV_DEVS; i++) {
		if (!strcasecmp(D[i].mac, mac))
			return &D[i];
		if (!D[i].mac[0] && !free_one)
			free_one = &D[i];
		if (!oldest || D[i].last < oldest->last)
			oldest = &D[i];
	}
	if (!create)
		return NULL;
	if (!free_one)
		free_one = oldest;      /* the device not seen for longest gives way */
	memset(free_one, 0, sizeof(*free_one));
	snprintf(free_one->mac, sizeof(free_one->mac), "%s", mac);
	return free_one;
}

static int ear_of(const char *id)
{
	int e;

	if (!id || !*id)
		return 0;
	for (e = 1; e < nears; e++)
		if (!strcasecmp(ears[e], id))
			return e;
	if (nears >= COV_EARS)
		return -1;
	snprintf(ears[nears], sizeof(ears[0]), "%s", id);
	return nears++;
}

static void measured(const struct device *d, struct cov_sample *s)
{
	int loss, rtt, n;

	if (!d)
		return;
	heal_device(d, &loss, &rtt, &n);
	s->loss_pct = n > 0 ? loss : -1;
	if (!s->rate_mbps && d->wifi_rate_kbps)
		s->rate_mbps = (int)(d->wifi_rate_kbps / 1000);
}

static void sample(long now)
{
	static uint8_t done[DEV_MAX];
	struct mesh_seen ms;
	struct cov_sample s;
	struct cov_dev *c;
	int hour, i, j, k;
	long day;

	local_time(now, &hour, &day);
	memset(done, 0, sizeof(done));
	for (k = 0; mesh_seen_at(k, &ms) == 0; k++) {
		struct device *d = dev_find_mac(ms.mac, 0);

		memset(&s, 0, sizeof(s));
		s.t = now, s.hour = hour, s.day = day, s.loss_pct = -1;
		if ((s.ear = ear_of(ms.on)) < 0 || !ms.dbm)
			continue;
		s.band = ms.band;
		s.dbm = ms.dbm;
		s.retry_pct = ms.retry_pct;
		for (j = 0; j < ms.n && j < 4; j++) {
			int e = ear_of(ms.id[j]);

			if (e >= 0)
				s.heard[e] = ms.heard[j];
		}
		measured(d, &s);
		if (d)
			done[d - devs] = 1;
		home_on_look(ms.mac, &s, d && d->wifi_dual);
		if ((c = slot(ms.mac, 1)))
			cov_add(c, &s, &P);
	}
	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];

		if (!d->used || !d->wifi_connected || !d->wifi_signal || done[i] || !d->mac[0])
			continue;
		memset(&s, 0, sizeof(s));
		s.t = now, s.hour = hour, s.day = day;
		s.band = d->wifi_band;
		s.dbm = (int)(d->wifi_sig_avg < 0 ? d->wifi_sig_avg - 0.5 : d->wifi_signal);
		s.retry_pct = -1;
		measured(d, &s);
		home_on_look(d->mac, &s, d->wifi_dual);
		if ((c = slot(d->mac, 1)))
			cov_add(c, &s, &P);
	}
}

/* ---------- judging, words ---------- */

static int iot_cb(int i, void *ctx)
{
	const struct device *d = dev_find_mac(D[i].mac, 0);

	(void)ctx;
	return d && (strcasestr(d->type, "iot") || strcasestr(d->type, "smart") || !strcmp(d->zone, "iot"));
}

static const char *dev_cb(int i, void *ctx)
{
	const struct device *d = i >= 0 && i < COV_DEVS ? dev_find_mac(D[i].mac, 0) : NULL;

	(void)ctx;
	return d && d->hostname[0] ? d->hostname : i >= 0 && i < COV_DEVS ? D[i].mac : NULL;
}

static const char *ear_cb(int e, void *ctx)
{
	static char w[COV_EARS][64];

	(void)ctx;
	if (e <= 0 || e >= nears)
		return "the gateway";
	mesh_agent_words(ears[e], w[e], sizeof(w[e]));
	return w[e];
}

static struct cov_names NM = { dev_cb, ear_cb, NULL, 1 };

static void judge(void)
{
	params();
	cov_judge(D, COV_DEVS, &P, iot_cb, NULL, &R);
	NM.ears = nears;
	judged_at = (long)ag_wall_s();
}

static void fresh(void)
{
	if (!judged_at || (long)ag_wall_s() - judged_at >= (cfg.cov_interval > 0 ? cfg.cov_interval : 60))
		judge();
}

static int index_of(const char *mac)
{
	struct cov_dev *c = mac && *mac ? slot(mac, 0) : NULL;

	return c ? (int)(c - D) : -1;
}

int coverage_words(const char *mac, char *advice, size_t an, char *line, size_t ln, char *note, size_t nn)
{
	int i;

	if (advice && an)
		advice[0] = '\0';
	if (line && ln)
		line[0] = '\0';
	if (note && nn)
		note[0] = '\0';
	if (!cfg.cov)
		return -1;
	fresh();
	i = index_of(mac);
	if (mac && *mac && (i < 0 || R.d[i].kind == CK_LEARNING || R.d[i].kind == CK_FINE))
		i = -1;                 /* not known yet, or fine: the home's */
	if (advice)
		cov_advice(&R, i, &NM, advice, an);
	/* the home's advice: what the home model predicts the fixes would do (before you buy) */
	if (advice && i < 0 && advice[0] && (R.verdict == CV_EXTENDER || R.verdict == CV_MOVE_DEVICE)) {
		char h[600];
		size_t l = strlen(advice);

		home_advice_line(h, sizeof(h));
		if (*h && l + 20 < an)
			snprintf(advice + l, an - l, " The home model: %s", h);
	}
	if (line && (i >= 0 || R.worst >= 0))
		cov_dev_words(&R, i >= 0 ? i : R.worst, &NM, line, ln);
	if (note)
		cov_note(&R, i, note, nn);
	/* fine: a weak finding just now is news the history doesn't explain yet */
	return (advice && !advice[0]) || (R.verdict == CV_FINE && i < 0) ? -1 : R.verdict;
}

const char *coverage_advice(const char *code, const char *mac, int isp)
{
	static char buf[600];
	int i;

	if (!code || !cfg.cov || (strcmp(code, "WIFI_COVERAGE") && strcmp(code, "WIFI_DROPS")))
		return why_advice(code);
	fresh();
	i = index_of(mac);
	/* drops: only when the device is the one that is weak */
	if (!strcmp(code, "WIFI_DROPS") && (i < 0 || (R.d[i].kind != CK_SOMETIMES && R.d[i].kind != CK_ALWAYS)))
		return why_advice(code);
	if (coverage_words(i >= 0 ? mac : NULL, buf, sizeof(buf), NULL, 0, NULL, 0) < 0)
		return why_advice(code);
	if (!strcmp(code, "WIFI_DROPS")) {
		char w[900];

		snprintf(w, sizeof(w), "%s It drops when it is weak: %s", why_advice(code), buf);
		snprintf(buf, sizeof(buf), "%.599s", w);
	}
	if (isp)
		brief_scrub_isp(buf, sizeof(buf));
	return buf;
}

const char *coverage_kind_of(const char *mac)
{
	int i;

	if (!cfg.cov)
		return "";
	fresh();
	i = index_of(mac);
	return i < 0 ? "" : cov_kind_name(R.d[i].kind);
}

int coverage_usual_of(const char *mac)
{
	int i;

	if (!cfg.cov)
		return 0;
	fresh();
	i = index_of(mac);
	return i < 0 || !D[i].n ? 0 : R.d[i].usual;
}

static void spot_blob(struct blob_buf *b, int k, int isp)
{
	const struct cov_home_spot *h = &R.spot[k];
	char w[300];
	void *t = blobmsg_open_table(b, NULL);

	cov_spot_words(&R, k, &NM, w, sizeof(w));
	if (isp)
		brief_scrub_isp(w, sizeof(w));
	blobmsg_add_string(b, "where", w);
	blobmsg_add_string(b, "when", cov_part_words(h->when));
	blobmsg_add_u32(b, "devices", (uint32_t)h->ndev);
	blobmsg_add_u32(b, "suffering", (uint32_t)h->suffering);
	blobmsg_add_u8(b, "shared", (uint8_t)h->shared);
	blobmsg_add_u32(b, "samples", (uint32_t)h->n);
	blobmsg_add_u32(b, "signal", (uint32_t)h->best_dbm);
	if (nears > 1 && h->best_ear >= 0)
		blobmsg_add_string(b, "heard_best_by", ear_cb(h->best_ear, NULL));
	blobmsg_close_table(b, t);
}

/* names: devices by name (else by kind); list: each device's history (at home only: it has the MACs) */
static void blob(struct blob_buf *b, int names, int list)
{
	char w[600], note[400];
	void *a, *t;
	int i, k;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.cov);
	if (!cfg.cov)
		return;
	fresh();
	blobmsg_add_u8(b, "names", (uint8_t)names);  /* the texts name devices (else kinds: brief.names) */
	blobmsg_add_string(b, "verdict", cov_verdict_name(R.verdict));
	coverage_words(NULL, w, sizeof(w), NULL, 0, note, sizeof(note));
	if (!names)
		brief_scrub_isp(w, sizeof(w));
	blobmsg_add_string(b, "advice", w);
	blobmsg_add_string(b, "customer_note", note);
	blobmsg_add_u32(b, "devices", (uint32_t)R.devices);
	blobmsg_add_u32(b, "judged", (uint32_t)R.judged);
	blobmsg_add_u32(b, "weak_devices", (uint32_t)R.weak_devices);
	blobmsg_add_u32(b, "to_move", (uint32_t)R.movers);
	blobmsg_add_u32(b, "weak_transmitters", (uint32_t)R.weak_tx);
	blobmsg_add_u32(b, "shared_spots", (uint32_t)R.shared);
	blobmsg_add_u32(b, "access_points", (uint32_t)nears);
	blobmsg_add_u32(b, "interval_s", (uint32_t)cfg.cov_interval);
	blobmsg_add_u32(b, "weak_dbm", (uint32_t)cfg.heal_weak);
	a = blobmsg_open_array(b, "spots");
	for (k = 0; k < R.nspot; k++)
		spot_blob(b, k, !names);
	blobmsg_close_array(b, a);
	if (!list)
		return;
	a = blobmsg_open_array(b, "list");
	for (i = 0; i < COV_DEVS; i++) {
		const struct cov_sum *s = &R.d[i];
		char when[24] = "";

		if (!D[i].mac[0] || !D[i].n)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "mac", D[i].mac);
		blobmsg_add_string(b, "name", dev_cb(i, NULL));
		blobmsg_add_string(b, "kind", cov_kind_name(s->kind));
		blobmsg_add_u32(b, "samples", D[i].n);
		blobmsg_add_u32(b, "usual", (uint32_t)s->usual);
		blobmsg_add_u32(b, "low", (uint32_t)s->low);
		blobmsg_add_u32(b, "high", (uint32_t)s->high);
		blobmsg_add_u32(b, "weak_pct", (uint32_t)s->weak_pct);
		if (s->from_h >= 0) {
			snprintf(when, sizeof(when), "%02d:00-%02d:00", s->from_h, s->to_h);
			blobmsg_add_string(b, "hours", when);
		}
		blobmsg_add_u32(b, "days", (uint32_t)s->days);
		if (s->loss_weak >= 0)
			blobmsg_add_u32(b, "loss_weak_pct", (uint32_t)s->loss_weak);
		if (s->loss_ok >= 0)
			blobmsg_add_u32(b, "loss_ok_pct", (uint32_t)s->loss_ok);
		blobmsg_add_u32(b, "on_24ghz_pct", (uint32_t)s->on24_pct);
		blobmsg_add_u32(b, "drops", D[i].drops);
		blobmsg_add_u32(b, "roams", D[i].roams);
		if (s->spot >= 0)
			blobmsg_add_u32(b, "spot", (uint32_t)s->spot + 1);
		cov_dev_words(&R, i, &NM, w, sizeof(w));
		blobmsg_add_string(b, "words", w);
		cov_advice(&R, i, &NM, w, sizeof(w));
		blobmsg_add_string(b, "advice", w);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

void coverage_blob(struct blob_buf *b, int names)
{
	blob(b, names, names);
}

/* for the bridge's report to cortex: the verdict and counts, the spots without names (kinds unless brief.names) */
void coverage_report_blob(struct blob_buf *b)
{
	void *t;

	if (!cfg.cov || !cfg.cov_share)
		return;
	t = blobmsg_open_table(b, "coverage");
	blob(b, cfg.brief_names, 0);
	blobmsg_close_table(b, t);
}

/* ---------- the data model: Device.X_AIGW_.Coverage. ---------- */

int coverage_spot_ids(uint32_t *out, int max)
{
	int k;

	if (!cfg.cov)
		return 0;
	fresh();
	for (k = 0; k < R.nspot && k < max; k++)
		out[k] = (uint32_t)k + 1;
	return k;
}

int coverage_spot_field(uint32_t id, const char *f, char *buf, size_t n)
{
	const struct cov_home_spot *h;

	fresh();
	if (!id || (int)id > R.nspot)
		return -1;
	h = &R.spot[id - 1];
	if (!strcmp(f, "Where")) {
		cov_spot_words(&R, (int)id - 1, &NM, buf, n);
		brief_scrub_isp(buf, n);
	} else if (!strcmp(f, "When")) snprintf(buf, n, "%s", cov_part_words(h->when));
	else if (!strcmp(f, "Devices")) snprintf(buf, n, "%d", h->ndev);
	else if (!strcmp(f, "Suffering")) snprintf(buf, n, "%d", h->suffering);
	else if (!strcmp(f, "Shared")) snprintf(buf, n, "%s", h->shared ? "true" : "false");
	else if (!strcmp(f, "Signal")) snprintf(buf, n, "%d", h->best_dbm);
	else if (!strcmp(f, "HeardBestBy")) snprintf(buf, n, "%s", nears > 1 && h->best_ear >= 0 ? ear_cb(h->best_ear, NULL) : "");
	else
		return -1;
	return 0;
}

int coverage_field(const char *f, char *buf, size_t n)
{
	if (!strcmp(f, "Enable")) {
		snprintf(buf, n, "%s", cfg.cov ? "true" : "false");
		return 0;
	}
	fresh();
	if (!strcmp(f, "Status")) snprintf(buf, n, "%s", cfg.cov ? cov_verdict_name(R.verdict) : "Disabled");
	else if (!strcmp(f, "Advice") || !strcmp(f, "CustomerNote")) {
		char a[600], note[400];

		coverage_words(NULL, a, sizeof(a), NULL, 0, note, sizeof(note));
		brief_scrub_isp(a, sizeof(a));
		snprintf(buf, n, "%s", !strcmp(f, "Advice") ? a : note);
	} else if (!strcmp(f, "Devices")) snprintf(buf, n, "%d", R.judged);
	else if (!strcmp(f, "WeakDevices")) snprintf(buf, n, "%d", R.weak_devices);
	else if (!strcmp(f, "DevicesToMove")) snprintf(buf, n, "%d", R.movers);
	else if (!strcmp(f, "WeakTransmitters")) snprintf(buf, n, "%d", R.weak_tx);
	else if (!strcmp(f, "SharedSpots")) snprintf(buf, n, "%d", R.shared);
	else if (!strcmp(f, "SpotNumberOfEntries")) snprintf(buf, n, "%d", cfg.cov ? R.nspot : 0);
	else
		return -1;
	return 0;
}

/* ---------- events ---------- */

void coverage_on_wifi(struct blob_attr *msg)
{
	static const struct blobmsg_policy pol[2] = { { "event", BLOBMSG_TYPE_STRING }, { "mac", BLOBMSG_TYPE_STRING } };
	struct blob_attr *tb[2];
	struct cov_dev *c;

	if (!cfg.cov)
		return;
	blobmsg_parse(pol, 2, tb, blob_data(msg), blob_len(msg));
	if (!tb[0] || !tb[1] || !(c = slot(blobmsg_get_string(tb[1]), 0)))
		return;
	if (!strcmp(blobmsg_get_string(tb[0]), "disconnected"))
		c->drops++;
	else if (!strcmp(blobmsg_get_string(tb[0]), "roamed"))
		c->roams++;
}

static void on_timer(struct uloop_timeout *t)
{
	sample((long)ag_wall_s());
	judge();
	home_tick();                    /* the home model learns from the same looks */
	uloop_timeout_set(t, (cfg.cov_interval > 0 ? cfg.cov_interval : 60) * 1000);
}

void coverage_init(void)
{
	params();
	if (!cfg.cov) {
		AG_INFO("coverage advice: off (coverage.enabled)");
		return;
	}
	timer.cb = on_timer;
	uloop_timeout_set(&timer, (cfg.cov_interval > 0 ? cfg.cov_interval : 60) * 1000);
	AG_INFO("coverage advice: a look at each Wi-Fi device every %ds, weak at %d dBm, judged after %d looks",
		cfg.cov_interval, cfg.heal_weak, P.learn);
}

/* ---------- state ---------- */

/*
 * Each device's history as it is in memory, in hex: the state file stays on
 * this gateway, and a record of another layout (another version, another
 * architecture) is dropped and learned again.
 */
void coverage_save(struct blob_buf *b)
{
	static char hex[2 * sizeof(struct cov_dev) + 1];
	void *t = blobmsg_open_table(b, "coverage"), *a;
	int i, e;
	size_t k;

	blobmsg_add_u32(b, "layout", (uint32_t)sizeof(struct cov_dev));
	a = blobmsg_open_array(b, "ears");
	for (e = 1; e < nears; e++)
		blobmsg_add_string(b, NULL, ears[e]);
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "devices");
	for (i = 0; i < COV_DEVS; i++) {
		const uint8_t *p = (const uint8_t *)&D[i];

		if (!D[i].mac[0] || !D[i].n)
			continue;
		for (k = 0; k < sizeof(D[i]); k++)
			snprintf(hex + 2 * k, 3, "%02x", p[k]);
		blobmsg_add_string(b, NULL, hex);
	}
	blobmsg_close_array(b, a);
	blobmsg_close_table(b, t);
}

void coverage_load(struct blob_attr *t)
{
	struct blob_attr *c, *a;
	size_t rem, r2, k;
	uint32_t layout = 0;

	nears = 1;
	blobmsg_for_each_attr(c, t, rem) {
		const char *n = blobmsg_name(c);

		if (!strcmp(n, "layout") && blobmsg_type(c) == BLOBMSG_TYPE_INT32)
			layout = blobmsg_get_u32(c);
		else if (!strcmp(n, "ears") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(a, c, r2) {
				if (blobmsg_type(a) == BLOBMSG_TYPE_STRING && nears < COV_EARS)
					snprintf(ears[nears++], sizeof(ears[0]), "%s", blobmsg_get_string(a));
			}
		else if (!strcmp(n, "devices") && blobmsg_type(c) == BLOBMSG_TYPE_ARRAY && layout == sizeof(struct cov_dev))
			blobmsg_for_each_attr(a, c, r2) {
				struct cov_dev x, *d;
				const char *h = blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "";
				uint8_t *p = (uint8_t *)&x;
				unsigned v;

				if (strlen(h) != 2 * sizeof(x))
					continue;
				for (k = 0; k < sizeof(x) && sscanf(h + 2 * k, "%2x", &v) == 1; k++)
					p[k] = (uint8_t)v;
				x.mac[sizeof(x.mac) - 1] = '\0';
				if (k == sizeof(x) && x.mac[0] && (d = slot(x.mac, 1)))
					*d = x;
			}
	}
	if (layout && layout != sizeof(struct cov_dev))
		AG_INFO("coverage advice: the saved history is of another layout: learned again");
}
