/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW smarter mesh Wi-Fi: the Data Elements read and the rules (ag/mesh.h) */
#include <stdio.h>
#include <string.h>

#include "ag/mesh.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 1800000000L
#define DE "Device.WiFi.DataElements.Network."

static struct mesh_net net;
static struct mesh_params p;

static void set(const char *path, const char *v)
{
	char full[300];

	snprintf(full, sizeof(full), DE "%s", path);
	mesh_set(&net, full, v);
}

static void setf(const char *path, int v)
{
	char b[24];

	snprintf(b, sizeof(b), "%d", v);
	set(path, b);
}

/* dBm -> RCPI */
static int rcpi(int dbm)
{
	return (dbm + 110) * 2;
}

/*
 * A home: the gateway (2.4 GHz ch 1, 5 GHz ch 36/80) and the extender "bedroom" on a wireless
 * 5 GHz backhaul (2.4 GHz ch 1, 5 GHz ch 36/80, the backhaul station on the 5 GHz radio).
 * The laptop sits on the gateway's 2.4 GHz at -80 dBm; the extender's 5 GHz hears it at -60.
 */
static void home(int ext_bh_dbm)
{
	mesh_reset(&net);
	set("ID", "home-1");
	set("ControllerID", "02:00:00:00:00:01");
	set("Device.1.ID", "02:00:00:00:00:01");
	set("Device.1.MultiAPDevice.Backhaul.LinkType", "None");
	set("Device.1.Radio.1.ID", "02:00:00:00:01:00");
	set("Device.1.Radio.1.CurrentOperatingClassProfile.1.Class", "81");
	set("Device.1.Radio.1.CurrentOperatingClassProfile.1.Channel", "1");
	setf("Device.1.Radio.1.Utilization", 60);
	set("Device.1.Radio.1.BSS.1.BSSID", "02:00:00:00:01:01");
	set("Device.1.Radio.1.BSS.1.SSID", "home");
	set("Device.1.Radio.2.ID", "02:00:00:00:02:00");
	set("Device.1.Radio.2.CurrentOperatingClassProfile.1.Class", "115");
	set("Device.1.Radio.2.CurrentOperatingClassProfile.1.Channel", "36");
	set("Device.1.Radio.2.CurrentOperatingClassProfile.2.Class", "128");
	set("Device.1.Radio.2.CurrentOperatingClassProfile.2.Channel", "42");
	set("Device.1.Radio.2.BSS.1.BSSID", "02:00:00:00:02:01");
	set("Device.1.Radio.2.BSS.1.SSID", "home");
	set("Device.1.Radio.2.BSS.2.BSSID", "02:00:00:00:02:02");
	set("Device.1.Radio.2.BSS.2.SSID", "home-bh");
	set("Device.1.Radio.2.BSS.2.BackhaulUse", "true");
	set("Device.1.Radio.2.BSS.2.FronthaulUse", "false");

	set("Device.2.ID", "02:00:00:00:00:02");
	set("Device.2.MultiAPDevice.Backhaul.LinkType", "Wi-Fi");
	set("Device.2.MultiAPDevice.Backhaul.BackhaulMACAddress", "02:00:00:00:02:02");
	set("Device.2.MultiAPDevice.Backhaul.BackhaulDeviceID", "02:00:00:00:00:01");
	set("Device.2.MultiAPDevice.Backhaul.MACAddress", "02:00:00:00:00:2f");
	setf("Device.2.MultiAPDevice.Backhaul.Stats.SignalStrength", rcpi(ext_bh_dbm));
	setf("Device.2.MultiAPDevice.Backhaul.Stats.LastDataDownlinkRate", ext_bh_dbm <= -72 ? 54000 : 600000);
	set("Device.2.MultiAPDevice.Backhaul.CurrentOperatingClassProfile.1.Class", "128");
	set("Device.2.MultiAPDevice.Backhaul.CurrentOperatingClassProfile.1.Channel", "42");
	set("Device.2.Radio.1.ID", "02:00:00:00:03:00");
	set("Device.2.Radio.1.CurrentOperatingClassProfile.1.Class", "81");
	set("Device.2.Radio.1.CurrentOperatingClassProfile.1.Channel", "1");
	set("Device.2.Radio.1.BSS.1.BSSID", "02:00:00:00:03:01");
	set("Device.2.Radio.1.BSS.1.SSID", "home");
	set("Device.2.Radio.2.ID", "02:00:00:00:04:00");
	set("Device.2.Radio.2.CurrentOperatingClassProfile.1.Class", "128");
	set("Device.2.Radio.2.CurrentOperatingClassProfile.1.Channel", "42");
	set("Device.2.Radio.2.BSS.1.BSSID", "02:00:00:00:04:01");
	set("Device.2.Radio.2.BSS.1.SSID", "home");

	/* the laptop on the gateway's 2.4 GHz, weak; 802.11v and k */
	set("Device.1.Radio.1.BSS.1.STA.1.MACAddress", "AA:00:00:00:00:01");
	setf("Device.1.Radio.1.BSS.1.STA.1.SignalStrength", rcpi(-80));
	setf("Device.1.Radio.1.BSS.1.STA.1.LastDataDownlinkRate", 6500);
	setf("Device.1.Radio.1.BSS.1.STA.1.LastConnectTime", 3600);
	set("Device.1.Radio.1.BSS.1.STA.1.ClientCapabilities",
	    "3104 0a00 0004 686f6d65 7f080000080000000040 4605720000 0000");
	/* the extender's 5 GHz hears it well */
	set("Device.2.Radio.2.UnassociatedSTA.1.MACAddress", "aa:00:00:00:00:01");
	setf("Device.2.Radio.2.UnassociatedSTA.1.SignalStrength", rcpi(-60));
	/* a phone on the extender, fine */
	set("Device.2.Radio.2.BSS.1.STA.1.MACAddress", "aa:00:00:00:00:02");
	setf("Device.2.Radio.2.BSS.1.STA.1.SignalStrength", rcpi(-55));
	set("Device.2.Radio.2.BSS.1.STA.1.ClientCapabilities", "nonsense");
	mesh_finish(&net);
}

