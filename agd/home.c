/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW home Wi-Fi model (the rules: core/libag/home.c, ag/home.h).
 *
 * Coverage advice's looks (agd/coverage.c, every coverage.interval) feed the
 * model too: what each access point hears of each device, its band, loss and
 * rate. After each round of looks the model is judged: the home's places,
 * the last 30 days' coverage problems, and what an extender, moving the
 * gateway or another band would fix. A new best option is an event
 * (ag.home, HomeAdvice!); the best option goes into coverage advice and
 * decides whether the ISP's mesh Wi-Fi signal (offers) is worth making.
 *
 * Nothing here changes the home: it predicts. A person names places
 * (agctl home name <device> <zone>); the names stay home (the ISP sees
 * "place 2" and device kinds, unless brief.names).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libubox/blobmsg.h>

#include "ag/brief.h"
#include "ag/home.h"
#include "agd.h"

static struct hm_model M;
static struct hm_result R;
static struct hm_params P;
static long judged_at;
static int told = -1;                   /* the best option last told (kind, place, at), -1 none */
static int proof_told;

static void params(void)
{
	hm_params_default(&P);
	P.weak_dbm = cfg.heal_weak;
	P.learn = cfg.home_learn;
	P.interval_s = cfg.cov_interval > 0 ? cfg.cov_interval : 60;
	P.min_problem = P.interval_s >= 60 ? 5 : 300 / P.interval_s > 3 ? 300 / P.interval_s : 3;
	if (getenv("AIGW_TEST_HOME_PROBLEM"))   /* the tests: a problem after this many looks */
		P.min_problem = atoi(getenv("AIGW_TEST_HOME_PROBLEM"));
	P.decay = (uint32_t)(30L * 86400 / P.interval_s);
}

/* ---------- names: at home by name; for the ISP by kind, places by number (unless brief.names) ---------- */

static const char *home_name(const char *mac, void *ctx)
{
	const struct device *d = dev_find_mac(mac, 0);

	(void)ctx;
	return d && d->hostname[0] ? d->hostname : mac;
}

static const char *isp_name(const char *mac, void *ctx)
{
	static char k[4][40];
	static int r;
	const struct device *d = dev_find_mac(mac, 0);

	(void)ctx;
	if (!d)
		return "a device";
	if (cfg.brief_names && d->hostname[0])
		return d->hostname;
	r = (r + 1) % 4;
	brief_kind(d->type, d->hostname, k[r], sizeof(k[r]));
	return k[r];
}

static struct hm_names NM_HOME = { home_name, NULL, 1 };
static struct hm_names NM_ISP = { isp_name, NULL, 0 };

static const struct hm_names *names(int isp)
{
	NM_ISP.zones = cfg.brief_names;
	return isp ? &NM_ISP : &NM_HOME;
}

/* ---------- judging ---------- */

static void judge(long now)
{
	params();
	hm_expire(&M, now, &P);
	hm_judge(&M, now, &P, &R);
	/* the standing Extender prediction: measured once an extender appears */
	if (R.best_buy >= 0 && M.chk_place < 0) {
		M.ext_place = (int8_t)R.o[R.best_buy].place;
		M.ext_gain = (int8_t)R.o[R.best_buy].gain_db;
	}
	judged_at = now;
}

static void fresh(void)
{
	long now = (long)ag_wall_s();

	if (!judged_at || now - judged_at >= (cfg.cov_interval > 0 ? cfg.cov_interval : 60))
		judge(now);
}

void home_on_look(const char *mac, const struct cov_sample *s, int dual)
{
	struct hm_look l;

	if (!cfg.home)
		return;
	l.mac = mac;
	l.s = s;
	l.dual = dual;
	l.weak_tx = !strcmp(coverage_kind_of(mac), "WeakTransmitter");
	params();
	hm_add(&M, &l, &P);
}

