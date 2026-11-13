/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Diagnose: from what agd measured to a verdict (see ag/diag.h).
 *
 * Every finding has a code (for counting across homes), an area, a severity
 * and plain words. They are put in order of what explains the problem best:
 * AIGW itself keeping the device off first (the quickest fix), then the line
 * to the ISP, DNS, the device being away, AIGW slowing it, the line's quality,
 * Wi-Fi, and last what is fine.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/diag.h"
#include "ag/heal.h"

static int prio[DIAG_FINDINGS];

static void add(struct diag_result *r, int p, const char *code, const char *area, int sev, const char *fmt, ...)
{
	struct diag_finding *f;
	va_list ap;

	if (r->n >= DIAG_FINDINGS)
		return;
	prio[r->n] = p;
	f = &r->f[r->n++];
	f->code = code;
	f->area = area;
	f->sev = sev;
	va_start(ap, fmt);
	vsnprintf(f->text, sizeof(f->text), fmt, ap);
	va_end(ap);
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

int diag_median(int *v, int n)
{
	int i, k = 0;

	for (i = 0; i < n; i++)
		if (v[i] >= 0)
			v[k++] = v[i];
	if (!k)
		return -1;
	qsort(v, (size_t)k, sizeof(*v), cmp_int);
	return v[k / 2];
}

static int loss(const struct diag_ping *p)
{
	return p->sent ? p->lost * 100 / p->sent : 0;
}

static int answers(const struct diag_ping *p)
{
	return p->sent > 0 && p->lost < p->sent;
}

static const char *cause_code(int cause)
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

static const char *band(int b)
{
	return b == 5 ? "5 GHz" : b == 2 ? "2.4 GHz" : "Wi-Fi";
}

/* a radio's trouble, in Self-Healing Wi-Fi's words, with what it did about it today */
static void radio_finding(struct diag_result *out, int p, const struct diag_radio *r)
{
	char done[120] = "";

	if (r->kept_today || r->tried_today)
		snprintf(done, sizeof(done), " Today AIGW tried %d other channel%s and kept %d.", r->kept_today + r->tried_today,
			 r->kept_today + r->tried_today == 1 ? "" : "s", r->kept_today);
	add(out, p, cause_code(r->cause), "WiFi", r->status == HEAL_POOR ? DS_PROBLEM : DS_WARN, "%s%s",
	    r->explanation[0] ? r->explanation : "Wi-Fi devices lose traffic.", done);
}

void diag_judge(const struct diag_input *in, struct diag_result *out)
{
	const struct diag_host *h = &in->host;
	int i, j, gl = loss(&in->gw_ping), il = loss(&in->inet), gw_ok = answers(&in->gw_ping),
	    inet_ok = answers(&in->inet), worst = DS_OK;
	char gw[80];

	memset(out, 0, sizeof(*out));
	snprintf(gw, sizeof(gw), in->gw[0] ? "the next router (%s)" : "the next router", in->gw);

	/* AIGW itself */
	if (h->found && h->blocked)
		add(out, 10, "DEVICE_BLOCKED", "AIGW", DS_PROBLEM,
		    "AIGW itself keeps %s off the network: %s. Undo it if that isn't wanted.", h->name, h->actions);

	/* the line to the ISP */
	if (!in->route) {
		add(out, 20, "NO_ROUTE", "Internet", DS_PROBLEM,
		    "The gateway has no way to the internet (no default route): its internet connection is down or not set up.");
	} else if (!inet_ok && in->inet.sent) {
		if (in->gw_ping.sent && gw_ok)
			add(out, 31, "ISP_DOWN", "Internet", DS_PROBLEM,
			    "%c%s answers, the internet doesn't: the problem is beyond the home, in the ISP's network.",
			    gw[0] - 32, gw + 1);
		else
			add(out, 30, "LINE_DOWN", "Internet", DS_PROBLEM,
			    in->gw_ping.sent ? "Neither %s nor the internet answers: the line between the gateway and the ISP "
					       "is down (modem, cable or fibre)." :
					       "The internet doesn't answer: the line between the gateway and the ISP is down%.0s.",
			    gw);
	}

	/* names */
	if (in->route && inet_ok && (in->dns == 0 || (in->dns == 1 && in->dns_rcode == 2)))
		add(out, 40, "DNS_FAILING", "DNS", DS_PROBLEM,
		    in->dns == 0 ? "The internet answers, but names don't resolve (no DNS answer in time): websites and apps "
				   "fail although the line works." :
				   "The internet answers, but names don't resolve (the DNS server says it failed): websites "
				   "and apps fail although the line works.");

	/* the device asked about */
	if (h->given && !h->found)
		add(out, 50, "DEVICE_UNKNOWN", "Device", DS_WARN, "The gateway doesn't know a device called %s.", h->name);
	else if (h->found && !h->blocked && !h->connected && (h->seen_ago < 0 || h->seen_ago > 300)) {
		if (h->seen_ago < 0)
			add(out, 51, "DEVICE_OFFLINE", "Device", DS_PROBLEM,
			    "%s isn't connected: it's off, out of range, or on another network.", h->name);
		else
			add(out, 51, "DEVICE_OFFLINE", "Device", DS_PROBLEM,
			    "%s isn't connected (last seen %d min ago): it's off, out of range, or on another network.", h->name,
			    h->seen_ago / 60);
	}
	if (h->found && h->limited && !h->blocked)
		add(out, 60, "DEVICE_LIMITED", "AIGW", DS_WARN, "AIGW slows %s on purpose: %s.", h->name, h->actions);

	/* the line's quality */
	if (in->route && gw_ok && gl >= 5)
		add(out, 70, "LINE_LOSS", "Internet", gl >= 20 ? DS_PROBLEM : DS_WARN,
		    "%d%% of packets to %s are lost: the line to the ISP (modem, cable, fibre) is unreliable.", gl, gw);
	else if (in->route && inet_ok && il >= 5)
		add(out, 71, "ISP_LOSS", "Internet", il >= 20 ? DS_PROBLEM : DS_WARN,
		    "%d%% of packets to the internet are lost beyond %s: in the ISP's network.", il, gw);
	if (in->route && gw_ok && in->gw_ping.rtt_ms >= 100)
		add(out, 72, "LINE_SLOW", "Internet", DS_WARN,
		    "%c%s answers in %d ms: the line is slow, or full (someone uploading or downloading a lot?).", gw[0] - 32,
		    gw + 1, in->gw_ping.rtt_ms);
	else if (in->route && inet_ok && in->inet.rtt_ms >= 150 &&
		 (in->gw_ping.rtt_ms < 0 || in->inet.rtt_ms - in->gw_ping.rtt_ms >= 100))
		add(out, 73, "ISP_SLOW", "Internet", DS_WARN, "The internet answers in %d ms (%s in %d ms): slow beyond the home.",
		    in->inet.rtt_ms, gw, in->gw_ping.rtt_ms < 0 ? 0 : in->gw_ping.rtt_ms);

	/* Wi-Fi: the device's own first, then any access point in trouble */
	if (h->found && h->connected && h->radio >= 0 && h->radio < in->nradios) {
		const struct diag_radio *r = &in->radios[h->radio];

		if (h->signal && h->signal <= in->weak_signal) {
			char felt[80] = "";

			if (h->samples && h->loss_pct >= in->loss_pct)
				snprintf(felt, sizeof(felt), " It loses %d%% of its traffic.", h->loss_pct);
			add(out, 80, "WIFI_COVERAGE", "WiFi", h->samples && h->loss_pct >= 20 ? DS_PROBLEM : DS_WARN,
			    "%s's Wi-Fi signal is weak (%d dBm on %s): too far from the access point, or walls in the way.%s",
			    h->name, h->signal, band(r->band), felt);
		} else if (r->status == HEAL_DEGRADED || r->status == HEAL_POOR) {
			radio_finding(out, 81, r);
		} else if (h->samples && h->loss_pct >= in->loss_pct) {
			add(out, 82, "WIFI_DEVICE", "WiFi", DS_WARN,
			    "%s loses %d%% of its traffic on Wi-Fi while the signal is fine (%d dBm): the device itself "
			    "(its Wi-Fi, power saving).", h->name, h->loss_pct, h->signal);
		} else {
			add(out, 900, "WIFI_OK", "WiFi", DS_OK, "%s's Wi-Fi is fine (%s, %d dBm%s).", h->name, band(r->band), h->signal,
			    h->samples ? ", answers well" : "");
		}
	}
	for (i = 0; i < in->nradios; i++) {
		const struct diag_radio *r = &in->radios[i];

		if (h->found && h->connected && i == h->radio)
			continue;
		if (r->status == HEAL_DEGRADED || r->status == HEAL_POOR)
			radio_finding(out, h->given ? 86 : 85, r);
	}

	/* names, slow */
	if (in->dns == 1 && in->dns_rcode != 2 && in->dns_ms >= 300)
		add(out, 90, "DNS_SLOW", "DNS", DS_WARN, "Names take %d ms to resolve (DNS): pages start slowly.", in->dns_ms);
	if (h->found && h->on24)
		add(out, 95, "DEVICE_ON_24GHZ", "WiFi", DS_INFO,
		    "%s is kept on 2.4 GHz for now (it kept switching bands at the edge of 5 GHz).", h->name);

	/* what is fine */
	if (in->route && inet_ok && il < 5 && in->inet.rtt_ms < 150)
		add(out, 910, "INTERNET_OK", "Internet", DS_OK, "The internet answers in %d ms, %d%% lost.", in->inet.rtt_ms, il);
	if (in->dns == 1 && in->dns_rcode != 2 && in->dns_ms < 300)
		add(out, 920, "DNS_OK", "DNS", DS_OK, "Names resolve in %d ms.", in->dns_ms);
	for (i = 0, j = 0; i < in->nradios; i++)
		j += in->radios[i].status == HEAL_GOOD;
	if (j && j == in->nradios && !(h->found && h->connected))
		add(out, 930, "WIFI_OK", "WiFi", DS_OK, "Wi-Fi is fine on %d access point%s.", j, j == 1 ? "" : "s");

	/* in order (a stable sort: the order found breaks ties) */
	for (i = 1; i < out->n; i++)
		for (j = i; j > 0 && prio[j] < prio[j - 1]; j--) {
			struct diag_finding t = out->f[j];
			int p = prio[j];

			out->f[j] = out->f[j - 1];
			prio[j] = prio[j - 1];
			out->f[j - 1] = t;
			prio[j - 1] = p;
		}
	for (i = 0; i < out->n; i++)
		if (out->f[i].sev > worst)
			worst = out->f[i].sev;
	if (worst <= DS_INFO) {
		/* nothing wrong: the verdict says so, the details follow */
		if (out->n >= DIAG_FINDINGS)
			out->n = DIAG_FINDINGS - 1;
		memmove(&out->f[1], &out->f[0], sizeof(out->f[0]) * (size_t)out->n);
		out->n++;
		out->f[0].code = "OK";
		out->f[0].area = "All";
		out->f[0].sev = DS_OK;
		snprintf(out->f[0].text, sizeof(out->f[0].text),
			 h->found ? "Everything the gateway can measure is fine, for %s too: the problem is likely the "
				    "service or website used, or the device itself." :
				    "Everything the gateway can measure is fine%.0s: the problem is likely the service or "
				    "website used, or one device.",
			 h->name);
	}
}

const char *diag_advice(const char *code)
{
	static const struct { const char *code, *advice; } t[] = {
		{ "OK", "Ask which app or website is slow, and try another device in the same place." },
		{ "DEVICE_BLOCKED", "Check why AIGW acted (the decision's reason); undo it if the device is safe." },
		{ "NO_ROUTE", "Check the gateway's internet connection settings, and that the modem or fibre box is on." },
		{ "LINE_DOWN", "Restart the modem or fibre box; check its cable. If it stays down, check the line from the ISP's side." },
		{ "ISP_DOWN", "The home is fine up to the ISP: check the ISP's network for an outage in this area." },
		{ "DNS_FAILING", "Restart the gateway's DNS (agdns), or check the DNS servers it uses." },
		{ "DEVICE_UNKNOWN", "Check the name, or give the device's MAC or IP address." },
		{ "DEVICE_OFFLINE", "Make sure the device is on, its Wi-Fi is on, and it joined this network." },
		{ "DEVICE_LIMITED", "Check the decision's reason; undo it if the device should have full speed." },
		{ "LINE_LOSS", "Check the cable between the gateway and the modem, and the line's quality from the ISP's side." },
		{ "ISP_LOSS", "The home is fine up to the ISP: check the ISP's network beyond the home." },
		{ "LINE_SLOW", "Look for a device using the whole line (agctl top); check the line's speed from the ISP's side." },
		{ "ISP_SLOW", "The home is fine up to the ISP: check congestion in the ISP's network at this time." },
		{ "WIFI_COVERAGE", "Move the device closer, or the access point higher and more central; a mesh node or extender would help." },
		{ "WIFI_CONGESTION", "AIGW tries quieter channels by itself; if it stays busy, use 5 GHz or move away from other networks." },
		{ "WIFI_HOPPING", "AIGW keeps the device on 2.4 GHz for a day; moving the access point closer helps for good." },
		{ "WIFI_DEVICE", "Restart the device's Wi-Fi; turn off its power saving; update it." },
		{ "WIFI_RADIO", "Restart the access point; if no device can use it afterwards, the radio may be faulty." },
		{ "DNS_SLOW", "Check the DNS servers the gateway uses (a closer or faster one)." },
	};
	size_t i;

	for (i = 0; code && i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].code, code))
			return t[i].advice;
	return "";
}

const char *diag_sev_name(int sev)
{
	static const char *const n[] = { "ok", "info", "warning", "problem" };

	return sev >= 0 && sev <= DS_PROBLEM ? n[sev] : "ok";
}
