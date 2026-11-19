/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW proactive care, before the call (the rules: core/libag/proactive.c,
 * ag/proactive.h).
 *
 * Every proactive.interval seconds agd looks at what it already watches:
 *   - proactive care's open problem (its quiet Diagnose, the nightly speed test)
 *   - the why timeline's last half hour (the line, DNS, Wi-Fi, a full line with
 *     rough calls or games on it)
 *   - Self-Healing Wi-Fi's verdict on each access point, and how many devices
 *     it touches
 *   - devices that keep dropping off Wi-Fi, or keep changing band
 *   - the week's experience score (a home having a bad week calls sooner)
 * and the rules say which faults are worth acting on. For each new one:
 *   - a fix AIGW can make (the help-desk brief's: smart queueing on a full
 *     line, keeping a device on 2.4 GHz) is made as act.mode allows: enforce
 *     applies it, advise proposes it (a person says yes); off: none
 *   - Wi-Fi trouble Self-Healing Wi-Fi works on by itself is watched for
 *     care.wifi_grace first
 *   - anything else is raised to the ISP at once: Device.X_AIGW_.ProactiveTicket!
 *     with the brief, the evidence, the next step and a note for the customer
 * When it is over, a ticket AIGW fixed is told as "we noticed X and fixed it".
 *
 * Names: the ISP's ticket follows brief.names (device kinds by default); the
 * customer's note never names anything. At home, agctl shows the device.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/brief.h"
#include "ag/calls.h"
#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/proactive.h"
#include "ag/why.h"
#include "agd.h"

#define SIG_MAX   24
#define DROPS     8             /* Wi-Fi leaves kept per device */
#define DROPS_MIN 3             /* left Wi-Fi this often in an hour: a failing device */
#define PDEV_MAX  32
#define WHY_SPAN  1800          /* the timeline looked at for rough calls: the last half hour */
#define WHY_NOW   180           /* ... for what is happening now */
#define ORIGIN    "proactive care"

static struct pro_state S;
static struct pro_params P;
static struct uloop_timeout timer;

/* devices that keep dropping off or changing band */
static struct pdev {
	char mac[18];
	long left[DROPS];
	int head;
	long hop_since;
} pd[PDEV_MAX];

static struct why_result wr;
static struct why_in wi;

static void params(void)
{
	pro_params_default(&P);
	P.threshold = cfg.pro_threshold;
	P.min_s = cfg.pro_min_s;
	P.clear_s = cfg.pro_clear_s;
	P.cooldown_s = cfg.pro_cooldown_s;
	P.max_per_day = cfg.pro_max_per_day;
	P.wifi_grace = cfg.care_wifi_grace;
}

/*
 * An outage across the area (agd/area.c: cortex saw many homes of this uplink
 * go down at once) holds back tickets about the line: the ISP knows already.
 */
static int area_outage(void)
{
	struct area_now o;

	return area_get(&o) > 0;
}

static struct pdev *pdev_of(const char *mac, int create)
{
	struct pdev *free_one = NULL, *oldest = NULL;
	int i;

	for (i = 0; i < PDEV_MAX; i++) {
		if (!strcmp(pd[i].mac, mac))
			return &pd[i];
		if (!pd[i].mac[0] && !free_one)
			free_one = &pd[i];
		if (!oldest || pd[i].left[(pd[i].head + DROPS - 1) % DROPS] < oldest->left[(oldest->head + DROPS - 1) % DROPS])
			oldest = &pd[i];
	}
	if (!create)
		return NULL;
	if (!free_one) {
		free_one = oldest;
		memset(free_one, 0, sizeof(*free_one));
	}
	snprintf(free_one->mac, sizeof(free_one->mac), "%s", mac);
	return free_one;
}

void proactive_on_wifi(struct blob_attr *msg)
{
	static const struct blobmsg_policy pol[2] = { { "event", BLOBMSG_TYPE_STRING }, { "mac", BLOBMSG_TYPE_STRING } };
	struct blob_attr *tb[2];
	struct pdev *d;

	if (!cfg.pro)
		return;
	blobmsg_parse(pol, 2, tb, blob_data(msg), blob_len(msg));
	if (!tb[0] || !tb[1] || strcmp(blobmsg_get_string(tb[0]), "disconnected"))
		return;
	if (!(d = pdev_of(blobmsg_get_string(tb[1]), 1)))
		return;
	d->left[d->head] = (long)ag_wall_s();
	d->head = (d->head + 1) % DROPS;
}

/* ---------- what is seen now ---------- */

static struct pro_signal sig[SIG_MAX];
static int nsig;
static char sig_code[SIG_MAX][24], sig_target[SIG_MAX][24];

static void signal_add(const char *code, int sev, long since, int devices, int rough, const char *target,
		       const char *source)
{
	struct pro_signal *s;
	char key[PRO_KEY];
	int i, fault = brief_fault_of(code);

	if (!code || sev < DS_WARN)
		return;
	snprintf(key, sizeof(key), "%s%s%s", code, target && *target ? ":" : "", target ? target : "");
	for (i = 0; i < nsig; i++)
		if (!strcmp(sig[i].key, key)) {
			/* the same fault from two sources: one signal, the earliest start, the most devices */
			s = &sig[i];
			if (since && (!s->since || since < s->since))
				s->since = since;
			if (devices > s->devices)
				s->devices = devices;
			if (sev > s->sev)
				s->sev = sev;
			if (rough > s->rough_calls)
				s->rough_calls = rough;
			return;
		}
	if (nsig >= SIG_MAX)
		return;
	s = &sig[nsig];
	memset(s, 0, sizeof(*s));
	snprintf(s->key, sizeof(s->key), "%s", key);
	snprintf(sig_code[nsig], sizeof(sig_code[0]), "%s", code);
	snprintf(sig_target[nsig], sizeof(sig_target[0]), "%s", target ? target : "");
	s->code = sig_code[nsig];
	s->target = sig_target[nsig][0] ? sig_target[nsig] : NULL;
	s->fault = fault;
	s->sev = sev;
	s->since = since;
	s->devices = devices;
	s->rough_calls = rough;
	s->source = source;
	nsig++;
}

