/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW help-desk assistant: what the gateway knows about a period, as one
 * short answer for the ISP's support agent (see ag/brief.h).
 *
 * Which verdict decides: for a period that ends now, a fresh check that found
 * something (it is happening now), else proactive care's open problem, else
 * the timeline's verdict. For a period in the past, the timeline's verdict
 * (the fresh check is only "and now"). Two sources that point to the same
 * fault make it more certain; sources that disagree make it less.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ag/brief.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/why.h"

#define NOW_SLACK   120         /* a period ending this close to now is "now" */
#define LASTED_S    1800        /* a line problem that lasted this long is not a blip */

const char *brief_fault_name(int f)
{
	static const char *const n[BF_N] = { "NONE", "LINE", "HOME_WIFI", "DEVICE", "OUTSIDE", "UNKNOWN" };

	return f >= 0 && f < BF_N ? n[f] : "UNKNOWN";
}

const char *brief_visit_name(int v)
{
	return v == BV_YES ? "Yes" : v == BV_IF_PERSISTS ? "IfPersists" : "No";
}

const char *brief_conf_name(int c)
{
	return c == BC_HIGH ? "high" : c == BC_MEDIUM ? "medium" : "low";
}

int brief_fault_of(const char *code)
{
	static const struct { const char *code; int fault; } t[] = {
		{ "OK", BF_NONE }, { "LINE_FULL", BF_NONE }, { "INTERNET_OK", BF_NONE }, { "DNS_OK", BF_NONE },
		{ "WIFI_OK", BF_NONE }, { "AIGW_CHANGED", BF_NONE }, { "FIRMWARE_CHANGED", BF_NONE },
		{ "NO_ROUTE", BF_LINE }, { "LINE_DOWN", BF_LINE }, { "ISP_DOWN", BF_LINE }, { "LINE_LOSS", BF_LINE },
		{ "ISP_LOSS", BF_LINE }, { "LINE_SLOW", BF_LINE }, { "ISP_SLOW", BF_LINE }, { "DNS_FAILING", BF_LINE },
		{ "DNS_SLOW", BF_LINE }, { "LINE_SLOW_SPEED", BF_LINE },
		{ "WIFI_COVERAGE", BF_HOME_WIFI }, { "WIFI_CONGESTION", BF_HOME_WIFI }, { "WIFI_HOPPING", BF_HOME_WIFI },
		{ "WIFI_RADIO", BF_HOME_WIFI }, { "WIFI_PROBLEM", BF_HOME_WIFI }, { "MESH_BACKHAUL", BF_HOME_WIFI },
		{ "WIFI_DEVICE", BF_DEVICE }, { "WIFI_DROPS", BF_DEVICE }, { "DEVICE_OFFLINE", BF_DEVICE },
		{ "DEVICE_BLOCKED", BF_DEVICE }, { "DEVICE_LIMITED", BF_DEVICE },
		{ "OUTSIDE_SERVICE", BF_OUTSIDE }, { "AREA_OUTAGE", BF_LINE },
	};
	size_t i;

	for (i = 0; code && i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].code, code))
			return t[i].fault;
	return BF_UNKNOWN;              /* GATEWAY_OFF, NO_DATA, NOT_RECORDED, DEVICE_UNKNOWN, ... */
}

static int bad(const struct brief_finding *f)
{
	return f->code && f->sev >= DS_WARN;
}

/* ---------- words ---------- */

static void line_add(struct brief_line *l, int *n, long t, const char *src, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));

static void line_add(struct brief_line *l, int *n, long t, const char *src, const char *fmt, ...)
{
	va_list ap;

	if (*n >= BRIEF_LINES)
		return;
	l += (*n)++;
	l->t = t;
	l->source = src;
	va_start(ap, fmt);
	vsnprintf(l->text, sizeof(l->text), fmt, ap);
	va_end(ap);
}

/* the first sentence of a text (up to ". " or the end) */
static void first_sentence(const char *s, char *buf, size_t n)
{
	const char *e = s ? strstr(s, ". ") : NULL;
	size_t k = !s ? 0 : e ? (size_t)(e - s) : strlen(s);

	if (k && s[k - 1] == '.')
		k--;
	if (k >= n)
		k = n - 1;
	memcpy(buf, s ? s : "", k);
	buf[k] = '\0';
}

static void span(long a, long b, long now, char *buf, size_t n)
{
	char x[16], y[16];

	if (!a) {
		buf[0] = '\0';
		return;
	}
	why_clock(a, now, x, sizeof(x));
	if (!b || b - a < 90) {
		snprintf(buf, n, "%s", x);
		return;
	}
	why_clock(b, now, y, sizeof(y));
	snprintf(buf, n, "%s-%s", x, y);
}

static const char *fault_words(int f)
{
	switch (f) {
	case BF_LINE: return "The ISP's line or network";
	case BF_HOME_WIFI: return "Wi-Fi in the home";
	case BF_DEVICE: return "One device, not the line";
	case BF_OUTSIDE: return "A service beyond the ISP";
	case BF_NONE: return "Nothing wrong with the line or the Wi-Fi";
	}
	return "Not known";
}