static void tell(const char *kind, int fixed, int of, int gain, int conf, const char *text, const char *isp_text)
{
	struct blob_buf b = { 0 };
	char g[16], f[16], o[16];

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", kind);
	blobmsg_add_u32(&b, "fixed", (uint32_t)fixed);
	blobmsg_add_u32(&b, "of", (uint32_t)of);
	blobmsg_add_u32(&b, "gain_db", (uint32_t)gain);
	blobmsg_add_string(&b, "confidence", hm_confidence_name(conf));
	blobmsg_add_string(&b, "text", text);
	agd_emit(AG_EV_HOME, &b);
	blob_buf_free(&b);
	snprintf(g, sizeof(g), "%d", gain);
	snprintf(f, sizeof(f), "%d", fixed);
	snprintf(o, sizeof(o), "%d", of);
	dm_on_home(kind, f, o, g, hm_confidence_name(conf), isp_text);
	AG_INFO("home model: %s", text);
}

void home_tick(void)
{
	char text[700], isp[700];
	int key;

	if (!cfg.home)
		return;
	judge((long)ag_wall_s());
	if (R.status != HS_READY)
		return;
	key = R.best >= 0 ? R.o[R.best].kind * 10000 + (R.o[R.best].place + 1) * 100 + R.o[R.best].at + 1 : -1;
	if (key != told && R.best >= 0 && agd_connected()) {
		const struct hm_option *o = &R.o[R.best];

		hm_advice(&M, &R, names(0), text, sizeof(text));
		hm_advice(&M, &R, names(1), isp, sizeof(isp));
		tell(hm_kind_name(o->kind), o->fixed, R.problems, o->gain_db, o->confidence, text, isp);
		told = key;
	} else if (R.best < 0)
		told = -1;
	if (M.chk_place >= 0 && M.chk_measured != 127 && !proof_told && agd_connected()) {
		hm_proof_words(&M, names(0), text, sizeof(text));
		hm_proof_words(&M, names(1), isp, sizeof(isp));
		tell("Measured", 0, 0, M.chk_measured, 90, text, isp);
		proof_told = 1;
	}
}

void home_init(void)
{
	params();
	if (!cfg.home) {
		AG_INFO("home model: off (home.enabled)");
		return;
	}
	if (!cfg.cov)
		AG_WARN("home model: coverage advice is off (coverage.enabled): the model gets no looks");
	AG_INFO("home model: learns from coverage's looks; ready after %d looks", P.learn);
}

/* ---------- for others ---------- */

int home_extender_helps(char *why, size_t n)
{
	int b;

	if (why && n)
		why[0] = '\0';
	if (!cfg.home)
		return -1;
	fresh();
	if (R.status != HS_READY)
		return -1;
	b = R.best_buy;
	if (why && n) {
		if (!R.problems)
			snprintf(why, n, "the home model saw no coverage problems in the last 30 days");
		else if (R.extender_helps == 1)
			snprintf(why, n, "the home model expects an extender to fix %d of the last 30 days' %d coverage problems "
				 "(confidence %s)", R.o[b].fixed, R.problems, hm_confidence_name(R.o[b].confidence));
		else if (b >= 0)
			snprintf(why, n, "the home model expects a free change to fix as many coverage problems as an extender");
		else
			snprintf(why, n, "the home model expects no extender to fix the last 30 days' coverage problems");
	}
	return R.extender_helps;
}

void home_advice_line(char *buf, size_t n)
{
	if (n)
		buf[0] = '\0';
	if (!cfg.home)
		return;
	fresh();
	/* by device name (an ISP's copy is scrubbed by the caller); places by a person's name only with brief.names */
	if (R.status == HS_READY && R.best >= 0) {
		struct hm_names nm = { home_name, NULL, cfg.brief_names };

		hm_advice(&M, &R, &nm, buf, n);
	}
}