static int home_devices(long now)
{
	int i, n = 0;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].mac[0] && !is_local(devs[i].ip) &&
		    (devs[i].wifi_connected || devs[i].last_seen >= now - 600))
			n++;
	return n;
}

static int devices_of(const char *area, long now)
{
	if (!strcmp(area, "WiFi"))
		return wifi_station_count();
	if (!strcmp(area, "Device"))
		return 1;
	return home_devices(now);
}

static const char *heal_code(int cause)
{
	switch (cause) {
	case HC_CONGESTION: return "WIFI_CONGESTION";
	case HC_COVERAGE: return "WIFI_COVERAGE";
	case HC_HOPPING: return "WIFI_HOPPING";
	case HC_DEVICE: return "WIFI_DEVICE";
	case HC_RADIO: return "WIFI_RADIO";
	}
	return "WIFI_PROBLEM";
}

/* how badly: the worst loss among the affected devices (the radio's own figure is over all it measured, so one
   phone losing 38% beside three fine ones reads 10%), and the worst affected device's delay */
static void signal_measure(const char *key, const struct heal_radio *h, const char *only)
{
	const char *p;
	int i, worst = h->loss_pct > 0 ? h->loss_pct : 0;

	for (p = h->affected; *p; ) {
		size_t l = strcspn(p, ",");
		char mac[18];
		const struct device *d;
		int loss = -1, rtt = -1, n = 0;

		snprintf(mac, sizeof(mac), "%.*s", (int)(l < 17 ? l : 17), p);
		p += l + (p[l] == ',');
		if (only && strcasecmp(mac, only))
			continue;
		if ((d = dev_find_mac(mac, 0))) {
			heal_device(d, &loss, &rtt, &n);
			if (n > 0 && loss > worst)
				worst = loss;
		}
	}
	for (i = 0; i < nsig; i++)
		if (!strcmp(sig[i].key, key)) {
			if (worst > sig[i].loss_pct)
				sig[i].loss_pct = worst;
			if (h->rtt_ms > sig[i].delay_ms)
				sig[i].delay_ms = h->rtt_ms;
			return;
		}
}

static void gather(long now)
{
	const struct care_state *cs;
	char err[160];
	int i;

	nsig = 0;
	/* proactive care's open problem */
	if ((cs = care_open()) && cs->sev >= DS_WARN)
		signal_add(cs->code, cs->sev, cs->since, devices_of(cs->area, now), 0, NULL, "care");
	/* the timeline: rough calls and games over the last half hour; what is happening now (the last few
	   minutes, still going: how long it has lasted comes from the looks, ag/proactive.h) */
	if (cfg.why && why_ask(now - WHY_SPAN, now, "", &wr, &wi, err, sizeof(err)) == 0) {
		int rough = 0, step = cfg.why_interval > 0 ? cfg.why_interval : 60;
		long recent = now - 3L * step;

		for (i = 0; i < wi.ne; i++)
			if (wi.e[i].kind == WE_CALL && wi.e[i].flag == CV_ROUGH && (long)wi.e[i].t >= now - WHY_SPAN)
				rough++;
		if (why_ask(now - (WHY_NOW > 6L * step ? WHY_NOW : 6L * step), now, "", &wr, &wi, err, sizeof(err)) < 0)
			wr.n = 0;
		for (i = 0; i < wr.n; i++) {
			const struct why_finding *f = &wr.f[i];
			int fault = brief_fault_of(f->code);

			if (!f->code || f->sev < DS_WARN || f->until < recent)
				continue;
			if (fault == BF_LINE || fault == BF_HOME_WIFI)
				signal_add(f->code, f->sev, f->since, devices_of(f->area, now), 0, NULL, "timeline");
			else if (!strcmp(f->code, "LINE_FULL") && rough)
				signal_add(f->code, DS_WARN, f->since, home_devices(now), rough, NULL, "calls");
		}
	}
	/* Self-Healing Wi-Fi, each access point */
	for (i = 0; i < wifi_n_aps; i++) {
		const struct heal_radio *h = heal_for(wifi_aps[i].iface);
		const char *code, *p;
		int n = 0;

		if (!h || (h->status != HEAL_DEGRADED && h->status != HEAL_POOR))
			continue;
		code = heal_code(h->cause);
		if (h->cause == HC_DEVICE || h->cause == HC_HOPPING) {
			/* one device each: the device's own ticket */
			char mac[18];

			for (p = h->affected; *p; ) {
				size_t l = strcspn(p, ",");

				snprintf(mac, sizeof(mac), "%.*s", (int)(l < 17 ? l : 17), p);
				if (mac[0] && !(h->cause == HC_HOPPING && act_active("stay24", mac))) {
					char key[PRO_KEY];

					signal_add(code, h->status == HEAL_POOR ? DS_PROBLEM : DS_WARN, (long)h->problem_since, 1, 0,
						   mac, "wifi");
					snprintf(key, sizeof(key), "%s:%s", code, mac);
					signal_measure(key, h, mac);
				}
				p += l + (p[l] == ',');
			}
			continue;
		}
		for (p = h->affected; *p; p++)
			n += *p == ',';
		n += h->affected[0] != '\0';
		if (h->cause == HC_RADIO || !n)
			n = wifi_aps[i].stations;
		signal_add(code, h->status == HEAL_POOR ? DS_PROBLEM : DS_WARN, (long)h->problem_since, n, 0, NULL, "wifi");
		signal_measure(code, h, NULL);
	}
	/* devices that keep dropping off Wi-Fi, or keep changing band */
	for (i = 0; i < PDEV_MAX; i++) {
		struct pdev *d = &pd[i];
		struct device *dv;
		long first = 0;
		int k, n = 0;

		if (!d->mac[0])
			continue;
		for (k = 0; k < DROPS; k++)
			if (d->left[k] && d->left[k] >= now - 3600) {
				n++;
				if (!first || d->left[k] < first)
					first = d->left[k];
			}
		if (n >= DROPS_MIN)
			signal_add("WIFI_DROPS", DS_WARN, first, 1, 0, d->mac, "device");
		dv = dev_find_mac(d->mac, 0);
		/* kept on 2.4 GHz (stay24): it can't hop any more, whatever its last ten minutes say */
		if (dv && dv->wifi_connected && heal_hopping(dv) && !act_active("stay24", dv->mac)) {
			if (!d->hop_since)
				d->hop_since = now;
			signal_add("WIFI_HOPPING", DS_WARN, d->hop_since, 1, 0, d->mac, "device");
		} else
			d->hop_since = 0;
	}
	/* a device hopping that never dropped off: steering's own count */
	for (i = 0; i < DEV_MAX; i++) {
		struct device *dv = &devs[i];
		struct pdev *d;

		if (!dv->used || !dv->wifi_connected || !dv->mac[0] || !heal_hopping(dv) || pdev_of(dv->mac, 0) ||
		    act_active("stay24", dv->mac))
			continue;
		if ((d = pdev_of(dv->mac, 1))) {
			d->hop_since = now;
			signal_add("WIFI_HOPPING", DS_WARN, now, 1, 0, dv->mac, "device");
		}
	}
}