/* what to tell the customer */
static const char *tell_of(const char *code, int fault)
{
	static const struct { const char *code, *tell; } t[] = {
		{ "LINE_DOWN", "Please restart the modem or fibre box and check its cable; we are checking the line from our side." },
		{ "NO_ROUTE", "Please check that the modem or fibre box is on and connected to the gateway." },
		{ "ISP_DOWN", "Your home equipment is fine; there is a problem in our network and we are on it." },
		{ "ISP_LOSS", "Your home equipment is fine; part of our network is losing data and we are on it." },
		{ "ISP_SLOW", "Your home equipment is fine; our network is busy at this time and we are looking at it." },
		{ "LINE_LOSS", "Your line is losing data; we are checking it from our side." },
		{ "LINE_SLOW", "Your line answers slowly; we are checking it from our side." },
		{ "LINE_SLOW_SPEED", "Your line delivers less than its speed; we are checking it from our side." },
		{ "DNS_FAILING", "Websites fail to load by name; we are fixing the name service." },
		{ "DNS_SLOW", "Websites are slow to start loading; we are looking at the name service." },
		{ "LINE_FULL", "Your connection was fully used by downloads at that time; that slows everything else. "
			       "Smart queueing keeps calls and games smooth while that happens." },
		{ "WIFI_COVERAGE", "The Wi-Fi signal is weak where the device is: move it closer to the gateway, or "
				   "a mesh node or extender would help." },
		{ "WIFI_CONGESTION", "Nearby networks crowd your Wi-Fi channel; the gateway is moving to a quieter one by itself." },
		{ "WIFI_HOPPING", "The device is at the edge of the fast Wi-Fi band; the gateway can keep it on the "
				  "steadier band." },
		{ "WIFI_RADIO", "Please restart the gateway; if Wi-Fi stays down, it may need replacing." },
		{ "MESH_BACKHAUL", "The Wi-Fi extender is too far from the gateway, so its own link is weak and slows the "
				   "devices behind it: move it a room closer, or connect it with a cable." },
		{ "WIFI_PROBLEM", "There is a Wi-Fi problem in the home; the gateway is working on it." },
		{ "WIFI_DEVICE", "Please restart Wi-Fi on that device, or the device itself; an update may help." },
		{ "WIFI_DROPS", "That device keeps dropping off Wi-Fi: restart its Wi-Fi, turn off its power saving, or "
				"move it closer." },
		{ "DEVICE_OFFLINE", "That device is not connected: check it is on and joined to your Wi-Fi." },
		{ "DEVICE_BLOCKED", "The gateway's security kept that device off the network; we can check why with you." },
		{ "DEVICE_LIMITED", "The gateway slowed that device on purpose; we can check why with you." },
		{ "GATEWAY_OFF", "The gateway was off or restarting then: please check its power supply." },
		{ "OUTSIDE_SERVICE", "Your line and Wi-Fi were fine; the app or service you used had the problem." },
	};
	size_t i;

	for (i = 0; code && i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].code, code))
			return t[i].tell;
	if (fault == BF_NONE)
		return "Your line and Wi-Fi look fine; which app or website is slow, and on which device?";
	return "We could not see enough yet; please tell us when it happens next, or let us check again.";
}

/* ---------- the evidence ---------- */

static void finding_line(struct brief *o, const char *src, const char *label, const struct brief_finding *f, long now)
{
	char when[40];

	span(f->since, f->until, now, when, sizeof(when));
	line_add(o->ev, &o->nev, f->since, src, "%s%s%s: %s (%s)", label, *when ? " " : "", when,
		 f->text && *f->text ? f->text : f->code, f->code);
}