/* the place a device is at: its name (a person's), else "place N"; "" not known */
const char *home_zone_of(const char *mac)
{
	static char z[HM_NAME + 8];
	int k;

	if (!cfg.home || (k = hm_place_of(&M, mac)) < 0)
		return "";
	if (M.p[k].name[0])
		snprintf(z, sizeof(z), "%s", M.p[k].name);
	else
		snprintf(z, sizeof(z), "place %d", k + 1);
	return z;
}

/* a device by name, MAC or IP */
static const struct device *find_dev(const char *who)
{
	int i;

	for (i = 0; i < DEV_MAX; i++) {
		const struct device *d = &devs[i];

		if (d->used && (!strcasecmp(d->mac, who) || !strcmp(d->ip, who) || (d->hostname[0] && !strcasecmp(d->hostname, who))))
			return d;
	}
	return NULL;
}

int home_name_zone(const char *who, const char *zone, char *err, size_t en)
{
	const struct device *d;
	const char *s;
	int k = -1;

	for (s = zone; *s; s++)
		if ((unsigned char)*s < ' ' || strchr("\"'\\,;=<>&", *s)) {
			snprintf(err, en, "a zone's name: letters, digits, spaces and - _ . only");
			return -1;
		}
	if (strlen(zone) >= HM_NAME) {
		snprintf(err, en, "a zone's name: up to %d characters", HM_NAME - 1);
		return -1;
	}
	if (who[0] >= '1' && who[0] <= '9' && !who[1])
		k = who[0] - '1';
	else if ((d = find_dev(who)) && d->mac[0])
		k = hm_place_of(&M, d->mac);
	else {
		snprintf(err, en, "no device '%s' (a name, MAC or IP), and no place by that number", who);
		return -1;
	}
	if (k < 0 || hm_name(&M, k, zone) < 0) {
		snprintf(err, en, "AIGW has not placed '%s' yet: name it once it has been seen for a few minutes", who);
		return -1;
	}
	AG_INFO("home model: place %d named '%s'", k + 1, zone);
	judged_at = 0;
	return k + 1;
}

/* ---------- agctl home, the bridge ---------- */

static void place_blob(struct blob_buf *b, int k, int isp)
{
	const struct hm_place *pl = &M.p[k];
	char w[160];
	void *t = blobmsg_open_table(b, NULL), *a;
	int learned, ear, band, j;

	blobmsg_add_u32(b, "place", (uint32_t)k + 1);
	if (!isp || cfg.brief_names)
		blobmsg_add_string(b, "name", pl->name);
	hm_place_words(&M, k, names(isp), 0, w, sizeof(w));
	blobmsg_add_string(b, "where", w);
	blobmsg_add_u32(b, "samples", pl->n);
	blobmsg_add_u32(b, "signal", (uint32_t)(int)(pl->sig[0] - 0.5f));
	blobmsg_add_u32(b, "path_loss_24", (uint32_t)hm_path_loss(pl, 2, &P));
	blobmsg_add_u32(b, "path_loss_5", (uint32_t)hm_path_loss(pl, 5, &P));
	blobmsg_add_u32(b, "distance_like_m", (uint32_t)(hm_distance(pl, &P) + 0.5f));
	blobmsg_add_u32(b, "offset_24", (uint32_t)hm_offset24(pl, &learned));
	blobmsg_add_u8(b, "offset_learned", (uint8_t)learned);
	blobmsg_add_u32(b, "spread_db", (uint32_t)(pl->spread + 0.5f));
	blobmsg_add_u32(b, "problems", (uint32_t)hm_place_problems(&M, k, (long)ag_wall_s(), &P));
	if ((band = hm_best_link(pl, &ear)) > 0) {
		blobmsg_add_u32(b, "best_band", (uint32_t)band);
		blobmsg_add_u32(b, "best_ap", (uint32_t)ear);
	}
	a = blobmsg_open_array(b, "next_to");
	for (j = 0; j < M.np; j++)
		if (j != k && (pl->adj[j] || M.p[j].adj[k]))
			blobmsg_add_u32(b, NULL, (uint32_t)j + 1);
	blobmsg_close_array(b, a);
	blobmsg_close_table(b, t);
}