/* ---------- a ticket ---------- */

static int local_hour(long now)
{
	struct tm tm;
	time_t t = (time_t)now;

	return localtime_r(&t, &tm) ? tm.tm_hour : 12;
}

static int is_mac(const char *s)
{
	return s && strlen(s) == 17 && s[2] == ':';
}

static void emit(struct pro_ticket *t)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "id", t->id);
	blobmsg_add_string(&b, "status", pro_status_name(t->status));
	blobmsg_add_string(&b, "code", t->code);
	blobmsg_add_string(&b, "fault", brief_fault_name(t->fault));
	blobmsg_add_string(&b, "summary", t->summary);
	blobmsg_add_string(&b, "next_step", t->next_step);
	blobmsg_add_string(&b, "note", t->note);
	if (t->decision)
		blobmsg_add_u32(&b, "decision", t->decision);
	agd_emit(AG_EV_TICKET, &b);
	blob_buf_free(&b);
	dm_on_ticket(t->id);
}

/* tell the ISP: a new ticket counts against the day's limit, later news of it doesn't */
static void tell(struct pro_ticket *t, long now)
{
	if (S.area_hold) {
		/* a known area outage: the ISP tells its customers itself. News of a ticket it already has
		   (it is over) still goes, without a note for the customer; nothing new does */
		if (!t->notified) {
			AG_INFO("proactive: ticket %u %s: no word during the area outage", t->id, t->code);
			return;
		}
		t->note[0] = '\0';
	}
	if (!t->notified) {
		if (pro_budget(&S, now, &P) <= 0) {
			AG_INFO("proactive: ticket %u %s kept at home: %d new tickets in 24 hours already", t->id, t->code,
				P.max_per_day);
			if (t->status != PS_FIXED) {
				t->status = PS_SUPPRESSED;
				snprintf(t->next_step, sizeof(t->next_step), "Kept at home: the ISP heard of %d new problems in "
					 "24 hours already (proactive.max_per_day).", P.max_per_day);
			}
			S.c.suppressed++;
			return;
		}
		pro_notified(&S, t, now);
	}
	AG_INFO("proactive: ticket %u %s %s: %s", t->id, t->code, pro_status_name(t->status), t->summary);
	emit(t);
}

/* what the ticket is about, in AIGW's own words (a device by its kind unless brief.names) */
static void own_words(struct pro_ticket *t, long now)
{
	struct device *d = is_mac(t->target) ? dev_find_mac(t->target, 0) : NULL;
	char who[64] = "A device", for_[32];
	long mins = (now - t->since) / 60;

	if (mins < 1)
		snprintf(for_, sizeof(for_), "under a minute");
	else
		snprintf(for_, sizeof(for_), "%ld min", mins);

	if (d && cfg.brief_names)
		snprintf(who, sizeof(who), "%s", d->hostname[0] ? d->hostname : d->mac);
	else if (d) {
		brief_kind(d->type, d->hostname, who, sizeof(who));
		who[0] = (char)toupper((unsigned char)who[0]);
	}
	if (!strcmp(t->code, "WIFI_HOPPING"))
		snprintf(t->summary, sizeof(t->summary), "%s keeps moving between 2.4 and 5 GHz Wi-Fi (seen for %s): it is at "
			 "the edge of 5 GHz, and each move stops what it was doing for a moment (WIFI_HOPPING).", who, for_);
	else if (!strcmp(t->code, "WIFI_DROPS"))
		snprintf(t->summary, sizeof(t->summary), "%s keeps dropping off Wi-Fi (%d or more times in the last hour): each "
			 "drop stops what it was doing for a few seconds (WIFI_DROPS).", who, DROPS_MIN);
	else
		snprintf(t->summary, sizeof(t->summary), "%s for %s, on %s. %s", t->code, for_,
			 t->devices == 1 ? "one device" : "the whole home", diag_advice(t->code));
}

