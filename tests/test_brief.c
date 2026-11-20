/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW help-desk assistant: calls to the support line, answered (ag/brief.h). Run with TZ=UTC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/brief.h"
#include "ag/diag.h"
#include "ag/heal.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Wed 2026-11-18 21:30 UTC */
#define NOW 1795469400L

static struct brief_in in;
static struct brief o;

static void home(long from, long to)
{
	memset(&in, 0, sizeof(in));
	in.now = NOW;
	in.from = from;
	in.to = to;
	in.score_week = -1;
	in.check_loss_pct = in.check_inet_ms = in.check_gw_ms = in.check_dns_ms = -1;
	in.sqm = "Off";
}

static void fresh(const char *code, const char *area, int sev, const char *text)
{
	in.have_check = 1;
	in.check.code = code;
	in.check.area = area;
	in.check.sev = sev;
	in.check.text = text;
	in.check_loss_pct = 0;
	in.check_inet_ms = 18;
	in.check_gw_ms = 2;
	in.check_dns_ms = 9;
}

static void timeline(const char *code, const char *area, int sev, long since, long until, const char *text)
{
	in.have_why = 1;
	in.recorded_pct = 100;
	in.why.code = code;
	in.why.area = area;
	in.why.sev = sev;
	in.why.since = since;
	in.why.until = until;
	in.why.text = text;
}

static int has_ev(const char *src, const char *needle)
{
	int i;

	for (i = 0; i < o.nev; i++)
		if ((!src || !strcmp(o.ev[i].source, src)) && strstr(o.ev[i].text, needle))
			return 1;
	return 0;
}

static int has_done(const char *needle)
{
	int i;

	for (i = 0; i < o.ndone; i++)
		if (strstr(o.done[i].text, needle))
			return 1;
	return 0;
}

static void show(void)
{
	int i;

	printf("     %s %s (%s) visit %s\n     %s\n", brief_fault_name(o.fault), o.code, brief_conf_name(o.confidence),
	       brief_visit_name(o.visit), o.verdict);
	for (i = 0; i < o.nev; i++)
		printf("       - [%s] %s\n", o.ev[i].source, o.ev[i].text);
}