static void evidence(const struct brief_in *in, struct brief *o)
{
	int i, shown = 0;

	if (in->have_check) {
		char nums[120] = "";

		if (in->check_inet_ms >= 0 || in->check_loss_pct >= 0) {
			char inet[24] = "no answer", gw[24] = "no answer", dns[24] = "no answer";

			if (in->check_inet_ms >= 0)
				snprintf(inet, sizeof(inet), "%d ms", in->check_inet_ms);
			if (in->check_gw_ms >= 0)
				snprintf(gw, sizeof(gw), "%d ms", in->check_gw_ms);
			if (in->check_dns_ms >= 0)
				snprintf(dns, sizeof(dns), "%d ms", in->check_dns_ms);
			snprintf(nums, sizeof(nums), " [internet %s, %d%% lost; next hop %s; DNS %s]", inet,
				 in->check_loss_pct < 0 ? 0 : in->check_loss_pct, gw, dns);
		}
		line_add(o->ev, &o->nev, in->now, "now", "Checked just now: %s (%s)%s", in->check.text && *in->check.text ?
			 in->check.text : in->check.code, in->check.code ? in->check.code : "OK", nums);
	}
	if (in->have_why) {
		finding_line(o, "timeline", "Timeline", &in->why, in->to);
		for (i = 0; i < in->nwhy_more && shown < 3; i++)
			if (in->why_more[i].code && in->why_more[i].sev >= DS_WARN &&
			    strcmp(in->why_more[i].code, in->why.code ? in->why.code : "")) {
				finding_line(o, "timeline", "Also", &in->why_more[i], in->to);
				shown++;
			}
		if (in->recorded_pct < 90)
			line_add(o->ev, &o->nev, 0, "timeline", "Only %d%% of the period was recorded.", in->recorded_pct);
	} else
		line_add(o->ev, &o->nev, 0, "timeline", "No timeline for this period (why.enabled, or not recorded).");
	if (in->coverage_line && (!strcmp(o->code, "WIFI_COVERAGE") || !strcmp(o->code, "WIFI_DROPS")))
		line_add(o->ev, &o->nev, 0, "coverage", "Coverage: %s", in->coverage_line);
	if (in->care_open) {
		char when[16];

		why_clock(in->care.since, in->now, when, sizeof(when));
		line_add(o->ev, &o->nev, in->care.since, "care", "Proactive care: a problem open since %s, %s: %s (%s)", when,
			 in->care_reported ? "reported to the ISP" : "not reported (AIGW's to fix)",
			 in->care.text && *in->care.text ? in->care.text : diag_advice(in->care.code),
			 in->care.code ? in->care.code : "");
	}
	if (in->warning)
		line_add(o->ev, &o->nev, 0, "care", "Early warning: %s", in->warning);
	for (i = 0; i < in->nradio; i++) {
		const struct brief_radio *r = &in->radio[i];

		if (r->status != HEAL_DEGRADED && r->status != HEAL_POOR)
			continue;
		line_add(o->ev, &o->nev, r->since, "wifi", "Wi-Fi %s now %s (%s): %s", r->name ? r->name : "",
			 heal_status_name((enum heal_status)r->status), heal_cause_name((enum heal_cause)r->cause),
			 r->text ? r->text : "");
	}
	if (in->chan_plan)
		line_add(o->ev, &o->nev, 0, "wifi", "Channels, learned by the hour: %s", in->chan_plan);
	for (i = 0; i < in->nmesh; i++)
		if (in->mesh[i].code && in->mesh[i].since < in->to)
			line_add(o->ev, &o->nev, in->mesh[i].since, "mesh", "Mesh Wi-Fi now: %s (%s)",
				 in->mesh[i].text ? in->mesh[i].text : "", in->mesh[i].code);
	if (in->speed_at && in->speed_mbps > 0) {
		char when[16], line[48] = "";

		why_clock(in->speed_at, in->now, when, sizeof(when));
		if (in->line_mbps > 0)
			snprintf(line, sizeof(line), " of %.0f sold", in->line_mbps);
		line_add(o->ev, &o->nev, in->speed_at, "speed", "Speed test %s: %.0f Mbit/s%s%s", when, in->speed_mbps, line,
			 in->speed_low_nights ? (in->speed_low_nights == 1 ? "; low on 1 night of 7" : "") : "");
		if (in->speed_low_nights > 1)
			snprintf(o->ev[o->nev - 1].text + strlen(o->ev[o->nev - 1].text),
				 sizeof(o->ev[0].text) - strlen(o->ev[o->nev - 1].text), "; low on %d nights of 7",
				 in->speed_low_nights);
	}
	if (in->calls)
		line_add(o->ev, &o->nev, 0, "calls", "%d call%s or game%s in the period, %d rough.", in->calls,
			 in->calls == 1 ? "" : "s", in->calls == 1 ? "" : "s", in->calls_rough);
	if (in->upgrade && strcmp(in->upgrade, "None") && in->upgrade_at && in->upgrade_at >= in->from - 86400) {
		char when[16];

		why_clock(in->upgrade_at, in->now, when, sizeof(when));
		line_add(o->ev, &o->nev, in->upgrade_at, "upgrade", "Firmware %s to %s at %s: %s.",
			 in->upgrade_from ? in->upgrade_from : "?", in->upgrade_to ? in->upgrade_to : "?", when, in->upgrade);
	}
	if (in->score_week >= 0 && o->nev < BRIEF_LINES)
		line_add(o->ev, &o->nev, 0, "care", "Experience score this week: %d%%.", in->score_week);
}

/* what AIGW already did */
static void done(const struct brief_in *in, struct brief *o)
{
	int i;

	for (i = 0; i < in->ndec; i++) {
		const struct brief_decision *d = &in->dec[i];
		char when[16];

		if (!d->by_aigw || d->state == BD_PROPOSED)
			continue;
		why_clock(d->t, in->now, when, sizeof(when));
		line_add(o->done, &o->ndone, d->t, "decision", "%s %s %s (decision %u, %s)%s%s", when,
			 !strcmp(d->action, "channel") ? "moved Wi-Fi:" : !strcmp(d->action, "stay24") ? "kept on 2.4 GHz:" :
			 !strcmp(d->action, "sqm") ? "turned on smart queueing:" :
			 !strcmp(d->action, "meshsteer") ? "moved a device to a better access point:" :
			 !strcmp(d->action, "meshchan") ? "moved an extender's channel:" : d->action,
			 d->words ? d->words : "", d->id, d->state == BD_ACTIVE ? "in effect" : d->state == BD_KEPT ? "kept" :
			 "ended", d->reason && *d->reason ? ": " : "", d->reason ? d->reason : "");
	}
	if (in->sqm && (!strcmp(in->sqm, "On") || !strcmp(in->sqm, "Trial")))
		line_add(o->done, &o->ndone, 0, "sqm", "Smart queueing is %s on the line.", !strcmp(in->sqm, "On") ? "on" : "on trial");
	if (in->care_open && in->care_reported)
		line_add(o->done, &o->ndone, in->care.since, "care", "Told the ISP about the open problem (ProblemDetected!).");
	if (in->upgrade && in->upgrade_at >= in->from - 86400 &&
	    (!strcmp(in->upgrade, "RolledBack") || !strcmp(in->upgrade, "RollingBack")))
		line_add(o->done, &o->ndone, in->upgrade_at, "upgrade", "Went back from firmware %s to %s: the home was worse on it.",
			 in->upgrade_to ? in->upgrade_to : "?", in->upgrade_from ? in->upgrade_from : "?");
}

