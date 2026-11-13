/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW Diagnose: from what was measured to the verdict, case by case */
#include <stdio.h>
#include <string.h>

#include "ag/diag.h"
#include "ag/heal.h"

static int failed;

/* a home where everything works: a next hop at 2 ms, the internet at 15 ms, DNS 20 ms, Wi-Fi Good */
static void fine(struct diag_input *in)
{
	memset(in, 0, sizeof(*in));
	in->route = 1;
	snprintf(in->gw, sizeof(in->gw), "192.168.1.1");
	in->gw_ping = (struct diag_ping){ 10, 0, 2 };
	in->inet = (struct diag_ping){ 10, 0, 15 };
	in->dns = 1;
	in->dns_ms = 20;
	in->nradios = 2;
	snprintf(in->radios[0].name, sizeof(in->radios[0].name), "wlan1");
	in->radios[0].band = 2;
	in->radios[0].channel = 6;
	in->radios[0].status = HEAL_GOOD;
	snprintf(in->radios[1].name, sizeof(in->radios[1].name), "wlan0");
	in->radios[1].band = 5;
	in->radios[1].channel = 36;
	in->radios[1].status = HEAL_GOOD;
	in->host.radio = -1;
	in->host.seen_ago = -1;
	in->weak_signal = -75;
	in->loss_pct = 5;
}

/* a phone on the 2.4 GHz access point */
static void phone(struct diag_input *in)
{
	in->host.given = in->host.found = in->host.connected = 1;
	snprintf(in->host.name, sizeof(in->host.name), "phone");
	in->host.radio = 0;
	in->host.signal = -60;
	in->host.samples = 40;
	in->host.seen_ago = 0;
}

static int has(const struct diag_result *r, const char *code)
{
	int i;

	for (i = 0; i < r->n; i++)
		if (!strcmp(r->f[i].code, code))
			return 1;
	return 0;
}

static void expect(const char *name, const struct diag_input *in, const char *code, const char *also, const char *words)
{
	struct diag_result r;
	int i;

	diag_judge(in, &r);
	if (!r.n || strcmp(r.f[0].code, code) || (also && !has(&r, also)) || (words && !strstr(r.f[0].text, words)) ||
	    !diag_advice(r.f[0].code)[0]) {
		printf("FAIL %s: expected %s%s%s%s%s, got", name, code, also ? " with " : "", also ? also : "",
		       words ? " saying " : "", words ? words : "");
		for (i = 0; i < r.n; i++)
			printf(" %s", r.f[i].code);
		printf("\n     %s\n", r.n ? r.f[0].text : "(nothing)");
		failed++;
		return;
	}
	printf("ok   %s: %s (%s) -- %s\n", name, r.f[0].code, diag_sev_name(r.f[0].sev), r.f[0].text);
}