static void read_model(void)
{
	int d, r, b;

	home(-60);
	CHECK(net.n_dev == 2 && net.dev[0].gateway && !net.dev[1].gateway, "the controller's agent is the gateway");
	CHECK(net.dev[0].radio[1].band == 5 && net.dev[0].radio[1].channel == 36 && net.dev[0].radio[1].width == 80 &&
	      net.dev[0].radio[1].opclass == 128, "5 GHz: channel 36 at 80 MHz from classes 115 + 128 (%d/%d/%d)",
	      net.dev[0].radio[1].channel, net.dev[0].radio[1].width, net.dev[0].radio[1].opclass);
	CHECK(net.dev[1].radio[1].channel == 36 && net.dev[1].radio[1].width == 80,
	      "only class 128, centre 42: the primary channel is 36 (%d)", net.dev[1].radio[1].channel);
	CHECK(net.dev[0].radio[0].band == 2 && net.dev[0].radio[0].util == 23, "2.4 GHz, Utilization 60/255 = 23%% (%d)",
	      net.dev[0].radio[0].util);
	CHECK(net.dev[1].bh == MESH_BH_WIFI && net.dev[1].bh_dbm == -60 && net.dev[1].bh_band == 5 &&
	      !strcmp(net.dev[1].bh_parent, "02:00:00:00:00:01"), "the extender's backhaul: Wi-Fi, -60 dBm, 5 GHz");
	CHECK(net.n_sta == 2 && !strcmp(net.sta[0].mac, "aa:00:00:00:00:01") && net.sta[0].dbm == -80 &&
	      net.sta[0].dev == 0 && net.sta[0].radio == 0, "the laptop: lower-case MAC, RCPI 60 = -80 dBm, on the gateway");
	CHECK(net.sta[0].btm == MESH_CAP_YES && net.sta[0].rrm == MESH_CAP_YES, "its (Re)Association Request: 802.11v and k");
	CHECK(net.sta[1].btm == MESH_CAP_UNKNOWN, "a frame that doesn't parse: not known");
	CHECK(net.n_un == 1 && net.un[0].dbm == -60 && net.un[0].dev == 1, "the extender hears it at -60 dBm");
	CHECK(mesh_find_bss(&net, "02:00:00:00:04:01", &d, &r, &b) == 0 && d == 1 && r == 1, "BSS by BSSID");
	CHECK(mesh_set(&net, "Device.WiFi.Radio.1.Channel", "6") == -1, "not a Data Elements path: refused");
	CHECK(mesh_set(&net, DE "Device.1.Radio.1.Unknown", "6") == 1, "an unknown parameter: ignored");
	CHECK(mesh_rcpi_dbm(0) == 0 && mesh_rcpi_dbm(255) == 0 && mesh_rcpi_dbm(220) == 0, "RCPI 0 and 221+: not known");

	/* aliases instead of numbers, and the path without the prefix */
	mesh_reset(&net);
	mesh_set(&net, "Network.Device.cpe-gw.ID", "02:00:00:00:00:09");
	mesh_set(&net, "Network.Device.cpe-gw.Radio.cpe-r1.BSS.cpe-b1.STA.cpe-s1.MACAddress", "aa:00:00:00:00:09");
	mesh_set(&net, "Network.Device.cpe-gw.Radio.cpe-r1.BSS.cpe-b1.STA.cpe-s1.SignalStrength", "120");
	mesh_finish(&net);
	CHECK(net.n_dev == 1 && net.n_sta == 1 && net.sta[0].dbm == -50 && net.dev[0].gateway, "aliases as instances");
}