/* a decision of an action that is waiting for a yes; NULL none */
static const struct brief_decision *waiting(const struct brief_in *in, const char *action)
{
	int i;

	for (i = 0; i < in->ndec; i++)
		if (in->dec[i].state == BD_PROPOSED && (!action || !strcmp(in->dec[i].action, action)))
			return &in->dec[i];
	return NULL;
}

static const struct brief_decision *live(const struct brief_in *in, const char *const *actions)
{
	int i, k;

	for (i = 0; i < in->ndec; i++)
		for (k = 0; actions[k]; k++)
			if ((in->dec[i].state == BD_ACTIVE || in->dec[i].state == BD_KEPT) && !strcmp(in->dec[i].action, actions[k]))
				return &in->dec[i];
	return NULL;
}

static void fix(const struct brief_in *in, struct brief *o)
{
	struct brief_fix *x = &o->fix;
	const struct brief_decision *d;
	const char *c = o->code;

	memset(x, 0, sizeof(*x));
	if (!strcmp(c, "LINE_FULL")) {
		if (in->sqm && !strcmp(in->sqm, "Proposed") && in->sqm_decision) {
			x->action = "approve";
			x->decision = in->sqm_decision;
			snprintf(x->text, sizeof(x->text), "Smart queueing waits for a yes (decision %u): approve it.", x->decision);
		} else if (in->sqm && (!strcmp(in->sqm, "Off"))) {
			x->action = "sqm";
			snprintf(x->text, sizeof(x->text), "Turn on smart queueing (SQM): calls and games stay smooth while "
				 "the line is full. Needs the customer's yes.");
		} else if (in->sqm && !strcmp(in->sqm, "Tried"))
			snprintf(x->text, sizeof(x->text), "Smart queueing was tried here and didn't help: a faster plan would.");
		else
			snprintf(x->text, sizeof(x->text), "Nothing to repair; a faster plan if this happens often.");
	} else if (!strcmp(c, "WIFI_HOPPING") || !strcmp(c, "WIFI_COVERAGE")) {
		static const char *const st[] = { "stay24", NULL };

		if ((d = waiting(in, "meshsteer")) || (d = waiting(in, "stay24")) || (d = waiting(in, "channel"))) {
			x->action = "approve";
			x->decision = d->id;
			snprintf(x->text, sizeof(x->text), "A fix waits for a yes (decision %u, %s %s): approve it.", d->id,
				 d->action, d->words ? d->words : "");
		} else if (!strcmp(c, "WIFI_HOPPING") && in->host_target && *in->host_target && !live(in, st)) {
			x->action = "stay24";
			snprintf(x->target, sizeof(x->target), "%s", in->host_target);
			snprintf(x->text, sizeof(x->text), "Keep %s on 2.4 GHz for a day (steadier at the edge of 5 GHz). "
				 "Needs the customer's yes.", in->host ? in->host : "the device");
		} else if (!strcmp(c, "WIFI_COVERAGE") && in->coverage && *in->coverage)
			snprintf(x->text, sizeof(x->text), "No remote fix: %s", in->coverage);
		else
			snprintf(x->text, sizeof(x->text), "No remote fix: a mesh node or extender, or the device closer.");
	} else if (!strcmp(c, "MESH_BACKHAUL")) {
		if ((d = waiting(in, "meshsteer"))) {
			x->action = "approve";
			x->decision = d->id;
			snprintf(x->text, sizeof(x->text), "A device waits to move off the extender (decision %u, %s): approve "
				 "it; for good, the extender closer to the gateway or on a cable.", d->id, d->words ? d->words : "");
		} else
			snprintf(x->text, sizeof(x->text), "No remote fix: the extender a room closer to the gateway, or on a "
				 "cable (Ethernet, powerline or MoCA).");
	} else if (!strcmp(c, "WIFI_CONGESTION")) {
		if ((d = waiting(in, "channel")) || (d = waiting(in, "meshchan"))) {
			x->action = "approve";
			x->decision = d->id;
			snprintf(x->text, sizeof(x->text), "A quieter channel waits for a yes (decision %u, %s): approve it.", d->id,
				 d->words ? d->words : "");
		} else if (in->chan_plan)
			snprintf(x->text, sizeof(x->text), "AIGW learns each channel by the hour and moves only when one is "
				 "clearly better in the busy hours: %.170s", in->chan_plan);
		else
			snprintf(x->text, sizeof(x->text), "AIGW tries quieter channels by itself; nothing to do remotely.");
	} else if (!strcmp(c, "DEVICE_BLOCKED") || !strcmp(c, "DEVICE_LIMITED")) {
		static const char *const blk[] = { "quarantine", "disconnect", NULL };
		static const char *const lim[] = { "throttle", "deprioritize", NULL };

		if ((d = live(in, !strcmp(c, "DEVICE_BLOCKED") ? blk : lim))) {
			x->action = "undo";
			x->decision = d->id;
			snprintf(x->text, sizeof(x->text), "Undo decision %u (%s %s) if the device is safe; its reason: %s",
				 d->id, d->action, d->words ? d->words : "", d->reason && *d->reason ? d->reason : "see the decision on the gateway");
		} else
			snprintf(x->text, sizeof(x->text), "Check the decision's reason with the customer.");
	} else if (!strcmp(c, "LINE_DOWN") || !strcmp(c, "NO_ROUTE"))
		snprintf(x->text, sizeof(x->text), "No remote fix from the gateway: the customer restarts the modem or "
			 "fibre box; check the line from the ISP's side.");
	else if (o->fault == BF_LINE)
		snprintf(x->text, sizeof(x->text), "No fix on the gateway: the ISP's side (%s).", diag_advice(c));
	else if (o->fault == BF_DEVICE && !strcmp(c, "WIFI_DROPS") && in->coverage && *in->coverage)
		snprintf(x->text, sizeof(x->text), "No remote fix: it drops when it is weak; %s", in->coverage);
	else if (o->fault == BF_DEVICE)
		snprintf(x->text, sizeof(x->text), "No remote fix: the device itself (restart its Wi-Fi, update it).");
	else
		snprintf(x->text, sizeof(x->text), "Nothing to fix remotely.");
	x->possible = x->action != NULL;
}

