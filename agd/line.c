/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Line health and area fault prediction, on the gateway (docs/area-prediction.md;
 * the rules: core/libag/line.c; the ISP's side: cortex/aigw_cortex/areapredict.py).
 *
 *   the WAN handler   every line.interval: the line's own numbers, as TR-181 has
 *     │               them (Device.DSL.Line, Device.Optical.Interface, a cable
 *     │               modem's downstream): sync rate, noise margin, optical
 *     │               level, FEC and CRC counters, retrains. A vendor-neutral
 *     │               program (line.handler, else /usr/libexec/aigw/line-stats)
 *     │               prints one JSON line; without one (Ethernet, a fibre ONT
 *     │               that says nothing) the WAN's carrier changes count drops
 *   proactive care    each check's next-hop delay and loss; the speed tests
 *     ▼
 *   a value a day     per metric; then, once a day, each metric's trend: steady,
 *                     stepped, drifting or a fault, with when it would be one
 *     ▼
 *   Device.X_AIGW_.LineHealth.  LineDegrading! / LineDegradingCleared!  agctl line
 *   the bridge        a compact line health vector for cortex (line.share): the
 *                     line's physics only, no devices, no addresses, no traffic
 *   cortex            compares the homes of one cabinet, OLT or area: one home's
 *                     line, or the shared plant? Its answer comes back with the
 *                     report ("line", capability line.notice) and is shown here
 *   offers            no faster plan on a degrading line: LINE_REPAIR instead
 *
 * Nothing here changes the home: it watches, predicts and tells.
 */
#define _GNU_SOURCE                     /* tm_gmtoff */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

#include "ag/line.h"
#include "agd.h"

#define SAMPLES  160                    /* a gauge's samples kept for the day's median */
#define MIN_CONF 40                     /* a trend this sure, or more, is the line's status */

static struct line_day days[LINE_METRICS][LINE_DAYS];   /* [m][day % LINE_DAYS] */
static struct {
	long day;                       /* the local day being counted */
	float s[LINE_METRICS][SAMPLES];
	int ns[LINE_METRICS], at[LINE_METRICS];
	double sum[LINE_METRICS], secs[LINE_METRICS];   /* counters: errors, and the time they were counted */
	int drops;
} T;
static struct {                         /* the handler's last counters, for the deltas */
	double fec, crc, resyncs, uptime, carrier;
	long at;
	int have;
} C;
static struct {
	char tech[12], error[120];
	long at;
	float now[LINE_METRICS];
	unsigned have;                  /* bits: metrics seen (ever, kept with the state) */
} H;
static struct line_trend tr[LINE_METRICS];
static long analysed;                   /* the day the trends are of */
static struct {
	int status, metric;             /* the line's: the worst metric */
	long since;                     /* the status started (wall s) */
} S;
static struct {                         /* cortex's answer: the shared plant */
	uint32_t plant;
	int homes, of, calls_in;
	char kind[12], conf[12], node[64], metric[16], ticket[80], words[300];
	long at, until;
} P;
static struct uloop_timeout timer;
static int running;

static long now_s(void)
{
	return (long)ag_wall_s();
}

static long local_day(long t)
{
	static long test_day = -1;      /* tests only: a "day" of this many seconds (AIGW_TEST_LINE_DAY_S) */
	time_t tt = (time_t)t;
	struct tm tm;

	if (test_day < 0) {
		const char *e = getenv("AIGW_TEST_LINE_DAY_S");

		test_day = e && atol(e) > 0 ? atol(e) : 0;
	}
	if (test_day)
		return t / test_day;
	localtime_r(&tt, &tm);
	return (t + tm.tm_gmtoff) / 86400;
}

static int gauge(int m)
{
	return m != LM_FEC && m != LM_CRC && m != LM_DROPS;
}

/* ---------- the days ---------- */

static float day_value(int m, int *n)
{
	if (m == LM_DROPS) {
		*n = 1;
		return (float)T.drops;
	}
	if (!gauge(m)) {
		*n = (int)(T.secs[m] / 3600);
		return T.secs[m] > 0 ? (float)(T.sum[m] * 3600 / T.secs[m]) : 0;
	}
	*n = T.ns[m];
	if (!T.ns[m])
		return 0;
	{
		float v[SAMPLES];
		int i, k = T.ns[m];

		memcpy(v, T.s[m], sizeof(float) * (size_t)k);
		for (i = 1; i < k; i++) {       /* insertion sort: a few hundred at most */
			float x = v[i];
			int j = i - 1;

			while (j >= 0 && v[j] > x) {
				v[j + 1] = v[j];
				j--;
			}
			v[j + 1] = x;
		}
		return k % 2 ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
	}
}

static void words_of(int m, char *buf, size_t n)
{
	line_words(m, &tr[m], buf, n);
}

static void analyse(long today);

/* the day is over: keep its values, count the next one */
static void roll(long today)
{
	int m, n;

	if (T.day && T.day != today) {
		for (m = 0; m < LINE_METRICS; m++) {
			struct line_day *d = &days[m][T.day % LINE_DAYS];
			float v = day_value(m, &n);

			if (!(H.have & (1u << m)) || (n <= 0 && m != LM_DROPS))
				continue;
			d->day = (int32_t)T.day;
			d->v = v;
			d->n = (uint16_t)(n > 65535 ? 65535 : n < 1 ? 1 : n);
		}
		memset(&T, 0, sizeof(T));
	}
	T.day = today;
	if (analysed != today)
		analyse(today);
}

static void add_gauge(int m, float v)
{
	long now = now_s();

	if (!isfinite(v))
		return;
	roll(local_day(now));
	T.s[m][T.at[m]] = v;
	T.at[m] = (T.at[m] + 1) % SAMPLES;
	if (T.ns[m] < SAMPLES)
		T.ns[m]++;
	H.have |= 1u << m;
	H.now[m] = v;
}

/* ---------- the status, events ---------- */

static int worse(int a, int b)          /* metric a is worse than b */
{
	if (tr[a].state != tr[b].state)
		return tr[a].state > tr[b].state;
	if ((tr[a].days_left >= 0) != (tr[b].days_left >= 0))
		return tr[a].days_left >= 0;
	if (tr[a].days_left != tr[b].days_left)
		return tr[a].days_left < tr[b].days_left;
	return tr[a].conf > tr[b].conf;
}

static void tell(const char *event)
{
	struct blob_buf b = { 0 };
	char text[300];

	words_of(S.metric, text, sizeof(text));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", event);
	blobmsg_add_string(&b, "metric", line_metric_name(S.metric));
	blobmsg_add_string(&b, "state", line_state_name(tr[S.metric].state));
	blobmsg_add_string(&b, "summary", text);
	agd_emit(AG_EV_CARE, &b);
	blob_buf_free(&b);
}

static void analyse(long today)
{
	struct line_params p;
	int m, worst = -1, status = LS_NONE, learning = 0, was = S.status, wm = S.metric;
	long now = now_s();
	char text[300];

	line_params_default(&p);
	if (cfg.care_speed_mbps && cfg.care_speed_min_pct)
		p.fault[LM_SPEED] = (float)cfg.care_speed_mbps * (float)cfg.care_speed_min_pct / 100.0f;
	for (m = 0; m < LINE_METRICS; m++) {
		line_analyse(days[m], LINE_DAYS, today, m, &p, &tr[m]);
		if (tr[m].state == LS_LEARNING)
			learning = 1;
		if (tr[m].state >= LS_STEP && tr[m].conf < MIN_CONF && tr[m].state != LS_FAULT)
			continue;
		if (tr[m].state >= LS_OK && (worst < 0 || worse(m, worst)))
			worst = m;
	}
	analysed = today;
	if (worst >= 0)
		status = tr[worst].state;
	else if (learning)
		status = LS_LEARNING;
	S.metric = worst >= 0 ? worst : 0;
	if (status >= LS_STEP && (was < LS_STEP || wm != S.metric)) {
		S.since = now;
		words_of(S.metric, text, sizeof(text));
		AG_WARN("line: %s (%s, %s confidence): %s", line_state_name(status), line_metric_name(S.metric),
			line_conf_name(tr[S.metric].conf), text);
		S.status = status;
		dm_on_line(1);
		tell("line-degrading");
	} else if (status < LS_STEP && was >= LS_STEP) {
		S.metric = wm;
		AG_INFO("line: %s is fine again after %lds", line_metric_name(wm), now - S.since);
		dm_on_line(0);
		tell("line-cleared");
		S.metric = worst >= 0 ? worst : 0;
		S.status = status;
		S.since = now;
	} else {
		S.status = status;
	}
}

/* ---------- what is measured ---------- */

void line_on_check(float gw_ms, float gw_loss_pct)
{
	if (!cfg.line)
		return;
	if (gw_ms >= 0)
		add_gauge(LM_LATENCY, gw_ms);
	if (gw_loss_pct >= 0)
		add_gauge(LM_LOSS, gw_loss_pct);
}

void line_on_speed(float mbps)
{
	if (cfg.line && mbps > 0)
		add_gauge(LM_SPEED, mbps);
}

static double num(struct blob_attr *a, int *ok)
{
	*ok = 1;
	switch (a ? blobmsg_type(a) : -1) {
	case BLOBMSG_TYPE_DOUBLE: return blobmsg_get_double(a);
	case BLOBMSG_TYPE_INT64: return (double)(int64_t)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (double)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (double)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (double)blobmsg_get_u8(a);
	}
	*ok = 0;
	return 0;
}

/* a counter's growth since the last look: errors in this period; a counter that went back: reset */
static void counter(int m, double v, double *last, double dt, int reset)
{
	if (C.have && !reset && v >= *last && dt > 0) {
		T.sum[m] += v - *last;
		T.secs[m] += dt;
		H.have |= 1u << m;
		H.now[m] = (float)((v - *last) * 3600 / dt);
	}
	*last = v;
}

/* the WAN's carrier changes (each drop is two): the default route's interface */
static double carrier_changes(void)
{
	char line[256], ifc[32], path[96];
	unsigned long dst, via, flags;
	double v = -1;
	FILE *f = fopen("/proc/net/route", "r");

	if (!f)
		return -1;
	ifc[0] = '\0';
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %lx %lx %lx", ifc, &dst, &via, &flags) == 4 && dst == 0 && (flags & 1))
			break;
		else
			ifc[0] = '\0';
	fclose(f);
	if (!ifc[0] || strchr(ifc, '/'))
		return -1;
	snprintf(path, sizeof(path), "/sys/class/net/%s/carrier_changes", ifc);
	if ((f = fopen(path, "r"))) {
		if (fscanf(f, "%lf", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/* one look at the line: the handler's JSON (NULL: no handler), and the WAN's carrier */
void line_take(const char *json, long now)
{
	static const struct { const char *key; int m; } G[] = {
		{ "down_kbps", LM_SYNC }, { "margin_db", LM_MARGIN }, { "rx_dbm", LM_OPTICAL },
	};
	struct blob_buf b = { 0 };
	struct blob_attr *tb[16], *cur;
	double v, dt = C.at ? (double)(now - C.at) : 0, cc;
	size_t rem;
	int ok, reset = 0, k = 0, i, had_resyncs = 0;
	double fec = -1, crc = -1, resyncs = -1, uptime = -1;

	roll(local_day(now));
	if (dt > 3.0 * cfg.line_interval + 60)
		dt = 0;                 /* agd was away: no rate over the gap */
	H.at = now;
	H.error[0] = '\0';
	blob_buf_init(&b, 0);
	if (json && *json && !blobmsg_add_json_from_string(&b, json)) {
		snprintf(H.error, sizeof(H.error), "the line handler's answer is not JSON");
		json = NULL;
	}
	if (json && *json) {
		blobmsg_for_each_attr(cur, b.head, rem) {
			const char *key = blobmsg_name(cur);

			if (!strcmp(key, "tech") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
				const char *t = blobmsg_get_string(cur);

				for (i = 0; t[i] && i < (int)sizeof(H.tech) - 1; i++)
					H.tech[i] = (char)tolower((unsigned char)t[i]);
				H.tech[i] = '\0';
			} else if (!strcmp(key, "error") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
				snprintf(H.error, sizeof(H.error), "%s", blobmsg_get_string(cur));
			} else if (k < 16) {
				tb[k++] = cur;
			}
		}
		for (i = 0; i < k; i++) {
			const char *key = blobmsg_name(tb[i]);
			size_t g;

			v = num(tb[i], &ok);
			if (!ok)
				continue;
			for (g = 0; g < sizeof(G) / sizeof(G[0]); g++)
				if (!strcmp(key, G[g].key) && (G[g].m != LM_SYNC || v > 0))
					add_gauge(G[g].m, (float)v);
			if (!strcmp(key, "fec"))
				fec = v;
			else if (!strcmp(key, "crc"))
				crc = v;
			else if (!strcmp(key, "resyncs"))
				resyncs = v;
			else if (!strcmp(key, "uptime_s"))
				uptime = v;
		}
	}
	/* a retrain: the handler's count grew, else the line's uptime went back */
	if (resyncs >= 0) {
		had_resyncs = 1;
		if (C.have && C.resyncs >= 0 && resyncs > C.resyncs)
			T.drops += (int)(resyncs - C.resyncs);
		reset = C.have && C.resyncs >= 0 && resyncs < C.resyncs;
		C.resyncs = resyncs;
	} else if (uptime >= 0) {
		if (C.have && C.uptime >= 0 && uptime + 30 < C.uptime + dt) {
			T.drops++;
			reset = 1;
		}
	}
	if (uptime >= 0)
		C.uptime = uptime;
	if (fec >= 0)
		counter(LM_FEC, fec, &C.fec, dt, reset);
	if (crc >= 0)
		counter(LM_CRC, crc, &C.crc, dt, reset);
	/* with no line numbers of its own (Ethernet, an ONT that says nothing): the carrier's drops */
	cc = carrier_changes();
	if (!had_resyncs && uptime < 0 && cc >= 0) {
		if (C.have && C.carrier >= 0 && cc > C.carrier)
			T.drops += (int)((cc - C.carrier + 1) / 2);
		if (!H.tech[0])
			snprintf(H.tech, sizeof(H.tech), "ethernet");
	}
	C.carrier = cc;
	if (had_resyncs || uptime >= 0 || cc >= 0)
		H.have |= 1u << LM_DROPS;
	C.at = now;
	C.have = 1;
	blob_buf_free(&b);
}

static void handler_done(void *ctx, int code, const char *msg)
{
	(void)ctx;
	running = 0;
	if (code) {
		line_take(NULL, now_s());
		snprintf(H.error, sizeof(H.error), "line handler: exit %d%s%.80s", code, msg && *msg ? ": " : "",
			 msg ? msg : "");
		return;
	}
	line_take(msg, now_s());
}

static void look(struct uloop_timeout *t)
{
	const char *prog = handler_path(cfg.line_handler, "line-stats");

	(void)t;
	uloop_timeout_set(&timer, cfg.line_interval * 1000);
	if (!cfg.line || running)
		return;
	if (prog) {
		char *argv[] = { (char *)prog, "stats", NULL };

		if (handler_start(prog, argv, NULL, handler_done, NULL) == 0) {
			running = 1;
			return;
		}
		snprintf(H.error, sizeof(H.error), "cannot run %s", prog);
	}
	line_take(NULL, now_s());       /* no handler: the carrier only */
}

void line_init(void)
{
	timer.cb = look;
	uloop_timeout_set(&timer, 5000);
}

int line_look_now(void)
{
	if (!cfg.line)
		return -1;
	uloop_timeout_set(&timer, 10);
	return 0;
}

/* ---------- what cortex says: the shared plant ---------- */

static void clean(char *dst, size_t n, const char *src)
{
	size_t i, k = 0;

	for (i = 0; src && src[i] && k + 1 < n; i++)
		if (isprint((unsigned char)src[i]) && !strchr("\"\\;", src[i]))
			dst[k++] = src[i];
	dst[k] = '\0';
}

int line_notice(struct blob_attr *a, char *err, size_t en)
{
	struct blob_attr *cur;
	size_t rem;
	long plant = -1, hold = 86400;
	int ok, was = P.plant != 0;

	memset(&P, 0, sizeof(P));
	P.calls_in = -1;
	blobmsg_for_each_attr(cur, a, rem) {
		const char *k = blobmsg_name(cur);
		double v = num(cur, &ok);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			const char *s = blobmsg_get_string(cur);

			if (!strcmp(k, "kind")) clean(P.kind, sizeof(P.kind), s);
			else if (!strcmp(k, "confidence")) clean(P.conf, sizeof(P.conf), s);
			else if (!strcmp(k, "node")) clean(P.node, sizeof(P.node), s);
			else if (!strcmp(k, "metric")) clean(P.metric, sizeof(P.metric), s);
			else if (!strcmp(k, "ticket")) clean(P.ticket, sizeof(P.ticket), s);
			else if (!strcmp(k, "words")) clean(P.words, sizeof(P.words), s);
		} else if (ok) {
			if (!strcmp(k, "plant")) plant = (long)v;
			else if (!strcmp(k, "homes")) P.homes = (int)v;
			else if (!strcmp(k, "of")) P.of = (int)v;
			else if (!strcmp(k, "calls_in")) P.calls_in = (int)v;
			else if (!strcmp(k, "hold_s")) hold = (long)v;
		}
	}
	if (plant < 0 || plant > 0x7fffffffL) {
		memset(&P, 0, sizeof(P));
		snprintf(err, en, "a line notice needs its plant (0: none)");
		return -1;
	}
	if (!plant) {
		if (was)
			AG_INFO("line: cortex no longer sees a fault in the shared plant");
		memset(&P, 0, sizeof(P));
		return 0;
	}
	P.plant = (uint32_t)plant;
	P.at = now_s();
	P.until = P.at + (hold < 600 ? 600 : hold > 172800 ? 172800 : hold);
	if (!was)
		AG_WARN("line: cortex: %s", P.words[0] ? P.words : "the shared plant serving this home is degrading");
	return 0;
}

static int plant_on(void)
{
	if (P.plant && now_s() >= P.until)
		memset(&P, 0, sizeof(P));
	return P.plant != 0;
}

/* ---------- for others ---------- */

int line_degrading(char *words, size_t n)
{
	if (!cfg.line || S.status < LS_STEP || tr[S.metric].state < LS_STEP)
		return 0;
	if (words)
		words_of(S.metric, words, n);
	return tr[S.metric].conf >= MIN_CONF || tr[S.metric].state == LS_FAULT;
}

/* ---------- views ---------- */

static double r3(double v)              /* three significant digits: enough, and nothing more */
{
	double p;

	if (!isfinite(v) || v == 0)
		return v;
	p = pow(10, floor(log10(fabs(v))) - 2);
	return round(v / p) * p;
}

static void add_num(struct blob_buf *b, const char *k, double v)
{
	if (isfinite(v))
		blobmsg_add_double(b, k, r3(v));
}

static void trend_blob(struct blob_buf *b, int m, int isp)
{
	const struct line_trend *t = &tr[m];
	char text[300];

	blobmsg_add_string(b, "metric", line_metric_key(m));
	blobmsg_add_string(b, "state", line_state_name(t->state));
	blobmsg_add_u32(b, "days", (uint32_t)t->ndays);
	add_num(b, "level", t->level);
	add_num(b, "base", t->base);
	add_num(b, "slope", t->slope);
	add_num(b, "fault", t->fault);
	blobmsg_add_u32(b, "onset", (uint32_t)t->onset);
	blobmsg_add_u32(b, "days_left", (uint32_t)t->days_left);
	blobmsg_add_u32(b, "conf", (uint32_t)t->conf);
	if (t->ndays >= 7) {
		add_num(b, "tau7", t->tau7);
		add_num(b, "slope7", t->slope7);
	}
	if (!isp) {
		blobmsg_add_string(b, "name", line_metric_name(m));
		blobmsg_add_string(b, "unit", line_metric_unit(m));
		if (H.have & (1u << m))
			add_num(b, "now", H.now[m]);
		words_of(m, text, sizeof(text));
		blobmsg_add_string(b, "words", text);
	}
}

/* agctl line (isp 0), and the bridge's line health vector for cortex (isp 1) */
void line_blob(struct blob_buf *b, int isp)
{
	void *a, *t;
	char text[300];
	int m;

	roll(local_day(now_s()));
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.line);
	if (isp && !cfg.line_share)
		return;
	blobmsg_add_u32(b, "v", 1);
	blobmsg_add_string(b, "tech", H.tech[0] ? H.tech : "unknown");
	blobmsg_add_u32(b, "day", (uint32_t)analysed);
	blobmsg_add_string(b, "status", line_state_name(S.status));
	blobmsg_add_string(b, "worst", S.status >= LS_STEP ? line_metric_key(S.metric) : "");
	blobmsg_add_u32(b, "days_left", (uint32_t)(S.status >= LS_STEP ? tr[S.metric].days_left : -1));
	blobmsg_add_u32(b, "conf", (uint32_t)(S.status >= LS_STEP ? tr[S.metric].conf : 0));
	if (!isp) {
		blobmsg_add_u8(b, "shared", (uint8_t)cfg.line_share);
		blobmsg_add_u64(b, "since", (uint64_t)(S.status >= LS_STEP ? S.since : 0));
		blobmsg_add_u64(b, "looked", (uint64_t)H.at);
		blobmsg_add_string(b, "error", H.error);
		{
			const char *h = handler_path(cfg.line_handler, "line-stats");

			blobmsg_add_string(b, "handler", h ? h : "");
		}
		if (S.status >= LS_STEP)
			words_of(S.metric, text, sizeof(text));
		else
			snprintf(text, sizeof(text), "%s", S.status == LS_OK ? "The line is steady." :
				 "AIGW is learning the line (5 days of numbers first).");
		blobmsg_add_string(b, "summary", text);
		if (plant_on()) {
			t = blobmsg_open_table(b, "plant");
			blobmsg_add_u32(b, "id", P.plant);
			blobmsg_add_string(b, "node", P.node);
			blobmsg_add_string(b, "kind", P.kind);
			blobmsg_add_u32(b, "homes", (uint32_t)P.homes);
			blobmsg_add_u32(b, "of", (uint32_t)P.of);
			blobmsg_add_string(b, "metric", P.metric);
			blobmsg_add_u32(b, "calls_in", (uint32_t)P.calls_in);
			blobmsg_add_string(b, "confidence", P.conf);
			blobmsg_add_string(b, "ticket", P.ticket);
			blobmsg_add_string(b, "words", P.words);
			blobmsg_add_u64(b, "told", (uint64_t)P.at);
			blobmsg_close_table(b, t);
		}
	}
	a = blobmsg_open_array(b, "metrics");
	for (m = 0; m < LINE_METRICS; m++) {
		if (!(H.have & (1u << m)) && !tr[m].ndays)
			continue;
		t = blobmsg_open_table(b, NULL);
		trend_blob(b, m, isp);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

/* Device.X_AIGW_.LineHealth.: 0, or -1 for a field it doesn't know */
int line_field(const char *f, char *buf, size_t n)
{
	int on = S.status >= LS_STEP, sh = plant_on();
	const struct line_trend *t = &tr[S.metric];

	if (!strcmp(f, "Enable")) snprintf(buf, n, "%s", cfg.line ? "true" : "false");
	else if (!strcmp(f, "Share")) snprintf(buf, n, "%s", cfg.line_share ? "true" : "false");
	else if (!strcmp(f, "Technology")) snprintf(buf, n, "%s", H.tech[0] ? H.tech : "unknown");
	else if (!strcmp(f, "Status")) snprintf(buf, n, "%s", S.status ? line_state_name(S.status) : "Learning");
	else if (!strcmp(f, "WorstMetric")) snprintf(buf, n, "%s", on ? line_metric_name(S.metric) : "");
	else if (!strcmp(f, "Summary")) {
		if (on)
			words_of(S.metric, buf, n);
		else
			snprintf(buf, n, "%s", S.status == LS_OK ? "The line is steady." : "Learning the line.");
	} else if (!strcmp(f, "DaysToFault")) snprintf(buf, n, "%d", on ? t->days_left : -1);
	else if (!strcmp(f, "Confidence")) snprintf(buf, n, "%d", on ? t->conf : 0);
	else if (!strcmp(f, "Since")) snprintf(buf, n, "%ld", on ? S.since : 0);
	else if (!strcmp(f, "LastLook")) snprintf(buf, n, "%ld", H.at);
	else if (!strcmp(f, "Error")) snprintf(buf, n, "%s", H.error);
	else if (!strcmp(f, "SharedPlant")) snprintf(buf, n, "%s", sh ? P.node : "");
	else if (!strcmp(f, "SharedPlantHomes")) snprintf(buf, n, "%d", sh ? P.homes : 0);
	else if (!strcmp(f, "SharedPlantDaysToCalls")) snprintf(buf, n, "%d", sh ? P.calls_in : -1);
	else if (!strcmp(f, "SharedPlantConfidence")) snprintf(buf, n, "%s", sh ? P.conf : "");
	else if (!strcmp(f, "SharedPlantTicket")) snprintf(buf, n, "%s", sh ? P.ticket : "");
	else if (!strcmp(f, "SharedPlantSummary")) snprintf(buf, n, "%s", sh ? P.words : "");
	else if (!strcmp(f, "MetricNumberOfEntries")) {
		uint32_t ids[LINE_METRICS];

		snprintf(buf, n, "%d", line_metric_ids(ids, LINE_METRICS));
	} else
		return -1;
	return 0;
}

int line_metric_ids(uint32_t *out, int max)
{
	int m, k = 0;

	for (m = 0; m < LINE_METRICS && k < max; m++)
		if ((H.have & (1u << m)) || tr[m].ndays)
			out[k++] = (uint32_t)m + 1;
	return k;
}

int line_metric_field(uint32_t inst, const char *f, char *buf, size_t n)
{
	int m = (int)inst - 1;
	const struct line_trend *t;

	if (m < 0 || m >= LINE_METRICS || (!(H.have & (1u << m)) && !tr[m].ndays))
		return -1;
	t = &tr[m];
	if (!strcmp(f, "Name")) snprintf(buf, n, "%s", line_metric_name(m));
	else if (!strcmp(f, "Unit")) snprintf(buf, n, "%s", line_metric_unit(m));
	else if (!strcmp(f, "State")) snprintf(buf, n, "%s", t->state ? line_state_name(t->state) : "Learning");
	else if (!strcmp(f, "Now")) snprintf(buf, n, "%g", r3(H.now[m]));
	else if (!strcmp(f, "Level")) snprintf(buf, n, "%g", r3(t->level));
	else if (!strcmp(f, "Base")) snprintf(buf, n, "%g", r3(t->base));
	else if (!strcmp(f, "SlopePerDay")) snprintf(buf, n, "%g", r3(t->slope));
	else if (!strcmp(f, "FaultLevel")) snprintf(buf, n, "%g", isfinite(t->fault) ? r3(t->fault) : 0.0);
	else if (!strcmp(f, "Onset")) snprintf(buf, n, "%ld", t->onset ? (long)t->onset * 86400 : 0);
	else if (!strcmp(f, "DaysToFault")) snprintf(buf, n, "%d", t->days_left);
	else if (!strcmp(f, "Confidence")) snprintf(buf, n, "%d", t->conf);
	else if (!strcmp(f, "Days")) snprintf(buf, n, "%d", t->ndays);
	else if (!strcmp(f, "Summary")) words_of(m, buf, n);
	else
		return -1;
	return 0;
}

/* ---------- saved with the state ---------- */

void line_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "line"), *arr, *row;
	int m, k;

	blobmsg_add_u32(b, "have", H.have);
	blobmsg_add_string(b, "tech", H.tech);
	blobmsg_add_u32(b, "status", (uint32_t)S.status);
	blobmsg_add_u32(b, "metric", (uint32_t)S.metric);
	blobmsg_add_u64(b, "since", (uint64_t)S.since);
	arr = blobmsg_open_array(b, "days");
	for (m = 0; m < LINE_METRICS; m++)
		for (k = 0; k < LINE_DAYS; k++) {
			const struct line_day *d = &days[m][k];

			if (!d->day)
				continue;
			row = blobmsg_open_array(b, NULL);
			blobmsg_add_u32(b, NULL, (uint32_t)m);
			blobmsg_add_u32(b, NULL, (uint32_t)d->day);
			blobmsg_add_double(b, NULL, d->v);
			blobmsg_add_u32(b, NULL, d->n);
			blobmsg_close_array(b, row);
		}
	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, t);
}

void line_load(struct blob_attr *t)
{
	struct blob_attr *cur, *row, *v;
	size_t r1, r2, r3_;
	int ok, n = 0;

	blobmsg_for_each_attr(cur, t, r1) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "have"))
			H.have = (unsigned)num(cur, &ok) & ((1u << LINE_METRICS) - 1);
		else if (!strcmp(k, "tech") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			snprintf(H.tech, sizeof(H.tech), "%s", blobmsg_get_string(cur));
		else if (!strcmp(k, "status"))
			S.status = (int)num(cur, &ok) % (LS_FAULT + 1);
		else if (!strcmp(k, "metric"))
			S.metric = (int)num(cur, &ok) % LINE_METRICS;
		else if (!strcmp(k, "since"))
			S.since = (long)num(cur, &ok);
		else if (!strcmp(k, "days") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)
			blobmsg_for_each_attr(row, cur, r2) {
				double x[4] = { 0 };
				int j = 0;
				struct line_day *d;

				blobmsg_for_each_attr(v, row, r3_)
					if (j < 4)
						x[j++] = num(v, &ok);
				if (j < 4 || x[0] < 0 || x[0] >= LINE_METRICS || x[1] <= 0)
					continue;
				d = &days[(int)x[0]][(long)x[1] % LINE_DAYS];
				d->day = (int32_t)x[1];
				d->v = (float)x[2];
				d->n = (uint16_t)x[3];
				n++;
			}
	}
	analysed = 0;
	if (n)
		AG_INFO("line: restored %d days of line numbers", n);
}