static void caps(void)
{
	int btm, rrm;

	mesh_caps("3104 0a00 0004 74657374 7f03000008", &btm, &rrm);
	CHECK(btm == MESH_CAP_YES && rrm == MESH_CAP_NO, "association: BSS transition bit 19, no RM element");
	mesh_caps("3104 0a00 020000000001 0004 74657374 7f03000000 4605720000 0000", &btm, &rrm);
	CHECK(btm == MESH_CAP_NO && rrm == MESH_CAP_YES, "reassociation (current AP): no BSS transition, RM beacon reports");
	mesh_caps("3104 0a00 0004 74657374", &btm, &rrm);
	CHECK(btm == MESH_CAP_NO && rrm == MESH_CAP_NO, "no extended capabilities at all: neither");
	mesh_caps("", &btm, &rrm);
	CHECK(btm == MESH_CAP_UNKNOWN, "nothing: not known");
}

static void units(void)
{
	CHECK(mesh_center(5, 36, 80) == 42 && mesh_center(5, 153, 80) == 155 && mesh_center(5, 44, 40) == 46 &&
	      mesh_center(2, 1, 40) == 3 && mesh_center(6, 5, 80) == 7, "centre channels");
	CHECK(mesh_opclass(5, 149, 80) == 128 && mesh_opclass(5, 149, 20) == 124 && mesh_opclass(5, 36, 40) == 116 &&
	      mesh_opclass(5, 40, 40) == 117 && mesh_opclass(2, 6, 20) == 81 && mesh_opclass(6, 37, 80) == 133,
	      "operating classes");
	CHECK(mesh_chan_clash(2, 1, 20, 3, 20) && !mesh_chan_clash(2, 1, 20, 6, 20), "2.4 GHz: 1 and 3 overlap, 1 and 6 don't");
	CHECK(mesh_chan_clash(5, 36, 80, 44, 20) && !mesh_chan_clash(5, 36, 20, 40, 20) && !mesh_chan_clash(5, 36, 80, 149, 80),
	      "5 GHz: an 80 MHz block holds 44; 36 and 40 at 20 MHz don't clash");
}