int main(void)
{
	struct diag_input in;
	int v[] = { 30, -1, 10, 20, -1 }, none[] = { -1, -1 };

	fine(&in);
	expect("everything works", &in, "OK", "INTERNET_OK", "fine");
	fine(&in);
	phone(&in);
	expect("everything works, for the phone too", &in, "OK", "WIFI_OK", "for phone too");

	fine(&in);
	in.route = 0;
	expect("no default route", &in, "NO_ROUTE", NULL, "no way to the internet");
	fine(&in);
	in.inet = (struct diag_ping){ 10, 10, -1 };
	expect("the next router answers, the internet doesn't", &in, "ISP_DOWN", NULL, "beyond the home");
	fine(&in);
	in.gw_ping = (struct diag_ping){ 10, 10, -1 };
	in.inet = (struct diag_ping){ 10, 10, -1 };
	in.dns = 0;
	expect("nothing answers", &in, "LINE_DOWN", NULL, "192.168.1.1");
	if (1) {
		struct diag_result r;

		diag_judge(&in, &r);
		if (has(&r, "DNS_FAILING")) {
			printf("FAIL the line is down: DNS failing is a consequence, not a finding\n");
			failed++;
		}
	}

	fine(&in);
	in.dns = 0;
	expect("the internet answers, names don't", &in, "DNS_FAILING", "INTERNET_OK", "names don't resolve");
	fine(&in);
	in.dns_rcode = 2;
	expect("DNS says SERVFAIL", &in, "DNS_FAILING", NULL, "failed");
	fine(&in);
	in.dns_ms = 800;
	expect("names slow", &in, "DNS_SLOW", NULL, "800 ms");

	fine(&in);
	in.gw_ping = (struct diag_ping){ 10, 3, 4 };
	expect("30% lost to the next router", &in, "LINE_LOSS", NULL, "30%");
	fine(&in);
	in.inet = (struct diag_ping){ 10, 1, 18 };
	expect("10% lost beyond it", &in, "ISP_LOSS", NULL, "beyond the next router");
	fine(&in);
	in.gw_ping.rtt_ms = 180;
	in.inet.rtt_ms = 200;
	expect("the line itself is slow", &in, "LINE_SLOW", NULL, "180 ms");
	fine(&in);
	in.inet.rtt_ms = 260;
	expect("slow beyond the home", &in, "ISP_SLOW", NULL, "260 ms");

	/* the device asked about */
	fine(&in);
	phone(&in);
	in.host.blocked = 1;
	snprintf(in.host.actions, sizeof(in.host.actions), "quarantine (#4, 2h00m left, by policy dga)");
	in.gw_ping = (struct diag_ping){ 10, 10, -1 };
	in.inet = (struct diag_ping){ 10, 10, -1 };
	expect("AIGW itself keeps it off (even with the line down)", &in, "DEVICE_BLOCKED", "LINE_DOWN", "quarantine");
	fine(&in);
	phone(&in);
	in.host.blocked = 1;
	in.host.loss_pct = 12;          /* its last probes, as it was cut off */
	snprintf(in.host.actions, sizeof(in.host.actions), "disconnect (#6, 0h04m left, by you)");
	expect("kept off: its Wi-Fi isn't the story", &in, "DEVICE_BLOCKED", "INTERNET_OK", "disconnect");
	if (1) {
		struct diag_result r;

		diag_judge(&in, &r);
		if (has(&r, "WIFI_DEVICE")) {
			printf("FAIL a device AIGW keeps off shouldn't get Wi-Fi findings\n");
			failed++;
		}
	}
	fine(&in);
	phone(&in);
	in.host.limited = 1;
	snprintf(in.host.actions, sizeof(in.host.actions), "throttle (#5, 0h30m left, by you)");
	expect("AIGW slows it", &in, "DEVICE_LIMITED", NULL, "throttle");
	fine(&in);
	in.host.given = 1;
	snprintf(in.host.name, sizeof(in.host.name), "tablet");
	expect("a name the gateway doesn't know", &in, "DEVICE_UNKNOWN", NULL, "tablet");
	fine(&in);
	phone(&in);
	in.host.connected = 0;
	in.host.radio = -1;
	in.host.seen_ago = 1200;
	expect("not connected for 20 minutes", &in, "DEVICE_OFFLINE", NULL, "20 min ago");

	/* Wi-Fi */
	fine(&in);
	phone(&in);
	in.host.signal = -82;
	in.host.loss_pct = 25;
	expect("the phone far away (the PC access point's case)", &in, "WIFI_COVERAGE", "INTERNET_OK", "-82 dBm");
	fine(&in);
	phone(&in);
	in.radios[0].status = HEAL_POOR;
	in.radios[0].cause = HC_CONGESTION;
	in.radios[0].kept_today = 1;
	in.radios[0].tried_today = 1;
	snprintf(in.radios[0].explanation, sizeof(in.radios[0].explanation),
		 "2.4 GHz (channel 11): phone loses 24%% of its traffic, though the signal is fine (-67 dBm).");
	expect("its access point congested, and what AIGW did", &in, "WIFI_CONGESTION", NULL, "tried 2 other channels");
	fine(&in);
	phone(&in);
	in.host.loss_pct = 12;
	expect("only the phone suffers", &in, "WIFI_DEVICE", NULL, "12%");
	fine(&in);
	in.radios[1].status = HEAL_DEGRADED;
	in.radios[1].cause = HC_COVERAGE;
	expect("an access point with weak coverage, nobody asked about", &in, "WIFI_COVERAGE", NULL, NULL);
	fine(&in);
	phone(&in);
	in.host.on24 = 1;
	expect("kept on 2.4 GHz is only information", &in, "OK", "DEVICE_ON_24GHZ", NULL);

	/* the order: the line before Wi-Fi */
	fine(&in);
	phone(&in);
	in.host.signal = -82;
	in.inet = (struct diag_ping){ 10, 10, -1 };
	expect("the line down and a weak signal: the line first", &in, "ISP_DOWN", "WIFI_COVERAGE", NULL);

	if (diag_median(v, 5) != 20 || diag_median(none, 2) != -1) {
		printf("FAIL diag_median\n");
		failed++;
	} else
		printf("ok   median of the answered delays\n");

	if (failed) {
		printf("%d diagnose case(s) failed\n", failed);
		return 1;
	}
	printf("all diagnose cases passed\n");
	return 0;
}