static void blob(struct blob_buf *b, int isp, int full)
{
	char w[800];
	void *a, *t;
	int k;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.home);
	if (!cfg.home)
		return;
	fresh();
	blobmsg_add_string(b, "status", hm_status_name(R.status));
	blobmsg_add_u32(b, "looks", M.looks);
	blobmsg_add_u32(b, "learn", (uint32_t)P.learn);
	blobmsg_add_u32(b, "places", (uint32_t)R.places);
	blobmsg_add_u32(b, "devices", (uint32_t)R.devices);
	blobmsg_add_u32(b, "problems", (uint32_t)R.problems);
	blobmsg_add_u32(b, "access_points", (uint32_t)M.ears);
	blobmsg_add_string(b, "extender_helps", R.extender_helps < 0 ? "unknown" : R.extender_helps ? "yes" : "no");
	hm_advice(&M, &R, names(isp), w, sizeof(w));
	blobmsg_add_string(b, "advice", w);
	hm_proof_words(&M, names(isp), w, sizeof(w));
	if (*w) {
		blobmsg_add_string(b, "proof", w);
		if (M.chk_measured != 127) {
			blobmsg_add_u32(b, "predicted_db", (uint32_t)M.chk_pred);
			blobmsg_add_u32(b, "measured_db", (uint32_t)M.chk_measured);
		}
	}
	if (R.best >= 0)
		blobmsg_add_string(b, "best", hm_kind_name(R.o[R.best].kind));
	a = blobmsg_open_array(b, "options");
	for (k = 0; k < R.nopt; k++) {
		const struct hm_option *o = &R.o[k];

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "kind", hm_kind_name(o->kind));
		blobmsg_add_string(b, "cost", o->buy ? "buy" : "free");
		if (o->place >= 0)
			blobmsg_add_u32(b, "place", (uint32_t)o->place + 1);
		if (o->at >= 0)
			blobmsg_add_u32(b, "near", (uint32_t)o->at + 1);
		blobmsg_add_u32(b, "gain_db", (uint32_t)o->gain_db);
		blobmsg_add_u32(b, "plus_minus_db", (uint32_t)o->plus_minus);
		blobmsg_add_u32(b, "fixed", (uint32_t)o->fixed);
		blobmsg_add_u32(b, "of", (uint32_t)R.problems);
		blobmsg_add_u32(b, "may_turn_weak", (uint32_t)o->broken);
		blobmsg_add_u32(b, "confidence", (uint32_t)o->confidence);
		blobmsg_add_string(b, "confidence_name", hm_confidence_name(o->confidence));
		if (full) {
			hm_option_words(&M, &R, k, names(isp), w, sizeof(w));
			blobmsg_add_string(b, "text", w);
		}
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	if (!full)
		return;
	a = blobmsg_open_array(b, "list");
	for (k = 0; k < M.np; k++)
		if (M.p[k].n)
			place_blob(b, k, isp);
	blobmsg_close_array(b, a);
}

void home_blob(struct blob_buf *b)
{
	blob(b, 0, 1);
}

/* for the bridge's report to cortex: the status, counts and the best option's kind; no places, no names */
void home_report_blob(struct blob_buf *b)
{
	void *t;

	if (!cfg.home || !cfg.home_share)
		return;
	t = blobmsg_open_table(b, "home");
	blob(b, 1, 0);
	blobmsg_close_table(b, t);
}

/* ---------- the data model: Device.X_AIGW_.HomeModel. (the ISP's view) ---------- */

int home_place_ids(uint32_t *out, int max)
{
	int k, n = 0;

	if (!cfg.home)
		return 0;
	fresh();
	for (k = 0; k < M.np && n < max; k++)
		if (M.p[k].n)
			out[n++] = (uint32_t)k + 1;
	return n;
}

int home_option_ids(uint32_t *out, int max)
{
	int k;

	if (!cfg.home)
		return 0;
	fresh();
	for (k = 0; k < R.nopt && k < max; k++)
		out[k] = (uint32_t)k + 1;
	return k;
}