static void steering(void)
{
	struct mesh_hist h;
	struct mesh_extra x = { -1, 0, 0 };
	struct mesh_steer s;
	char skip[160];
	int sta;

	mesh_params_default(&p);
	home(-60);
	sta = mesh_find_sta(&net, "aa:00:00:00:00:01");
	memset(&h, 0, sizeof(h));
	h.retry_pct = -1;
	mesh_hist_update(&h, &net, sta, T0);
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0, &s, skip, sizeof(skip)) && strstr(skip, "waiting"),
	      "struggling just now: waits (%s)", skip);
	CHECK(mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)) &&
	      !strcmp(s.to, "02:00:00:00:04:01") && s.exp_dbm == -60 && s.method == MESH_STEER_BTM,
	      "a minute on: to the extender's 5 GHz, heard at -60 (%s)", s.why);
	CHECK(strstr(s.why, "the extender 02:00:00:00:00:02 5 GHz") && strstr(s.why, "measured by that access point"),
	      "why, in words: %s", s.why);
	strcpy(net.dev[1].name, "bedroom");
	mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip));
	CHECK(strstr(s.why, "the extender 'bedroom'") != NULL, "the extender by the home's name for it");

	/* hysteresis: heard only 6 dB better */
	net.un[0].dbm = -74;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)), "only 6 dB better (and below -68): stays (%s)",
	      skip);
	net.un[0].dbm = -60;
	net.dev[1].radio[1].util = 90;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)), "the target radio is 90%% busy: stays");
	net.dev[1].radio[1].util = -1;
	x.in_call = 1;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)) && strstr(skip, "call"),
	      "in a call: not now (%s)", skip);
	x.in_call = 0;
	x.protected_dev = 1;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)), "protected: never");
	x.protected_dev = 0;
	net.sta[sta].steer_s = 100;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)) && strstr(skip, "controller"),
	      "the controller steered it 100 s ago: not fighting it (%s)", skip);
	net.sta[sta].steer_s = -1;
	net.sta[sta].btm = MESH_CAP_NO;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)) && strstr(skip, "802.11v"),
	      "no 802.11v: not without mesh.legacy (%s)", skip);
	p.legacy = 1;
	CHECK(mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61, &s, skip, sizeof(skip)) && s.method == MESH_STEER_MANDATE,
	      "mesh.legacy 1: the controller's blocklist steering");
	p.legacy = 0;
	net.sta[sta].btm = MESH_CAP_YES;

	/* a steer of ours: cooldown; one that didn't land twice: a pause */
	h.last_steer = T0 + 61;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 600, &s, skip, sizeof(skip)), "cooldown after our own steer");
	CHECK(mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 61 + 901, &s, skip, sizeof(skip)), "after the cooldown: again");
	mesh_steer_failed(&h, &p, T0 + 1000, 0, s.from, s.to);
	CHECK(h.pause_until == 0 && h.fails == 1, "one miss: no pause yet");
	mesh_steer_failed(&h, &p, T0 + 2000, 0, s.from, s.to);
	CHECK(h.pause_until == T0 + 2000 + 3600, "two misses in a row: left alone an hour");
	mesh_steer_failed(&h, &p, T0 + 9000, 0, s.from, s.to);
	CHECK(h.pause_until == T0 + 9000 + 7200, "three: two hours (doubling)");
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 9100, &s, skip, sizeof(skip)) && strstr(skip, "didn't land"),
	      "paused (%s)", skip);
	/* a move that landed but didn't help: that pair avoided */
	memset(&h, 0, sizeof(h));
	h.retry_pct = -1;
	h.struggling_since = T0;
	mesh_steer_failed(&h, &p, T0 + 100, 1, "02:00:00:00:01:01", "02:00:00:00:04:01");
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 200, &s, skip, sizeof(skip)) && h.undone == 1,
	      "a move undone: that target avoided for a day (%s)", skip);
	CHECK(mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 100 + 86401, &s, skip, sizeof(skip)), "... and tried again after it");

	/* the device moved by itself */
	memset(&h, 0, sizeof(h));
	h.retry_pct = -1;
	mesh_hist_update(&h, &net, sta, T0);
	net.sta[sta].bss = 0;
	net.sta[sta].radio = 1;     /* now on the gateway's 5 GHz */
	mesh_hist_update(&h, &net, sta, T0 + 30);
	CHECK(h.moved_at == T0 + 30, "it changed BSS between reads: moved by itself");
	net.sta[sta].radio = 0;

	/* a strong link that loses traffic: retries */
	home(-60);
	memset(&h, 0, sizeof(h));
	net.sta[sta].dbm = -62;
	net.sta[sta].pkts = 1000;
	net.sta[sta].retrans = 0;
	mesh_hist_update(&h, &net, sta, T0);
	net.sta[sta].pkts = 2000;
	net.sta[sta].retrans = 300;
	mesh_hist_update(&h, &net, sta, T0 + 30);
	CHECK(h.retry_pct == 30, "30%% of packets sent again (%d)", h.retry_pct);
	h.struggling_since = T0 - 100;
	net.un[0].dbm = -50;
	CHECK(mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 30, &s, skip, sizeof(skip)) && strstr(s.why, "30% sent again"),
	      "strong but losing traffic, and heard 12 dB better elsewhere: moves (%s)", s.why);

	/* the same box's other band, estimated */
	home(-60);
	net.n_un = 0;
	net.sta[sta].dbm = -76;
	memset(&h, 0, sizeof(h));
	h.retry_pct = -1;
	h.struggling_since = T0;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 100, &s, skip, sizeof(skip)),
	      "-76 on 2.4 GHz: the gateway's 5 GHz would be about -83: no (%s)", skip);
	net.sta[sta].dbm = -76;
	net.sta[sta].radio = 1;     /* on 5 GHz at -76: 2.4 GHz would be about -69, only 7 dB better */
	net.sta[sta].bss = 0;
	CHECK(!mesh_steer_pick(&net, sta, &h, &x, &p, T0 + 100, &s, skip, sizeof(skip)), "hysteresis on the estimate too");
}