/* the brief, the evidence and the customer's note */
static void describe(struct pro_ticket *t, long now, struct brief *b)
{
	char line[BRIEF_TEXT + 24];
	int i, have = brief_quick(t->since > 60 ? t->since - 60 : t->since, now, is_mac(t->target) ? t->target : NULL, b) == 0;

	if (have && strcmp(b->code ? b->code : "", t->code)) {
		/* the brief's verdict is about something else (the home's line, while one device hops): say what this is */
		own_words(t, now);
		snprintf(line, sizeof(line), " The brief of the same time: %.200s", b->verdict);
		if (strlen(t->summary) + strlen(line) < sizeof(t->summary))
			strcat(t->summary, line);
	} else if (have) {
		char *e;

		snprintf(t->summary, sizeof(t->summary), "%s", b->verdict);
		/* the brief calls a timeline finding "earlier"; a ticket is about now */
		if (t->open && (e = strstr(t->summary, ", earlier: "))) {
			memmove(e + 7, e + 11, strlen(e + 11) + 1);
			memcpy(e, ", now: ", 7);
		}
	} else {
		own_words(t, now);
		memset(b, 0, sizeof(*b));
	}
	snprintf(t->evidence, sizeof(t->evidence), "Worth acting on (%d points: %s).", t->points, t->why);
	for (i = 0; have && i < b->nev && i < 5; i++) {
		snprintf(line, sizeof(line), " %s", b->ev[i].text);
		if (strlen(t->evidence) + strlen(line) < sizeof(t->evidence))
			strcat(t->evidence, line);
	}
	brief_scrub_isp(t->summary, sizeof(t->summary));
	brief_scrub_isp(t->evidence, sizeof(t->evidence));
	pro_note(t->code, 0, local_hour(t->since), t->note, sizeof(t->note));
}

static int watched_by_healing(const char *code)
{
	return !strcmp(code, "WIFI_CONGESTION") || !strcmp(code, "WIFI_HOPPING") || !strcmp(code, "WIFI_PROBLEM");
}

/* raise it to the ISP: the brief's next step */
static void raise_it(struct pro_ticket *t, long now, const struct brief *b, const char *why_now)
{
	t->status = PS_RAISED;
	snprintf(t->next_step, sizeof(t->next_step), "%s%.250s%s%.300s", why_now ? why_now : "",
		 b && b->fix.text[0] ? b->fix.text : "Check it from the ISP's side.",
		 b && b->visit_why[0] ? " Technician: " : "", b && b->visit_why[0] ? b->visit_why : "");
	brief_scrub_isp(t->next_step, sizeof(t->next_step));
	tell(t, now);
}

/* a new ticket (or one back after the ISP last heard of it long ago): fix, propose, watch or raise */
static void decide(struct pro_ticket *t, long now)
{
	struct brief b;
	char err[200], reason[300];
	uint32_t id = 0;
	int applied = 0, rc = -1;
	const char *act = NULL;

	describe(t, now, &b);
	/* the fix: what the ticket itself says first (a device hopping: keep it on 2.4 GHz; a full line with
	   rough calls: smart queueing), else the brief's */
	if (!strcmp(t->code, "WIFI_HOPPING") && is_mac(t->target) && !act_active("stay24", t->target) &&
	    (!b.fix.action || strcmp(b.fix.action, "approve"))) {
		b.fix.possible = 1;
		b.fix.action = "stay24";
		snprintf(b.fix.text, sizeof(b.fix.text), "Keep the device on 2.4 GHz for a day (steadier at the edge of 5 GHz).");
	} else if (!strcmp(t->code, "LINE_FULL") && (!b.fix.action || strcmp(b.fix.action, "approve")) && cfg.sqm &&
		   !sqm_on()) {
		b.fix.possible = 1;
		b.fix.action = "sqm";
		snprintf(b.fix.text, sizeof(b.fix.text), "Smart queueing on the line: calls and games stay smooth while it is full.");
	}
	if (cfg.pro_fix && b.fix.possible && b.fix.action) {
		snprintf(reason, sizeof(reason), "Proactive care, before the call (ticket %u): %.200s", t->id, b.verdict);
		brief_scrub_isp(reason, sizeof(reason));
		if (!strcmp(b.fix.action, "sqm")) {
			act = "sqm";
			rc = sqm_auto(ORIGIN, reason, &id, &applied, err, sizeof(err));
		} else if (!strcmp(b.fix.action, "stay24") && is_mac(t->target)) {
			act = "stay24";
			rc = act_auto("stay24", t->target, 86400, ORIGIN, reason, &id, &applied, err, sizeof(err));
		} else if (!strcmp(b.fix.action, "approve") && b.fix.decision) {
			act = "approve";
			id = b.fix.decision;
			rc = 0;                 /* a fix waits for a yes already: a person's to give */
		}
		if (act && rc < 0)
			AG_INFO("proactive: ticket %u: no fix (%s): %s", t->id, act, err);
	}
	if (act && rc == 0 && id) {
		t->decision = id;
		if (applied) {
			t->status = PS_FIXING;
			t->acted = 1;
			t->watch_until = now + P.wifi_grace;    /* not gone by then: the ISP hears of it */
			snprintf(t->next_step, sizeof(t->next_step), "Nothing to do: AIGW fixed it itself (decision %u: %.240s). "
				 "The ISP hears of it when it is over.", id, b.fix.text);
			brief_scrub_isp(t->next_step, sizeof(t->next_step));
			AG_INFO("proactive: ticket %u %s: fixed by AIGW (decision %u)", t->id, t->code, id);
			return;
		}
		t->status = PS_PROPOSED;
		S.c.proposed++;
		snprintf(t->next_step, sizeof(t->next_step), "A fix waits for the customer's yes (decision %u, advise mode): "
			 "%.240s Approve it: Decision.%u.Approve() over USP or TR-069, or agctl approve %u at home.", id,
			 b.fix.text, id, id);
		brief_scrub_isp(t->next_step, sizeof(t->next_step));
		tell(t, now);
		return;
	}
	if (watched_by_healing(t->code) && now - t->since < P.wifi_grace) {
		t->status = PS_WATCHING;
		t->watch_until = t->since + P.wifi_grace;
		snprintf(t->next_step, sizeof(t->next_step), "Nothing yet: Self-Healing Wi-Fi works on it; the ISP hears of it "
			 "if it lasts %d minutes.", (int)(P.wifi_grace / 60));
		AG_INFO("proactive: ticket %u %s: Self-Healing Wi-Fi has until %ld", t->id, t->code, t->watch_until);
		return;
	}
	raise_it(t, now, &b, NULL);
}

