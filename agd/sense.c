/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Wi-Fi sensing (the model: core/libag/sense.c, ag/sense.h).
 *
 * Opt-in, per home: it runs only when sensing.enabled is 1 and
 * sensing.consent says who agreed and when (agctl sense on --i-agree). Off,
 * it reads nothing, keeps nothing in memory beyond an empty table, and saves
 * nothing; turning it off wipes what it learned, from memory and the state
 * file. The ISP can't turn it on: the data model shows it read-only.
 *
 * On, at each Wi-Fi poll (5 s) it takes each associated device's signal and
 * frame counter from hostapd, as agd does anyway; a vendor's CSI motion
 * scores only through sensing.csi_handler. Every sensing.window polls the
 * model judges movement per zone, someone home or not, and (with
 * sensing.checkin) whether today's activity is as usual.
 *
 * Events stay home (ubus ag.sense: agctl watch, local plug-ins such as Home
 * Assistant). The ISP gets PresenceChanged! and UnusualInactivity! only with
 * sensing.share_events, and then nothing finer: no zones, no devices.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>

#include "ag/sense.h"
#include "agd.h"

static struct sn_state S;
static struct sn_params SP;
static unsigned polls;
static uint32_t fake_rx;

static int on(void)
{
	return cfg.sense && cfg.sense_consent[0];
}

static void params(void)
{
	const char *l = getenv("AIGW_TEST_SENSE_LEARN");       /* the tests: learn in fewer windows and days */

	sn_params_default(&SP);
	SP.window = cfg.sense_window;
	SP.checkin_min = cfg.sense_checkin_min;
	if (l && atoi(l) > 0) {
		SP.learn = atoi(l);
		SP.learn_days = 1;
	}
}

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

/* where a device stays: sensing.zone '<MAC>=<zone>', else the home model's place, else the home */
static const char *zone_of(const char *mac)
{
	static char z[SN_NAME];
	const char *p = cfg.sense_zones, *e;
	size_t ml = strlen(mac);

	while (p && *p) {
		while (*p == ' ')
			p++;
		e = strchr(p, ' ');
		if (!strncasecmp(p, mac, ml) && p[ml] == '=') {
			size_t n = (size_t)((e ? e : p + strlen(p)) - (p + ml + 1));

			snprintf(z, sizeof(z), "%.*s", (int)(n < sizeof(z) ? n : sizeof(z) - 1), p + ml + 1);
			return z;
		}
		p = e;
	}
	p = home_zone_of(mac);
	return *p ? p : "home";
}

static int personal(const struct device *d)
{
	return strcasestr(cfg.sense_personal, d->mac) || !strcmp(d->type, "iPhone") || !strcmp(d->type, "Android") ||
	       !strcmp(d->type, "watch");
}

void sense_on_station(const struct device *d, long rx_packets)
{
	if (!on() || !d->wifi_signal || !d->mac[0])
		return;
	/* without a frame counter from the driver, every look counts as new */
	sn_look(&S, d->mac, d->wifi_band, sn_zone(&S, zone_of(d->mac)), personal(d), d->wifi_signal,
		rx_packets >= 0 ? (uint32_t)rx_packets : ++fake_rx, (long)ag_wall_s());
}

/* a vendor handler's CSI motion scores: lines "<MAC> <0-100>" */
static void csi(void)
{
	char out[2048], *line, *save;
	char *argv[] = { cfg.sense_csi, "motion", NULL };

	if (!cfg.sense_csi[0] || handler_run(cfg.sense_csi, argv, out, sizeof(out), 1000) != 0)
		return;
	for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char mac[18];
		int score;

		if (sscanf(line, "%17s %d", mac, &score) == 2)
			sn_csi(&S, mac, score);
	}
}

static void fmt_since(long t, char *buf, size_t n)
{
	struct tm tm;
	time_t tt = (time_t)t;

	if (t <= 0 || !gmtime_r(&tt, &tm))
		snprintf(buf, n, "0001-01-01T00:00:00Z");
	else
		strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void event(const char *what, const char *k1, const char *v1, const char *text)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, k1, v1);
	blobmsg_add_string(&b, "text", text);
	agd_emit(AG_EV_SENSE, &b);
	blob_buf_free(&b);
}