/* whether a technician should go out: only for the line itself, and only when it lasted or two sources agree */
static void visit(const struct brief_in *in, struct brief *o, int agree, long lasted)
{
	const char *c = o->code;
	int firmware = in->upgrade && in->upgrade_at >= in->from - 86400 &&
		       (!strcmp(in->upgrade, "Worse") || !strcmp(in->upgrade, "RollingBack") || !strcmp(in->upgrade, "RolledBack"));
	int solid = agree || lasted >= LASTED_S || (in->care_open && in->care_reported);

	o->visit = BV_NO;
	if (o->fault != BF_LINE) {
		snprintf(o->visit_why, sizeof(o->visit_why), "%s",
			 o->fault == BF_HOME_WIFI ? "No: Wi-Fi in the home is fixed remotely or by moving things, not by a line repair." :
			 o->fault == BF_DEVICE ? "No: one device, not the line." :
			 o->fault == BF_OUTSIDE ? "No: the line and Wi-Fi were fine." :
			 o->fault == BF_NONE ? "No: nothing wrong on the line." :
			 "No: not enough evidence; check again when it happens.");
		return;
	}
	if (firmware) {
		snprintf(o->visit_why, sizeof(o->visit_why), "No: a firmware upgrade just before made the home worse; that, not the "
			 "line, is the likely cause.");
		return;
	}
	if (!strncmp(c, "ISP_", 4) || !strncmp(c, "DNS_", 4)) {
		snprintf(o->visit_why, sizeof(o->visit_why), "No: the home is fine up to the ISP; this is for the network team.");
		return;
	}
	if (!strcmp(c, "LINE_SLOW_SPEED")) {
		o->visit = in->speed_low_nights >= 2 ? BV_YES : BV_IF_PERSISTS;
		snprintf(o->visit_why, sizeof(o->visit_why), in->speed_low_nights >= 2 ?
			 "Yes: the nightly speed test was below the line's speed on %d nights; check the sync rate first." :
			 "Only if it persists: the speed was low once (%d night of 7); check the sync rate first.",
			 in->speed_low_nights);
		return;
	}
	if (!strcmp(c, "LINE_LOSS") && solid) {
		o->visit = BV_YES;
		snprintf(o->visit_why, sizeof(o->visit_why), "Yes: the next hop itself loses packets%s, so the line to the home "
			 "is the likely fault; check it from the ISP's side first.",
			 agree ? " (both now and in the timeline)" : lasted >= LASTED_S ? " and it lasted" : " (reported by care)");
		return;
	}
	if ((!strcmp(c, "LINE_DOWN") || !strcmp(c, "NO_ROUTE")) && solid) {
		o->visit = BV_IF_PERSISTS;
		snprintf(o->visit_why, sizeof(o->visit_why), "Only if it persists after the customer restarts the modem and checks "
			 "its cable, and the ISP's side shows the line down.");
		return;
	}
	o->visit = solid ? BV_IF_PERSISTS : BV_NO;
	snprintf(o->visit_why, sizeof(o->visit_why), "%s", solid ?
		 "Only if it persists: check the line from the ISP's side first." :
		 "No: it was short and seen once; check the line from the ISP's side if it comes back.");
}

static void area(const struct brief_in *in, struct brief *o, int ends_now);