int home_place_field(uint32_t id, const char *f, char *buf, size_t n)
{
	const struct hm_place *pl;
	int k = (int)id - 1, learned, ear, j;
	size_t l = 0;

	fresh();
	if (k < 0 || k >= M.np || !M.p[k].n)
		return -1;
	pl = &M.p[k];
	if (!strcmp(f, "Name")) snprintf(buf, n, "%s", cfg.brief_names ? pl->name : "");
	else if (!strcmp(f, "Where")) hm_place_words(&M, k, names(1), 0, buf, n);
	else if (!strcmp(f, "Samples")) snprintf(buf, n, "%u", pl->n);
	else if (!strcmp(f, "Signal")) snprintf(buf, n, "%d", (int)(pl->sig[0] - 0.5f));
	else if (!strcmp(f, "PathLoss24")) snprintf(buf, n, "%d", hm_path_loss(pl, 2, &P));
	else if (!strcmp(f, "PathLoss5")) snprintf(buf, n, "%d", hm_path_loss(pl, 5, &P));
	else if (!strcmp(f, "DistanceLike")) snprintf(buf, n, "%d", (int)(hm_distance(pl, &P) + 0.5f));
	else if (!strcmp(f, "Band24Offset")) snprintf(buf, n, "%d", hm_offset24(pl, &learned));
	else if (!strcmp(f, "Problems")) snprintf(buf, n, "%d", hm_place_problems(&M, k, (long)ag_wall_s(), &P));
	else if (!strcmp(f, "BestBand")) {
		int b = hm_best_link(pl, &ear);

		snprintf(buf, n, "%s", b == 2 ? "2.4GHz" : b == 5 ? "5GHz" : "");
	} else if (!strcmp(f, "NextTo")) {
		buf[0] = '\0';
		for (j = 0; j < M.np && l < n; j++)
			if (j != k && (pl->adj[j] || M.p[j].adj[k]))
				l += (size_t)snprintf(buf + l, n - l, "%s%d", l ? "," : "", j + 1);
	} else
		return -1;
	return 0;
}

int home_option_field(uint32_t id, const char *f, char *buf, size_t n)
{
	const struct hm_option *o;

	fresh();
	if (!id || (int)id > R.nopt)
		return -1;
	o = &R.o[id - 1];
	if (!strcmp(f, "Kind")) snprintf(buf, n, "%s", hm_kind_name(o->kind));
	else if (!strcmp(f, "Cost")) snprintf(buf, n, "%s", o->buy ? "Buy" : "Free");
	else if (!strcmp(f, "Place")) snprintf(buf, n, "%d", o->place + 1);
	else if (!strcmp(f, "Near")) snprintf(buf, n, "%d", o->at + 1);
	else if (!strcmp(f, "GainDb")) snprintf(buf, n, "%d", o->gain_db);
	else if (!strcmp(f, "PlusMinusDb")) snprintf(buf, n, "%d", o->plus_minus);
	else if (!strcmp(f, "Fixed")) snprintf(buf, n, "%d", o->fixed);
	else if (!strcmp(f, "Of")) snprintf(buf, n, "%d", R.problems);
	else if (!strcmp(f, "MayTurnWeak")) snprintf(buf, n, "%d", o->broken);
	else if (!strcmp(f, "Confidence")) snprintf(buf, n, "%s", hm_confidence_name(o->confidence));
	else if (!strcmp(f, "Summary")) hm_option_words(&M, &R, (int)id - 1, names(1), buf, n);
	else
		return -1;
	return 0;
}