void sense_tick(void)
{
	char text[240], since[40], q[16], u[16];
	int hour, ev, today, usual;
	long day, now;

	if (!on())
		return;
	params();
	if (++polls % (unsigned)SP.window)
		return;
	now = (long)ag_wall_s();
	local_time(now, &hour, &day);
	csi();
	ev = sn_window(&S, now, hour, day, &SP);
	if (ev & SN_EV_PRESENCE) {
		snprintf(text, sizeof(text), "%s (%s)", S.home ? "Someone is home" : "Nobody seems to be home", S.why);
		event("presence", "presence", S.home ? "home" : "away", text);
		fmt_since(S.home_since, since, sizeof(since));
		if (cfg.sense_share)
			dm_on_presence(S.home ? "Home" : "Away", since, S.home ? "Activity" : "NoActivity");
		AG_INFO("sensing: %s", text);
	}
	if (!cfg.sense_checkin)
		return;
	if (ev & (SN_EV_INACTIVE | SN_EV_ACTIVE_AGAIN)) {
		int started = (ev & SN_EV_INACTIVE) != 0;

		sn_activity_minutes(&S, hour, &SP, &today, &usual);
		sn_activity_words(&S, hour, &SP, text, sizeof(text));
		if (!started)
			snprintf(text, sizeof(text), "Movement seen again after a quiet stretch.");
		event(started ? "inactive" : "active", "since", "", text);
		fmt_since(started ? S.inactive_since : now, since, sizeof(since));
		snprintf(q, sizeof(q), "%ld", (now - S.inactive_since) / 60);
		snprintf(u, sizeof(u), "%d", usual < 0 ? 0 : usual);
		if (cfg.sense_share)
			dm_on_inactivity(started ? "Started" : "Ended", since, q, u, text);
		AG_INFO("sensing: %s", text);
	}
}

/* ---------- on and off ---------- */

int sense_set(int want, const char *consent, char *err, size_t en)
{
	if (want && (!consent || !*consent || strlen(consent) >= sizeof(cfg.sense_consent) || strpbrk(consent, "\n'"))) {
		snprintf(err, en, "sensing needs the home's consent: agctl sense on --i-agree (see docs/wifi-sensing.md)");
		return -1;
	}
	if (ag_config_ensure("sensing") < 0 || ag_config_set("sensing", "enabled", want ? "1" : "0") < 0 ||
	    ag_config_set("sensing", "consent", want ? consent : "") < 0) {
		snprintf(err, en, "could not write the setting (/etc/config/aigw)");
		return -1;
	}
	cfg.sense = want;
	snprintf(cfg.sense_consent, sizeof(cfg.sense_consent), "%s", want ? consent : "");
	if (!want) {
		/* consent withdrawn: forget everything learned, here and in the state file */
		sn_init(&S);
		polls = 0;
		dev_state_save();
	}
	AG_INFO("sensing: %s", want ? "on (consent given)" : "off: everything it learned is forgotten");
	return 0;
}

void sense_init(void)
{
	params();
	if (!on()) {
		AG_INFO("sensing: off (opt-in: agctl sense on)");
		return;
	}
	AG_INFO("sensing: on, a window every %d s; check-in %s; events to the ISP %s", SP.window * SP.look_s,
		cfg.sense_checkin ? "on" : "off", cfg.sense_share ? "on (coarse only)" : "off");
}

/* ---------- agctl sense, the data model ---------- */

static const char *const sensed[] = {
	"each connected device's signal strength and frame count, as the access point already measures them",
	"per zone: whether something moves (from how the signals spread), now and when it last did",
	"someone home or not (movement, and whether people's own phones or watches are here)",
	"with the check-in: how many minutes of movement each hour of the day, today and on usual days",
	"not sensed: who it is, what they do, sound or images; nothing leaves the gateway unless share_events",
};

void sense_blob(struct blob_buf *b)
{
	char w[240], since[40];
	void *a, *t;
	long now = (long)ag_wall_s(), day;
	int k, hour, links = 0;

	blobmsg_add_u8(b, "enabled", (uint8_t)on());
	blobmsg_add_u8(b, "consent", (uint8_t)(cfg.sense_consent[0] != 0));
	a = blobmsg_open_array(b, "sensed");
	for (k = 0; k < (int)(sizeof(sensed) / sizeof(sensed[0])); k++)
		blobmsg_add_string(b, NULL, sensed[k]);
	blobmsg_close_array(b, a);
	if (!on())
		return;
	params();
	blobmsg_add_string(b, "agreed", cfg.sense_consent);
	blobmsg_add_u8(b, "share_events", (uint8_t)cfg.sense_share);
	blobmsg_add_u8(b, "checkin", (uint8_t)cfg.sense_checkin);
	blobmsg_add_u8(b, "csi", (uint8_t)(cfg.sense_csi[0] != 0));
	blobmsg_add_u32(b, "window_s", (uint32_t)(SP.window * SP.look_s));
	for (k = 0; k < SN_LINKS; k++)
		links += S.l[k].mac[0] != '\0';
	blobmsg_add_u32(b, "links", (uint32_t)links);
	blobmsg_add_u32(b, "windows", S.windows);
	blobmsg_add_string(b, "presence", S.home ? "home" : "away");
	blobmsg_add_u32(b, "home_pct", (uint32_t)(S.p_home * 100 + 0.5f));
	fmt_since(S.home_since, since, sizeof(since));
	blobmsg_add_string(b, "since", since);
	blobmsg_add_string(b, "reason", S.why);
	a = blobmsg_open_array(b, "zones");
	for (k = 0; k < S.nz; k++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "zone", S.z[k].name);
		blobmsg_add_u8(b, "moving", S.z[k].moving);
		if (S.z[k].last_motion)
			blobmsg_add_u32(b, "last_motion_s_ago", (uint32_t)(now - S.z[k].last_motion));
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	if (cfg.sense_checkin) {
		local_time(now, &hour, &day);
		blobmsg_add_string(b, "activity", sn_activity(&S, hour, &SP));
		sn_activity_words(&S, hour, &SP, w, sizeof(w));
		blobmsg_add_string(b, "activity_text", w);
		blobmsg_add_u32(b, "days_learned", S.days);
	}
}