void brief_judge(const struct brief_in *in, struct brief *o)
{
	const struct brief_finding *main = NULL;
	int ends_now = in->to >= in->now - NOW_SLACK, agree = 0, conflict = 0, sources = 0, f;
	long lasted = 0;
	char first[240];

	memset(o, 0, sizeof(*o));
	o->code = "OK";
	/* which verdict decides */
	if (ends_now && in->have_check && bad(&in->check)) {
		main = &in->check;
		o->current = 1;
	} else if (ends_now && in->care_open && bad(&in->care)) {
		main = &in->care;
		o->current = 1;
	} else if (in->have_why && bad(&in->why))
		main = &in->why;
	else if (!ends_now && in->care_open && bad(&in->care) && in->care.since < in->to)
		main = &in->care;
	/* nothing else: an extender whose backhaul slows the devices behind it (mesh Wi-Fi, now) */
	for (f = 0; !main && f < in->nmesh; f++)
		if (bad(&in->mesh[f]) && in->mesh[f].since < in->to) {
			main = &in->mesh[f];
			o->current = ends_now;
		}
	if (main) {
		o->code = main->code;
		f = brief_fault_of(main->code);
		/* do the other sources point the same way */
		if (in->have_check && bad(&in->check) && &in->check != main)
			brief_fault_of(in->check.code) == f ? agree++ : conflict++;
		if (in->have_why && bad(&in->why) && &in->why != main)
			brief_fault_of(in->why.code) == f ? agree++ : conflict++;
		if (in->care_open && bad(&in->care) && &in->care != main)
			brief_fault_of(in->care.code) == f ? agree++ : conflict++;
		if (f == BF_HOME_WIFI) {        /* a weak extender backhaul backs a Wi-Fi verdict (not counted against others) */
			int k;

			for (k = 0; k < in->nmesh; k++)
				if (bad(&in->mesh[k]) && &in->mesh[k] != main) {
					agree++;
					break;
				}
		}
		if (in->have_check && !bad(&in->check) && main != &in->check && ends_now && f == BF_LINE)
			conflict++;     /* the line is fine now: it was, not is */
		if (!strcmp(main->code, "LINE_SLOW_SPEED") && in->speed_low_nights >= 2)
			agree++;        /* the nightly speed test, night after night */
		if (main->since && main->until)
			lasted = main->until - main->since;
		else if (main->since)
			lasted = in->now - main->since;
		sources = 1 + agree;
	} else {
		/* nothing found: is there enough to say so */
		int known = (in->have_check && in->check.code) || (in->have_why && in->recorded_pct >= 50 && in->why.code &&
								 brief_fault_of(in->why.code) == BF_NONE);

		if (in->have_why && in->why.code && !strcmp(in->why.code, "LINE_FULL"))
			o->code = "LINE_FULL";
		else if (in->have_why && in->why.code && brief_fault_of(in->why.code) == BF_UNKNOWN &&
			 strcmp(in->why.code, "NO_DATA") && in->why.sev >= DS_WARN)
			o->code = in->why.code;
		else if (known && in->calls_rough && in->calls_rough * 2 >= in->calls)
			o->code = "OUTSIDE_SERVICE";
		else if (!known)
			o->code = "NO_DATA";
		f = brief_fault_of(o->code);
		sources = known ? 1 + (in->have_check && in->have_why && in->recorded_pct >= 50) : 0;
	}
	/* LINE_FULL as the timeline's verdict: the home used the line; not the ISP's fault */
	if (main && !strcmp(o->code, "LINE_FULL"))
		f = BF_NONE;
	o->fault = f;
	o->confidence = f == BF_UNKNOWN ? BC_LOW : conflict ? BC_LOW : sources >= 2 ? BC_HIGH :
			(main && (main->sev >= DS_PROBLEM || lasted >= 300)) || (!main && sources) ? BC_MEDIUM : BC_LOW;
	if (!main && f == BF_OUTSIDE)
		o->confidence = BC_LOW;         /* the gateway can't see the service: only that the home was fine */
	if (in->have_why && in->recorded_pct < 50 && !o->current && o->confidence > BC_LOW)
		o->confidence--;

	/* the verdict: one sentence */
	if (main)
		first_sentence(main->text && *main->text ? main->text : main->code, first, sizeof(first));
	else if (!strcmp(o->code, "LINE_FULL") || brief_fault_of(o->code) == BF_UNKNOWN)
		first_sentence(in->have_why && in->why.text ? in->why.text : "There is no record of that period", first,
			       sizeof(first));
	else if (!strcmp(o->code, "OUTSIDE_SERVICE"))
		snprintf(first, sizeof(first), "%d of %d calls or games were rough while the line and Wi-Fi were fine",
			 in->calls_rough, in->calls);
	else
		snprintf(first, sizeof(first), "everything the gateway measured%s was normal", o->current || ends_now ? ", now too," : "");
	if (!strcmp(o->code, "LINE_FULL"))
		snprintf(o->verdict, sizeof(o->verdict), "Not a fault: the home used the whole line. %s (LINE_FULL).", first);
	else
		snprintf(o->verdict, sizeof(o->verdict), "%s%s: %s (%s%s).", fault_words(o->fault),
			 o->current ? ", now" : main ? ", earlier" : "", first, o->code, conflict ? "; the sources disagree" : "");
	evidence(in, o);
	done(in, o);
	snprintf(o->tell, sizeof(o->tell), "%s", in->coverage_note && *in->coverage_note && !strcmp(o->code, "WIFI_COVERAGE") ?
		 in->coverage_note : tell_of(o->code, o->fault));
	fix(in, o);
	visit(in, o, agree, lasted);
	if (conflict && o->visit == BV_YES) {      /* the sources disagree (fine now, say): not a truck yet */
		char why[320];

		snprintf(why, sizeof(why), "Only if it persists: %s But it is %s now.", o->visit_why + 5,
			 in->have_check && !bad(&in->check) ? "fine" : "different");
		snprintf(o->visit_why, sizeof(o->visit_why), "%s", why);
		o->visit = BV_IF_PERSISTS;
	}
	if (in->area && in->to >= in->area_since)
		area(in, o, ends_now);
}