int home_field(const char *f, char *buf, size_t n)
{
	if (!strcmp(f, "Enable")) {
		snprintf(buf, n, "%s", cfg.home ? "true" : "false");
		return 0;
	}
	fresh();
	if (!strcmp(f, "Status")) snprintf(buf, n, "%s", cfg.home ? hm_status_name(R.status) : "Disabled");
	else if (!strcmp(f, "Places")) snprintf(buf, n, "%d", R.places);
	else if (!strcmp(f, "Problems")) snprintf(buf, n, "%d", R.problems);
	else if (!strcmp(f, "AccessPoints")) snprintf(buf, n, "%d", M.ears);
	else if (!strcmp(f, "BestOption")) snprintf(buf, n, "%s", R.best >= 0 ? hm_kind_name(R.o[R.best].kind) : "");
	else if (!strcmp(f, "ExtenderHelps"))
		snprintf(buf, n, "%s", R.extender_helps < 0 ? "Unknown" : R.extender_helps ? "Yes" : "No");
	else if (!strcmp(f, "Advice")) hm_advice(&M, &R, names(1), buf, n);
	else if (!strcmp(f, "Proof")) hm_proof_words(&M, names(1), buf, n);
	else if (!strcmp(f, "PlaceNumberOfEntries")) snprintf(buf, n, "%d", cfg.home ? R.places : 0);
	else if (!strcmp(f, "OptionNumberOfEntries")) snprintf(buf, n, "%d", cfg.home ? R.nopt : 0);
	else
		return -1;
	return 0;
}

/* ---------- state: the model as it is in memory, in hex (another layout: learned again) ---------- */

void home_save(struct blob_buf *b)
{
	static char hex[2 * sizeof(struct hm_model) + 1];
	const uint8_t *p = (const uint8_t *)&M;
	void *t;
	size_t k;

	if (!M.looks)
		return;
	t = blobmsg_open_table(b, "home");
	blobmsg_add_u32(b, "layout", (uint32_t)sizeof(M));
	blobmsg_add_u32(b, "told", (uint32_t)told);
	blobmsg_add_u8(b, "proof_told", (uint8_t)proof_told);
	for (k = 0; k < sizeof(M); k++)
		snprintf(hex + 2 * k, 3, "%02x", p[k]);
	blobmsg_add_string(b, "model", hex);
	blobmsg_close_table(b, t);
}

void home_load(struct blob_attr *t)
{
	struct blob_attr *c;
	uint32_t layout = 0;
	size_t rem, k;

	hm_init(&M);
	blobmsg_for_each_attr(c, t, rem) {
		const char *n = blobmsg_name(c);

		if (!strcmp(n, "layout") && blobmsg_type(c) == BLOBMSG_TYPE_INT32)
			layout = blobmsg_get_u32(c);
		else if (!strcmp(n, "told") && blobmsg_type(c) == BLOBMSG_TYPE_INT32)
			told = (int)blobmsg_get_u32(c);
		else if (!strcmp(n, "proof_told") && blobmsg_type(c) == BLOBMSG_TYPE_INT8)
			proof_told = blobmsg_get_u8(c);
		else if (!strcmp(n, "model") && blobmsg_type(c) == BLOBMSG_TYPE_STRING && layout == sizeof(M)) {
			const char *h = blobmsg_get_string(c);
			struct hm_model x;
			uint8_t *p = (uint8_t *)&x;
			unsigned v;

			if (strlen(h) != 2 * sizeof(x))
				continue;
			for (k = 0; k < sizeof(x) && sscanf(h + 2 * k, "%2x", &v) == 1; k++)
				p[k] = (uint8_t)v;
			if (k == sizeof(x) && x.np >= 0 && x.np <= HM_PLACES) {
				for (k = 0; k < HM_DEVS; k++)
					x.d[k].mac[sizeof(x.d[k].mac) - 1] = '\0';
				for (k = 0; k < HM_PLACES; k++)
					x.p[k].name[HM_NAME - 1] = '\0';
				M = x;
			}
		}
	}
	if (layout && layout != sizeof(M))
		AG_INFO("home model: the saved model is of another layout: learned again");
}

/* first use without a saved model */
__attribute__((constructor)) static void home_start(void)
{
	hm_init(&M);
}