static void trials(void)
{
	struct mesh_sample a = { -80, 6500, -1, 12 }, z = { -60, 200000, -1, 1 };
	char why[240];

	mesh_params_default(&p);
	CHECK(mesh_trial_judge(&a, &z, &p, why, sizeof(why)) == MESH_KEEP, "stronger, faster, losing less: kept (%s)", why);
	z.loss_pct = 20;
	CHECK(mesh_trial_judge(&a, &z, &p, why, sizeof(why)) == MESH_UNDO && strstr(why, "lost 12% -> 20%"),
	      "faster but losing more: undone (%s)", why);
	z = a;
	z.dbm = -78;
	CHECK(mesh_trial_judge(&a, &z, &p, why, sizeof(why)) == MESH_UNDO && strstr(why, "not clearly better"),
	      "about the same: undone (%s)", why);
	z.kbps = 7000;
	CHECK(mesh_trial_judge(&a, &z, &p, why, sizeof(why)) == MESH_UNDO, "7.7%% faster is within the margin");
	z.kbps = 8000;
	CHECK(mesh_trial_judge(&a, &z, &p, why, sizeof(why)) == MESH_KEEP, "23%% faster, nothing worse: kept (%s)", why);
}

static void channels(void)
{
	struct mesh_chan c[4];
	int k;

	/* a wired extender on the gateway's channels: both bands move */
	home(-60);
	net.dev[1].bh = MESH_BH_ETH;
	net.dev[1].bh_band = 0;
	k = mesh_chan_plan(&net, c, 4);
	CHECK(k == 2 && c[0].dev == 1 && c[0].from == 1 && c[0].to == 6 && c[1].from == 36 && c[1].to == 149 &&
	      c[1].opclass == 128, "wired extender: 2.4 GHz 1 -> 6, 5 GHz 36 -> 149 at 80 MHz (%d changes)", k);
	CHECK(k > 0 && strstr(c[0].why, "the same air as the gateway"), "why: %s", k > 0 ? c[0].why : "");

	/* a wireless 5 GHz backhaul: that radio is tied to the gateway's channel; 2.4 GHz still moves */
	home(-60);
	k = mesh_chan_plan(&net, c, 4);
	CHECK(k == 1 && c[0].from == 1 && c[0].to == 6, "wireless 5 GHz backhaul: only 2.4 GHz moves (%d)", k);

	/* the gateway is never moved; nothing clashes: no plan */
	home(-60);
	net.dev[1].radio[0].prof_ch[0] = 11;
	mesh_finish(&net);
	CHECK(mesh_chan_plan(&net, c, 4) == 0, "extender on 11, gateway on 1: nothing to do");

	/* every channel taken by the home's own radios: unavoidable, nothing proposed */
	home(-60);
	net.dev[1].bh = MESH_BH_ETH;
	net.dev[0].radio[0].n_prof = 1;
	net.dev[0].n_radio = 3;
	memset(&net.dev[0].radio[2], 0, sizeof(net.dev[0].radio[2]));
	net.dev[0].radio[2].n_prof = 1;
	net.dev[0].radio[2].prof_class[0] = 128;
	net.dev[0].radio[2].prof_ch[0] = 155;
	mesh_finish(&net);
	k = mesh_chan_plan(&net, c, 4);
	CHECK(k == 1 && c[0].from == 1, "gateway on 36 and 149: the extender's 5 GHz has nowhere to go; only 2.4 GHz moves");
}