/* a known outage in the area (cortex): the ISP's network, not the home, whatever it looked like from here */
static void area(const struct brief_in *in, struct brief *o, int ends_now)
{
	static const struct { const char *kind, *words; } k[] = {
		{ "DOWN", "down" }, { "LOSS", "losing packets" }, { "SLOW", "slow" }, { "DNS", "failing to look up names" },
	};
	const char *words = "affected";
	char when[16], own[120];
	size_t i;

	for (i = 0; in->area_kind && i < sizeof(k) / sizeof(k[0]); i++)
		if (!strcmp(k[i].kind, in->area_kind))
			words = k[i].words;
	why_clock(in->area_since, in->now, when, sizeof(when));
	first_sentence(o->verdict, own, sizeof(own));
	if (o->fault != BF_LINE)
		o->code = "AREA_OUTAGE";
	o->fault = BF_LINE;
	o->confidence = BC_HIGH;
	o->current = o->current || ends_now;
	snprintf(o->verdict, sizeof(o->verdict), "Known outage in the area: %d of %d homes on this uplink %.24s since %.15s "
		 "(area outage %u). The ISP's network, not this home. From here: %.119s.", in->area_homes, in->area_of, words,
		 when, in->area, own);
	/* first in the evidence (the last line gives way when it is full) */
	if (o->nev == BRIEF_LINES)
		o->nev--;
	memmove(&o->ev[1], &o->ev[0], sizeof(o->ev[0]) * (size_t)o->nev);
	o->nev++;
	o->ev[0].t = in->area_since;
	o->ev[0].source = "area";
	snprintf(o->ev[0].text, sizeof(o->ev[0].text), "cortex: %d of %d homes on this uplink %s since %s, within minutes "
		 "of each other (area outage %u).", in->area_homes, in->area_of, words, when, in->area);
	if (in->area_held && *in->area_held)
		line_add(o->done, &o->ndone, in->area_since, "area", "Held its own fixes during the area outage: %s.",
			 in->area_held);
	snprintf(o->tell, sizeof(o->tell), "There is a known outage in your area since %s; our network team is working "
		 "on it. Your equipment at home is fine: there is no need to restart or reset anything.", when);
	memset(&o->fix, 0, sizeof(o->fix));
	snprintf(o->fix.text, sizeof(o->fix.text), "Nothing to fix in the home: AIGW holds its own fixes there until the "
		 "area is back.");
	o->visit = BV_NO;
	snprintf(o->visit_why, sizeof(o->visit_why), "No: a known outage in the area (area outage %u, %d homes); a visit to "
		 "this home would not help.", in->area, in->area_homes);
}

/* ---------- the period ---------- */

static int clock_of(const char *s, int *h, int *m, int *k)
{
	return sscanf(s, "%d:%d%n", h, m, k) == 2 && *h >= 0 && *h <= 24 && *m >= 0 && *m < 60;
}

/* today's local h:m (wall s) as of now */
static long at_clock(long now, int h, int m, int days_back)
{
	time_t t = (time_t)now;
	struct tm tm;

	localtime_r(&t, &tm);
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = 0;
	tm.tm_mday -= days_back;
	tm.tm_isdst = -1;
	return (long)mktime(&tm);
}