int sense_field(const char *f, char *buf, size_t n)
{
	long now = (long)ag_wall_s(), day;
	int hour;

	if (!strcmp(f, "Enable")) snprintf(buf, n, "%s", on() ? "true" : "false");
	else if (!strcmp(f, "Status")) snprintf(buf, n, "%s", !on() ? "Off" : S.windows < (uint32_t)SP.learn ? "Learning" : "On");
	else if (!strcmp(f, "ShareEvents")) snprintf(buf, n, "%s", on() && cfg.sense_share ? "true" : "false");
	/* the rest only with share_events: someone home or not, and the check-in's verdict; never zones */
	else if (!strcmp(f, "Presence")) snprintf(buf, n, "%s", on() && cfg.sense_share ? (S.home ? "Home" : "Away") : "");
	else if (!strcmp(f, "PresenceSince")) {
		if (on() && cfg.sense_share)
			fmt_since(S.home_since, buf, n);
		else
			fmt_since(0, buf, n);
	} else if (!strcmp(f, "Activity")) {
		local_time(now, &hour, &day);
		snprintf(buf, n, "%s", on() && cfg.sense_share && cfg.sense_checkin ? sn_activity(&S, hour, &SP) : "");
	} else
		return -1;
	return 0;
}

/* ---------- state: only while on ---------- */

void sense_save(struct blob_buf *b)
{
	static char hex[2 * sizeof(struct sn_state) + 1];
	const uint8_t *p = (const uint8_t *)&S;
	void *t;
	size_t k;

	if (!on() || !S.windows)
		return;
	t = blobmsg_open_table(b, "sensing");
	blobmsg_add_u32(b, "layout", (uint32_t)sizeof(S));
	for (k = 0; k < sizeof(S); k++)
		snprintf(hex + 2 * k, 3, "%02x", p[k]);
	blobmsg_add_string(b, "state", hex);
	blobmsg_close_table(b, t);
}

void sense_load(struct blob_attr *t)
{
	struct blob_attr *c;
	uint32_t layout = 0;
	size_t rem, k;

	if (!on())
		return;                 /* off: what an older run left is dropped at the next save */
	blobmsg_for_each_attr(c, t, rem) {
		if (!strcmp(blobmsg_name(c), "layout") && blobmsg_type(c) == BLOBMSG_TYPE_INT32)
			layout = blobmsg_get_u32(c);
		else if (!strcmp(blobmsg_name(c), "state") && blobmsg_type(c) == BLOBMSG_TYPE_STRING && layout == sizeof(S)) {
			const char *h = blobmsg_get_string(c);
			struct sn_state x;
			uint8_t *p = (uint8_t *)&x;
			unsigned v;

			if (strlen(h) != 2 * sizeof(x))
				continue;
			for (k = 0; k < sizeof(x) && sscanf(h + 2 * k, "%2x", &v) == 1; k++)
				p[k] = (uint8_t)v;
			if (k == sizeof(x) && x.nz >= 0 && x.nz <= SN_ZONES) {
				for (k = 0; k < SN_LINKS; k++)
					x.l[k].mac[sizeof(x.l[k].mac) - 1] = '\0';
				for (k = 0; k < SN_ZONES; k++)
					x.z[k].name[SN_NAME - 1] = '\0';
				x.frozen = 0;
				S = x;
			}
		}
	}
}

/* ---------- agd --sense-bench: what sensing costs on this machine ---------- */

int sense_bench(void)
{
	static struct sn_state b;
	struct sn_params p;
	struct timespec t0, t1;
	uint32_t rx[16] = { 0 }, x = 2463534242u;
	long w, looks = 0, t = 1800000000L;
	char mac[18];
	double us;
	int k, l;

	sn_params_default(&p);
	sn_init(&b);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (w = 0; w < 2880; w++) {            /* a day of windows, 16 devices */
		for (l = 0; l < p.window; l++)
			for (k = 0; k < 16; k++) {
				x ^= x << 13, x ^= x >> 17, x ^= x << 5;
				snprintf(mac, sizeof(mac), "02:00:00:00:be:%02x", k);
				rx[k] += x & 1;
				sn_look(&b, mac, 5, sn_zone(&b, k < 8 ? "a" : "b"), k == 0, -50 - k - (int)(x % 5), rx[k],
					t + w * 30 + l * 5);
				looks++;
			}
		sn_window(&b, t + w * 30 + 30, (int)(w / 120), 20000, &p);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	us = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3) / (double)looks;
	/* at agd's rate (a look per device every 5 s), the share of one CPU 32 devices would take */
	printf("sense-bench: %.2f us a look, %zu bytes of state, %.4f%% of a CPU for 32 devices\n", us, sizeof(S),
	       us * 32 / 5 / 1e4);
	return 0;
}