static void closed(struct pro_ticket *t, long now)
{
	int was_notified = t->notified, quiet = t->quiet, st = pro_close(&S, t, now);

	AG_INFO("proactive: ticket %u %s over after %ld min: %s", t->id, t->code, (now - t->since) / 60, pro_status_name(st));
	if (quiet)
		return;                 /* a flap the ISP never heard of: it heard it was over already */
	if (st == PS_FIXED) {
		pro_note(t->code, 1, local_hour(t->since), t->note, sizeof(t->note));
		snprintf(t->next_step, sizeof(t->next_step), "Nothing to do: AIGW fixed it%s. The note can go to the customer.",
			 t->decision ? " (its decision is in effect)" : "");
		tell(t, now);
	} else if (st == PS_RESOLVED && was_notified) {
		snprintf(t->next_step, sizeof(t->next_step), "Over: the fault cleared after %ld min.", (now - t->since) / 60);
		tell(t, now);
	}
}

static void step(long now)
{
	struct pro_change ch[PRO_TICKETS * 2];
	struct care_day week;
	int i, n, score = -1;

	params();
	gather(now);
	if (cfg.care && care_report_of(7, &week) == 0)
		score = care_score(&week);
	S.area_hold = area_outage();
	n = pro_step(&S, sig, nsig, now, score, &P, ch, (int)(sizeof(ch) / sizeof(ch[0])));
	for (i = 0; i < n; i++) {
		struct pro_ticket *t = ch[i].t;
		struct brief b;

		switch (ch[i].kind) {
		case PC_NEW:
			AG_INFO("proactive: ticket %u %s worth acting on (%d points: %s)", t->id, t->code, t->points, t->why);
			decide(t, now);
			break;
		case PC_REOPENED:
			if (t->notified && now - t->notified_at < P.cooldown_s) {
				/* back soon after: the ISP knows of it; no new word unless it stays (a flap, not a fault) */
				t->status = t->status == PS_FIXED ? PS_FIXING : t->status == PS_RESOLVED ? PS_RAISED : t->status;
				t->quiet = 1;
				t->watch_until = now + 2 * P.clear_s;
				S.c.deduped++;
				AG_INFO("proactive: ticket %u %s back (flap %d): the ISP knows already", t->id, t->code, t->flaps);
			} else {
				t->status = PS_WATCHING;
				decide(t, now);
			}
			break;
		case PC_GRACE_OVER: {
			int fixing = t->status == PS_FIXING;

			if (t->quiet) {
				/* it came back and stays: the ISP hears it isn't over after all (no new ticket) */
				t->quiet = 0;
				t->status = PS_RAISED;
				char was[PRO_EVIDENCE];

				snprintf(was, sizeof(was), "%s", t->next_step);
				snprintf(t->next_step, sizeof(t->next_step), "It came back after it had cleared, and stays. %.500s", was);
				tell(t, now);
				break;
			}
			describe(t, now, &b);
			t->acted = 0;           /* whatever AIGW changed didn't fix it: not "fixed by AIGW" if it clears now */
			raise_it(t, now, &b, fixing ? "AIGW's fix didn't clear it. " : "Self-Healing Wi-Fi didn't clear it. ");
			break;
		}
		case PC_CLEARED:
			closed(t, now);
			break;
		}
	}
	if (n)
		dev_state_save();
}

int proactive_run(void)
{
	if (!cfg.pro)
		return -1;
	step((long)ag_wall_s());
	return 0;
}

static void on_timer(struct uloop_timeout *tm)
{
	proactive_run();
	uloop_timeout_set(tm, (cfg.pro_interval > 0 ? cfg.pro_interval : 60) * 1000);
}