int main(void)
{
	struct brief_decision dec[3];
	struct brief_radio radio[1];
	long from, to;
	char buf[400];

	if (!getenv("TZ") || strcmp(getenv("TZ"), "UTC"))
		setenv("TZ", "UTC", 1);

	printf("== the line down now, the timeline and care agree: a visit only if it persists after a restart\n");
	home(NOW - 3600, NOW);
	fresh("LINE_DOWN", "Internet", DS_PROBLEM, "Nothing answers beyond the gateway, not even the modem. Restart it.");
	timeline("LINE_DOWN", "Internet", DS_PROBLEM, NOW - 2400, NOW, "The line was down from 20:50 to 21:30.");
	in.care_open = 1;
	in.care.code = "LINE_DOWN";
	in.care.area = "Internet";
	in.care.sev = DS_PROBLEM;
	in.care.since = NOW - 2300;
	in.care.text = "The line is down.";
	in.care_reported = 1;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && !strcmp(o.code, "LINE_DOWN") && o.current, "LINE, LINE_DOWN, now");
	CHECK(o.confidence == BC_HIGH, "three sources agree: high confidence");
	CHECK(o.visit == BV_IF_PERSISTS && strstr(o.visit_why, "restarts the modem"), "visit only if it persists after a restart");
	CHECK(strstr(o.tell, "restart the modem") != NULL, "tell: restart the modem");
	CHECK(!o.fix.possible, "no remote fix for a line down");
	CHECK(has_ev("now", "LINE_DOWN") && has_ev("timeline", "20:50") && has_ev("care", "reported to the ISP"),
	      "evidence from now, the timeline and care");
	CHECK(has_done("Told the ISP"), "done: told the ISP");
	CHECK(strstr(o.verdict, "The ISP's line or network, now:") == o.verdict, "verdict: one sentence, the line, now");

	printf("== mesh Wi-Fi: everything else fine, an extender's weak backhaul slows the devices behind it\n");
	{
		struct brief_finding mesh[1];

		home(NOW - 3600, NOW);
		fresh("OK", "All", DS_OK, "Everything is fine.");
		mesh[0].code = "MESH_BACKHAUL";
		mesh[0].area = "WiFi";
		mesh[0].sev = DS_WARN;
		mesh[0].since = NOW - 1800;
		mesh[0].until = 0;
		mesh[0].text = "the extender 'bedroom' has a weak wireless backhaul to the gateway (-78 dBm, 54 Mbit/s, 5 GHz); "
			       "2 devices are behind it. Move it a room closer to the gateway.";
		in.mesh = mesh;
		in.nmesh = 1;
		brief_judge(&in, &o);
		show();
		CHECK(o.fault == BF_HOME_WIFI && !strcmp(o.code, "MESH_BACKHAUL") && o.visit == BV_NO,
		      "HOME_WIFI, MESH_BACKHAUL, no visit");
		CHECK(strstr(o.verdict, "the extender 'bedroom' has a weak wireless backhaul") != NULL, "verdict names the extender");
		CHECK(has_ev("mesh", "MESH_BACKHAUL") && strstr(o.tell, "extender") && strstr(o.fix.text, "closer"),
		      "evidence from the mesh; tell and fix: move it closer, or a cable");
		mesh[0].sev = DS_INFO;
		brief_judge(&in, &o);
		CHECK(strcmp(o.code, "MESH_BACKHAUL") && has_ev("mesh", "weak wireless backhaul"),
		      "no devices behind it: only evidence, not the verdict (%s)", o.code);
	}

	printf("== the next hop losing packets for 40 min last night, reported: a visit\n");
	home(NOW - 3 * 3600, NOW - 3600);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("LINE_LOSS", "Internet", DS_PROBLEM, NOW - 9000, NOW - 6600, "The next router lost 12% of packets from 19:00 to 19:40.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && !strcmp(o.code, "LINE_LOSS") && !o.current, "LINE_LOSS, earlier");
	CHECK(o.visit == BV_YES && strstr(o.visit_why, "lasted"), "visit: yes, it lasted");

	printf("== the same in the last hour, but the line is fine now: only if it persists\n");
	home(NOW - 3600, NOW);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("LINE_LOSS", "Internet", DS_PROBLEM, NOW - 3500, NOW - 900, "The next router lost 12% of packets from 20:32 to 21:15.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && o.visit == BV_IF_PERSISTS && strstr(o.visit_why, "fine now") && o.confidence == BC_LOW,
	      "fine now: IfPersists, low");

	printf("== the same for 3 minutes: no visit\n");
	home(NOW - 3 * 3600, NOW - 3600);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("LINE_LOSS", "Internet", DS_WARN, NOW - 9000, NOW - 8820, "The next router lost 5% of packets at 19:00.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && o.visit == BV_NO && o.confidence == BC_LOW, "a short warning, seen once: no visit, low");

	printf("== the ISP's network beyond the home: never a visit\n");
	home(NOW - 3600, NOW);
	fresh("ISP_DOWN", "Internet", DS_PROBLEM, "The modem answers, the internet doesn't.");
	timeline("ISP_DOWN", "Internet", DS_PROBLEM, NOW - 3000, NOW, "The internet didn't answer from 20:40.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && o.visit == BV_NO && strstr(o.visit_why, "network team"), "ISP_DOWN: LINE, no visit");
	CHECK(strstr(o.tell, "home equipment is fine") != NULL, "tell: the home is fine");

	printf("== the home filled the line: not a fault; smart queueing offered\n");
	home(NOW - 3600, NOW);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("LINE_FULL", "Internet", DS_WARN, NOW - 3000, NOW - 1200,
		 "The line was full from 20:40 to 21:10: the TV downloaded 48 Mbit/s. The rest waited 180 ms.");
	in.calls = 2;
	in.calls_rough = 1;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_NONE && !strcmp(o.code, "LINE_FULL"), "LINE_FULL: NONE");
	CHECK(strstr(o.verdict, "Not a fault: the home used the whole line.") == o.verdict, "verdict: not a fault");
	CHECK(o.fix.possible && !strcmp(o.fix.action, "sqm"), "fix: smart queueing (a proposal)");
	CHECK(o.visit == BV_NO, "no visit");
	CHECK(has_ev("calls", "2 calls or games in the period, 1 rough"), "the calls in the evidence");
	in.sqm = "Proposed";
	in.sqm_decision = 7;
	brief_judge(&in, &o);
	CHECK(o.fix.possible && !strcmp(o.fix.action, "approve") && o.fix.decision == 7, "a waiting proposal: approve it");
	in.sqm = "On";
	brief_judge(&in, &o);
	CHECK(!o.fix.possible && has_done("Smart queueing is on"), "smart queueing on: nothing to propose, said as done");

	printf("== a laptop hopping between bands: Wi-Fi in the home, keep it on 2.4 GHz\n");
	home(NOW - 3600, NOW);
	in.host = "a laptop";
	in.host_target = "02:00:00:00:00:02";
	fresh("WIFI_HOPPING", "WiFi", DS_WARN, "a laptop changed band 6 times in 10 min: it is at the edge of 5 GHz.");
	timeline("WIFI_HOPPING", "WiFi", DS_WARN, NOW - 1800, NOW, "a laptop changed band 9 times.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_HOME_WIFI && o.fix.possible && !strcmp(o.fix.action, "stay24") &&
	      !strcmp(o.fix.target, "02:00:00:00:00:02") && strstr(o.fix.text, "a laptop"), "HOME_WIFI; fix: stay24 on the laptop");
	CHECK(o.visit == BV_NO && o.confidence == BC_HIGH, "no visit; now and the timeline agree");
	memset(dec, 0, sizeof(dec));
	dec[0].id = 12;
	dec[0].action = "stay24";
	dec[0].words = "a laptop";
	dec[0].state = BD_ACTIVE;
	dec[0].by_aigw = 1;
	dec[0].t = NOW - 600;
	dec[0].reason = "it kept changing band";
	in.dec = dec;
	in.ndec = 1;
	brief_judge(&in, &o);
	CHECK(!o.fix.possible && has_done("kept on 2.4 GHz: a laptop (decision 12, in effect)"), "already kept on 2.4 GHz: done");

	printf("== a busy channel with a quieter one waiting for a yes\n");
	home(NOW - 3600, NOW);
	radio[0].name = "2.4 GHz (wlan0)";
	radio[0].status = HEAL_POOR;
	radio[0].cause = HC_CONGESTION;
	radio[0].since = NOW - 1200;
	radio[0].text = "14% of probes lost with fine signals: the channel is busy.";
	in.radio = radio;
	in.nradio = 1;
	fresh("WIFI_CONGESTION", "WiFi", DS_PROBLEM, "2.4 GHz loses 14% with fine signals: the channel is busy.");
	memset(dec, 0, sizeof(dec));
	dec[0].id = 9;
	dec[0].action = "channel";
	dec[0].words = "wlan0 to channel 11";
	dec[0].state = BD_PROPOSED;
	dec[0].by_aigw = 1;
	dec[0].t = NOW - 300;
	in.dec = dec;
	in.ndec = 1;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_HOME_WIFI && o.fix.possible && !strcmp(o.fix.action, "approve") && o.fix.decision == 9,
	      "fix: approve the channel waiting");
	CHECK(has_ev("wifi", "Poor (Congestion)"), "the access point's health in the evidence");
	CHECK(!has_done("decision 9"), "a proposal is not something AIGW did");

	printf("== a device AIGW quarantined: the device, undo after a check\n");
	home(NOW - 3600, NOW);
	fresh("DEVICE_BLOCKED", "Device", DS_PROBLEM, "AIGW keeps a camera off the network (quarantine, 20h left).");
	memset(dec, 0, sizeof(dec));
	dec[0].id = 4;
	dec[0].action = "quarantine";
	dec[0].words = "a camera";
	dec[0].state = BD_ACTIVE;
	dec[0].by_aigw = 1;
	dec[0].t = NOW - 4 * 3600;
	dec[0].reason = "it looked up 40 machine-made names";
	in.dec = dec;
	in.ndec = 1;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_DEVICE && !strcmp(o.fix.action, "undo") && o.fix.decision == 4 && strstr(o.fix.text, "machine-made"),
	      "DEVICE; fix: undo decision 4, with its reason");
	CHECK(has_done("quarantine a camera (decision 4, in effect)"), "done: the quarantine");

	printf("== all fine, but the calls were rough: a service beyond the ISP (low)\n");
	home(NOW - 3600, NOW);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("OK", "All", DS_OK, NOW - 3600, NOW, "Everything the gateway measured from 20:30 to 21:30 was normal.");
	in.calls = 2;
	in.calls_rough = 2;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_OUTSIDE && !strcmp(o.code, "OUTSIDE_SERVICE") && o.confidence == BC_LOW && o.visit == BV_NO,
	      "OUTSIDE, low confidence, no visit");

	printf("== all fine: NONE\n");
	in.calls = in.calls_rough = 0;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_NONE && !strcmp(o.code, "OK") && o.confidence == BC_HIGH, "NONE, high (now and the timeline)");
	CHECK(strstr(o.tell, "which app") != NULL, "tell: ask which app");

	printf("== nothing to go on: UNKNOWN\n");
	home(NOW - 3600, NOW);
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_UNKNOWN && !strcmp(o.code, "NO_DATA") && o.confidence == BC_LOW && o.visit == BV_NO,
	      "UNKNOWN, NO_DATA, no visit");
	CHECK(has_ev("timeline", "No timeline"), "says there is no timeline");

	printf("== the sources disagree: low confidence, no visit\n");
	home(NOW - 3600, NOW);
	fresh("WIFI_COVERAGE", "WiFi", DS_WARN, "a phone has a weak signal (-79 dBm).");
	timeline("LINE_LOSS", "Internet", DS_WARN, NOW - 900, NOW - 700, "The next router lost 4% at 21:15.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_HOME_WIFI && o.confidence == BC_LOW && strstr(o.verdict, "disagree"), "now decides; they disagree");

	printf("== the line losing packets right after a firmware upgrade that AIGW rolled back: no visit\n");
	home(NOW - 3600, NOW);
	timeline("LINE_LOSS", "Internet", DS_PROBLEM, NOW - 3500, NOW - 600, "The next router lost 9% from 20:32 to 21:20.");
	in.upgrade = "RolledBack";
	in.upgrade_at = NOW - 3000;
	in.upgrade_from = "0.36.0";
	in.upgrade_to = "0.37.0";
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && o.visit == BV_NO && strstr(o.visit_why, "firmware"), "a rolled-back upgrade: no visit");
	CHECK(has_done("Went back from firmware 0.37.0 to 0.36.0") && has_ev("upgrade", "RolledBack"), "the rollback: done, and evidence");

	printf("== the nightly speed far below the line's on 3 nights: a visit\n");
	home(NOW - 3600, NOW);
	fresh("LINE_SLOW_SPEED", "Internet", DS_WARN, "The line delivers 31 Mbit/s of 100.");
	in.speed_at = NOW - 18 * 3600;
	in.speed_mbps = 31;
	in.line_mbps = 100;
	in.speed_low_nights = 3;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && o.visit == BV_YES && o.confidence == BC_HIGH &&
	      has_ev("speed", "31 Mbit/s of 100 sold; low on 3 nights of 7"), "LINE_SLOW_SPEED on 3 nights: visit, high, with the numbers");

	printf("== the gateway was off: not known why; no visit\n");
	home(NOW - 3 * 3600, NOW - 3600);
	timeline("GATEWAY_OFF", "All", DS_PROBLEM, NOW - 9000, NOW - 7000, "The gateway was off or restarting for 33 min.");
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_UNKNOWN && o.visit == BV_NO && strstr(o.tell, "power supply"), "GATEWAY_OFF: UNKNOWN, check the power");

	printf("== a period in the past: the timeline decides, care's problem opened later doesn't\n");
	home(NOW - 5 * 3600, NOW - 4 * 3600);
	fresh("ISP_SLOW", "Internet", DS_WARN, "The internet answers in 240 ms.");
	timeline("OK", "All", DS_OK, NOW - 5 * 3600, NOW - 4 * 3600, "Everything the gateway measured was normal.");
	in.care_open = 1;
	in.care.code = "ISP_SLOW";
	in.care.sev = DS_WARN;
	in.care.since = NOW - 600;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_NONE && !o.current, "then it was fine: NONE");

	printf("== codes to faults\n");
	CHECK(brief_fault_of("WIFI_DROPS") == BF_DEVICE && brief_fault_of("DNS_FAILING") == BF_LINE &&
	      brief_fault_of("WIFI_RADIO") == BF_HOME_WIFI && brief_fault_of("NOT_RECORDED") == BF_UNKNOWN &&
	      brief_fault_of(NULL) == BF_UNKNOWN, "every code has its fault");

	printf("== periods in plain words (now: Wed 21:30 UTC)\n");
	CHECK(brief_period("now", NOW, &from, &to) == 0 && to == NOW && from == NOW - 900, "now: the last 15 min");
	CHECK(brief_period("", NOW, &from, &to) == 0 && from == NOW - 900, "nothing: now");
	CHECK(brief_period("last hour", NOW, &from, &to) == 0 && from == NOW - 3600, "last hour");
	CHECK(brief_period("Last 2h", NOW, &from, &to) == 0 && from == NOW - 7200, "last 2h");
	CHECK(brief_period("90m", NOW, &from, &to) == 0 && from == NOW - 5400, "90m");
	CHECK(brief_period("last 30 min", NOW, &from, &to) == 0 && from == NOW - 1800, "last 30 min");
	CHECK(brief_period("last 12s", NOW, &from, &to) == 0 && from == NOW - 12, "last 12s");
	CHECK(brief_period("tonight 20:00-22:00", NOW, &from, &to) == 0 && from == NOW - 5400 && to == NOW,
	      "tonight 20:00-22:00: from 20:00 until now (it is 21:30)");
	CHECK(brief_period("19:00-20:00", NOW, &from, &to) == 0 && from == NOW - 9000 && to == NOW - 5400, "19:00-20:00 today");
	CHECK(brief_period("22:00-23:00", NOW, &from, &to) == 0 && from == NOW - 86400 + 1800 && to == NOW - 86400 + 5400,
	      "22:00-23:00: later than now, so yesterday's");
	CHECK(brief_period("yesterday 19:00 to 23:00", NOW, &from, &to) == 0 && from == NOW - 86400 - 9000 &&
	      to == NOW - 86400 + 5400, "yesterday 19:00 to 23:00");
	CHECK(brief_period("22:00-01:00", NOW, &from, &to) == 0 && from == NOW - 86400 + 1800 && to == NOW - 73800,
	      "22:00-01:00: over midnight");
	CHECK(brief_period("since 19:30", NOW, &from, &to) == 0 && from == NOW - 7200 && to == NOW, "since 19:30");
	CHECK(brief_period("last week", NOW, &from, &to) < 0 && brief_period("48h", NOW, &from, &to) < 0 &&
	      brief_period("25:00-26:00", NOW, &from, &to) < 0, "not a period (or longer than a day): refused");

	printf("== a known outage in the area (cortex: 5 of 8 homes on the uplink): not this home, no visit\n");
	home(NOW - 3600, NOW);
	fresh("ISP_LOSS", "Internet", DS_PROBLEM, "The modem answers; 30% of packets to the internet are lost.");
	timeline("ISP_LOSS", "Internet", DS_PROBLEM, NOW - 1500, NOW, "The internet lost packets from 20:55.");
	in.area = 7;
	in.area_kind = "LOSS";
	in.area_since = NOW - 1500;
	in.area_homes = 5;
	in.area_of = 8;
	in.area_held = "band steering";
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && !strcmp(o.code, "ISP_LOSS") && o.confidence == BC_HIGH && o.current,
	      "the line's own code stays; LINE, high, now");
	CHECK(strstr(o.verdict, "Known outage in the area: 5 of 8 homes on this uplink losing packets since 21:05 "
			 "(area outage 7). The ISP's network, not this home.") == o.verdict, "verdict: %s", o.verdict);
	CHECK(o.visit == BV_NO && strstr(o.visit_why, "area outage 7"), "no visit: %s", o.visit_why);
	CHECK(strstr(o.tell, "known outage in your area since 21:05") && strstr(o.tell, "no need to restart"),
	      "tell: %s", o.tell);
	CHECK(!o.fix.possible && strstr(o.fix.text, "Nothing to fix in the home"), "no fix in the home");
	CHECK(o.nev >= 2 && !strcmp(o.ev[0].source, "area") && strstr(o.ev[0].text, "area outage 7"), "evidence: the area first");
	CHECK(has_done("Held its own fixes during the area outage: band steering"), "done: what AIGW held");

	printf("== ... the home itself sees nothing wrong, and smart queueing would be on offer: still the area's\n");
	home(NOW - 3600, NOW);
	fresh("OK", "All", DS_OK, "Everything is fine.");
	timeline("LINE_FULL", "Internet", DS_WARN, NOW - 3000, NOW - 1200, "The line was full from 20:40 to 21:10.");
	in.sqm = "Off";
	in.area = 7;
	in.area_kind = "DOWN";
	in.area_since = NOW - 600;
	in.area_homes = 4;
	in.area_of = 30;
	brief_judge(&in, &o);
	show();
	CHECK(o.fault == BF_LINE && !strcmp(o.code, "AREA_OUTAGE") && brief_fault_of("AREA_OUTAGE") == BF_LINE,
	      "AREA_OUTAGE: LINE");
	CHECK(!o.fix.possible && !o.fix.action, "no smart queueing proposal during an area outage");
	CHECK(strstr(o.verdict, "down since 21:20") && strstr(o.verdict, "From here: Not a fault"), "verdict: %s", o.verdict);
	CHECK(!has_done("Held its own fixes"), "nothing held yet: not said");

	printf("== ... a period before it began: the home's own answer\n");
	home(NOW - 3 * 3600, NOW - 2 * 3600);
	timeline("LINE_LOSS", "Internet", DS_PROBLEM, NOW - 9000, NOW - 8000, "The next router lost 12% of packets.");
	in.area = 7;
	in.area_kind = "DOWN";
	in.area_since = NOW - 600;
	in.area_homes = 4;
	in.area_of = 30;
	brief_judge(&in, &o);
	CHECK(!strcmp(o.code, "LINE_LOSS") && !strstr(o.verdict, "Known outage") && o.ev[0].source != NULL &&
	      strcmp(o.ev[0].source, "area"), "before the outage: untouched");

	printf("== ... the evidence full: the area still first, nothing past the end\n");
	home(NOW - 3600, NOW);
	fresh("ISP_DOWN", "Internet", DS_PROBLEM, "Down.");
	timeline("ISP_DOWN", "Internet", DS_PROBLEM, NOW - 1500, NOW, "Down from 20:55.");
	{
		static struct brief_finding more[8];
		int i;

		for (i = 0; i < 8; i++) {
			more[i].code = i % 2 ? "LINE_LOSS" : "DNS_SLOW";
			more[i].area = "Internet";
			more[i].sev = DS_WARN;
			more[i].since = NOW - 1000 + i;
			more[i].text = "more";
		}
		in.why_more = more;
		in.nwhy_more = 8;
		in.warning = "rising";
		in.score_week = 80;
		in.speed_at = NOW - 7200;
		in.speed_mbps = 40;
		in.calls = 3;
		in.care_open = 1;
		in.care.code = "ISP_DOWN";
		in.care.area = "Internet";
		in.care.sev = DS_PROBLEM;
		in.care.since = NOW - 1400;
		radio[0].name = "5 GHz (wlan1)";
		radio[0].status = HEAL_POOR;
		radio[0].cause = HC_CONGESTION;
		radio[0].since = NOW - 600;
		radio[0].text = "busy";
		in.radio = radio;
		in.nradio = 1;
		in.upgrade = "Kept";
		in.upgrade_at = NOW - 3000;
		in.upgrade_from = "0.38.1";
		in.upgrade_to = "0.39.0";
		brief_judge(&in, &o);
		CHECK(o.nev == BRIEF_LINES, "the evidence is full without the area: %d lines", o.nev);
		in.area = 8;
		in.area_kind = "DOWN";
		in.area_since = NOW - 1500;
		in.area_homes = 9;
		in.area_of = 12;
		brief_judge(&in, &o);
		CHECK(o.nev == BRIEF_LINES && !strcmp(o.ev[0].source, "area"), "full evidence: %d lines, area first", o.nev);
	}

	printf("== names out\n");
	brief_kind("", "laptop", buf, sizeof(buf));
	CHECK(!strcmp(buf, "a laptop"), "laptop: %s", buf);
	brief_kind("Android", "sara-phone", buf, sizeof(buf));
	CHECK(!strcmp(buf, "an Android phone"), "Android: %s", buf);
	brief_kind("IoT", "esp32-1", buf, sizeof(buf));
	CHECK(!strcmp(buf, "a smart-home device"), "IoT: %s", buf);
	brief_kind("", "", buf, sizeof(buf));
	CHECK(!strcmp(buf, "a device"), "nothing known: %s", buf);
	{
		const char *nm[] = { "tv", "tv-kitchen", "02:00:00:00:00:05", "Sara's iPhone" };
		const char *kd[] = { "a TV", "a TV", "a device", "an iPhone" };

		snprintf(buf, sizeof(buf), "tv-kitchen moved to 2.4 GHz; tv left; 02:00:00:00:00:05 joined; tvbox stayed; "
			 "Sara's iPhone weak; the tv.");
		brief_scrub(buf, sizeof(buf), nm, kd, 4);
		CHECK(!strcmp(buf, "a TV moved to 2.4 GHz; a TV left; a device joined; tvbox stayed; an iPhone weak; the a TV."),
		      "whole names only, longest first: %s", buf);
		snprintf(buf, 24, "tv tv tv tv tv");
		brief_scrub(buf, 24, nm, kd, 1);
		CHECK(strlen(buf) < 24 && !strncmp(buf, "a TV a TV", 9) && !strstr(buf, " tv"),
		      "no room: cut there, no name left, no overflow: %s", buf);
	}
	{
		/* more names than fit on the stack (a big home: name, MAC and address each) */
		static char nb[600][16], kb[600][8];
		const char *nm[600], *kd[600];
		int i;

		for (i = 0; i < 600; i++) {
			snprintf(nb[i], sizeof(nb[i]), "host%03d", i);
			snprintf(kb[i], sizeof(kb[i]), "a TV");
			nm[i] = nb[i];
			kd[i] = kb[i];
		}
		snprintf(buf, sizeof(buf), "host007 and host599 left");
		brief_scrub(buf, sizeof(buf), nm, kd, 600);
		CHECK(!strcmp(buf, "a TV and a TV left"), "600 names: all taken out: %s", buf);
	}

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