static void backhaul(void)
{
	struct mesh_bh_finding f[4];
	int k;

	mesh_params_default(&p);
	home(-78);
	strcpy(net.dev[1].name, "bedroom");
	k = mesh_backhaul(&net, &p, f, 4);
	CHECK(k == 1 && f[0].kind == MBH_WEAK && f[0].hurts && f[0].stations == 1,
	      "-78 dBm backhaul with a phone behind it: weak, hurts more than it helps (%d)", k);
	CHECK(k == 1 && strstr(f[0].words, "the extender 'bedroom' has a weak wireless backhaul to the gateway (-78 dBm, 54 Mbit/s, 5 GHz)"),
	      "%s", k ? f[0].words : "");
	CHECK(k == 1 && strstr(f[0].advice, "closer"), "advice: %s", k ? f[0].advice : "");

	home(-60);
	CHECK(mesh_backhaul(&net, &p, f, 4) == 0, "a good 5 GHz backhaul: nothing to say");
	net.dev[1].bh = MESH_BH_ETH;
	CHECK(mesh_backhaul(&net, &p, f, 4) == 0, "wired: nothing to say");

	home(-60);
	net.dev[1].bh_band = 2;
	k = mesh_backhaul(&net, &p, f, 4);
	CHECK(k == 1 && f[0].kind == MBH_24 && !f[0].hurts, "a 2.4 GHz backhaul where both ends have 5 GHz: On24");

	home(-60);
	net.dev[0].radio[0].band = 6;
	net.dev[1].radio[0].band = 6;
	k = mesh_backhaul(&net, &p, f, 4);
	CHECK(k == 1 && f[0].kind == MBH_TRY6, "both ends with 6 GHz and -60 dBm on 5 GHz: try a 6 GHz backhaul");
}

int main(void)
{
	read_model();
	caps();
	units();
	steering();
	trials();
	channels();
	backhaul();
	printf("%s\n", failed ? "FAILED" : "all passed");
	return failed ? 1 : 0;
}