void proactive_init(void)
{
	params();
	if (!cfg.pro) {
		AG_INFO("proactive care before the call: off (proactive.enabled)");
		return;
	}
	timer.cb = on_timer;
	uloop_timeout_set(&timer, (cfg.pro_interval > 0 ? cfg.pro_interval : 60) * 1000);
	AG_INFO("proactive care before the call: a look every %ds, worth acting on at %d points, fixes %s (act mode %s), "
		"at most %d new tickets to the ISP a day", cfg.pro_interval, P.threshold,
		cfg.pro_fix ? "by AIGW" : "never", ag_mode_str(cfg.act_mode), P.max_per_day);
}

/* ---------- hooks ---------- */

void proactive_on_acted(const char *action, const char *target, uint32_t id)
{
	int i;

	for (i = 0; i < PRO_TICKETS; i++) {
		struct pro_ticket *t = &S.t[i];
		int match;

		if (!t->used || !t->open)
			continue;
		match = t->decision == id ||
			(!strcmp(action, "sqm") && !strcmp(t->code, "LINE_FULL")) ||
			(!strcmp(action, "stay24") && !strcmp(t->target, target ? target : "")) ||
			(!strcmp(action, "channel") && t->fault == BF_HOME_WIFI);
		if (match && !t->acted) {
			t->acted = 1;
			if (!t->decision)
				t->decision = id;
			AG_INFO("proactive: ticket %u %s: AIGW acted (decision %u, %s)", t->id, t->code, id, action);
		}
	}
}

void proactive_on_called(void)
{
	pro_on_called(&S);
}

/* ---------- output ---------- */

static void ticket_blob(struct blob_buf *b, const struct pro_ticket *t, int names)
{
	void *o = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "id", t->id);
	blobmsg_add_string(b, "status", pro_status_name(t->status));
	blobmsg_add_u8(b, "open", (uint8_t)t->open);
	blobmsg_add_string(b, "code", t->code);
	blobmsg_add_string(b, "fault", brief_fault_name(t->fault));
	blobmsg_add_string(b, "severity", diag_sev_name(t->sev_max));
	blobmsg_add_u32(b, "points", (uint32_t)t->points);
	blobmsg_add_string(b, "reason", t->why);
	blobmsg_add_u32(b, "devices", (uint32_t)t->devices);
	blobmsg_add_u32(b, "episodes", (uint32_t)t->episodes);
	blobmsg_add_u32(b, "flaps", (uint32_t)t->flaps);
	blobmsg_add_string(b, "source", t->source);
	if (names && t->target[0])
		blobmsg_add_string(b, "target", t->target);
	blobmsg_add_u64(b, "since", (uint64_t)t->since);
	blobmsg_add_u64(b, "opened", (uint64_t)t->opened);
	blobmsg_add_u64(b, "last_seen", (uint64_t)t->last_seen);
	if (t->closed)
		blobmsg_add_u64(b, "closed", (uint64_t)t->closed);
	blobmsg_add_u8(b, "told_isp", (uint8_t)t->notified);
	blobmsg_add_u8(b, "called", (uint8_t)t->called);
	blobmsg_add_u8(b, "acted", (uint8_t)t->acted);
	if (t->decision)
		blobmsg_add_u32(b, "decision", t->decision);
	blobmsg_add_string(b, "summary", t->summary);
	blobmsg_add_string(b, "evidence", t->evidence);
	blobmsg_add_string(b, "next_step", t->next_step);
	blobmsg_add_string(b, "customer_note", t->note);
	blobmsg_close_table(b, o);
}

static int newest_first(const void *a, const void *b)
{
	const struct pro_ticket *x = *(const struct pro_ticket *const *)a, *y = *(const struct pro_ticket *const *)b;

	if (x->open != y->open)
		return y->open - x->open;
	return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

/* the counts, by the same names in agd's state and in what agctl and cortex get */
#define CFU(n, f) { n, offsetof(struct pro_counts, f) }
static const struct { const char *name; size_t off; } CF[] = {
	CFU("tickets", tickets), CFU("told_isp", raised), CFU("fixed_by_aigw", fixed), CFU("proposed", proposed),
	CFU("suppressed", suppressed), CFU("deduplicated", deduped), CFU("flaps", flaps), CFU("called_anyway", called),
	CFU("calls_likely_avoided", avoided), CFU("held_area_outage", area_held),
};

static void counts_add(struct blob_buf *b)
{
	size_t i;

	for (i = 0; i < sizeof(CF) / sizeof(CF[0]); i++)
		blobmsg_add_u32(b, CF[i].name, *(const uint32_t *)((const char *)&S.c + CF[i].off));
	blobmsg_add_u64(b, "since", (uint64_t)S.c.since);
}

static void counts_blob(struct blob_buf *b)
{
	void *c = blobmsg_open_table(b, "counts");

	counts_add(b);
	blobmsg_close_table(b, c);
}

void proactive_blob(struct blob_buf *b, int names)
{
	const struct pro_ticket *list[PRO_TICKETS];
	void *a;
	int i, n = 0;

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.pro);
	blobmsg_add_u8(b, "fix", (uint8_t)cfg.pro_fix);
	blobmsg_add_string(b, "mode", ag_mode_str(cfg.act_mode));
	blobmsg_add_u32(b, "interval_s", (uint32_t)cfg.pro_interval);
	blobmsg_add_u32(b, "threshold", (uint32_t)P.threshold);
	blobmsg_add_u32(b, "max_per_day", (uint32_t)P.max_per_day);
	blobmsg_add_u32(b, "left_today", (uint32_t)pro_budget(&S, (long)ag_wall_s(), &P));
	blobmsg_add_u32(b, "open", (uint32_t)pro_open_count(&S));
	blobmsg_add_u8(b, "area_outage", (uint8_t)area_outage());
	blobmsg_add_u8(b, "names", (uint8_t)cfg.brief_names);  /* the ISP's texts name devices (brief.names) */
	counts_blob(b);
	for (i = 0; i < PRO_TICKETS; i++)
		if (S.t[i].used)
			list[n++] = &S.t[i];
	qsort(list, (size_t)n, sizeof(list[0]), newest_first);
	a = blobmsg_open_array(b, "tickets");
	for (i = 0; i < n; i++)
		ticket_blob(b, list[i], names);
	blobmsg_close_array(b, a);
}