int brief_period(const char *s, long now, long *from, long *to)
{
	char w[64];
	size_t i, k = 0;
	const char *p;
	int h1, m1, h2, m2, n, back = 0;
	double v;
	char unit[8] = "";

	while (s && isspace((unsigned char)*s))
		s++;
	for (i = 0; s && s[i] && k < sizeof(w) - 1; i++)
		w[k++] = (char)tolower((unsigned char)s[i]);
	w[k] = '\0';
	while (k && isspace((unsigned char)w[k - 1]))
		w[--k] = '\0';
	*to = now;
	if (!k || !strcmp(w, "now")) {
		*from = now - 900;
		return 0;
	}
	if (!strcmp(w, "last hour") || !strcmp(w, "the last hour") || !strcmp(w, "hour")) {
		*from = now - 3600;
		return 0;
	}
	p = w;
	if (!strncmp(p, "last ", 5))
		p += 5;
	if (sscanf(p, "%lf%n", &v, &n) == 1 && v > 0) {
		const char *u = p + n;

		while (*u == ' ')
			u++;
		snprintf(unit, sizeof(unit), "%.7s", u);
		if (!strcmp(unit, "h") || !strcmp(unit, "hour") || !strcmp(unit, "hours"))
			*from = now - (long)(v * 3600);
		else if (!strcmp(unit, "m") || !strcmp(unit, "min") || !strcmp(unit, "mins") || !strcmp(unit, "minutes"))
			*from = now - (long)(v * 60);
		else if (!strcmp(unit, "s") || !strcmp(unit, "sec") || !strcmp(unit, "seconds"))
			*from = now - (long)v;
		else
			goto clock;
		return *from < now && now - *from <= 86400 ? 0 : -1;
	}
clock:
	p = w;
	if (!strncmp(p, "since ", 6)) {
		if (!clock_of(p + 6, &h1, &m1, &n) || p[6 + n])
			return -1;
		*from = at_clock(now, h1, m1, 0);
		if (*from > now)
			*from = at_clock(now, h1, m1, 1);
		return 0;
	}
	if (!strncmp(p, "tonight ", 8))
		p += 8;
	else if (!strncmp(p, "today ", 6))
		p += 6;
	else if (!strncmp(p, "yesterday ", 10)) {
		p += 10;
		back = 1;
	}
	if (!clock_of(p, &h1, &m1, &n))
		return -1;
	p += n;
	while (*p == ' ')
		p++;
	if (*p != '-' && strncmp(p, "to ", 3))
		return -1;
	p += *p == '-' ? 1 : 3;
	while (*p == ' ')
		p++;
	if (!clock_of(p, &h2, &m2, &n) || p[n])
		return -1;
	*from = at_clock(now, h1, m1, back);
	*to = at_clock(now, h2, m2, back);
	if (*to <= *from)               /* "22:00-01:00": over midnight */
		*to = at_clock(now, h2, m2, back - 1);
	if (!back && *from > now) {     /* later today: yesterday's */
		long d = *to - *from;

		*from = at_clock(now, h1, m1, 1);
		*to = *from + d;
	}
	if (*to > now)
		*to = now;              /* tonight, still going */
	return *to > *from && *to - *from <= 86400 ? 0 : -1;
}

/* ---------- names ---------- */

void brief_kind(const char *type, const char *host, char *buf, size_t n)
{
	static const struct { const char *needle, *kind; } hints[] = {
		{ "laptop", "laptop" }, { "macbook", "laptop" }, { "notebook", "laptop" }, { "iphone", "phone" },
		{ "phone", "phone" }, { "ipad", "tablet" }, { "tablet", "tablet" }, { "tv", "TV" }, { "xbox", "games console" },
		{ "playstation", "games console" }, { "ps5", "games console" }, { "switch", "games console" },
		{ "printer", "printer" }, { "camera", "camera" }, { "desktop", "computer" }, { "pc", "computer" },
	};
	static const struct { const char *type, *kind; } types[] = {
		{ "Android", "Android phone" }, { "Windows", "Windows computer" }, { "Linux", "Linux device" },
		{ "iPhone", "iPhone" }, { "iPad", "iPad" }, { "Mac", "Mac" }, { "IoT", "smart-home device" },
		{ "media player", "media player" }, { "smart home", "smart-home device" },
	};
	const char *k = NULL;
	size_t i;

	for (i = 0; type && *type && i < sizeof(types) / sizeof(types[0]); i++)
		if (!strcmp(type, types[i].type))
			k = types[i].kind;
	if (!k && type && *type)
		k = type;
	for (i = 0; !k && host && *host && i < sizeof(hints) / sizeof(hints[0]); i++) {
		const char *h;

		for (h = host; *h && !k; h++)
			if (!strncasecmp(h, hints[i].needle, strlen(hints[i].needle)))
				k = hints[i].kind;
	}
	if (!k)
		k = "device";
	snprintf(buf, n, "%s %s", strchr("aeiouAEIOU", k[0]) ? "an" : "a", k);
}

static int word_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.';
}

void brief_scrub(char *text, size_t n, const char *const *name, const char *const *kind, int k)
{
	int stack[256], *order = stack, i, j, m = k;

	if (!text || !n || k <= 0)
		return;
	if (k > 256 && !(order = malloc(sizeof(*order) * (size_t)k))) {
		/* no memory: nothing may leave with a name in it */
		snprintf(text, n, "(not shown: no memory to take the names out)");
		return;
	}
	for (i = 0; i < m; i++)
		order[i] = i;
	for (i = 1; i < m; i++)         /* longest first: "tv-kitchen" before "tv" */
		for (j = i; j > 0 && strlen(name[order[j]]) > strlen(name[order[j - 1]]); j--) {
			int t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (i = 0; i < m; i++) {
		const char *nm = name[order[i]], *kd = kind[order[i]];
		size_t nl = strlen(nm), kl = strlen(kd);
		char *p = text;

		if (!nl)
			continue;
		while ((p = strstr(p, nm))) {
			size_t len = strlen(text), rest;
			int start = p == text || !word_char(p[-1]);
			char after = p[nl];
			int end = !after || !word_char(after) || ((after == '.' || after == ':') && (!p[nl + 1] || p[nl + 1] == ' '));

			if (!start || !end) {
				p += nl;
				continue;
			}
			rest = len - (size_t)(p - text) - nl;
			if ((size_t)(p - text) + kl + rest >= n) {
				*p = '\0';      /* no room: cut it here, so no name is left after */
				break;
			}
			memmove(p + kl, p + nl, rest + 1);
			memcpy(p, kd, kl);
			p += kl;
		}
	}
	if (order != stack)
		free(order);
}