/* for the bridge's report to cortex: the counts and the tickets, never a device */
void proactive_report_blob(struct blob_buf *b)
{
	void *t;

	if (!cfg.pro)
		return;
	t = blobmsg_open_table(b, "proactive");
	proactive_blob(b, 0);
	blobmsg_close_table(b, t);
}

/* ---------- the data model ---------- */

int proactive_ids(uint32_t *out, int max)
{
	int i, j, n = 0;

	for (i = 0; i < PRO_TICKETS && n < max; i++)
		if (S.t[i].used)
			out[n++] = S.t[i].id;
	for (i = 1; i < n; i++)                 /* ascending */
		for (j = i; j > 0 && out[j - 1] > out[j]; j--) {
			uint32_t x = out[j];

			out[j] = out[j - 1];
			out[j - 1] = x;
		}
	return n;
}

int proactive_field(uint32_t id, const char *f, char *buf, size_t n)
{
	const struct pro_ticket *t = pro_find(&S, id);

	if (!t)
		return -1;
	if (!strcmp(f, "Status")) snprintf(buf, n, "%s", pro_status_name(t->status));
	else if (!strcmp(f, "Open")) snprintf(buf, n, "%d", t->open);
	else if (!strcmp(f, "Code")) snprintf(buf, n, "%s", t->code);
	else if (!strcmp(f, "Fault")) snprintf(buf, n, "%s", brief_fault_name(t->fault));
	else if (!strcmp(f, "Severity")) snprintf(buf, n, "%s", diag_sev_name(t->sev_max));
	else if (!strcmp(f, "Points")) snprintf(buf, n, "%d", t->points);
	else if (!strcmp(f, "Reason")) snprintf(buf, n, "%s", t->why);
	else if (!strcmp(f, "Devices")) snprintf(buf, n, "%d", t->devices);
	else if (!strcmp(f, "Episodes")) snprintf(buf, n, "%d", t->episodes);
	else if (!strcmp(f, "Flaps")) snprintf(buf, n, "%d", t->flaps);
	else if (!strcmp(f, "Since")) snprintf(buf, n, "%ld", t->since);
	else if (!strcmp(f, "Opened")) snprintf(buf, n, "%ld", t->opened);
	else if (!strcmp(f, "LastSeen")) snprintf(buf, n, "%ld", t->last_seen);
	else if (!strcmp(f, "Closed")) snprintf(buf, n, "%ld", t->closed);
	else if (!strcmp(f, "ToldISP")) snprintf(buf, n, "%d", t->notified);
	else if (!strcmp(f, "CustomerCalled")) snprintf(buf, n, "%d", t->called);
	else if (!strcmp(f, "Decision")) snprintf(buf, n, "%u", t->decision);
	else if (!strcmp(f, "Summary")) snprintf(buf, n, "%s", t->summary);
	else if (!strcmp(f, "Evidence")) snprintf(buf, n, "%s", t->evidence);
	else if (!strcmp(f, "NextStep")) snprintf(buf, n, "%s", t->next_step);
	else if (!strcmp(f, "CustomerNote")) snprintf(buf, n, "%s", t->note);
	else
		return -1;
	return 0;
}

int proactive_count(const char *f, char *buf, size_t n)
{
	if (!strcmp(f, "Enable")) snprintf(buf, n, "%d", cfg.pro);
	else if (!strcmp(f, "Fix")) snprintf(buf, n, "%d", cfg.pro_fix);
	else if (!strcmp(f, "OpenTickets")) snprintf(buf, n, "%d", pro_open_count(&S));
	else if (!strcmp(f, "Tickets")) snprintf(buf, n, "%u", S.c.tickets);
	else if (!strcmp(f, "ToldISP")) snprintf(buf, n, "%u", S.c.raised);
	else if (!strcmp(f, "FixedByAIGW")) snprintf(buf, n, "%u", S.c.fixed);
	else if (!strcmp(f, "Proposed")) snprintf(buf, n, "%u", S.c.proposed);
	else if (!strcmp(f, "Suppressed")) snprintf(buf, n, "%u", S.c.suppressed);
	else if (!strcmp(f, "Deduplicated")) snprintf(buf, n, "%u", S.c.deduped);
	else if (!strcmp(f, "CalledAnyway")) snprintf(buf, n, "%u", S.c.called);
	else if (!strcmp(f, "CallsLikelyAvoided")) snprintf(buf, n, "%u", S.c.avoided);
	else if (!strcmp(f, "HeldForAreaOutage")) snprintf(buf, n, "%u", S.c.area_held);
	else if (!strcmp(f, "MaxPerDay")) snprintf(buf, n, "%d", P.max_per_day);
	else if (!strcmp(f, "Since")) snprintf(buf, n, "%ld", S.c.since);
	else
		return -1;
	return 0;
}

/* ---------- state ---------- */

/* a ticket's fields as kept in agd's state (ints and uint32_t alike are int-sized) */
enum { F_INT, F_LONG, F_STR };
#define TFI(n, f) { n, offsetof(struct pro_ticket, f), F_INT, 0 }
#define TFL(n, f) { n, offsetof(struct pro_ticket, f), F_LONG, 0 }
#define TFS(n, f) { n, offsetof(struct pro_ticket, f), F_STR, sizeof(((struct pro_ticket *)0)->f) }
static const struct { const char *name; size_t off; int kind; size_t size; } TF[] = {
	TFI("id", id), TFI("open", open), TFS("key", key), TFS("code", code), TFS("target", target),
	TFS("source", source), TFI("fault", fault), TFI("sev", sev_max), TFI("status", status), TFI("points", points),
	TFS("why", why), TFI("devices", devices), TFI("episodes", episodes), TFI("flaps", flaps),
	TFI("notified", notified), TFI("quiet", quiet), TFI("called", called), TFI("acted", acted),
	TFI("decision", decision), TFL("since", since), TFL("opened", opened), TFL("last_seen", last_seen),
	TFL("closed", closed), TFL("notified_at", notified_at), TFL("watch_until", watch_until),
	TFS("summary", summary), TFS("evidence", evidence), TFS("next_step", next_step), TFS("note", note),
};

void proactive_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "proactive"), *a;
	int i, k;

	blobmsg_add_u32(b, "next_id", S.next_id);
	counts_add(b);
	a = blobmsg_open_array(b, "sent");
	for (i = 0; i < PRO_SENT; i++)
		if (S.sent[i])
			blobmsg_add_u64(b, NULL, (uint64_t)S.sent[i]);
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "list");
	for (i = 0; i < PRO_TICKETS; i++) {
		const struct pro_ticket *x = &S.t[i];
		void *o;

		if (!x->used)
			continue;
		o = blobmsg_open_table(b, NULL);
		for (k = 0; k < (int)(sizeof(TF) / sizeof(TF[0])); k++) {
			const char *p = (const char *)x + TF[k].off;

			if (TF[k].kind == F_STR)
				blobmsg_add_string(b, TF[k].name, p);
			else if (TF[k].kind == F_LONG)
				blobmsg_add_u64(b, TF[k].name, (uint64_t)*(const long *)p);
			else
				blobmsg_add_u32(b, TF[k].name, (uint32_t)*(const int *)p);
		}
		blobmsg_close_table(b, o);
	}
	blobmsg_close_array(b, a);
	blobmsg_close_table(b, t);
}

static long jnum(struct blob_attr *a)
{
	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return (long)blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return (long)blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	}
	return 0;
}

static void str(char *dst, size_t n, struct blob_attr *a)
{
	snprintf(dst, n, "%s", a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "");
}

static void ticket_load(struct blob_attr *tbl)
{
	struct pro_ticket *x = NULL;
	struct blob_attr *cur;
	size_t rem;
	int i;

	for (i = 0; i < PRO_TICKETS; i++)
		if (!S.t[i].used) {
			x = &S.t[i];
			break;
		}
	if (!x)
		return;
	memset(x, 0, sizeof(*x));
	blobmsg_for_each_attr(cur, tbl, rem) {
		for (i = 0; i < (int)(sizeof(TF) / sizeof(TF[0])); i++)
			if (!strcmp(blobmsg_name(cur), TF[i].name)) {
				char *p = (char *)x + TF[i].off;

				if (TF[i].kind == F_STR)
					str(p, TF[i].size, cur);
				else if (TF[i].kind == F_LONG)
					*(long *)p = jnum(cur);
				else
					*(int *)p = (int)jnum(cur);
			}
	}
	x->sev = x->sev_max;
	if (!x->id || !x->key[0] || !x->code[0] || x->status < 0 || x->status >= PS_N)
		return;
	x->used = 1;
	if (x->id > S.next_id)
		S.next_id = x->id;
}

void proactive_load(struct blob_attr *t)
{
	struct blob_attr *cur, *e;
	size_t rem, r2;
	int k = 0;

	memset(&S, 0, sizeof(S));
	blobmsg_for_each_attr(cur, t, rem) {
		const char *n = blobmsg_name(cur);

		int j;

		for (j = 0; j < (int)(sizeof(CF) / sizeof(CF[0])); j++)
			if (!strcmp(n, CF[j].name))
				*(uint32_t *)((char *)&S.c + CF[j].off) = (uint32_t)jnum(cur);
		if (!strcmp(n, "next_id")) S.next_id = (uint32_t)jnum(cur);
		else if (!strcmp(n, "since")) S.c.since = jnum(cur);
		else if (!strcmp(n, "sent") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(e, cur, r2) {
				if (k < PRO_SENT)
					S.sent[k++] = jnum(e);
			}
		} else if (!strcmp(n, "list") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_for_each_attr(e, cur, r2) {
				if (blobmsg_type(e) == BLOBMSG_TYPE_TABLE)
					ticket_load(e);
			}
		}
	}
	S.sent_head = k % PRO_SENT;
	AG_INFO("proactive: restored %d open ticket%s, %u calls likely avoided so far", pro_open_count(&S),
		pro_open_count(&S) == 1 ? "" : "s", S.c.avoided);
}
