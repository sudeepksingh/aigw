/* SPDX-License-Identifier: Apache-2.0 */
/* agctl: command-line view of the AIGW brain over ubus. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"
#include "ag/qr.h"
#include "ag/sign.h"

static struct ubus_context *ctx;
static int json_out;
static const char *device_filter;
static int call_timeout = 30000;        /* ms; a TR-143 test may take minutes */

static void usage(void)
{
	fprintf(stderr,
		"usage: agctl [-j] <command>\n"
		"\n"
		"  status              brain and sensor status\n"
		"  devices             devices: identity, Wi-Fi, lookups, traffic\n"
		"  device <ip|mac|name>  one device in detail\n"
		"  dns [N]             last N DNS lookups (default 30)\n"
		"  dns cache           the DNS cache: hits, answer times, prefetches, stale answers\n"
		"  wifi                access points (band, channel), their devices, band steering\n"
		"  wifi --report       what Self-Healing Wi-Fi found and fixed: last day, 7 and 30 days\n"
		"  channels            neighbour-aware channels: each channel's air by hour of day, the home's\n"
		"                      busy hours, whether another is clearly better, a planned move's trial\n"
		"  diagnose [device]   \"my internet is slow\": the line, DNS, Wi-Fi and the device, checked\n"
		"                      in a few seconds; what is wrong, most likely first, and what to do\n"
		"  care proactive [--run]  before the call: faults a customer would notice, fixed by AIGW\n"
		"                      (as act.mode allows) or raised to the ISP with a note for the customer\n"
		"  care [--check|--speed]  proactive care: problems found by the gateway itself, early\n"
		"                      warnings, the experience score and the nightly speed (--speed: test now;\n"
		"                      --speed <http://server/file>: once, against a server of your own)\n"
		"  repair [--stats|--shadow]  proven repair: each fix AIGW made with what it predicted, what\n"
		"                      was measured after it (helped, no change, worse) and whether it was undone;\n"
		"                      the fix rate by fault and kind of fix (last day, 7, 30 days); in advise\n"
		"                      mode, what AIGW would have done (shadows, never applied)\n"
		"  upgrade [--forget]  the last firmware upgrade: the home before and after it, kept or rolled\n"
		"                      back, and why (--forget: a version rolled back may be installed again)\n"
		"  why [device] [--at 20:00 | --from 19:30 --to 21:00 | --last 2h] [--raw]\n"
		"                      \"why was it slow?\" afterwards: the home's timeline (the last hour by\n"
		"                      default) explained, with what happened in order\n"
		"  brief [device] [now | \"last 2h\" | \"tonight 20:00-22:00\" | --since 19:30] [--no-check]\n"
		"        [--speed] [--propose]\n"
		"                      the help desk's answer: the fault in one word, the evidence, what AIGW\n"
		"                      did, what to tell the customer, a remote fix (--propose: a decision\n"
		"                      that waits for a yes) and whether a technician should go out\n"
		"  sqm [--on|--off]    smart queueing on the line: AIGW turns it on when the line is full and\n"
		"                      everything else slows (bufferbloat), and keeps it only if it helped\n"
		"  calls               calls and games first: the ones now (first in smart queueing), how\n"
		"                      the last one went while the line was full, the counts\n"
		"  mesh [--run]        mesh Wi-Fi over the EasyMesh controller: access points, their backhaul,\n"
		"                      each device's access point and link, the channel plan, AIGW's moves\n"
		"  mesh steer <device> <BSSID> [time]   move a device to another access point of the mesh\n"
		"  mesh channel <radio> <ch> [time]     move a mesh radio to another channel (trials both)\n"
		"  coverage            where Wi-Fi is weak: which device, when, where in the home (spots named\n"
		"                      by the devices seen there), and what fixes it (move it, or an extender)\n"
		"  home                the home Wi-Fi model: its places, and what an extender, moving the\n"
		"                      gateway or another band would fix of the last 30 days' problems\n"
		"  home name <device|N> <zone>   call the place a device is at (or place N) e.g. \"bedroom\"\n"
		"  sense               Wi-Fi sensing (opt-in): what is sensed, someone home, movement by zone\n"
		"  sense on --i-agree  turn it on with the home's consent; sense off: off, and forget it all\n"
		"  offers              signals for the ISP from the last week: a faster plan, smart queueing,\n"
		"                      mesh Wi-Fi, a customer at risk, a line to repair; with the evidence\n"
		"  test ping <host> [count]          TR-143 tests, as an ISP runs them (USP, TR-069):\n"
		"  test trace <host>                 echo requests, the route,\n"
		"  test download <url> [seconds]     an HTTP download (seconds: time based),\n"
		"  test upload <url> [bytes|Ns]      an HTTP upload (N s: time based),\n"
		"  test dns <name> [server]          a name looked up (default: the gateway's own DNS)\n"
		"  flows [N]           last N finished connections (default 30)\n"
		"  alerts [N]          what AIGW thinks you should know, and why\n"
		"  score <domain>      the domain-risk model's view of a name\n"
		"  watch [dns|flows|wifi|home|alerts|decisions|plugins|models|upgrade|repair|all]   follow activity\n"
		"                      live\n"
		"                      (Ctrl+C to stop)\n"
		"\n"
		"  decisions [N]       actions waiting for you, in effect, and past ones\n"
		"  approve <id> [time] carry out a proposed action (optionally for another time limit)\n"
		"  reject <id>         say no to a proposed action\n"
		"  undo <id>           end an action now\n"
		"  prioritize <device> [time]    send a device's traffic first (Wi-Fi video queue)\n"
		"  deprioritize <device> [time]  send it last (Wi-Fi background queue)\n"
		"  throttle <device> [rate] [time]  limit its speed, e.g. 2mbit, 500kbit (default from config)\n"
		"  block <domain> [time] [device]   stop resolving a domain and its subdomains,\n"
		"                                   for every device or just one\n"
		"  quarantine <device> [time]    cut a device off from everything but the gateway\n"
		"  disconnect <device> [time]    kick it off Wi-Fi and refuse it until the time is up\n"
		"  channel <ap|2.4|5> <ch> [time]  move an access point to another channel (devices\n"
		"                                follow); back to the old one when the time is up\n"
		"  do <plugin.action> <device|target> [time]   an action a plug-in offers (agctl plugins)\n"
		"                      time: 90s, 30m, 2h, 1d (default from config, 1h)\n"
		"\n"
		"  plugins           plug-ins: sensors, actuators, skills, connectors; state and why\n"
		"  plugin <name>       one plug-in in detail\n"
		"  plugin add <folder>           install a plug-in folder (needs sudo); it starts by itself\n"
		"  plugin remove <name>          remove a plug-in you added\n"
		"  plugin allow|deny <name>      let a plug-in you added use its privileged capabilities\n"
		"  plugin enable|disable <name>  turn a plug-in on or off\n"
		"  plugin config <name> <key> <value|->  change a plug-in's setting (- reads it from stdin)\n"
		"\n"
		"  models              models in use: version, where from, who signed it\n"
		"  model install <file.agm> [file.sig] [--force]  load a model update now (needs a\n"
		"                      trusted signature; --force also allows an older version)\n"
		"  model rollback [name]         go back to the previous model and hold that version off\n"
		"  trust <key.pub>     accept models signed with this usign key (e.g. your cortex's)\n"
		"  untrust <fingerprint>         stop accepting a key\n"
		"  bridge [url]        link to cortex: show the bridge, or set cortex's address\n"
		"  bridge token        set this gateway's token (paste it; read from stdin)\n"
		"  fed                 federated learning: your answers, rounds joined, privacy spent\n"
		"  label <domain> safe|threat    tell AIGW what a name is (kept here; it trains the\n"
		"                      shared layer without the name leaving)\n"
		"  fed forget [domain] drop one answer, or all of them\n"
		"  fleet               fleet learning: what was shared, what came back, the choices it changed\n"
		"  fleet rank <code> [device]    how each fix did for this fault, here and in the fleet\n"
		"\n"
		"  remote              remote access: on or off, peers, who is connected\n"
		"  remote on <address> turn it on; <address>: how phones reach this home (public IP or\n"
		"                      DDNS name; forward UDP 51820 to the gateway if it is behind a router)\n"
		"  remote off          turn it off\n"
		"  remote add <name> [--conf]  a WireGuard QR code for a phone (--conf: the config as text)\n"
		"  remote revoke <name>        take a peer's access away\n"
		"\n"
		"  dm get <path>...    the data model (TR-181) as USP and TR-069 see it; a path\n"
		"                      ending in '.' means everything under it (dm get Device.)\n"
		"  dm set <path> <value>...      change values (all or nothing)\n"
		"  dm do <command()> [Arg=value]...  run a command, e.g. dm do Device.X_AIGW_.Decision.3.Approve()\n"
		"  dm schema           every parameter, command and event\n"
		"\n"
		"  keys                Wi-Fi keys of individual devices\n"
		"  key add <name> [zone] [time]  new key and a QR code to scan; the first device to\n"
		"                                join with it owns it (zone e.g. iot; time: how long it waits)\n"
		"  key qr <name>       show a key's QR code again\n"
		"  key revoke <name>   take a device's key away and disconnect it\n"
		"  zone <device> <zone|none>     put a device in a zone; policies can be limited to one\n"
		"  qr <text> [--pbm]   any text as a QR code\n"
		"  selftest            checks on this device's CPU: signatures, model, QR, DNS via agdns\n"
		"                      (aigw-selftest runs it with the service checks)\n"
		"  version             agctl version\n"
		"\n"
		"  -j                  print raw JSON\n");
}

/* ---------- formatting helpers ---------- */

static const char *ago(uint64_t ts, char *out, size_t n)
{
	int64_t d = ag_wall_s() - (int64_t)ts;

	if (!ts)
		snprintf(out, n, "-");
	else if (d < 60)
		snprintf(out, n, "%llds ago", (long long)d);
	else if (d < 3600)
		snprintf(out, n, "%lldm ago", (long long)(d / 60));
	else if (d < 86400)
		snprintf(out, n, "%lldh ago", (long long)(d / 3600));
	else
		snprintf(out, n, "%lldd ago", (long long)(d / 86400));
	return out;
}

static const char *human_bytes(uint64_t v, char *out, size_t n)
{
	if (v < 1024)
		snprintf(out, n, "%lluB", (unsigned long long)v);
	else if (v < 1024 * 1024)
		snprintf(out, n, "%.1fK", v / 1024.0);
	else if (v < 1024ull * 1024 * 1024)
		snprintf(out, n, "%.1fM", v / (1024.0 * 1024));
	else
		snprintf(out, n, "%.2fG", v / (1024.0 * 1024 * 1024));
	return out;
}

static struct blob_attr *field(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, tbl, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static const char *fstr(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	return (a && blobmsg_type(a) == BLOBMSG_TYPE_STRING) ? blobmsg_get_string(a) : "";
}

static const char *orb(const char *s, const char *dflt)
{
	return (s && *s) ? s : dflt;
}

static uint64_t fnum(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8:  return blobmsg_get_u8(a);
	default: return 0;
	}
}

/* What to show when a lookup returned no IP address. */
static const char *no_address_text(uint64_t rcode, const char *qtype, char *tmp, size_t n)
{
	switch (rcode) {
	case 0:
		if (!strcmp(qtype, "A") || !strcmp(qtype, "AAAA"))
			return "(no address)";
		snprintf(tmp, n, "(%s record)", qtype);
		return tmp;
	case 2: return "SERVFAIL";
	case 3: return "NXDOMAIN";
	case 5: return "REFUSED";
	default:
		snprintf(tmp, n, "RCODE %llu", (unsigned long long)rcode);
		return tmp;
	}
}

static void print_json(struct blob_attr *msg)
{
	char *s = blobmsg_format_json_indent(msg, true, 0);

	if (s) {
		printf("%s\n", s);
		free(s);
	}
}

static void show_radios(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "radios"), *r, *s, *st = field(msg, "steering");
	size_t rem, rem2;

	if (json_out)
		return print_json(msg);
	if (!arr || !blobmsg_len(arr)) {
		printf("No access point found (hostapd control sockets).\n");
		return;
	}
	blobmsg_for_each_attr(r, arr, rem) {
		printf("%s  \"%s\"  %s GHz channel %llu (%llu MHz, %llu MHz wide)  %s\n", fstr(r, "iface"), fstr(r, "ssid"),
		       orb(fstr(r, "band"), "?"), (unsigned long long)fnum(r, "channel"), (unsigned long long)fnum(r, "freq"),
		       (unsigned long long)fnum(r, "width"), fstr(r, "bssid"));
		if (field(r, "busy_pct"))
			printf("  air             %llu%% used by others%s\n", (unsigned long long)fnum(r, "busy_pct"),
			       fnum(r, "busy_since") ? " (busy for a while)" : "");
		else if (*fstr(r, "air"))
			printf("  air             %s\n", fstr(r, "air"));
		if (*fstr(r, "neighbours"))
			printf("  neighbours      by channel, weighted by strength: %s%s", fstr(r, "neighbours"),
			       fnum(r, "suggest") ? "" : "\n");
		if (fnum(r, "suggest"))
			printf("; quieter: channel %llu\n", (unsigned long long)fnum(r, "suggest"));
		if (*fstr(r, "plan"))
			printf("  channels        %s (agctl channels)\n", fstr(r, "plan"));
		if (field(r, "health")) {
			struct blob_attr *h = field(r, "health");

			printf("  health          %s%s%s\n", fstr(h, "status"),
			       strcmp(fstr(h, "cause"), "None") ? ", " : "", strcmp(fstr(h, "cause"), "None") ? fstr(h, "cause") : "");
			if (*fstr(h, "explanation"))
				printf("                  %s\n", fstr(h, "explanation"));
		}
		blobmsg_for_each_attr(s, field(r, "stations"), rem2) {
			printf("  %-17.17s %-15.15s %4d dBm %-4s %7.1f Mbit/s  %-13s %s",
			       orb(fstr(s, "hostname"), fstr(s, "mac")), orb(fstr(s, "ip"), "-"), (int32_t)fnum(s, "signal"),
			       fstr(s, "near"), fnum(s, "rate_kbps") / 1000.0,
			       fnum(s, "dual_band") ? "2.4 + 5 GHz" : "2.4 GHz only", fnum(s, "bss_transition") ? "802.11v" : "");
			if (fnum(s, "steered") || fnum(s, "steer_missed"))
				printf("  moved %llu, stayed %llu", (unsigned long long)fnum(s, "steered"),
				       (unsigned long long)fnum(s, "steer_missed"));
			printf("\n");
		}
	}
	if (st) {
		if (!fnum(st, "dual_band"))
			printf("band steering   needs a 2.4 GHz and a 5 GHz access point with one name\n");
		else if (!fnum(st, "enabled"))
			printf("band steering   off (wifi.steer 0)\n");
		else
			printf("band steering   on: to 5 GHz at %d dBm or better, to 2.4 GHz at %d dBm or worse; "
			       "%llu moves asked, %llu done\n", (int32_t)fnum(st, "up_dbm"), (int32_t)fnum(st, "down_dbm"),
			       (unsigned long long)fnum(st, "moves_asked"), (unsigned long long)fnum(st, "moves_done"));
	}
}

#if AIGW_FEATURE_CARE || AIGW_FEATURE_PROACTIVE || AIGW_FEATURE_WIFI_HEAL || AIGW_FEATURE_UPGRADE
static const char *dur_short(unsigned long long s, char *out, size_t n)
{
	if (s >= 86400)
		snprintf(out, n, "%llud %lluh", s / 86400, s % 86400 / 3600);
	else if (s >= 3600)
		snprintf(out, n, "%lluh %llum", s / 3600, s % 3600 / 60);
	else if (s >= 60 || !s)
		snprintf(out, n, "%llum", s / 60);
	else
		snprintf(out, n, "%llus", s);
	return out;
}
#endif

/* agctl diagnose [device]: "my internet is slow", answered */
static int diag_failed;

/* "12 ms (0% lost)", or "no answer" when nothing came back (there is no delay to show) */
static void show_ping(struct blob_attr *m, const char *ms, const char *loss)
{
	if (fnum(m, loss) >= 100)
		printf("no answer");
	else
		printf("%llu ms (%llu%% lost)", (unsigned long long)fnum(m, ms), (unsigned long long)fnum(m, loss));
}

static void show_diag(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const char *const mark[] = { "ok", "info", "warning", "problem" };
	static const char *const sym[] = { "  ok     ", "  info   ", "  warning", "  problem" };
	struct blob_attr *m = field(msg, "measured"), *f;
	size_t rem;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (fstr(msg, "error")[0]) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
		diag_failed = 1;
		return;
	}
	printf("%s%s%s\n", fstr(msg, "host")[0] ? fstr(msg, "host") : "", fstr(msg, "host")[0] ? ": " : "",
	       fstr(msg, "summary"));
	printf("  code      %s (%s, %s)\n", fstr(msg, "code"), fstr(msg, "area"), fstr(msg, "severity"));
	if (fstr(msg, "advice")[0])
		printf("  advice    %s\n", fstr(msg, "advice"));
	if (m) {
		printf("  measured  ");
		if (!fnum(m, "route"))
			printf("no default route");
		else {
			printf("next hop %s ", orb(fstr(m, "next_hop"), "-"));
			show_ping(m, "next_hop_ms", "next_hop_loss_pct");
			printf(", internet ");
			show_ping(m, "internet_ms", "internet_loss_pct");
		}
		printf(", DNS %s", fstr(m, "dns"));
		if (!strcmp(fstr(m, "dns"), "ok"))
			printf(" %llu ms", (unsigned long long)fnum(m, "dns_ms"));
		printf("\n");
	}
	printf("  findings\n");
	blobmsg_for_each_attr(f, field(msg, "findings"), rem) {
		int s;

		for (s = 0; s < 4 && strcmp(mark[s], fstr(f, "severity")); s++)
			;
		printf("%s  %-16s %s\n", sym[s < 4 ? s : 0], fstr(f, "code"), fstr(f, "text"));
	}
}

/* agctl care [--check]: proactive care, what it found and the experience score */
#if AIGW_FEATURE_CARE
static int care_asked_check;
#endif

#if AIGW_FEATURE_PROACTIVE
/* agctl care proactive [--run]: before the call: the tickets, what AIGW fixed, what the ISP was told */
static void show_proactive(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *c = field(msg, "counts"), *t;
	unsigned long long now = (unsigned long long)time(NULL);
	char t1[32];
	size_t rem;
	int k = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (fstr(msg, "error")[0]) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
		diag_failed = 1;
		return;
	}
	if (!fnum(msg, "enabled")) {
		printf("Proactive care before the call is off (proactive.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Before the call: a look every %s; worth acting on at %llu points; fixes %s (act mode %s); "
	       "%llu of %llu new tickets to the ISP left today\n", dur_short(fnum(msg, "interval_s"), t1, sizeof(t1)),
	       (unsigned long long)fnum(msg, "threshold"), fnum(msg, "fix") ? "by AIGW" : "never by AIGW", fstr(msg, "mode"),
	       (unsigned long long)fnum(msg, "left_today"), (unsigned long long)fnum(msg, "max_per_day"));
	if (fnum(msg, "area_outage"))
		printf("  an outage across the area: no tickets or customer notes while it lasts (the ISP knows)\n");
	if (c)
		printf("  so far    %llu worth acting on: %llu fixed by AIGW, %llu proposed, %llu told to the ISP, %llu kept at "
		       "home, %llu repeats not told again; calls likely avoided %llu (called anyway %llu)\n",
		       (unsigned long long)fnum(c, "tickets"), (unsigned long long)fnum(c, "fixed_by_aigw"),
		       (unsigned long long)fnum(c, "proposed"), (unsigned long long)fnum(c, "told_isp"),
		       (unsigned long long)fnum(c, "suppressed"), (unsigned long long)fnum(c, "deduplicated"),
		       (unsigned long long)fnum(c, "calls_likely_avoided"), (unsigned long long)fnum(c, "called_anyway"));
	blobmsg_for_each_attr(t, field(msg, "tickets"), rem) {
		unsigned long long since = fnum(t, "since");

		if (!k++)
			printf("  tickets\n");
		printf("    #%-3llu %-10s %-16s %s%s%s, %s for %s, %llu device%s, %llu point%s (%s)%s\n",
		       (unsigned long long)fnum(t, "id"), fstr(t, "status"), fstr(t, "code"), fstr(t, "fault"),
		       *fstr(t, "target") ? " " : "", fstr(t, "target"), fnum(t, "open") ? "open" : "over",
		       dur_short(((fnum(t, "open") ? now : (unsigned long long)fnum(t, "closed")) > since ?
				  (fnum(t, "open") ? now : (unsigned long long)fnum(t, "closed")) - since : 0), t1, sizeof(t1)),
		       (unsigned long long)fnum(t, "devices"), fnum(t, "devices") == 1 ? "" : "s",
		       (unsigned long long)fnum(t, "points"), fnum(t, "points") == 1 ? "" : "s", fstr(t, "reason"),
		       fnum(t, "flaps") ? " (came back)" : "");
		printf("         %s\n", fstr(t, "summary"));
		printf("         next: %s\n", fstr(t, "next_step"));
		printf("         customer: %s\n", fstr(t, "customer_note"));
	}
	if (!k)
		printf("  tickets   none: nothing a customer would notice\n");
}
#endif

#if AIGW_FEATURE_PROVEN
/* "9 fixes: 7 helped (fix rate 78%), 1 no change, 1 made worse; 2 undone; 3 not known" */
static void repair_line(struct blob_attr *s)
{
	if (!s || (!fnum(s, "fixes") && !fnum(s, "shadows"))) {
		printf("no fixes measured yet");
		return;
	}
	printf("%llu fix%s", (unsigned long long)fnum(s, "fixes"), fnum(s, "fixes") == 1 ? "" : "es");
	if (fnum(s, "judged"))
		printf(": %llu helped (fix rate %llu%%), %llu no change, %llu made worse", (unsigned long long)fnum(s, "helped"),
		       (unsigned long long)fnum(s, "fix_rate"), (unsigned long long)fnum(s, "no_change"),
		       (unsigned long long)fnum(s, "worse"));
	if (fnum(s, "undone"))
		printf("; %llu undone", (unsigned long long)fnum(s, "undone"));
	if (fnum(s, "unknown"))
		printf("; %llu not known (the device left, too little measured)", (unsigned long long)fnum(s, "unknown"));
	if (fnum(s, "calls_likely_avoided"))
		printf("; calls likely avoided %llu", (unsigned long long)fnum(s, "calls_likely_avoided"));
	if (fnum(s, "shadows"))
		printf("; %llu shadow%s (not applied), %llu needed", (unsigned long long)fnum(s, "shadows"),
		       fnum(s, "shadows") == 1 ? "" : "s", (unsigned long long)fnum(s, "shadows_needed"));
}

static void repair_receipt(struct blob_attr *r)
{
	char t1[32];
	int shadow = !strcmp(fstr(r, "mode"), "shadow");

	printf("    #%-3llu %-8s %-9s %-16s %s%s, decision %llu, %s\n", (unsigned long long)fnum(r, "id"),
	       shadow ? "shadow" : "applied", fstr(r, "action"), fstr(r, "fault"), fstr(r, "result"),
	       fnum(r, "met") && !shadow ? " (as predicted)" : "", (unsigned long long)fnum(r, "decision"),
	       ago(fnum(r, "created"), t1, sizeof(t1)));
	printf("         predicted  %s\n", fstr(r, "prediction"));
	if (*fstr(r, "summary"))
		printf("         %-10s %s\n", shadow ? "without it" : "measured", fstr(r, "summary"));
	else
		printf("         measured   not yet: judged %s\n", fnum(r, "trial") ? "when its own trial decides" :
		       "at the deadline");
	if (field(r, "confidence") && shadow)
		printf("         confidence %llu%% (the fix rate measured here)\n", (unsigned long long)fnum(r, "confidence"));
	else if (shadow)
		printf("         confidence not measured here yet (fewer than 3 such fixes judged)\n");
}

/* agctl repair [--stats|--shadow]: proven repair's receipts and evidence */
static void show_repair(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const char *const per[] = { "day", "week", "month" };
	static const char *const label[] = { "last day", "last 7 days", "last 30 days" };
	struct blob_attr *g = field(msg, "gate"), *s = field(msg, "summary"), *x;
	size_t rem;
	unsigned k;
	int n = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Proven repair is off (repair.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Proven repair: every fix is predicted, measured for %llu s (or by its own trial) and judged; act mode %s\n",
	       (unsigned long long)fnum(msg, "window_s"), fstr(msg, "mode"));
	printf("  undo      %s\n", !fnum(msg, "undo") ? "off: a fix that didn't help stays (repair.undo 0)" :
	       !strcmp(fstr(msg, "mode"), "enforce") ? "a fix of AIGW's own that didn't help is undone, as it was before" :
	       "only in enforce mode (now: said, not done)");
	printf("  shadows   %s\n", fnum(msg, "shadow") ? "on: in advise mode, what AIGW would have done is measured without it" :
	       "off (repair.shadow 0)");
	if (g && !fnum(g, "enabled"))
		printf("  gate      off: AIGW acts as act.mode says\n");
	else if (g)
		printf("  gate      on: acts on its own only with kinds of fix at %llu%% or better over %llu trials\n",
		       (unsigned long long)fnum(g, "fix_rate"), (unsigned long long)fnum(g, "trials"));
	if (fnum(msg, "pending"))
		printf("  now       %llu being measured\n", (unsigned long long)fnum(msg, "pending"));
	for (k = 0; k < 3; k++) {
		printf("  %-13s", label[k]);
		repair_line(field(s, per[k]));
		printf("\n");
	}
	blobmsg_for_each_attr(x, field(msg, "stats"), rem) {
		struct blob_attr *m = field(x, "month");

		if (!n++)
			printf("  by fault and fix (30 days)\n");
		printf("    %-18s %-9s ", fstr(x, "fault"), fstr(x, "action"));
		repair_line(m);
		printf("\n");
		if (!strcmp(fstr(x, "gate"), "held"))
			printf("%33s gate: held (%s)\n", "", fstr(x, "gate_why"));
	}
	n = 0;
	blobmsg_for_each_attr(x, field(msg, "receipts"), rem) {
		if (!n++)
			printf("  receipts (newest first)\n");
		repair_receipt(x);
	}
	if (!n && field(msg, "receipts"))
		printf("  receipts  none yet\n");
}
#endif

#if AIGW_FEATURE_CARE
static void show_care(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const char *const per[] = { "day", "week", "month" };
	static const char *const label[] = { "last day", "last 7 days", "last 30 days" };
	struct blob_attr *p = field(msg, "problem"), *rep = field(msg, "report");
	unsigned long long now = (unsigned long long)time(NULL);
	char t1[32], t2[32];
	unsigned k;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (fstr(msg, "error")[0]) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
		diag_failed = 1;
		return;
	}
	if (!fnum(msg, "enabled")) {
		printf("Proactive care is off (care.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Proactive care: the gateway checks itself every %s", dur_short(fnum(msg, "interval_s"), t1, sizeof(t1)));
	if (field(msg, "last_check") && now >= fnum(msg, "last_check"))
		printf(" (last %s ago)", dur_short(now - fnum(msg, "last_check"), t2, sizeof(t2)));
	printf("\n");
	if (care_asked_check == 1)
		printf("  a check starts now: see agctl care again in a few seconds\n");
	else if (care_asked_check == 2)
		printf("  a speed test starts now: see agctl care again in about %d seconds\n", 15);
	if (p) {
		printf("  now       %s (%s, %s) for %s, %s\n", fstr(p, "code"), fstr(p, "area"), fstr(p, "severity"),
		       dur_short(now > fnum(p, "since") ? now - fnum(p, "since") : 0, t1, sizeof(t1)),
		       fnum(p, "reported") ? "reported to the ISP" :
		       !strcmp(fstr(p, "owner"), "AIGW") ? "Self-Healing Wi-Fi is on it (not reported yet)" :
		       !strcmp(fstr(p, "owner"), "ISP") ? "to be reported to the ISP" : "not the ISP's");
		printf("            %s\n", fstr(p, "summary"));
		if (fstr(p, "advice")[0])
			printf("  advice    %s\n", fstr(p, "advice"));
	} else
		printf("  now       OK: no problem found\n");
#if AIGW_FEATURE_PROVEN
	if (field(msg, "repair")) {
		struct blob_attr *rp = field(msg, "repair");

		printf("  repairs   last 7 days: ");
		repair_line(field(rp, "week"));
		if (fnum(rp, "pending"))
			printf("; %llu being measured", (unsigned long long)fnum(rp, "pending"));
		printf(" (agctl repair)\n");
	}
#endif
	{
		struct blob_attr *w = field(msg, "early_warning"), *nm = field(msg, "normal"), *sp = field(msg, "speed"), *cur;
		size_t rem;
		int any = 0;

		if (w && !fnum(w, "enabled"))
			printf("  warning   early warnings are off (care.warn)\n");
		else if (w && !strcmp(fstr(w, "status"), "learning"))
			printf("  warning   learning this home's normal: %llu of %llu days, early warnings after that\n",
			       (unsigned long long)fnum(w, "learned_days"), (unsigned long long)fnum(w, "learn_days"));
		else if (w && !strcmp(fstr(w, "status"), "warning")) {
			printf("  warning   %s (%s, %s) for %s\n", fstr(w, "metric"), fstr(w, "area"), fstr(w, "kind"),
			       dur_short(now > fnum(w, "since") ? now - fnum(w, "since") : 0, t1, sizeof(t1)));
			printf("            %s\n", fstr(w, "summary"));
			printf("  advice    %s\n", fstr(w, "advice"));
		} else if (w)
			printf("  warning   none: the line is as it normally is\n");
		blobmsg_for_each_attr(cur, nm, rem) {
			static const struct { const char *name, *words, *unit; } u[] = {
				{ "NextHopLatency", "next hop", "ms" }, { "InternetLatency", "internet", "ms" },
				{ "InternetLoss", "loss", "%" }, { "DNSLatency", "DNS", "ms" }, { "DownloadSpeed", "speed", "Mbit/s" },
			};
			unsigned j;

			for (j = 0; j < sizeof(u) / sizeof(u[0]); j++)
				if (!strcmp(blobmsg_name(cur), u[j].name)) {
					printf("%s%s %s%s%s", any ? ", " : "  normal    at this hour: ", u[j].words,
					       blobmsg_get_string(cur), u[j].unit[0] == '%' ? "" : " ", u[j].unit);
					any = 1;
				}
		}
		if (any)
			printf(" (learned over %llu day%s)\n", (unsigned long long)fnum(w, "learned_days"),
			       fnum(w, "learned_days") == 1 ? "" : "s");
		if (sp && (fnum(sp, "enabled") || strcmp(fstr(sp, "status"), "None"))) {
			printf("  speed     ");
			if (!strcmp(fstr(sp, "status"), "None"))
				printf("no test yet");
			else if (!strcmp(fstr(sp, "status"), "Error"))
				printf("the last test failed (%s)", fstr(sp, "why"));
			else
				printf("%s Mbit/s, %s%s%s%s", fstr(sp, "mbps"), ago(fnum(sp, "last"), t1, sizeof(t1)),
				       fstr(sp, "by")[0] ? ", a test by " : "", fstr(sp, "by"),
				       !strcmp(fstr(sp, "status"), "Low") ? " (slow)" : "");
			if (fnum(sp, "expected_mbps"))
				printf("; the line's speed %llu, at least %llu%% required", (unsigned long long)fnum(sp, "expected_mbps"),
				       (unsigned long long)fnum(sp, "min_pct"));
			if (fnum(sp, "next") > now)
				printf("; next in %s", dur_short(fnum(sp, "next") - now, t2, sizeof(t2)));
			printf("\n");
		}
		if (sp && fnum(sp, "running"))
			printf("  speed     a test you asked for is running\n");
		if (sp && fstr(sp, "asked_why")[0])
			printf("  speed     the test you asked for didn't count: %s\n", fstr(sp, "asked_why"));
		/* v0.44.1: say so when the line's speed isn't known, and how to fix it */
		if (sp && !strcmp(fstr(sp, "line"), "unknown"))
			printf("  line      its speed isn't known: the timeline and smart queueing guess it from the most seen.\n"
			       "            Set what was sold (care.speed_mbps), a speed test server (care.speed_url), or\n"
			       "            test once now: agctl care --speed http://<server>/<big file>\n");
	}
	for (k = 0; k < 3; k++) {
		struct blob_attr *s = field(rep, per[k]);

		if (!s)
			continue;
		printf("  %-13s", label[k]);
		if (!fnum(s, "checks")) {
			printf("no checks yet\n");
			continue;
		}
		printf("experience score %llu%% (", (unsigned long long)fnum(s, "score"));
		/* the share of the time watched without a problem (older days: of the checks) */
		if (fnum(s, "watched_s"))
			printf("%s watched, ", dur_short(fnum(s, "watched_s"), t1, sizeof(t1)));
		printf("%llu check%s)", (unsigned long long)fnum(s, "checks"), fnum(s, "checks") == 1 ? "" : "s");
		if (fnum(s, "problems") || fstr(s, "main_cause")[0]) {
			printf("; %llu problem%s", (unsigned long long)fnum(s, "problems"), fnum(s, "problems") == 1 ? "" : "s");
			if (fstr(s, "main_cause")[0])
				printf(", mostly %s", fstr(s, "main_cause"));
			printf(": reported to the ISP %llu, fixed by AIGW %llu, ", (unsigned long long)fnum(s, "reported"),
			       (unsigned long long)fnum(s, "fixed_by_aigw"));
			/* over without AIGW changing anything: not its fix (v0.29.2) */
			if (fnum(s, "cleared_by_itself"))
				printf("cleared by itself %llu, ", (unsigned long long)fnum(s, "cleared_by_itself"));
			printf("%s with a problem", dur_short(fnum(s, "problem_s"), t1, sizeof(t1)));
		}
		if (fnum(s, "early_warnings")) {
			printf("; %llu early warning%s", (unsigned long long)fnum(s, "early_warnings"),
			       fnum(s, "early_warnings") == 1 ? "" : "s");
			if (fnum(s, "warned_of"))
				printf(" (%llu problem%s seen coming, %s ahead on average)", (unsigned long long)fnum(s, "warned_of"),
				       fnum(s, "warned_of") == 1 ? "" : "s", dur_short(fnum(s, "warning_lead_s"), t1, sizeof(t1)));
		}
		if (fnum(s, "speed_tests"))
			printf("; speed %s Mbit/s on average (%s at least, %llu test%s)", fstr(s, "speed_mbps"),
			       fstr(s, "speed_min_mbps"), (unsigned long long)fnum(s, "speed_tests"),
			       fnum(s, "speed_tests") == 1 ? "" : "s");
		printf("\n");
	}
}
#endif

/* agctl test ...: a TR-143 test's outputs, in words */
#if AIGW_FEATURE_TR143
static int test_kind;                   /* 0 ping, 1 trace, 2 download, 3 upload, 4 dns */
#endif
#define TR_HOPS_SHOWN 64

#if AIGW_FEATURE_TR143
/* a TR-106 dateTime (with microseconds) as seconds; 0 unknown */
static double dt_s(const char *s)
{
	struct tm tm;
	const char *rest;
	double frac = 0;

	memset(&tm, 0, sizeof(tm));
	if (!s || !(rest = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) || tm.tm_year < 100)
		return 0;
	if (*rest == '.')
		frac = strtod(rest, NULL);
	return (double)timegm(&tm) + frac;
}
#endif

#if AIGW_FEATURE_TR143
static void show_test(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *o = field(msg, "out"), *cur;
	const char *st = fstr(o, "Status");
	size_t rem;
	int i;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "ok")) {
		fprintf(stderr, "agctl: %s\n", orb(fstr(msg, "message"), "the test could not run"));
		diag_failed = 1;
		return;
	}
	if (strcmp(st, "Complete")) {
		printf("%s\n", st);
		diag_failed = 1;
		return;
	}
	switch (test_kind) {
	case 0:
		printf("%llu of %llu answered", strtoull(fstr(o, "SuccessCount"), NULL, 10),
		       strtoull(fstr(o, "SuccessCount"), NULL, 10) + strtoull(fstr(o, "FailureCount"), NULL, 10));
		if (strtoull(fstr(o, "SuccessCount"), NULL, 10))
			printf(": %.2f ms on average (%.2f to %.2f)", strtoull(fstr(o, "AverageResponseTimeDetailed"), NULL, 10) / 1000.0,
			       strtoull(fstr(o, "MinimumResponseTimeDetailed"), NULL, 10) / 1000.0,
			       strtoull(fstr(o, "MaximumResponseTimeDetailed"), NULL, 10) / 1000.0);
		printf(", from %s\n", orb(fstr(o, "IPAddressUsed"), "-"));
		break;
	case 1:
		for (i = 1; i <= TR_HOPS_SHOWN; i++) {
			char k[48];

			snprintf(k, sizeof(k), "RouteHops.%d.HostAddress", i);
			if (!field(o, k))
				break;
			printf("%3d  %-16s", i, orb(fstr(o, k), "*"));
			snprintf(k, sizeof(k), "RouteHops.%d.RTTimes", i);
			printf("  %s%s\n", fstr(o, k), fstr(o, k)[0] ? " ms" : "");
		}
		printf("reached in %d hop%s, %s ms\n", i - 1, i == 2 ? "" : "s", fstr(o, "ResponseTime"));
		break;
	case 2:
	case 3: {
		unsigned long long bytes = strtoull(fstr(o, test_kind == 2 ? "TestBytesReceived" : "TestBytesSent"), NULL, 10);
		double s = dt_s(fstr(o, "EOMTime")) - dt_s(fstr(o, "BOMTime")),
		       open = dt_s(fstr(o, "TCPOpenResponseTime")) - dt_s(fstr(o, "TCPOpenRequestTime"));

		printf("%s %llu bytes in %.2f s: %.1f Mbit/s (connection opened in %.1f ms, from %s)\n",
		       test_kind == 2 ? "received" : "sent", bytes, s, s > 0 ? bytes * 8 / s / 1e6 : 0, open * 1000,
		       orb(fstr(o, "IPAddressUsed"), "-"));
		break;
	}
	default:
		printf("%s answered\n", fstr(o, "SuccessCount"));
		blobmsg_for_each_attr(cur, o, rem) {
			const char *k = blobmsg_name(cur), *dot;
			char base[48], key[80];

			if (strncmp(k, "Result.", 7) || !(dot = strrchr(k, '.')) || strcmp(dot, ".Status"))
				continue;
			snprintf(base, sizeof(base), "%.*s", (int)(dot - k), k);
			printf("  %-28s", blobmsg_get_string(cur));
			snprintf(key, sizeof(key), "%s.IPAddresses", base);
			printf(" %-24s", orb(fstr(o, key), "-"));
			snprintf(key, sizeof(key), "%s.ResponseTime", base);
			printf(" %s ms", fstr(o, key));
			snprintf(key, sizeof(key), "%s.DNSServerIP", base);
			printf(" from %s", fstr(o, key));
			snprintf(key, sizeof(key), "%s.AnswerType", base);
			printf(" (%s)\n", fstr(o, key));
		}
	}
}
#endif

#if AIGW_FEATURE_WIFI_HEAL
/* agctl wifi --report: what Self-Healing Wi-Fi found and fixed, per access point */
static void show_report(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const char *const per[] = { "day", "week", "month" };
	static const char *const label[] = { "last day", "last 7 days", "last 30 days" };
	static const char *const causes[] = { "Congestion", "WeakCoverage", "BandHopping", "Device", "Radio" };
	struct blob_attr *arr = field(msg, "radios"), *r;
	size_t rem;
	int any = 0;

	if (json_out)
		return print_json(msg);
	blobmsg_for_each_attr(r, arr, rem) {
		struct blob_attr *rep = field(field(r, "health"), "report");
		unsigned k, c;

		if (!rep)
			continue;
		any = 1;
		printf("%s  %s GHz, channel %llu\n", fstr(r, "iface"), orb(fstr(r, "band"), "?"),
		       (unsigned long long)fnum(r, "channel"));
		for (k = 0; k < 3; k++) {
			struct blob_attr *p = field(rep, per[k]);
			char t1[32], t2[32], t3[32], found[160] = "";
			unsigned long long meas;

			if (!p)
				continue;
			meas = fnum(p, "good_s") + fnum(p, "degraded_s") + fnum(p, "poor_s");
			printf("  %-13s", label[k]);
			if (!meas) {
				printf("nothing measured yet\n");
				continue;
			}
			printf("Good %llu%% of %s (Degraded %s, Poor %s)\n", (unsigned long long)fnum(p, "good_pct"),
			       dur_short(meas, t1, sizeof(t1)), dur_short(fnum(p, "degraded_s"), t2, sizeof(t2)),
			       dur_short(fnum(p, "poor_s"), t3, sizeof(t3)));
			for (c = 0; c < 5; c++)
				if (fnum(p, causes[c]))
					snprintf(found + strlen(found), sizeof(found) - strlen(found), "%s%s %llu",
						 found[0] ? ", " : "", causes[c], (unsigned long long)fnum(p, causes[c]));
			printf("               found %llu problem%s%s%s%s\n", (unsigned long long)fnum(p, "problems"),
			       fnum(p, "problems") == 1 ? "" : "s", found[0] ? " (" : "", found, found[0] ? ")" : "");
			printf("               fixed %llu: channel trials kept %llu, switched back %llu; devices kept on 2.4 GHz %llu",
			       (unsigned long long)fnum(p, "fixes"), (unsigned long long)fnum(p, "trials_kept"),
			       (unsigned long long)fnum(p, "trials_tried"), (unsigned long long)fnum(p, "band_rules"));
			if (field(p, "time_to_fix_s"))
				printf("; %s from problem to fix on average", dur_short(fnum(p, "time_to_fix_s"), t1, sizeof(t1)));
			printf("\n");
			if (field(p, "loss_before_pct"))
				printf("               kept channels: loss %llu%% -> %llu%%, delay %llu -> %llu ms\n",
				       (unsigned long long)fnum(p, "loss_before_pct"), (unsigned long long)fnum(p, "loss_after_pct"),
				       (unsigned long long)fnum(p, "rtt_before_ms"), (unsigned long long)fnum(p, "rtt_after_ms"));
		}
	}
	if (!any)
		printf("No healing report yet (Self-Healing Wi-Fi off, or no access point measured).\n");
}
#endif

static void show_dnscache(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "cache")) {
		printf("DNS cache off (dns.cache 0)\n");
		return;
	}
	printf("DNS cache       %llu of %llu answers kept\n", (unsigned long long)fnum(msg, "entries"),
	       (unsigned long long)fnum(msg, "size"));
	printf("  lookups       %llu from the cache, %llu from upstream (%llu%% from the cache)\n",
	       (unsigned long long)fnum(msg, "hits"), (unsigned long long)fnum(msg, "misses"),
	       (unsigned long long)fnum(msg, "hit_pct"));
	printf("  answer time   %.2f ms from the cache, %.1f ms from upstream\n", fnum(msg, "hit_avg_us") / 1000.0,
	       fnum(msg, "upstream_avg_us") / 1000.0);
	printf("  prefetched    %llu names refreshed before they expired\n", (unsigned long long)fnum(msg, "prefetches"));
	printf("  stale         %llu expired answers served while upstream failed\n",
	       (unsigned long long)fnum(msg, "stale"));
	printf("  not kept      %llu answers marked \"don't cache\" (TTL 0), %llu others (errors, truncated)\n",
	       (unsigned long long)fnum(msg, "uncached_ttl0"), (unsigned long long)fnum(msg, "uncached_other"));
	printf("  blocked       %llu   timeouts %llu   dropped %llu\n", (unsigned long long)fnum(msg, "blocked"),
	       (unsigned long long)fnum(msg, "timeouts"), (unsigned long long)fnum(msg, "dropped"));
}

static const char *dev_name(struct blob_attr *d)
{
	return orb(fstr(d, "hostname"), orb(fstr(d, "mac"), "-"));
}

/* ---------- renderers ---------- */

static void show_status(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s = field(msg, "sensors");
	char t[32];

	if (json_out)
		return print_json(msg);
	printf("AIGW OS %s on %s  (phase: %s)\n", fstr(msg, "version"), fstr(msg, "host"), fstr(msg, "phase"));
	printf("  uptime          %llus\n", (unsigned long long)fnum(msg, "uptime_s"));
	printf("  devices         %llu (%llu home, %llu on Wi-Fi now)\n", (unsigned long long)fnum(msg, "devices"),
	       (unsigned long long)fnum(msg, "home"), (unsigned long long)fnum(msg, "wifi_stations"));
	if (fnum(msg, "radios") > 1)
		printf("  wi-fi           %llu access points, %llu band moves (agctl wifi)\n",
		       (unsigned long long)fnum(msg, "radios"), (unsigned long long)fnum(msg, "steered"));
	printf("  dns lookups     %llu (last %s)\n", (unsigned long long)fnum(msg, "dns_queries"),
	       ago(fnum(msg, "dns_last_event"), t, sizeof(t)));
	printf("  connections     %llu, %llu without a DNS lookup, %llu named from their handshake\n",
	       (unsigned long long)fnum(msg, "flows"), (unsigned long long)fnum(msg, "flows_no_dns"),
	       (unsigned long long)fnum(msg, "flows_sni"));
	{
		struct blob_attr *bu = field(msg, "bursts");

		if (bu && fnum(bu, "enabled"))
			printf("  lookup bursts   %llu, control servers named %llu; the home has used %llu sites\n",
			       (unsigned long long)fnum(bu, "raised"), (unsigned long long)fnum(bu, "servers_named"),
			       (unsigned long long)fnum(bu, "home_sites"));
	}
	if (s)
		printf("  sensors         dns %s, flow %s, names %s, wifi %s\n",
		       fnum(s, "dns") ? "on" : "waiting", fnum(s, "flow") ? "on" : "OFF",
		       fnum(s, "sni") ? "on" : "off", fnum(s, "wifi") ? "on" : "no stations");
	{
		struct blob_attr *m = field(msg, "meter"), *f = field(msg, "fastpath"), *e;
		size_t rem;

		if (m) {
			printf("  traffic meter   ");
			if (fnum(m, "active")) {
				printf("eBPF on");
				blobmsg_for_each_attr(e, field(m, "ifaces"), rem)
					printf(" %s", blobmsg_get_string(e));
				printf(" (live rates per device)\n");
			} else {
				printf("off: %s\n", orb(fstr(m, "why"), "?"));
			}
		}
		if (f && fnum(f, "enabled"))
			printf("  fast path       %llu connections offloaded (%llu by the switch chip), %llu sent back for actions\n",
			       (unsigned long long)fnum(f, "offloaded"), (unsigned long long)fnum(f, "hw_offloaded"),
			       (unsigned long long)fnum(f, "released"));
	}
	printf("  alerts          %llu\n", (unsigned long long)fnum(msg, "alerts"));
	{
		struct blob_attr *d = field(msg, "decisions");
		const char *mode = orb(fstr(msg, "mode"), "advise");

		printf("  acting          %s mode (%s)\n", mode,
		       !strcmp(mode, "enforce") ? "policies act on their own, within guardrails" :
		       !strcmp(mode, "off") ? "alerts only" : "actions wait for your approval");
		if (d)
			printf("  decisions       %llu waiting for you, %llu in effect, %llu policies\n",
			       (unsigned long long)fnum(d, "waiting"), (unsigned long long)fnum(d, "active"),
			       (unsigned long long)fnum(d, "policies"));
	}
	{
		struct blob_attr *p = field(msg, "plugins");

		if (p)
			printf("  plug-ins        %llu found, %llu working\n", (unsigned long long)fnum(p, "found"),
			       (unsigned long long)fnum(p, "working"));
		printf("  wi-fi keys      %llu per-device keys\n", (unsigned long long)fnum(msg, "wifi_keys"));
		if (field(msg, "remote")) {
			struct blob_attr *r = field(msg, "remote");

			if (fnum(r, "enabled"))
				printf("  remote access   on, %llu peers, %llu connected\n", (unsigned long long)fnum(r, "peers"),
				       (unsigned long long)fnum(r, "online"));
			else
				printf("  remote access   off\n");
		}
	}
	{
		struct blob_attr *m = field(msg, "models"), *v = field(msg, "model_versions"), *e;
		size_t rem;
		int k = 0;

		printf("  models         ");
		if (m)
			blobmsg_for_each_attr(e, m, rem) {
				uint64_t ver = fnum(v, blobmsg_name(e));

				if (ver)
					printf(" %s v%llu (%u trees)", blobmsg_name(e), (unsigned long long)ver, blobmsg_get_u32(e));
				else
					printf(" %s (packaged, %u trees)", blobmsg_name(e), blobmsg_get_u32(e));
				k++;
			}
		printf("%s\n", k ? "" : " none loaded");
	}
}

static void top_line(struct blob_attr *arr, const char *count, int bytes, int max)
{
	struct blob_attr *e;
	size_t rem;
	int k = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		char b[16];

		if (k++ == max)
			break;
		if (bytes)
			printf("%s%s(%s)", k > 1 ? ", " : "", fstr(e, "name"),
			       human_bytes(fnum(e, count), b, sizeof(b)));
		else
			printf("%s%s(%llu)", k > 1 ? ", " : "", fstr(e, "name"),
			       (unsigned long long)fnum(e, count));
	}
}

static void show_devices(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "devices"), *d;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-15s %-18s %-11s %-6s %6s %6s %5s %8s %8s  %s\n", "NAME", "IP", "TYPE", "SIGNAL",
	       "DNS", "CONNS", "NODNS", "DOWN", "UP", "TOP DESTINATIONS");
	if (!arr)
		return;
	blobmsg_for_each_attr(d, arr, rem) {
		struct blob_attr *w = field(d, "wifi");
		char sig[12] = "-", dn[16], up[16];

		if (w && fnum(w, "connected"))
			snprintf(sig, sizeof(sig), "%d", (int32_t)fnum(w, "signal"));
		printf("%-15.15s %-18.18s %-11.11s %-6s %6llu %6llu %5llu %8s %8s  ", dev_name(d),
		       orb(fstr(d, "ip"), "-"), orb(fstr(d, "type"), "-"), sig,
		       (unsigned long long)fnum(d, "queries"), (unsigned long long)fnum(d, "flows"),
		       (unsigned long long)fnum(d, "flows_no_dns"),
		       human_bytes(fnum(d, "bytes_down"), dn, sizeof(dn)),
		       human_bytes(fnum(d, "bytes_up"), up, sizeof(up)));
		if (field(d, "top_destinations") && fnum(d, "bytes_down") + fnum(d, "bytes_up"))
			top_line(field(d, "top_destinations"), "bytes", 1, 2);
		else
			top_line(field(d, "top_domains"), "lookups", 0, 2);
		printf("\n");
	}
}

static int dev_matches(struct blob_attr *d, const char *q)
{
	return !strcasecmp(fstr(d, "ip"), q) || !strcasecmp(fstr(d, "mac"), q) ||
	       strcasestr(fstr(d, "hostname"), q) != NULL;
}

static void show_device(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "devices"), *d, *w, *e;
	size_t rem, r2;
	char t1[32], t2[32], b1[16], b2[16];
	int found = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(d, arr, rem) {
		if (!dev_matches(d, device_filter))
			continue;
		found++;
		if (json_out) {
			print_json(d);
			continue;
		}
		w = field(d, "wifi");
		printf("%s\n", dev_name(d));
		printf("  ip / mac        %s / %s\n", orb(fstr(d, "ip"), "-"), orb(fstr(d, "mac"), "-"));
		printf("  type            %s\n", orb(fstr(d, "type"), "unknown"));
		if (*fstr(d, "zone") || *fstr(d, "wifi_key"))
			printf("  zone / key      %s / %s\n", orb(fstr(d, "zone"), "-"), orb(fstr(d, "wifi_key"), "shared password"));
		if (field(d, "plugins"))
			printf("  plug-ins        %llu reports, last %s %s\n",
			       (unsigned long long)fnum(field(d, "plugins"), "events"), fstr(field(d, "plugins"), "last"),
			       ago(fnum(field(d, "plugins"), "last_ts"), t1, sizeof(t1)));
		if (*fstr(d, "vendor_class") || *fstr(d, "dhcp_options"))
			printf("  dhcp            %s  options %s\n", orb(fstr(d, "vendor_class"), "-"),
			       orb(fstr(d, "dhcp_options"), "-"));
		printf("  seen            first %s, last %s\n", ago(fnum(d, "first_seen"), t1, sizeof(t1)),
		       ago(fnum(d, "last_seen"), t2, sizeof(t2)));
		if (w && fnum(w, "connected"))
			printf("  wi-fi           %d dBm (%s), %.1f Mbit/s, %s%s%son %s since %s\n", (int32_t)fnum(w, "signal"),
			       orb(fstr(w, "near"), "-"), fnum(w, "rate_kbps") / 1000.0, *fstr(w, "band") ? fstr(w, "band") : "",
			       *fstr(w, "band") ? " GHz " : "", "", fstr(w, "iface"), ago(fnum(w, "since"), t1, sizeof(t1)));
		else
			printf("  wi-fi           not connected\n");
		if (w && (field(w, "dual_band") || fnum(w, "steered") || fnum(w, "steer_missed")))
			printf("  bands           %s, %s; moved %llu times, stayed %llu\n",
			       fnum(w, "dual_band") ? "2.4 and 5 GHz" : "2.4 GHz only (as far as seen)",
			       fnum(w, "bss_transition") ? "takes 802.11v move requests" : "no 802.11v",
			       (unsigned long long)fnum(w, "steered"), (unsigned long long)fnum(w, "steer_missed"));
		if (field(d, "presence"))
			printf("  home            %s since %s\n", fnum(field(d, "presence"), "home") ? "yes" : "no",
			       ago(fnum(field(d, "presence"), "since"), t2, sizeof(t2)));
		printf("  traffic         %s down, %s up, %llu connections (%llu without DNS)\n",
		       human_bytes(fnum(d, "bytes_down"), b1, sizeof(b1)),
		       human_bytes(fnum(d, "bytes_up"), b2, sizeof(b2)),
		       (unsigned long long)fnum(d, "flows"), (unsigned long long)fnum(d, "flows_no_dns"));
		if (field(d, "meter")) {
			struct blob_attr *m = field(d, "meter");

			printf("  right now       %.2f Mbit/s down, %.2f Mbit/s up (kernel meter: %s down, %s up so far)\n",
			       fnum(m, "rate_down") / 1e6, fnum(m, "rate_up") / 1e6,
			       human_bytes(fnum(m, "bytes_down"), b1, sizeof(b1)), human_bytes(fnum(m, "bytes_up"), b2, sizeof(b2)));
		}
		{
			struct blob_attr *bl = field(d, "baseline"), *h;
			size_t r3;
			int k = 0;

			if (bl) {
				printf("  baseline        %s: %llu known sites, %.1f new/day%s, usual upload %s per hour\n",
				       fstr(bl, "state"), (unsigned long long)fnum(bl, "known_sites"),
				       field(bl, "new_sites_per_day") ? blobmsg_get_double(field(bl, "new_sites_per_day")) : 0,
				       fnum(bl, "stable") ? " (stable pattern)" : "",
				       human_bytes(field(bl, "upload_per_bucket") ?
						   (uint64_t)blobmsg_get_double(field(bl, "upload_per_bucket")) : 0, b1, sizeof(b1)));
				printf("  active hours   ");
				if (field(bl, "active_hours"))
					blobmsg_for_each_attr(h, field(bl, "active_hours"), r3) {
						printf("%s", blobmsg_get_u32(h) ? "#" : ".");
						k++;
					}
				printf("  (00-23h, # = seen active)\n");
			}
		}
		printf("  top by traffic\n");
		if (field(d, "top_destinations"))
			blobmsg_for_each_attr(e, field(d, "top_destinations"), r2)
				printf("    %-44s %s\n", fstr(e, "name"), human_bytes(fnum(e, "bytes"), b1, sizeof(b1)));
		printf("  top by lookups (%llu total)\n", (unsigned long long)fnum(d, "queries"));
		if (field(d, "top_domains"))
			blobmsg_for_each_attr(e, field(d, "top_domains"), r2)
				printf("    %-44s %llu\n", fstr(e, "name"), (unsigned long long)fnum(e, "lookups"));
	}
	if (!found)
		fprintf(stderr, "agctl: no device matches '%s'\n", device_filter);
}

static void show_dns(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "log"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-8s %-16s %-16s %-5s %-44s %s\n", "TIME", "CLIENT", "NAME", "TYPE", "QUERY", "ANSWER");
	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		time_t ts = (time_t)fnum(e, "ts");
		struct tm tm;
		char hhmmss[16], tmp[32];

		localtime_r(&ts, &tm);
		strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);
		printf("%-8s %-16s %-16.16s %-5s %-44.44s %s\n", hhmmss, fstr(e, "client"),
		       orb(fstr(e, "hostname"), "-"), fstr(e, "qtype"), fstr(e, "qname"),
		       *fstr(e, "answer") ? fstr(e, "answer") :
		       no_address_text(fnum(e, "rcode"), fstr(e, "qtype"), tmp, sizeof(tmp)));
	}
}

static void show_flows(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "flows"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%-8s %-16s %-44s %-9s %8s %8s %6s  %s\n", "ENDED", "CLIENT", "DESTINATION", "PROTO",
	       "DOWN", "UP", "SECS", "NOTE");
	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		time_t ts = (time_t)fnum(e, "end");
		struct tm tm;
		char hhmmss[16], proto[16], dn[16], up[16];

		localtime_r(&ts, &tm);
		strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);
		snprintf(proto, sizeof(proto), "%s/%llu", fstr(e, "proto"), (unsigned long long)fnum(e, "dport"));
		printf("%-8s %-16.16s %-44.44s %-9s %8s %8s %6llu  %s\n", hhmmss,
		       orb(fstr(e, "hostname"), fstr(e, "client")), orb(fstr(e, "domain"), fstr(e, "dst")),
		       proto, human_bytes(fnum(e, "bytes_down"), dn, sizeof(dn)),
		       human_bytes(fnum(e, "bytes_up"), up, sizeof(up)),
		       (unsigned long long)fnum(e, "duration_s"), fnum(e, "no_dns") ? "no DNS lookup" :
		       !strcmp(fstr(e, "named_by"), "sni") ? "name from handshake" : "");
	}
}

static void show_alerts(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "alerts"), *e;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			char t1[32];

			n++;
			printf("#%-4llu %-8s %-10s %-16.16s %s\n", (unsigned long long)fnum(e, "id"),
			       fstr(e, "severity"), fstr(e, "kind"), fstr(e, "device"),
			       ago(fnum(e, "last"), t1, sizeof(t1)));
			if (fnum(e, "count") > 1)
				printf("      seen %llu times\n", (unsigned long long)fnum(e, "count"));
			printf("      %s\n\n", fstr(e, "reason"));
		}
	if (!n)
		printf("No alerts. AIGW is watching in advise mode and hasn't seen anything worth flagging.\n");
}

static void show_score(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s = field(msg, "score"), *f = field(msg, "features"), *e;
	size_t rem;

	if (json_out)
		return print_json(msg);
	printf("%s\n", fstr(msg, "name"));
	if (!s) {
		printf("  %s\n", fstr(msg, "verdict"));
		return;
	}
	printf("  label     %s\n", fstr(msg, "label"));
	printf("  score     %.3f  (alert at %.2f)\n", blobmsg_get_double(s),
	       field(msg, "threshold") ? blobmsg_get_double(field(msg, "threshold")) : 0);
	if (field(msg, "trees"))
		printf("  trees     %.3f  (before the layer homes learned together)\n",
		       blobmsg_get_double(field(msg, "trees")));
	printf("  verdict   %s\n", fstr(msg, "verdict"));
	if (f) {
		printf("  features ");
		blobmsg_for_each_attr(e, f, rem)
			printf(" %s=%.3g", blobmsg_name(e), blobmsg_get_double(e));
		printf("\n");
	}
}

static const char *duration(int64_t s, char *out, size_t n)
{
	if (s < 0)
		s = 0;
	if (s < 120)
		snprintf(out, n, "%llds", (long long)s);
	else if (s < 7200)
		snprintf(out, n, "%lldm", (long long)(s / 60));
	else if (s < 172800)
		snprintf(out, n, "%lldh%02lldm", (long long)(s / 3600), (long long)(s % 3600 / 60));
	else
		snprintf(out, n, "%lldd", (long long)(s / 86400));
	return out;
}

static const char *action_words(const char *action)
{
	return !strcmp(action, "priority") ? "prioritize" : action;
}

/* "block youtube.com (kids-tablet)", "throttle tv  10.0.0.5  2.0 Mbit/s", ... */
static void decision_subject(struct blob_attr *e, char *out, size_t n)
{
	const char *action = fstr(e, "action");

	if (!strcmp(action, "block"))
		snprintf(out, n, "%s  (%s)", fstr(e, "target"), fstr(e, "device"));
	else if (!strcmp(action, "throttle"))
		snprintf(out, n, "%s  %s  to %.1f Mbit/s", fstr(e, "device"), fstr(e, "target"),
			 fnum(e, "rate") / 1000.0);
	else
		snprintf(out, n, "%s  %s", fstr(e, "device"), fstr(e, "target"));
}

static void show_decisions(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "decisions"), *e;
	size_t rem;
	int n = 0, waiting = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			const char *st = fstr(e, "state"), *action = fstr(e, "action");
			char t1[32], t2[32], subj[256];

			n++;
			decision_subject(e, subj, sizeof(subj));
			printf("#%-4llu %-9s %-12s %s\n", (unsigned long long)fnum(e, "id"), st,
			       action_words(action), subj);
			if (!strcmp(st, "proposed")) {
				waiting++;
				printf("      proposed by %s %s, for %s\n", fstr(e, "origin"),
				       ago(fnum(e, "created"), t1, sizeof(t1)), duration((int64_t)fnum(e, "ttl"), t2, sizeof(t2)));
			} else if (!strcmp(st, "active")) {
				printf("      by %s%s, %s left\n", fstr(e, "origin"), fnum(e, "automatic") ? " (automatic)" : "",
				       duration((int64_t)fnum(e, "expires") - ag_wall_s(), t1, sizeof(t1)));
			} else {
				printf("      by %s, %s %s\n", fstr(e, "origin"), st, ago(fnum(e, "decided"), t1, sizeof(t1)));
			}
			printf("      why: %s\n", fstr(e, "reason"));
			if (*fstr(e, "note"))
				printf("      note: %s\n", fstr(e, "note"));
			printf("\n");
		}
	if (!n)
		printf("No decisions yet. AIGW is in %s mode.\n", orb(fstr(msg, "mode"), "advise"));
	else if (waiting)
		printf("%d waiting for you: agctl approve <id>  or  agctl reject <id>\n", waiting);
}

static int change_failed;

static void show_change(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (fnum(msg, "ok"))
		printf("done (decision #%llu). See: agctl decisions\n", (unsigned long long)fnum(msg, "id"));
	else
		fprintf(stderr, "agctl: refused: %s\n", fstr(msg, "error"));
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb);

/* ---------- QR codes ---------- */

static int qr_pbm;

/* Two modules per character cell, dark on light whatever the terminal's colours. */
static void print_qr(const char *text)
{
	struct ag_qr q;
	int x, y, quiet = 2, tty = isatty(1);

	if (ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_M) < 0 &&
	    ag_qr_encode(&q, (const uint8_t *)text, strlen(text), AG_QR_ECC_L) < 0) {
		fprintf(stderr, "agctl: too long for a QR code\n");
		return;
	}
	if (qr_pbm) {
		int scale = 8, side;

		quiet = 4;
		side = (q.size + 2 * quiet) * scale;
		printf("P1\n%d %d\n", side, side);
		for (y = 0; y < side; y++) {
			for (x = 0; x < side; x++) {
				int mx = x / scale - quiet, my = y / scale - quiet;

				putchar(mx >= 0 && my >= 0 && mx < q.size && my < q.size && q.m[my][mx] ? '1' : '0');
			}
			putchar('\n');
		}
		return;
	}
	for (y = -quiet; y < q.size + quiet; y += 2) {
		printf("  %s", tty ? "\033[30;47m" : "");
		for (x = -quiet; x < q.size + quiet; x++) {
			int top = x >= 0 && y >= 0 && x < q.size && y < q.size && q.m[y][x];
			int bot = x >= 0 && y + 1 >= 0 && x < q.size && y + 1 < q.size && q.m[y + 1][x];

			fputs(top && bot ? "\u2588" : top ? "\u2580" : bot ? "\u2584" : " ", stdout);
		}
		printf("%s\n", tty ? "\033[0m" : "");
	}
}

/* ---------- plug-ins ---------- */

#if AIGW_FEATURE_PLUGINS
static void list_line(const char *label, struct blob_attr *arr)
{
	struct blob_attr *e;
	size_t rem;
	int k = 0;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		if (!k++)
			printf("  %-15s ", label);
		else
			printf(", ");
		printf("%s", blobmsg_get_string(e));
	}
	if (k)
		printf("\n");
}

static void show_plugins(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "plugins"), *p;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(p, arr, rem) {
			if (!n++ && !device_filter)
				printf("%-16s %-10s %-11s %-8s %-8s %s\n", "NAME", "KIND", "STATE", "FROM", "VERSION", "NOTE");
			if (!device_filter) {
				printf("%-16.16s %-10s %-11s %-8s %-8.8s %s\n", fstr(p, "name"), fstr(p, "kind"), fstr(p, "state"),
				       fstr(p, "signed_by") && *fstr(p, "signed_by") ? "signed" : fstr(p, "source"),
				       fstr(p, "version"), orb(fstr(p, "why"), fstr(p, "description")));
				continue;
			}
			{
				struct blob_attr *pr = field(p, "process"), *bud = field(p, "budget"), *card = field(p, "card"), *e;
				size_t r2;
				char t1[32];

				printf("%s %s (%s)\n", fstr(p, "name"), fstr(p, "version"), fstr(p, "kind"));
				if (*fstr(p, "description"))
					printf("  %s\n", fstr(p, "description"));
				printf("  state           %s%s%s\n", fstr(p, "state"), *fstr(p, "why") ? ": " : "", fstr(p, "why"));
				printf("  from            %s (%s), %s\n", fstr(p, "source"), fstr(p, "dir"),
				       fnum(p, "trusted") ? "trusted" : "privileged capabilities need your OK");
				if (fstr(p, "signed_by") && *fstr(p, "signed_by"))
					printf("  signed by       key %s (trusted)\n", fstr(p, "signed_by"));
				else if (fstr(p, "signature") && *fstr(p, "signature"))
					printf("  signed          %s\n", fstr(p, "signature"));
				list_line("emits", field(p, "emits"));
				list_line("listens to", field(p, "subscribes"));
				list_line("actions", field(p, "actions"));
				list_line("alerts", field(p, "alerts"));
				list_line("needs", field(p, "needs"));
				list_line("allowed", field(p, "granted"));
				list_line("waiting for OK", field(p, "waiting_for_ok"));
				if (field(p, "policies"))
					blobmsg_for_each_attr(e, field(p, "policies"), r2)
						printf("  policy          on %s alert%s%s: %s (%s)\n", fstr(e, "alert"),
						       *fstr(e, "zone") ? " in zone " : "", fstr(e, "zone"), fstr(e, "action"),
						       fstr(e, "mode"));
				if (card)
					printf("  card            %s: %s\n", fstr(card, "title"), fstr(card, "text"));
				if (bud)
					printf("  budget          %llu KB memory, %llu%% CPU\n", (unsigned long long)fnum(bud, "ram_kb"),
					       (unsigned long long)fnum(bud, "cpu_pct"));
				if (pr)
					printf("  process         pid %llu since %s, %llu KB, %.1f%% CPU\n",
					       (unsigned long long)fnum(pr, "pid"), ago(fnum(pr, "started"), t1, sizeof(t1)),
					       (unsigned long long)fnum(pr, "rss_kb"),
					       field(pr, "cpu_pct") ? blobmsg_get_double(field(pr, "cpu_pct")) : 0.0);
				printf("  messages        %llu in, %llu out, %llu dropped, %llu restarts\n",
				       (unsigned long long)fnum(p, "messages_in"), (unsigned long long)fnum(p, "messages_out"),
				       (unsigned long long)fnum(p, "dropped"), (unsigned long long)fnum(p, "restarts"));
			}
		}
	if (!n)
		printf(device_filter ? "No plug-in called '%s'.\n" :
		       "No plug-ins yet. Packages add them to %s; agctl plugin add <folder> adds your own.\n",
		       device_filter ? device_filter : fstr(msg, "dir"));
}
#endif

static void show_ok(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (!fnum(msg, "ok"))
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	else
		printf("done\n");
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

/* ---------- the data model ---------- */

static void dm_errors(struct blob_attr *errs)
{
	struct blob_attr *e;
	size_t rem;

	if (!errs)
		return;
	blobmsg_for_each_attr(e, errs, rem) {
		fprintf(stderr, "agctl: %s: %s (%s)\n", blobmsg_name(e), fstr(e, "message"), fstr(e, "code"));
		change_failed = 1;
	}
}

static void show_dm_get(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *vals = field(msg, "values"), *e;
	size_t rem;

	if (json_out)
		print_json(msg);
	else if (vals)
		blobmsg_for_each_attr(e, vals, rem)
			printf("%s = %s\n", blobmsg_name(e),
			       blobmsg_type(e) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(e) : "");
	dm_errors(field(msg, "errors"));
}

static void show_dm_result(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *out = field(msg, "out"), *e;
	size_t rem;

	if (json_out) {
		print_json(msg);
	} else if (fnum(msg, "ok")) {
		printf("done\n");
		if (out)
			blobmsg_for_each_attr(e, out, rem)
				printf("  %s = %s\n", blobmsg_name(e),
				       blobmsg_type(e) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(e) : "");
	} else if (fstr(msg, "message")[0]) {
		fprintf(stderr, "agctl: refused: %s (%s)\n", fstr(msg, "message"), fstr(msg, "code"));
	}
	if (!fnum(msg, "ok"))
		change_failed = 1;
	dm_errors(field(msg, "errors"));
}

static void show_dm_schema(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *e, *a;
	size_t rem, r2;

	if (json_out)
		return print_json(msg);
	printf("Data model %s\n", fstr(msg, "version"));
	if (field(msg, "params"))
		blobmsg_for_each_attr(e, field(msg, "params"), rem)
			printf("  %-52s %-13s %s%s\n", fstr(e, "path"), fstr(e, "type"),
			       fnum(e, "writable") ? "writable" : "read-only", fnum(e, "notify") ? ", notify" : "");
	if (field(msg, "commands"))
		blobmsg_for_each_attr(e, field(msg, "commands"), rem) {
			printf("  %-52s command", fstr(e, "path"));
			if (field(e, "in"))
				blobmsg_for_each_attr(a, field(e, "in"), r2)
					printf(" %s", blobmsg_get_string(a));
			printf("\n");
		}
	if (field(msg, "events"))
		blobmsg_for_each_attr(e, field(msg, "events"), rem)
			printf("  %-52s event\n", fstr(e, "path"));
}

static void show_models(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "models"), *keys = field(msg, "trusted_keys"), *e;
	size_t rem;
	char t[32];
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("Models (updates in %s; signature %s)\n", fstr(msg, "dir"),
	       fnum(msg, "require_signature") ? "required" : "NOT required");
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			const char *src = orb(fstr(e, "source"), "-");

			if (!fnum(e, "loaded")) {
				printf("  %-12s %s\n", fstr(e, "name"), strcmp(orb(fstr(e, "kind"), ""), "model") ?
				       "none yet (comes from cortex through the bridge)" : "not loaded");
			} else if (!strcmp(orb(fstr(e, "kind"), ""), "layer")) {
				printf("  %-12s version %llu, shared layer learned by homes together, from %s, signed by %s, "
				       "published %s\n", fstr(e, "name"), (unsigned long long)fnum(e, "version"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"));
			} else if (!strcmp(orb(fstr(e, "kind"), ""), "list")) {
				printf("  %-12s version %llu, %llu names, from %s, signed by %s, published %s\n", fstr(e, "name"),
				       (unsigned long long)fnum(e, "version"), (unsigned long long)fnum(e, "entries"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"));
			} else if (!strcmp(src, "package"))
				printf("  %-12s packaged model, %llu trees\n", fstr(e, "name"), (unsigned long long)fnum(e, "trees"));
			else
				printf("  %-12s version %llu, %llu trees, from %s, signed by %s, trained %s, since %s\n",
				       fstr(e, "name"), (unsigned long long)fnum(e, "version"), (unsigned long long)fnum(e, "trees"),
				       orb(fstr(e, "origin"), "?"), orb(fstr(e, "signer"), "nobody (unsigned)"),
				       orb(fstr(e, "trained"), "?"), ago(fnum(e, "installed"), t, sizeof(t)));
			if (fnum(e, "held_back"))
				printf("  %-12s held back: version %llu and older (you rolled back)\n", "",
				       (unsigned long long)fnum(e, "held_back"));
			if (fstr(e, "refused") && *fstr(e, "refused"))
				printf("  %-12s last refused %s: %s\n", "", ago(fnum(e, "refused_at"), t, sizeof(t)), fstr(e, "refused"));
		}
	if (field(msg, "backends")) {
		printf("Computed by (models.backend %s; libraries in %s)\n", fstr(msg, "backend_choice"),
		       fstr(msg, "backend_dir"));
		blobmsg_for_each_attr(e, field(msg, "backends"), rem) {
			struct blob_attr *ns = field(e, "ns_per_score");
			double v = ns && blobmsg_type(ns) == BLOBMSG_TYPE_DOUBLE ? blobmsg_get_double(ns) : 0;

			if (fnum(e, "ok"))
				printf("  %-12s %s%.2f µs per score, same verdicts as the reference, %s\n",
				       fstr(e, "name"), fnum(e, "in_use") ? "IN USE, " : "", v / 1000, fstr(e, "source"));
			else
				printf("  %-12s not usable: %s (%s)\n", fstr(e, "name"), orb(fstr(e, "why"), "no model"),
				       fstr(e, "source"));
		}
		if (*fstr(msg, "backend_notes"))
			printf("  note: %s\n", fstr(msg, "backend_notes"));
	}
	printf("Trusted keys (%s)\n", fstr(msg, "keys_dir"));
	if (keys)
		blobmsg_for_each_attr(e, keys, rem) {
			printf("  %s  %s\n", fstr(e, "fingerprint"), orb(fstr(e, "comment"), ""));
			k++;
		}
	if (!k)
		printf("  none: model updates are refused until you trust a key (agctl trust <key.pub>)\n");
}

#if AIGW_FEATURE_FED || AIGW_FEATURE_UPGRADE
static double fdbl(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *a = field(tbl, name);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_DOUBLE ? blobmsg_get_double(a) : (double)fnum(tbl, name);
}
#endif

#if AIGW_FEATURE_FED
static void show_fed(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *list = field(msg, "list"), *ad = field(msg, "adapter"), *e = field(msg, "epsilon"), *x;
	size_t rem;
	char t[32];
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("Federated learning %s (names never leave this home; each round sends 14 noisy numbers)\n",
	       fnum(msg, "enabled") ? "on" : "OFF (alerts.fed)");
	printf("  examples   %llu: %llu safe (false alarms), %llu threats; a round needs %llu\n",
	       (unsigned long long)fnum(msg, "examples"), (unsigned long long)fnum(msg, "safe"),
	       (unsigned long long)fnum(msg, "threat"), (unsigned long long)fnum(msg, "min_examples"));
	printf("  rounds     %llu joined%s%s\n", (unsigned long long)fnum(msg, "rounds"),
	       *fstr(msg, "last") ? "; last: " : "", fstr(msg, "last"));
	if (e && blobmsg_type(e) == BLOBMSG_TYPE_STRING)
		printf("  privacy    %s\n", blobmsg_get_string(e));
	else
		printf("  privacy    ε = %.2f at δ = %g over all rounds (noise at least %.2f)\n", fdbl(msg, "epsilon"),
		       fdbl(msg, "delta"), fdbl(msg, "min_noise"));
	if (ad && fnum(ad, "loaded"))
		printf("  layer      version %llu in use\n", (unsigned long long)fnum(ad, "version"));
	else
		printf("  layer      none yet: the trees' own score is used\n");
	if (list)
		blobmsg_for_each_attr(x, list, rem) {
			if (!k++)
				printf("Your answers (kept here only):\n");
			printf("  %-7s %-40s %s, %s\n", fstr(x, "label"), fstr(x, "name"), fstr(x, "source"),
			       ago(fnum(x, "at"), t, sizeof(t)));
		}
}
#endif

#if AIGW_FEATURE_FLEETLEARN
static void show_fleet(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *sh = field(msg, "shared"), *rx = field(msg, "received"), *arr, *x;
	size_t rem;
	char t[32];
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("Fleet learning %s (every home teaches the next: how fixes went in homes like this one)\n",
	       fnum(msg, "enabled") ? "on" : "OFF (fleet.enabled)");
	if (!fnum(msg, "share"))
		printf("  sharing    off: nothing leaves this home (fleet.share 0, the default: sharing is opt-in)\n");
	else if (fnum(sh, "at"))
		printf("  sharing    on, every %llu s; last summary %s: %llu buckets of outcomes, %llu normals (each count "
		       "at most %llu, noise %.2f)\n", (unsigned long long)fnum(msg, "interval"), ago(fnum(sh, "at"), t, sizeof(t)),
		       (unsigned long long)fnum(sh, "buckets"), (unsigned long long)fnum(sh, "normals"),
		       (unsigned long long)fnum(msg, "cap"), fdbl(msg, "noise"));
	else
		printf("  sharing    on, every %llu s; nothing sent yet (the bridge sends it at its next check-in)\n",
		       (unsigned long long)fnum(msg, "interval"));
	printf("  here       %llu outcomes in %llu buckets (kept here)\n", (unsigned long long)fnum(msg, "outcomes"),
	       (unsigned long long)fnum(msg, "cells"));
	if (fnum(rx, "loaded"))
		printf("  received   priors version %llu from %llu homes (k %llu; used here at %llu or more): %llu buckets, "
		       "%llu normals, %s\n", (unsigned long long)fnum(rx, "version"), (unsigned long long)fnum(rx, "homes"),
		       (unsigned long long)fnum(rx, "k"), (unsigned long long)fnum(msg, "k"),
		       (unsigned long long)fnum(rx, "buckets"), (unsigned long long)fnum(rx, "normals"),
		       ago(fnum(rx, "at"), t, sizeof(t)));
	else
		printf("  received   nothing yet: fixes are chosen by the rules alone, warnings wait for this home's normal\n");
	if (fnum(rx, "cold_start"))
		printf("  warnings   judged against the fleet's normal (\"from the fleet\") while this gateway learns its own\n");
	printf("  choices    %llu informed by the fleet, %llu changed (only among the fixes the rules allow)\n",
	       (unsigned long long)fnum(msg, "choices"), (unsigned long long)fnum(msg, "changed"));
	if ((arr = field(msg, "last_choices")))
		blobmsg_for_each_attr(x, arr, rem)
			printf("    %s  %s: %s%s%s. %s\n", ago(fnum(x, "at"), t, sizeof(t)), fstr(x, "code"), fstr(x, "default"),
			       fnum(x, "changed") ? " -> " : " kept", fnum(x, "changed") ? fstr(x, "chosen") : "", fstr(x, "why"));
	if ((arr = field(msg, "local")))
		blobmsg_for_each_attr(x, arr, rem) {
			if (!k++)
				printf("Outcomes here (code, action, bucket: band/width/congestion/device/hours/line):\n");
			printf("  %-16s %-9s %-36s tried %llu, helped %llu, worse %llu, undone %llu\n", fstr(x, "code"),
			       fstr(x, "action"), fstr(x, "bucket"), (unsigned long long)fnum(x, "tried"),
			       (unsigned long long)fnum(x, "helped"), (unsigned long long)fnum(x, "worse"),
			       (unsigned long long)fnum(x, "undone"));
		}
}

static void show_fleet_rank(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "actions"), *x;
	size_t rem;
	int k = 0;

	if (json_out)
		return print_json(msg);
	printf("%s in %s (band/width/congestion/device/hours/line), best first by the blend of fleet and home:\n",
	       fstr(msg, "code"), fstr(msg, "context"));
	if (arr)
		blobmsg_for_each_attr(x, arr, rem) {
			printf("  %-9s score %+.2f: %s; the fleet's share %.0f%%\n", fstr(x, "action"), fdbl(x, "score"),
			       fstr(x, "words"), 100 * fdbl(x, "fleet_share"));
			k++;
		}
	if (!k)
		printf("  nothing known yet, here or from the fleet\n");
}
#endif

#if AIGW_FEATURE_REMOTE
static int remote_conf;
#endif

#if AIGW_FEATURE_REMOTE
static void show_remote(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "peers"), *e;
	size_t rem;
	char t[32], rx[16], tx[16];
	int k = 0;

	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Remote access is off. Turn it on: agctl remote on <public address or DDNS name>\n");
		return;
	}
	printf("Remote access %s: %s, UDP %llu, tunnel %s (%s)\n", fnum(msg, "running") ? "on" : "NOT running",
	       orb(fstr(msg, "endpoint"), "(no endpoint)"), (unsigned long long)fnum(msg, "port"), fstr(msg, "subnet"),
	       fnum(msg, "full_tunnel") ? "all traffic goes home" : "home networks only");
	if (fstr(msg, "error") && *fstr(msg, "error"))
		printf("  problem: %s\n", fstr(msg, "error"));
	if (arr)
		blobmsg_for_each_attr(e, arr, rem) {
			if (!k++)
				printf("  %-20s %-12s %-10s %-22s %s\n", "PEER", "ADDRESS", "STATE", "FROM", "TRAFFIC");
			printf("  %-20.20s %-12s %-10s %-22.22s %s in, %s out\n", fstr(e, "name"), fstr(e, "ip"),
			       fnum(e, "online") ? "online" : fnum(e, "handshake") ? "away" : "never yet",
			       *fstr(e, "endpoint") ? fstr(e, "endpoint") : "-", human_bytes(fnum(e, "rx"), rx, sizeof(rx)),
			       human_bytes(fnum(e, "tx"), tx, sizeof(tx)));
			if (fnum(e, "handshake") && !fnum(e, "online"))
				printf("  %-20s last seen %s\n", "", ago(fnum(e, "handshake"), t, sizeof(t)));
		}
	if (!k)
		printf("  no peers yet: agctl remote add <name>\n");
}

static void show_remote_added(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out) {
		print_json(msg);
	} else if (!fnum(msg, "ok")) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	} else {
		printf("Scan this with the WireGuard app (+ > scan from QR code) on %s's phone.\n", fstr(msg, "name"));
		print_qr(fstr(msg, "config"));
		if (remote_conf)
			printf("\n%s\n", fstr(msg, "config"));
		printf("Tunnel address %s. The private key is not kept: this is the only time it is shown.\n",
		       fstr(msg, "ip"));
	}
	if (!fnum(msg, "ok"))
		change_failed = 1;
}
#endif

static void show_model_change(struct ubus_request *req, int type, struct blob_attr *msg)
{
	if (json_out)
		print_json(msg);
	else if (!fnum(msg, "ok"))
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
	else if (fnum(msg, "version"))
		printf("version %llu is in use now (no restart needed)\n", (unsigned long long)fnum(msg, "version"));
	else
		printf("done\n");
	if (!fnum(msg, "ok"))
		change_failed = 1;
}

/* copy a usign public key into the trusted keys directory, named by its fingerprint */
static int trust(const char *file)
{
	struct ag_config c;
	uint8_t fp[8], pk[32];
	char text[1100], fph[17], dst[400];
	size_t n;
	FILE *f = fopen(file, "r"), *o;

	if (!f) {
		fprintf(stderr, "agctl: %s: %s\n", file, strerror(errno));
		return 1;
	}
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	if (ag_usign_pubkey(text, fp, pk)) {
		fprintf(stderr, "agctl: %s is not a usign public key (untrusted comment line + base64 line)\n", file);
		return 1;
	}
	ag_config_load(&c);
	ag_fingerprint_hex(fp, fph);
	if (mkdir(c.trusted_keys, 0755) && errno != EEXIST) {
		fprintf(stderr, "agctl: cannot create %s: %s (sudo?)\n", c.trusted_keys, strerror(errno));
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s.pub", c.trusted_keys, fph);
	if (!(o = fopen(dst, "w")) || fputs(text, o) < 0 || fclose(o)) {
		fprintf(stderr, "agctl: cannot write %s: %s (sudo?)\n", dst, strerror(errno));
		return 1;
	}
	chmod(dst, 0644);
	printf("trusting key %s: models it signs are accepted (%s)\n", fph, dst);
	return 0;
}

static int untrust(const char *fph)
{
	struct ag_config c;
	char path[400];

	ag_config_load(&c);
	snprintf(path, sizeof(path), "%s/%s.pub", c.trusted_keys, fph);
	if (strchr(fph, '/') || unlink(path)) {
		fprintf(stderr, "agctl: no trusted key %s (agctl models lists them)\n", fph);
		return 1;
	}
	printf("key %s is no longer trusted\n", fph);
	return 0;
}

#if AIGW_FEATURE_PLUGINS
static int plugin_config(const char *name, const char *key, const char *value)
{
	struct blob_buf a = { 0 };
	char line[512];
	int rc;

	if (!strcmp(value, "-")) {
		if (isatty(0))
			fprintf(stderr, "%s %s: ", name, key);
		if (!fgets(line, sizeof(line), stdin)) {
			fprintf(stderr, "agctl: nothing read\n");
			return 1;
		}
		line[strcspn(line, "\r\n")] = '\0';
		value = line;
	}
	blob_buf_init(&a, 0);
	blobmsg_add_string(&a, "name", name);
	blobmsg_add_string(&a, "key", key);
	blobmsg_add_string(&a, "value", value);
	rc = call("ag.plugin", "config", a.head, show_ok) || change_failed;
	blob_buf_free(&a);
	return rc;
}

/* fork/exec without a shell; 0 on success */
static int run(char *const argv[])
{
	pid_t pid = fork();
	int status = -1;

	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int plugin_add(const char *folder)
{
	struct ag_config c;
	struct blob_buf m = { 0 };
	struct blob_attr *cur;
	char manifest[512], dst[400], name[64] = "";
	size_t rem;
	struct stat st;

	ag_config_load(&c);
	snprintf(manifest, sizeof(manifest), "%s/manifest.json", folder);
	blob_buf_init(&m, 0);
	if (!blobmsg_add_json_from_file(&m, manifest)) {
		fprintf(stderr, "agctl: %s is missing or not valid JSON\n", manifest);
		blob_buf_free(&m);
		return 1;
	}
	blob_for_each_attr(cur, m.head, rem)
		if (!strcmp(blobmsg_name(cur), "name") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			snprintf(name, sizeof(name), "%s", blobmsg_get_string(cur));
	blob_buf_free(&m);
	if (!ag_plugin_name_ok(name)) {
		fprintf(stderr, "agctl: manifest name '%s' must be 1-31 of a-z, 0-9 and -\n", name);
		return 1;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "agctl: adding a plug-in needs root: sudo agctl plugin add %s\n", folder);
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s", c.plugin_user_dir, name);
	{
		char *mk[] = { "mkdir", "-p", c.plugin_user_dir, NULL };
		char *rm[] = { "rm", "-rf", dst, NULL };
		char *cp[] = { "cp", "-R", (char *)folder, dst, NULL };
		char *own[] = { "chown", "-R", "root:root", dst, NULL };
		char *mode[] = { "chmod", "-R", "go-w", dst, NULL };

		if (run(mk) || (stat(dst, &st) == 0 && run(rm)) || run(cp) || run(own) || run(mode)) {
			fprintf(stderr, "agctl: could not copy %s to %s\n", folder, dst);
			return 1;
		}
	}
	printf("installed %s in %s\n", name, dst);
	device_filter = name;
	if (call("ag.plugin", "rescan", NULL, NULL))
		return 1;
	{
		struct blob_buf a = { 0 };
		int rc;

		blob_buf_init(&a, 0);
		blobmsg_add_string(&a, "name", name);
		rc = call("ag.plugin", "list", a.head, show_plugins);
		blob_buf_free(&a);
		return rc;
	}
}

static int plugin_remove(const char *name)
{
	struct ag_config c;
	char dst[400];
	struct stat st;

	ag_config_load(&c);
	if (!ag_plugin_name_ok(name)) {
		fprintf(stderr, "agctl: no plug-in '%s'\n", name);
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/%s", c.plugin_user_dir, name);
	if (stat(dst, &st) != 0) {
		snprintf(dst, sizeof(dst), "%s/%s", c.plugin_dir, name);
		fprintf(stderr, stat(dst, &st) == 0 ? "agctl: %s comes from a package; remove the package instead\n" :
			"agctl: no plug-in '%s' that you added\n", name);
		return 1;
	}
	{
		char *rm[] = { "rm", "-rf", dst, NULL };

		if (geteuid() != 0 || run(rm)) {
			fprintf(stderr, "agctl: could not remove %s (sudo?)\n", dst);
			return 1;
		}
	}
	printf("removed %s\n", name);
	return call("ag.plugin", "rescan", NULL, NULL);
}
#endif

/* ---------- Wi-Fi keys ---------- */

#if AIGW_FEATURE_KEYS
static void show_keys(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "keys"), *k;
	size_t rem;
	int n = 0;

	if (json_out)
		return print_json(msg);
	if (arr)
		blobmsg_for_each_attr(k, arr, rem) {
			char t1[32];

			if (device_filter) {     /* one key: its QR again */
				n++;
				if (!*fstr(k, "qr")) {
					fprintf(stderr, "agctl: no access point answers, so the Wi-Fi name is unknown\n");
					continue;
				}
				printf("Scan to join \"%s\" as \"%s\":\n\n", fstr(k, "ssid"), fstr(k, "name"));
				print_qr(fstr(k, "qr"));
				printf("\n  Wi-Fi     %s\n  password  %s\n  state     %s%s%s\n", fstr(k, "ssid"),
				       fstr(k, "passphrase"), fstr(k, "state"), *fstr(k, "mac") ? ", bound to " : "", fstr(k, "mac"));
				continue;
			}
			if (!n++)
				printf("%-22s %-8s %-21s %-18s %s\n", "NAME", "ZONE", "STATE", "DEVICE", "SINCE");
			printf("%-22.22s %-8s %-21s %-18.18s %s\n", fstr(k, "name"), orb(fstr(k, "zone"), "-"),
			       fstr(k, "state"), orb(fstr(k, "device"), orb(fstr(k, "mac"), "-")),
			       ago(fnum(k, "bound") ? fnum(k, "bound") : fnum(k, "created"), t1, sizeof(t1)));
		}
	if (!n)
		printf(device_filter ? "No key called '%s'.\n" :
		       "No per-device keys. Give a device its own: agctl key add \"Living room plug\" iot\n", device_filter);
}

static void show_key_added(struct ubus_request *req, int type, struct blob_attr *msg)
{
	char until[64];
	time_t t = (time_t)fnum(msg, "expires");
	struct tm tm;

	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "ok")) {
		fprintf(stderr, "agctl: %s\n", fstr(msg, "error"));
		change_failed = 1;
		return;
	}
	localtime_r(&t, &tm);
	strftime(until, sizeof(until), "%Y-%m-%d %H:%M", &tm);
	printf("Scan to join \"%s\" as \"%s\"%s%s%s:\n\n", fstr(msg, "ssid"), fstr(msg, "name"),
	       *fstr(msg, "zone") ? " (zone " : "", fstr(msg, "zone"), *fstr(msg, "zone") ? ")" : "");
	print_qr(fstr(msg, "qr"));
	printf("\n  Wi-Fi     %s\n  password  %s   (for one device only)\n  waits     until %s for a device to join\n",
	       fstr(msg, "ssid"), fstr(msg, "passphrase"), until);
	printf("\nThe first device to join owns this key. Take it away any time:\n  agctl key revoke \"%s\"\n",
	       fstr(msg, "name"));
	if (*fstr(msg, "error"))
		printf("\nnote: %s\n", fstr(msg, "error"));
}
#endif

/* "90", "90s", "30m", "2h", "1d" -> seconds; -1 when unreadable */
static int parse_time(const char *s)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || v <= 0)
		return -1;
	switch (*end) {
	case '\0': case 's': break;
	case 'm': v *= 60; break;
	case 'h': v *= 3600; break;
	case 'd': v *= 86400; break;
	default: return -1;
	}
	if (*end && end[1])
		return -1;
	return v > 365L * 86400 ? 365 * 86400 : (int)v;
}

/* "2mbit", "500kbit", "1.5mbit", "800" (kbit/s) -> kbit/s; -1 when it isn't a rate */
static int parse_rate(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v <= 0)
		return -1;
	if (!*end || !strcasecmp(end, "k") || !strcasecmp(end, "kbit") || !strcasecmp(end, "kbps"))
		return (int)v;
	if (!strcasecmp(end, "m") || !strcasecmp(end, "mbit") || !strcasecmp(end, "mbps"))
		return (int)(v * 1000);
	if (!strcasecmp(end, "g") || !strcasecmp(end, "gbit") || !strcasecmp(end, "gbps"))
		return (int)(v * 1000000);
	return -1;
}

#if AIGW_FEATURE_UPGRADE
/* agctl upgrade [--forget]: the last firmware upgrade, as AIGW checked it */
static void show_upgrade(struct ubus_request *req, int type, struct blob_attr *msg)
{
	unsigned long long now = (unsigned long long)time(NULL);
	struct blob_attr *d, *ch = field(msg, "checks"), *bf = field(msg, "before"), *m;
	const char *st = fstr(msg, "state");
	char t1[32], t2[32];
	size_t rem;
	int missing = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (field(msg, "forgot"))
		printf("%s\n", fnum(msg, "forgot") ? "The hold is lifted: that version may be installed again." :
		       "No version was held off.");
	if (!fnum(msg, "check")) {
		printf("Firmware upgrades are not checked (upgrade.check in /etc/config/aigw).\n");
		return;
	}
	printf("Firmware upgrades are checked for %s after the boot%s.\n", dur_short(fnum(msg, "watch_s"), t1, sizeof(t1)),
	       fnum(msg, "rollback") ? ", and rolled back when the home is worse on them" : " (not rolled back: upgrade.rollback 0)");
	if (*fstr(msg, "rollback_method")) {
		const char *rm = fstr(msg, "rollback_method");

		printf("  way back  %s%s%s\n", !strcmp(rm, "TwoBanks") ? "two firmware banks" :
		       !strcmp(rm, "KeptCopy") ? "one firmware image, and a kept copy" : !strcmp(rm, "None") ? "none" : "not known",
		       *fstr(msg, "rollback_note") ? ": " : "", fstr(msg, "rollback_note"));
	}
	if (*fstr(msg, "held"))
		printf("  held off  %s, rolled back: %s\n", fstr(msg, "held"), fstr(msg, "held_why"));
	if (!strcmp(st, "None")) {
		printf("  no upgrade checked yet\n");
		return;
	}
	printf("  upgrade   %s -> %s, activated %s ago: %s\n", fstr(msg, "from"), fstr(msg, "to"),
	       dur_short(now > fnum(msg, "activated") ? now - fnum(msg, "activated") : 0, t1, sizeof(t1)),
	       !strcmp(st, "Activated") ? "boots next" : !strcmp(st, "Watching") ? "watching" : !strcmp(st, "Kept") ? "kept" :
	       !strcmp(st, "Worse") ? "WORSE, not rolled back" : !strcmp(st, "RollingBack") ? "worse: rolling back" :
	       !strcmp(st, "RolledBack") ? "worse: rolled back" : !strcmp(st, "NotBooted") ? "never started" : st);
	if (!strcmp(st, "Watching"))
		printf("            %s watched, %s to go\n", dur_short(fnum(msg, "watched_for_s"), t1, sizeof(t1)),
		       dur_short(fnum(msg, "left_s"), t2, sizeof(t2)));
	if (fnum(msg, "starts") > 1)
		printf("            agd started %llu times on it\n", (unsigned long long)fnum(msg, "starts"));
	printf("  devices   %llu of the %llu connected before came back on %s", (unsigned long long)fnum(msg, "devices_back"),
	       (unsigned long long)(fnum(msg, "devices_back") + fnum(msg, "devices_missing")), fstr(msg, "to"));
	blobmsg_for_each_attr(d, field(msg, "devices"), rem)
		if (!fnum(d, "back"))
			missing++;
	if (missing) {
		int k = 0;

		printf("; stayed away:");
		blobmsg_for_each_attr(d, field(msg, "devices"), rem)
			if (!fnum(d, "back"))
				printf("%s %s (%s%s)", k++ ? "," : "", orb(fstr(d, "name"), fstr(d, "mac")),
				       fnum(d, "wifi") ? "Wi-Fi" : "wired",
				       !field(d, "back_after_rollback") ? "" :
				       fnum(d, "back_after_rollback") ? ", back after the rollback" : ", not back after the rollback");
	}
	printf("\n");
	if (ch && fnum(ch, "n")) {
		printf("  checks    %llu, %llu without the internet", (unsigned long long)fnum(ch, "n"),
		       (unsigned long long)fnum(ch, "without_internet"));
		blobmsg_for_each_attr(m, ch, rem) {
			if (blobmsg_type(m) != BLOBMSG_TYPE_TABLE)
				continue;
			printf("; %s %.1f (normal %.1f)", blobmsg_name(m), fdbl(m, "measured"), fdbl(m, "normal"));
		}
		if (field(ch, "speed_mbps"))
			printf("; speed %.0f Mbit/s", fdbl(ch, "speed_mbps"));
		printf("\n");
	}
	if (field(msg, "score_after") || field(msg, "score_before")) {
		snprintf(t1, sizeof(t1), "%llu%%", (unsigned long long)fnum(msg, "score_after"));
		snprintf(t2, sizeof(t2), "%llu%%", (unsigned long long)fnum(msg, "score_before"));
		printf("  score     %s of the time without a problem (%s the week before)\n",
		       field(msg, "score_after") ? t1 : "-", field(msg, "score_before") ? t2 : "-");
	}
	if (bf && strcmp(fstr(bf, "internet"), "unknown") && *fstr(bf, "internet"))
		printf("  before    the internet answered: %s\n", fstr(bf, "internet"));
	if (*fstr(msg, "summary"))
		printf("  verdict   %s\n", fstr(msg, "summary"));
	if (*fstr(msg, "confirmed"))
		printf("  confirmed %s\n", fstr(msg, "confirmed"));
}
#endif

#if AIGW_FEATURE_WHY || AIGW_FEATURE_BRIEF
/* "20:00" -> the last time it was 20:00 (today, else yesterday); "yesterday 20:00"; seconds since 1970.
   -1 when unreadable */
static long parse_clock(const char *s)
{
	time_t now = time(NULL), t;
	struct tm tm;
	int h, m, back = 0;
	char *end;
	long v;

	v = strtol(s, &end, 10);
	if (!*end && v > 1000000000L)
		return v;
	if (!strncmp(s, "yesterday ", 10))
		s += 10, back = 1;
	if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
		return -1;
	localtime_r(&now, &tm);
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	if (t > now || back)                    /* later today: yesterday's; "yesterday": the day before today */
		t -= 86400;
	return (long)t;
}
#endif

#if AIGW_FEATURE_WHY || AIGW_FEATURE_BRIEF || AIGW_FEATURE_CALLS || AIGW_FEATURE_OFFERS
static void clock_of(unsigned long long t, char *buf, size_t n)
{
	time_t tt = (time_t)t, now = time(NULL);
	struct tm a, b;

	localtime_r(&tt, &a);
	localtime_r(&now, &b);
	strftime(buf, n, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M" : "%a %H:%M", &a);
}
#endif

#if AIGW_FEATURE_WHY
static int why_failed;
#endif

/* agctl why [device] [--at HH:MM | --from HH:MM --to HH:MM | --last 2h] [--raw]: a stretch of the
   home's timeline explained */
#if AIGW_FEATURE_WHY
static void show_why(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *f, *nm = field(msg, "normal");
	char a[32], b[32], c[32];
	size_t rem;
	int k = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (*fstr(msg, "error")) {
		printf("%s\n", fstr(msg, "error"));
		why_failed = 1;
		return;
	}
	clock_of(fnum(msg, "from"), a, sizeof(a));
	clock_of(fnum(msg, "to"), b, sizeof(b));
	printf("%s to %s%s%s: %s\n", a, b, *fstr(msg, "host") ? ", " : "", fstr(msg, "host"), fstr(msg, "summary"));
	printf("  code      %s (%s, %s)\n", fstr(msg, "code"), fstr(msg, "area"), fstr(msg, "severity"));
	if (*fstr(msg, "advice"))
		printf("  advice    %s\n", fstr(msg, "advice"));
	clock_of(fnum(msg, "recorded_since"), c, sizeof(c));
	printf("  recorded  %llu of %llu samples (one every %llu s; the timeline goes back to %s)\n",
	       (unsigned long long)fnum(msg, "recorded"), (unsigned long long)fnum(msg, "expected"),
	       (unsigned long long)fnum(msg, "interval"), fnum(msg, "recorded_since") ? c : "-");
	if (nm && (field(nm, "internet_ms") || field(nm, "line_mbps"))) {
		printf("  normal   ");
		if (field(nm, "internet_ms"))
			printf(" internet %llu ms%s", (unsigned long long)fnum(nm, "internet_ms"), field(nm, "next_hop_ms") ? "," : "");
		if (field(nm, "next_hop_ms"))
			printf(" next router %llu ms", (unsigned long long)fnum(nm, "next_hop_ms"));
		if (field(nm, "dns_ms"))
			printf(", DNS %llu ms", (unsigned long long)fnum(nm, "dns_ms"));
		if (field(nm, "line_mbps"))
			printf("%sthe line %llu Mbit/s", field(nm, "internet_ms") ? "; " : " ",
			       (unsigned long long)fnum(nm, "line_mbps"));
		printf("\n");
	}
	blobmsg_for_each_attr(f, field(msg, "story"), rem) {
		if (!k++)
			printf("  what happened\n");
		printf("    %-9s %s\n", fstr(f, "clock"), fstr(f, "text"));
	}
#if AIGW_FEATURE_PROVEN
	k = 0;
	blobmsg_for_each_attr(f, field(msg, "repairs"), rem) {
		char t[32];

		if (!k++)
			printf("  AIGW's fixes then (agctl repair)\n");
		clock_of(fnum(f, "created"), t, sizeof(t));
		printf("    %-9s #%llu %s %s: %s\n", t, (unsigned long long)fnum(f, "id"),
		       !strcmp(fstr(f, "mode"), "shadow") ? "shadow (not applied)" : fstr(f, "action"), fstr(f, "result"),
		       *fstr(f, "summary") ? fstr(f, "summary") : fstr(f, "prediction"));
	}
#endif
	printf("  findings\n");
	blobmsg_for_each_attr(f, field(msg, "findings"), rem)
		printf("    %-8s %-16s %s\n", fstr(f, "severity"), fstr(f, "code"), fstr(f, "text"));
}
#endif

#if AIGW_FEATURE_BRIEF
static int brief_failed;
#endif

#if AIGW_FEATURE_BRIEF
/* agctl brief [device] [period] [--no-check] [--speed] [--propose]: the help desk's answer */
static void show_brief(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *f, *fx = field(msg, "fix");
	char a[32], b[32];
	size_t rem;
	int k = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (*fstr(msg, "error")) {
		printf("%s\n", fstr(msg, "error"));
		brief_failed = 1;
		return;
	}
	clock_of(fnum(msg, "from"), a, sizeof(a));
	clock_of(fnum(msg, "to"), b, sizeof(b));
	printf("%s to %s%s\n", a, b, fnum(msg, "checked") ? " (checked just now)" : "");
	printf("  fault     %s (%s, %s confidence)\n", fstr(msg, "fault"), fstr(msg, "code"), fstr(msg, "confidence"));
	printf("  verdict   %s\n", fstr(msg, "verdict"));
	blobmsg_for_each_attr(f, field(msg, "evidence"), rem) {
		if (!k++)
			printf("  evidence\n");
		printf("    %-9s %s\n", *fstr(f, "clock") ? fstr(f, "clock") : fstr(f, "source"), fstr(f, "text"));
	}
	k = 0;
	blobmsg_for_each_attr(f, field(msg, "done"), rem) {
		if (!k++)
			printf("  AIGW did\n");
		printf("    %-9s %s\n", *fstr(f, "clock") ? fstr(f, "clock") : "", fstr(f, "text"));
	}
	printf("  tell      %s\n", fstr(msg, "tell"));
	if (fx) {
		printf("  fix       %s%s\n", fnum(fx, "possible") ? "remote: " : "", fstr(fx, "text"));
		if (fnum(fx, "decision"))
			printf("            decision %llu %s: agctl approve %llu (only with the customer's yes)\n",
			       (unsigned long long)fnum(fx, "decision"), fnum(fx, "proposed") ? "proposed now" : "waits",
			       (unsigned long long)fnum(fx, "decision"));
		else if (fnum(fx, "possible") && strcmp(fstr(fx, "action"), "undo"))
			printf("            not proposed: agctl brief --propose makes it a decision that waits for a yes\n");
		if (*fstr(fx, "error"))
			printf("            not proposed: %s\n", fstr(fx, "error"));
	}
	printf("  visit     %s\n", fstr(msg, "visit_reason"));
}
#endif

#if AIGW_FEATURE_SQM
static int sqm_failed;
#endif

#if AIGW_FEATURE_SQM
/* agctl sqm [--on|--off]: self-tuning smart queueing */
static void show_sqm(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *t = field(msg, "trial");
	const char *st = fstr(msg, "status");

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (*fstr(msg, "error")) {
		printf("%s\n", fstr(msg, "error"));
		sqm_failed = 1;
	} else if (field(msg, "asked")) {
		printf("Asked (decision %llu).\n", (unsigned long long)fnum(msg, "asked"));
	}
	if (!fnum(msg, "enabled")) {
		printf("Smart queueing on bufferbloat is off (sqm.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Smart queueing: %s\n", !strcmp(st, "Off") ? "off: no bufferbloat seen (AIGW proposes it when it sees some)" :
	       !strcmp(st, "Proposed") ? "proposed, waiting for you (agctl decisions)" :
	       !strcmp(st, "Trial") ? "on trial: kept if the delay while the line is full clearly falls" :
	       !strcmp(st, "On") ? "on" : !strcmp(st, "Tried") ? "off: it was tried and didn't help" :
	       !strcmp(st, "Unavailable") ? "not available here" : st);
	if (*fstr(msg, "interface")) {
		char up[32];

		if (fnum(msg, "up_kbps"))
			snprintf(up, sizeof(up), "%.1f Mbit/s", fnum(msg, "up_kbps") / 1000.0);
		else
			snprintf(up, sizeof(up), "not shaped");
		printf("  line      %s, shaped at %.1f Mbit/s down, %s up\n", fstr(msg, "interface"),
		       fnum(msg, "down_kbps") / 1000.0, up);
	}
	if (*fstr(msg, "shaping"))
		printf("  now       %s\n", fstr(msg, "shaping"));
	if (*fstr(msg, "unavailable"))
		printf("  why not   %s\n", fstr(msg, "unavailable"));
	if (t) {
		printf("  before    while the line was full (%llu samples): the internet in %llu ms, downloads %.1f Mbit/s\n",
		       (unsigned long long)fnum(t, "before_full"), (unsigned long long)fnum(t, "before_ms"),
		       fnum(t, "before_kbps") / 1000.0);
		printf("  after     %llu of %llu full samples: the internet in %llu ms, downloads %.1f Mbit/s (%s)\n",
		       (unsigned long long)fnum(t, "after_full"), (unsigned long long)fnum(t, "needed"),
		       (unsigned long long)fnum(t, "after_ms"), fnum(t, "after_kbps") / 1000.0, fstr(t, "verdict"));
		if (*fstr(t, "summary"))
			printf("  verdict   %s\n", fstr(t, "summary"));
	}
}
#endif

#if AIGW_FEATURE_CALLS
/* agctl calls: calls and games now, first in the queue or not; the last one's report; the counts */
static void show_calls(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s, *e, *st = field(msg, "stats"), *l = field(msg, "last");
	char since[32];
	size_t rem, r2;
	int n = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Calls and games first is off (calls.enabled in /etc/config/aigw).\n");
		return;
	}
	if (!fnum(msg, "flows")) {
		printf("Calls and games first needs the flow sensor (flow.enabled), which is off.\n");
		return;
	}
	printf("Calls and games first: ");
	if (!fnum(msg, "priority"))
		printf("reports only (calls.priority 0)\n");
	else if (*fstr(msg, "sqm"))
		printf("they go first inside smart queueing on %s\n", fstr(msg, "sqm"));
	else
		printf("smart queueing is off, so nothing is shaped for them (agctl sqm)\n");
	if (*fstr(msg, "priority_error"))
		printf("  no priority: %s\n", fstr(msg, "priority_error"));
	blobmsg_for_each_attr(s, field(msg, "live"), rem) {
		clock_of(fnum(s, "since"), since, sizeof(since));
		printf("  now       %s on %s%s%s%s since %s%s; far end", !strcmp(fstr(s, "kind"), "Game") ? "a game" :
		       !strcmp(fstr(s, "kind"), "Call") ? "a call" : "a call or game", fstr(s, "device"),
		       *fstr(s, "app") ? " (" : "", fstr(s, "app"), *fstr(s, "app") ? ")" : "", since,
		       fnum(s, "first") ? ", going first" : "");
		blobmsg_for_each_attr(e, field(s, "far_ends"), r2)
			printf(" %s", blobmsg_get_string(e));
		printf("\n");
		n++;
	}
	if (!n)
		printf("  now       no call or game\n");
	if (l) {
		clock_of(fnum(l, "end"), since, sizeof(since));
		printf("  last      %s (ended %s)\n", fstr(l, "summary"), since);
	}
	if (st)
		printf("  so far    %llu calls, %llu games; %llu met a full line, %llu of them smooth; %llu went first\n",
		       (unsigned long long)fnum(st, "calls"), (unsigned long long)fnum(st, "games"),
		       (unsigned long long)fnum(st, "on_full_line"), (unsigned long long)fnum(st, "smooth_on_full_line"),
		       (unsigned long long)fnum(st, "prioritized"));
	printf("  the ISP   %s\n", fnum(msg, "shared") ? "sees the reports, without device or app names" :
	       "sees nothing (calls.share 0)");
}
#endif

#if AIGW_FEATURE_WIFI_HEAL
/* an hour's value from an array of 24 (-1: not known) */
static int hour_val(struct blob_attr *arr, int h)
{
	struct blob_attr *v;
	size_t rem;
	int k = 0;

	if (arr)
		blobmsg_for_each_attr(v, arr, rem)
			if (k++ == h)
				return (int32_t)blobmsg_get_u32(v);
	return -1;
}
#endif

#if AIGW_FEATURE_WIFI_HEAL
/* agctl channels: neighbour-aware channels, each access point's channels by hour of day */
static void show_channels(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *arr = field(msg, "radios"), *r, *c, *a;
	size_t rem, rem2;
	int h;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Neighbour-aware channels are off (wifi.chan_plan, wifi.chan_heal in /etc/config/aigw).\n");
		return;
	}
	if (!arr || !blobmsg_len(arr)) {
		printf("No access point learned yet (each channel is learned hour by hour once the Wi-Fi is up).\n");
		return;
	}
	blobmsg_for_each_attr(r, arr, rem) {
		struct blob_attr *use = field(r, "use"), *chs = field(r, "channels");
		int max = 0;

		printf("%s GHz (%s), channel %llu: %s", fstr(r, "band"), fstr(r, "iface"),
		       (unsigned long long)fnum(r, "channel"), fstr(r, "status"));
		if (!strcmp(fstr(r, "status"), "Confirming"))
			printf(" (look %llu)", (unsigned long long)fnum(r, "looks"));
		printf("\n  Learned for %llu days, %llu scans. The home's busy hours: %s.\n  %s\n",
		       (unsigned long long)fnum(r, "days"), (unsigned long long)fnum(r, "scans"), fstr(r, "busy_hours"),
		       fstr(r, "verdict"));
		if (*fstr(r, "waits"))
			printf("  The move waits: %s.\n", fstr(r, "waits"));
		if (chs) {
			for (h = 0; h < 24; h++)
				if (hour_val(use, h) > max)
					max = hour_val(use, h);
			printf("  Air used by others, %% by hour of day (. not measured, * the home's busy hours):\n   hour");
			for (h = 0; h < 24; h++)
				printf("%3d", h);
			printf("  neighbours\n       ");
			for (h = 0; h < 24; h++)
				printf("  %c", max >= 1 && hour_val(use, h) * 2 >= max ? '*' : ' ');
			printf("\n");
			blobmsg_for_each_attr(c, chs, rem2) {
				struct blob_attr *air = field(c, "air"), *nb = field(c, "neighbours");
				int sum = 0, n = 0;

				printf("  %s%3llu ", fnum(c, "channel") == fnum(r, "channel") ? ">" : " ",
				       (unsigned long long)fnum(c, "channel"));
				for (h = 0; h < 24; h++) {
					int v = hour_val(air, h), w = hour_val(nb, h);

					if (v < 0)
						printf("  .");
					else
						printf("%3d", v > 99 ? 99 : v);
					if (w >= 0)
						sum += w, n++;
				}
				if (n)
					printf("  %.1f\n", sum / 10.0 / n);
				else
					printf("  .\n");
			}
		}
		if ((a = field(r, "avoided")) && blobmsg_len(a)) {
			struct blob_attr *x;
			size_t rem3;

			printf("  Avoided (didn't help lately):");
			blobmsg_for_each_attr(x, a, rem3) {
				char t[32];
				time_t u = (time_t)fnum(x, "until");

				strftime(t, sizeof(t), "%Y-%m-%d %H:%M", localtime(&u));
				printf(" %llu until %s", (unsigned long long)fnum(x, "channel"), t);
			}
			printf("\n");
		}
		if (fnum(r, "trial_decision"))
			printf("  Trial: from channel %llu (decision %llu), %llu s of busy hours measured so far.\n",
			       (unsigned long long)fnum(r, "trial_from"), (unsigned long long)fnum(r, "trial_decision"),
			       (unsigned long long)fnum(r, "trial_measured_s"));
		printf("  Planned moves: %llu proposed, %llu kept, %llu moved back.\n",
		       (unsigned long long)fnum(r, "proposed"), (unsigned long long)fnum(r, "kept"),
		       (unsigned long long)fnum(r, "tried"));
		if (*fstr(r, "last"))
			printf("  Last: %s\n", fstr(r, "last"));
	}
}
#endif

/* agctl mesh: the EasyMesh network as AIGW sees it: access points and their backhaul, each device's access point
   and link (and why AIGW left it there), the channel plan, the trial now, the counts */
#if AIGW_FEATURE_MESH
static void show_mesh(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *m = field(msg, "mesh"), *a, *r, *s, *x;
	size_t rem, r2, r3;
	char when[32];

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!m)
		return;
	if (!fnum(m, "enabled")) {
		printf("Mesh Wi-Fi is off (mesh.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Mesh Wi-Fi: %s\n", fstr(m, "summary"));
	if (strcmp(fstr(m, "status"), "Ok")) {
		printf("  status    %s%s%s\n", fstr(m, "status"), *fstr(m, "error") ? ": " : "", fstr(m, "error"));
		if (!strcmp(fstr(m, "status"), "NoController"))
			printf("  AIGW reads a standard EasyMesh controller through its Data Elements model;\n"
			       "  see docs/mesh.md for setting one up, or another through mesh.handler.\n");
	}
	if (field(m, "read"))
		printf("  read      %s from the controller %s (%s; steering %s, channel plan %s, act.mode %s)\n",
		       ago(fnum(m, "read"), when, sizeof(when)), fstr(m, "controller"), fstr(m, "source"),
		       fnum(m, "steer") ? "on" : "off", fnum(m, "channel") ? "on" : "off", fstr(m, "mode"));
	blobmsg_for_each_attr(a, field(m, "agents"), rem) {
		struct blob_attr *bh = field(a, "backhaul");

		printf("\n  %s%s%s%s%s (%s)\n", fstr(a, "words"), *fstr(a, "model") ? ", " : "", fstr(a, "model"),
		       "", "", fstr(a, "id"));
		if (bh) {
			char sig[24] = "", rate[32] = "";

			if (field(bh, "signal"))
				snprintf(sig, sizeof(sig), ", %d dBm", (int)fnum(bh, "signal"));
			if (field(bh, "rate_kbps"))
				snprintf(rate, sizeof(rate), ", %llu Mbit/s", (unsigned long long)fnum(bh, "rate_kbps") / 1000);
			printf("    backhaul  %s%s%s%s%s to %s\n", fstr(bh, "type"),
			       fnum(bh, "band") ? " " : "", fnum(bh, "band") == 2 ? "2.4 GHz" : fnum(bh, "band") == 5 ? "5 GHz" :
			       fnum(bh, "band") == 6 ? "6 GHz" : "", sig, rate, *fstr(bh, "parent") ? fstr(bh, "parent") : "?");
		}
		blobmsg_for_each_attr(r, field(a, "radios"), r2) {
			int k = 0;

			printf("    radio     %s %s channel %llu/%llu MHz%s", fstr(r, "id"),
			       fnum(r, "band") == 2 ? "2.4 GHz" : fnum(r, "band") == 5 ? "5 GHz" : fnum(r, "band") == 6 ? "6 GHz" : "?",
			       (unsigned long long)fnum(r, "channel"), (unsigned long long)fnum(r, "width"),
			       field(r, "utilization") ? "" : "\n");
			if (field(r, "utilization"))
				printf(", %llu%% busy\n", (unsigned long long)fnum(r, "utilization"));
			blobmsg_for_each_attr(x, field(r, "bss"), r3) {
				printf("%s%s%s%s", k++ ? ", " : "              BSS ", fstr(x, "bssid"),
				       *fstr(x, "ssid") ? " " : "", fstr(x, "ssid"));
				if (fnum(x, "backhaul"))
					printf(fnum(x, "fronthaul") ? " (also backhaul)" : " (backhaul)");
			}
			if (k)
				printf("\n");
		}
	}
	if (field(m, "stations")) {
		printf("\n  devices\n");
		blobmsg_for_each_attr(s, field(m, "stations"), rem) {
			char sig[16] = "?", rate[40] = "", rt[40] = "";

			if (field(s, "signal"))
				snprintf(sig, sizeof(sig), "%d dBm", (int)fnum(s, "signal"));
			if (field(s, "rate_kbps"))
				snprintf(rate, sizeof(rate), ", %llu Mbit/s", (unsigned long long)fnum(s, "rate_kbps") / 1000);
			if (field(s, "retry_pct"))
				snprintf(rt, sizeof(rt), ", %llu%% sent again", (unsigned long long)fnum(s, "retry_pct"));
			printf("    %-18s %s %s, %s%s%s; 11v %s, 11k %s", *fstr(s, "name") ? fstr(s, "name") : fstr(s, "mac"),
			       fstr(s, "on"), fnum(s, "band") == 2 ? "2.4 GHz" : fnum(s, "band") == 5 ? "5 GHz" :
			       fnum(s, "band") == 6 ? "6 GHz" : "", sig, rate, rt, fstr(s, "btm"), fstr(s, "rrm"));
			if (fnum(s, "steers"))
				printf("; moved %llu (kept %llu, back %llu, missed %llu)", (unsigned long long)fnum(s, "steers"),
				       (unsigned long long)fnum(s, "kept"), (unsigned long long)fnum(s, "undone"),
				       (unsigned long long)fnum(s, "misses"));
			printf("\n");
			if (*fstr(s, "now") && strcmp(fstr(s, "now"), "fine"))
				printf("    %-18s %s\n", "", fstr(s, "now"));
		}
	}
	blobmsg_for_each_attr(x, field(m, "backhaul"), rem)
		printf("\n  backhaul  %s%s\n            %s\n", fstr(x, "words"),
		       fnum(x, "hurts") ? " It hurts more than it helps." : "", fstr(x, "advice"));
	blobmsg_for_each_attr(x, field(m, "plan"), rem)
		printf("\n  channels  %s%s\n", fstr(x, "why"), fnum(x, "tried") ? " (tried in the last day: not again yet)" : "");
	if ((x = field(m, "trial")))
		printf("\n  trial     decision %llu: %s %s -> %s, %s; before: %s\n", (unsigned long long)fnum(x, "decision"),
		       fstr(x, "kind"), !strcmp(fstr(x, "kind"), "steer") ? fstr(x, "from") : fstr(x, "radio"),
		       !strcmp(fstr(x, "kind"), "steer") ? fstr(x, "to") : "new channel",
		       fnum(x, "landed") ? "measuring" : "waiting for it to take effect", fstr(x, "before"));
	if ((x = field(m, "last")))
		printf("\n  last      decision %llu (%s, %s, %s): %s\n", (unsigned long long)fnum(x, "decision"), fstr(x, "kind"),
		       fstr(x, "state"), ago(fnum(x, "at"), when, sizeof(when)), fstr(x, "summary"));
	if ((x = field(m, "counts")))
		printf("  so far    moved %llu device%s (kept %llu, moved back %llu, missed %llu); %llu channel change%s "
		       "(kept %llu, back %llu); %llu proposed; %llu move%s by the devices or the controller\n",
		       (unsigned long long)fnum(x, "steers"), fnum(x, "steers") == 1 ? "" : "s", (unsigned long long)fnum(x, "kept"),
		       (unsigned long long)fnum(x, "undone"), (unsigned long long)fnum(x, "misses"),
		       (unsigned long long)fnum(x, "channel_changes"), fnum(x, "channel_changes") == 1 ? "" : "s",
		       (unsigned long long)fnum(x, "channels_kept"), (unsigned long long)fnum(x, "channels_undone"),
		       (unsigned long long)fnum(x, "proposed"), (unsigned long long)fnum(x, "roams"),
		       fnum(x, "roams") == 1 ? "" : "s");
}
#endif

#if AIGW_FEATURE_COVERAGE
/* agctl coverage: where Wi-Fi is weak, which device, when, and what fixes it */
static void show_coverage(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s;
	size_t rem;
	int k = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Coverage advice is off (coverage.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Coverage: %s (%llu device%s judged, %llu weak, %llu to move, %llu with a weak radio of their own; "
	       "weak at %d dBm)\n", fstr(msg, "verdict"), (unsigned long long)fnum(msg, "judged"),
	       fnum(msg, "judged") == 1 ? "" : "s", (unsigned long long)fnum(msg, "weak_devices"),
	       (unsigned long long)fnum(msg, "to_move"), (unsigned long long)fnum(msg, "weak_transmitters"),
	       (int)fnum(msg, "weak_dbm"));
	if (*fstr(msg, "advice"))
		printf("  advice    %s\n", fstr(msg, "advice"));
	else
		printf("  advice    nothing yet: AIGW learns each device's coverage first (an hour or so of looks)\n");
	if (*fstr(msg, "customer_note"))
		printf("  customer  %s\n", fstr(msg, "customer_note"));
	blobmsg_for_each_attr(s, field(msg, "spots"), rem) {
		printf("%s  spot %d    %s: %llu device%s, %llu suffer%s%s%s%s\n", k ? "" : "\n", k + 1, fstr(s, "where"),
		       (unsigned long long)fnum(s, "devices"), fnum(s, "devices") == 1 ? "" : "s",
		       (unsigned long long)fnum(s, "suffering"), fnum(s, "suffering") == 1 ? "s" : "",
		       fnum(s, "shared") ? " (shared: the gateway's place)" : "", *fstr(s, "heard_best_by") ? "; heard best by " : "",
		       fstr(s, "heard_best_by"));
		k++;
	}
	k = 0;
	blobmsg_for_each_attr(s, field(msg, "list"), rem) {
		printf("%s  %s\n", k++ ? "" : "\n  devices\n", fstr(s, "words"));
		if (strcmp(fstr(s, "kind"), "Fine") && strcmp(fstr(s, "kind"), "Learning") && *fstr(s, "advice"))
			printf("      -> %s\n", fstr(s, "advice"));
	}
}
#endif

#if AIGW_FEATURE_HOMEMODEL
/* agctl home: the home Wi-Fi model: places, what-if options, before you buy */
static void show_home(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s, *x;
	size_t rem, r2;
	int k = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (*fstr(msg, "error"))
		fprintf(stderr, "agctl home: %s\n", fstr(msg, "error"));
	if (fnum(msg, "named"))
		printf("Place %llu named.\n", (unsigned long long)fnum(msg, "named"));
	if (!fnum(msg, "enabled")) {
		printf("The home model is off (home.enabled in /etc/config/aigw).\n");
		return;
	}
	printf("Home model: %s (%llu looks, %llu place%s, %llu access point%s; %llu coverage problem%s in the last 30 days)\n",
	       fstr(msg, "status"), (unsigned long long)fnum(msg, "looks"), (unsigned long long)fnum(msg, "places"),
	       fnum(msg, "places") == 1 ? "" : "s", (unsigned long long)fnum(msg, "access_points"),
	       fnum(msg, "access_points") == 1 ? "" : "s", (unsigned long long)fnum(msg, "problems"),
	       fnum(msg, "problems") == 1 ? "" : "s");
	if (!strcmp(fstr(msg, "status"), "Learning"))
		printf("  learning  %llu of %llu looks so far (a look at each device per coverage.interval)\n",
		       (unsigned long long)fnum(msg, "looks"), (unsigned long long)fnum(msg, "learn"));
	if (*fstr(msg, "advice"))
		printf("  advice    %s\n", fstr(msg, "advice"));
	printf("  extender  worth buying: %s\n", fstr(msg, "extender_helps"));
	if (*fstr(msg, "proof"))
		printf("  proof     %s\n", fstr(msg, "proof"));
	blobmsg_for_each_attr(s, field(msg, "options"), rem) {
		printf("%s  %d. %-15s %-4s fixes %llu of %llu, %s confidence\n     %s\n", k ? "" : "\n  what if (best first)\n",
		       k + 1, fstr(s, "kind"), fstr(s, "cost"), (unsigned long long)fnum(s, "fixed"),
		       (unsigned long long)fnum(s, "of"), fstr(s, "confidence_name"), fstr(s, "text"));
		k++;
	}
	k = 0;
	blobmsg_for_each_attr(s, field(msg, "list"), rem) {
		char nb[64] = "";
		size_t l = 0;

		blobmsg_for_each_attr(x, field(s, "next_to"), r2)
			if (l < sizeof(nb))
				l += (size_t)snprintf(nb + l, sizeof(nb) - l, "%s%u", l ? "," : "", blobmsg_get_u32(x));
		printf("%s  place %llu  %-28s %4d dBm (gateway, 5 GHz terms), path loss %llu/%llu dB (2.4/5 GHz), "
		       "like %llu m; %llu problem%s%s%s\n", k++ ? "" : "\n  places\n", (unsigned long long)fnum(s, "place"),
		       *fstr(s, "name") ? fstr(s, "name") : fstr(s, "where"), (int)fnum(s, "signal"),
		       (unsigned long long)fnum(s, "path_loss_24"), (unsigned long long)fnum(s, "path_loss_5"),
		       (unsigned long long)fnum(s, "distance_like_m"), (unsigned long long)fnum(s, "problems"),
		       fnum(s, "problems") == 1 ? "" : "s", *nb ? "; next to " : "", nb);
	}
}
#endif

#if AIGW_FEATURE_SENSING
/* agctl sense: Wi-Fi sensing (opt-in) */
static int sense_what;

static void show_sense(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s;
	size_t rem;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (*fstr(msg, "error"))
		fprintf(stderr, "agctl sense: %s\n", fstr(msg, "error"));
	if (sense_what || !fnum(msg, "enabled")) {
		printf("Wi-Fi sensing is %s. What it senses, when on:\n", fnum(msg, "enabled") ? "on" : "off (opt-in)");
		blobmsg_for_each_attr(s, field(msg, "sensed"), rem)
			printf("  - %s\n", blobmsg_get_string(s));
		printf("Everything stays on this gateway%s. Turn it on: agctl sense on --i-agree; off (and forget it all):\n"
		       "agctl sense off. Details: docs/wifi-sensing.md.\n", fnum(msg, "share_events") ?
		       ", except coarse events to the ISP (sensing.share_events)" : "");
		if (!fnum(msg, "enabled"))
			return;
	}
	printf("Wi-Fi sensing: on (%s); a window every %llu s, %llu device link%s\n", fstr(msg, "agreed"),
	       (unsigned long long)fnum(msg, "window_s"), (unsigned long long)fnum(msg, "links"),
	       fnum(msg, "links") == 1 ? "" : "s");
	printf("  presence  %s (%llu%%; %s) since %s\n", !strcmp(fstr(msg, "presence"), "home") ? "someone is home" :
	       "nobody seems to be home", (unsigned long long)fnum(msg, "home_pct"), *fstr(msg, "reason") ?
	       fstr(msg, "reason") : "learning", fstr(msg, "since"));
	blobmsg_for_each_attr(s, field(msg, "zones"), rem) {
		if (field(s, "last_motion_s_ago"))
			printf("  zone      %-16s %s (movement %llu s ago)\n", fstr(s, "zone"), fnum(s, "moving") ? "moving" :
			       "still", (unsigned long long)fnum(s, "last_motion_s_ago"));
		else
			printf("  zone      %-16s %s\n", fstr(s, "zone"), fnum(s, "moving") ? "moving" : "still");
	}
	if (*fstr(msg, "activity"))
		printf("  check-in  %s: %s\n", fstr(msg, "activity"), fstr(msg, "activity_text"));
	printf("  shared    %s\n", fnum(msg, "share_events") ? "coarse events with the ISP (PresenceChanged!, "
	       "UnusualInactivity!)" : "nothing: it all stays home");
}
#endif

#if AIGW_FEATURE_OFFERS
/* agctl offers: signals for the ISP, with the evidence (and here, the devices by name) */
static void show_offers(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *s;
	char none[160] = "", since[32];
	size_t rem, l = 0;
	int n = 0;

	(void)req;
	(void)type;
	if (json_out)
		return print_json(msg);
	if (!fnum(msg, "enabled")) {
		printf("Signals for the ISP are off (offers.enabled in /etc/config/aigw).\n");
		return;
	}
	blobmsg_for_each_attr(s, field(msg, "signals"), rem)
		n += fnum(s, "active") != 0;
	printf("Signals for the ISP, from the last 7 days (%llu watched): %d found%s\n",
	       (unsigned long long)fnum(msg, "days_watched"), n,
	       fnum(msg, "learning") ? "; still learning: the home's own signals need 3 days" : "");
	blobmsg_for_each_attr(s, field(msg, "signals"), rem) {
		if (!fnum(s, "active")) {
			if (l < sizeof(none))
				l += (size_t)snprintf(none + l, sizeof(none) - l, "%s%s", l ? ", " : "", fstr(s, "product"));
			continue;
		}
		clock_of(fnum(s, "since"), since, sizeof(since));
		printf("  %-20s %s (%s, %s confidence, %llu of %llu days)\n", fstr(s, "product"), fstr(s, "code"),
		       fstr(s, "kind"), fstr(s, "confidence"), (unsigned long long)fnum(s, "days"),
		       (unsigned long long)fnum(s, "of_days"));
		printf("  %-20s %s\n", "", fstr(s, "evidence"));
		if (*fstr(s, "devices"))
			printf("  %-20s devices: %s (stays home)\n", "", fstr(s, "devices"));
	}
	if (none[0])
		printf("  %-20s %s\n", "not found", none);
	printf("  %-20s %s\n", "the ISP", fnum(msg, "shared") ? "sees them as counts and words, without names (offers.share)" :
	       "doesn't see them (offers.share 0)");
}
#endif

/* ---------- calls ---------- */

static int call(const char *path, const char *method, struct blob_attr *args, ubus_data_handler_t cb)
{
	uint32_t id;
	int rc;

	if (ubus_lookup_id(ctx, path, &id)) {
		fprintf(stderr, "agctl: '%s' not found on ubus. Is agd running (and are you allowed to use it)?\n", path);
		return 1;
	}
	/* generous: some calls run tools (remote on reloads the firewall: seconds on a small router) */
	rc = ubus_invoke(ctx, id, method, args, cb, NULL, call_timeout);
	if (rc) {
		fprintf(stderr, "agctl: %s %s failed: %s\n", path, method, ubus_strerror(rc));
		return 1;
	}
	return 0;
}

static void watch_cb(struct ubus_context *c, struct ubus_event_handler *ev,
		     const char *type, struct blob_attr *msg)
{
	char tmp[16], tmp2[32], b1[16], b2[16];
	struct blob_attr *ans, *a;
	size_t rem;
	int k = 0;

	if (json_out) {
		printf("%s ", type);
		return print_json(msg);
	}
	if (!strcmp(type, AG_EV_DNS)) {
		const char *qtype = ag_dns_type_str((uint16_t)fnum(msg, "qtype"), tmp, sizeof(tmp));

		printf("dns   %-16s %-5s %-44s ->", fstr(msg, "client"), qtype, fstr(msg, "qname"));
		ans = field(msg, "answers");
		if (ans)
			blobmsg_for_each_attr(a, ans, rem) {
				if (k++ == 2)
					break;
				printf(" %s", blobmsg_get_string(a));
			}
		if (!k)
			printf(" %s", no_address_text(fnum(msg, "rcode"), qtype, tmp2, sizeof(tmp2)));
		printf("  (%.1f ms)\n", fnum(msg, "latency_us") / 1000.0);
	} else if (!strcmp(type, AG_EV_FLOW)) {
		const char *what = fstr(msg, "event");
		const char *dst = orb(fstr(msg, "domain"), fstr(msg, "dst"));

		if (!strcmp(what, "new"))
			printf("conn  %-16s %-5s %-44s port %llu%s\n", fstr(msg, "client"), fstr(msg, "proto"), dst,
			       (unsigned long long)fnum(msg, "dport"),
			       fnum(msg, "no_dns") ? "   ** no DNS lookup **" : "");
		else if (!strcmp(what, "named"))
			printf("name  %-16s %-5s %-44s port %llu (from its handshake)\n", fstr(msg, "client"),
			       fstr(msg, "proto"), dst, (unsigned long long)fnum(msg, "dport"));
		else
			printf("end   %-16s %-5s %-44s %s down, %s up, %llus\n", fstr(msg, "client"),
			       fstr(msg, "proto"), dst, human_bytes(fnum(msg, "bytes_down"), b1, sizeof(b1)),
			       human_bytes(fnum(msg, "bytes_up"), b2, sizeof(b2)),
			       (unsigned long long)fnum(msg, "duration_s"));
	} else if (!strcmp(type, AG_EV_ALERT)) {
		printf("ALERT [%s] %s\n", fstr(msg, "kind"), fstr(msg, "reason"));
	} else if (!strcmp(type, AG_EV_DECISION)) {
		char subj[256];

		decision_subject(msg, subj, sizeof(subj));
		printf("DECISION #%llu %s: %s %s", (unsigned long long)fnum(msg, "id"), fstr(msg, "state"),
		       action_words(fstr(msg, "action")), subj);
		if (!strcmp(fstr(msg, "state"), "proposed"))
			printf("  (agctl approve %llu)", (unsigned long long)fnum(msg, "id"));
		if (*fstr(msg, "note"))
			printf("  -- %s", fstr(msg, "note"));
		printf("\n");
	} else if (!strcmp(type, AG_EV_WIFI)) {
		const char *ev = fstr(msg, "event"), *who = orb(fstr(msg, "hostname"), fstr(msg, "mac"));

		if (!strcmp(ev, "joined-with-key")) {
			printf("wifi  %-16s joined with its own key \"%s\"%s%s\n", fstr(msg, "mac"), fstr(msg, "key"),
			       *fstr(msg, "zone") ? ", zone " : "", fstr(msg, "zone"));
		} else if (!strcmp(ev, "steer")) {
			printf("wifi  %-16s asked to move %s -> %s GHz (%s): %s\n", who, fstr(msg, "from"), fstr(msg, "to"),
			       !strcmp(fstr(msg, "method"), "btm") ? "802.11v" : "refused on its band", fstr(msg, "reason"));
		} else if (!strcmp(ev, "steered") || !strcmp(ev, "steer-missed")) {
			printf("wifi  %-16s %s\n", who, !strcmp(ev, "steered") ? "moved as asked" : "stayed where it was");
		} else if (!strcmp(ev, "steer-bounced")) {
			printf("wifi  %-16s back on 2.4 GHz soon after a move up: not moved up again for a while\n", who);
		} else {
			printf("wifi  %-16s %s on %s", who, ev, fstr(msg, "iface"));
			if (*fstr(msg, "band"))
				printf(" (%s GHz", fstr(msg, "band"));
			if (!strcmp(ev, "connected") || !strcmp(ev, "roamed"))
				printf("%s%d dBm", *fstr(msg, "band") ? ", " : " (", (int32_t)fnum(msg, "signal"));
			if (*fstr(msg, "band") || !strcmp(ev, "connected") || !strcmp(ev, "roamed"))
				printf(")");
			printf("\n");
		}
	} else if (!strcmp(type, AG_EV_PRESENCE)) {
		uint64_t s = fnum(msg, !strcmp(fstr(msg, "event"), "arrived") ? "away_s" : "home_s");

		printf("home  %-16s %s", orb(fstr(msg, "hostname"), fstr(msg, "mac")), fstr(msg, "event"));
		if (s)
			printf(" (%s %lluh%02llum)", !strcmp(fstr(msg, "event"), "arrived") ? "away" : "home",
			       (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60));
		printf("\n");
	} else if (!strcmp(type, AG_EV_PLUGIN)) {
		printf("PLUG-IN %s (%s): %s%s%s\n", fstr(msg, "plugin"), fstr(msg, "kind"), fstr(msg, "state"),
		       *fstr(msg, "why") ? " -- " : "", fstr(msg, "why"));
	} else if (!strcmp(type, AG_EV_UPGRADE)) {
		printf("UPGRADE %s -> %s: %s%s%s\n", fstr(msg, "from"), fstr(msg, "to"), fstr(msg, "event"),
		       *fstr(msg, "summary") ? " -- " : "", fstr(msg, "summary"));
	} else {
		char *body = blobmsg_format_json(msg, true);

		printf("%-5.5s %s  %s\n", type, type, body ? body : "");
		free(body);
	}
	fflush(stdout);
}

static int watch(const char *what)
{
	static struct ubus_event_handler ev;
	int all = !strcmp(what, "all"), rc = 0;

	uloop_init();
	ubus_add_uloop(ctx);
	ev.cb = watch_cb;
	if ((all || !strcmp(what, "dns")) && ubus_register_event_handler(ctx, &ev, AG_EV_DNS))
		rc = 1;
	if ((all || !strcmp(what, "flows")) && ubus_register_event_handler(ctx, &ev, AG_EV_FLOW))
		rc = 1;
	if ((all || !strcmp(what, "wifi")) && ubus_register_event_handler(ctx, &ev, AG_EV_WIFI))
		rc = 1;
	if ((all || !strcmp(what, "wifi") || !strcmp(what, "home")) &&
	    ubus_register_event_handler(ctx, &ev, AG_EV_PRESENCE))
		rc = 1;
	if ((all || !strcmp(what, "alerts")) && ubus_register_event_handler(ctx, &ev, AG_EV_ALERT))
		rc = 1;
	if ((all || !strcmp(what, "alerts") || !strcmp(what, "decisions")) &&
	    ubus_register_event_handler(ctx, &ev, AG_EV_DECISION))
		rc = 1;
	if ((all || !strcmp(what, "plugins")) &&
	    (ubus_register_event_handler(ctx, &ev, AG_EV_PLUGIN) || ubus_register_event_handler(ctx, &ev, "iot.*")))
		rc = 1;
	if ((all || !strcmp(what, "models")) && ubus_register_event_handler(ctx, &ev, AG_EV_MODEL))
		rc = 1;
	if ((all || !strcmp(what, "upgrade")) && ubus_register_event_handler(ctx, &ev, AG_EV_UPGRADE))
		rc = 1;
	if ((all || !strcmp(what, "repair")) && ubus_register_event_handler(ctx, &ev, AG_EV_REPAIR))
		rc = 1;
	if (rc) {
		fprintf(stderr, "agctl: cannot subscribe to events (permission?)\n");
	} else {
		fprintf(stderr, "watching %s (Ctrl+C to stop)\n", what);
		uloop_run();
	}
	uloop_done();
	return rc;
}

/* the commands of features (docs/features.md): a build without one says so */
static const struct { const char *cmd, *sub, *feature; int built; } features[] = {
	{ "care", "proactive", "PROACTIVE", AIGW_FEATURE_PROACTIVE },
	{ "care", NULL, "CARE", AIGW_FEATURE_CARE },
	{ "upgrade", NULL, "UPGRADE", AIGW_FEATURE_UPGRADE },
	{ "sqm", NULL, "SQM", AIGW_FEATURE_SQM },
	{ "calls", NULL, "CALLS", AIGW_FEATURE_CALLS },
	{ "channels", NULL, "WIFI_HEAL", AIGW_FEATURE_WIFI_HEAL },
	{ "channel", NULL, "WIFI_HEAL", AIGW_FEATURE_WIFI_HEAL },
	{ "wifi", "report", "WIFI_HEAL", AIGW_FEATURE_WIFI_HEAL },
	{ "wifi", "--report", "WIFI_HEAL", AIGW_FEATURE_WIFI_HEAL },
	{ "mesh", NULL, "MESH", AIGW_FEATURE_MESH },
	{ "coverage", NULL, "COVERAGE", AIGW_FEATURE_COVERAGE },
	{ "offers", NULL, "OFFERS", AIGW_FEATURE_OFFERS },
	{ "brief", NULL, "BRIEF", AIGW_FEATURE_BRIEF },
	{ "why", NULL, "WHY", AIGW_FEATURE_WHY },
	{ "test", NULL, "TR143", AIGW_FEATURE_TR143 },
	{ "plugins", NULL, "PLUGINS", AIGW_FEATURE_PLUGINS },
	{ "plugin", NULL, "PLUGINS", AIGW_FEATURE_PLUGINS },
	{ "bridge", NULL, "PLUGINS", AIGW_FEATURE_PLUGINS },
	{ "remote", NULL, "REMOTE", AIGW_FEATURE_REMOTE },
	{ "fed", NULL, "FED", AIGW_FEATURE_FED },
	{ "label", NULL, "FED", AIGW_FEATURE_FED },
	{ "fleet", NULL, "FLEETLEARN", AIGW_FEATURE_FLEETLEARN },
	{ "keys", NULL, "KEYS", AIGW_FEATURE_KEYS },
	{ "key", NULL, "KEYS", AIGW_FEATURE_KEYS },
	{ "home", NULL, "HOMEMODEL", AIGW_FEATURE_HOMEMODEL },
	{ "sense", NULL, "SENSING", AIGW_FEATURE_SENSING },
	{ "repair", NULL, "PROVEN", AIGW_FEATURE_PROVEN },
};

/* 1 (and says why) when cmd [sub] belongs to a feature this build does not have */
static int not_built(const char *cmd, const char *sub)
{
	size_t k;

	for (k = 0; k < sizeof(features) / sizeof(features[0]); k++) {
		if (strcmp(features[k].cmd, cmd) || (features[k].sub && (!sub || strcmp(features[k].sub, sub))))
			continue;
		if (features[k].built)
			return 0;
		fprintf(stderr, "agctl %s%s%s: not built in (AIGW_FEATURE_%s)\n", cmd, features[k].sub ? " " : "",
			features[k].sub ? features[k].sub : "", features[k].feature);
		return 1;
	}
	return 0;
}

/* agctl is a link to agd: agd's main() runs this when it is called as "agctl" (agd/main.c) */
int agctl_main(int argc, char **argv);

int agctl_main(int argc, char **argv)
{
	struct blob_buf args = { 0 };
	const char *cmd;
	int rc = 0, i = 1;

	if (i < argc && !strcmp(argv[i], "-j")) {
		json_out = 1;
		i++;
	}
	if (i >= argc) {
		usage();
		return 1;
	}
	cmd = argv[i++];
	if (!strcmp(cmd, "version")) {
		printf("agctl %s\nfeatures: %s\nprofile: %s (flash budget %d KB)\n", AG_VERSION,
		       *AG_FEATURES ? AG_FEATURES : "the core only", AG_PROFILE, AG_FLASH_BUDGET);
		return 0;
	}
	if (not_built(cmd, i < argc ? argv[i] : NULL))
		return 1;
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}
	if (!strcmp(cmd, "qr") && i < argc) {      /* no brain needed */
		qr_pbm = i + 1 < argc && !strcmp(argv[i + 1], "--pbm");
		print_qr(argv[i]);
		return 0;
	}
	if (!strcmp(cmd, "selftest")) {             /* on this CPU: crypto, model, QR, DNS through agdns */
		int agctl_selftest(void);

		return agctl_selftest() ? 1 : 0;
	}
	if (!strcmp(cmd, "trust") && i < argc)     /* files only: agd reads the keys at each offer */
		return trust(argv[i]);
	if (!strcmp(cmd, "untrust") && i < argc)
		return untrust(argv[i]);

	ctx = ubus_connect(ag_ubus_path());
	if (!ctx) {
		fprintf(stderr, "agctl: cannot connect to ubus. Is ubusd running?\n");
		return 1;
	}
	if (!json_out && ag_demo())
		printf("DEMO  a simulated home (the AIGW OS demo image): not real data\n");

	if (!strcmp(cmd, "status")) {
		rc = call("ag", "status", NULL, show_status);
	} else if (!strcmp(cmd, "devices")) {
		rc = call("ag.device", "list", NULL, show_devices);
	} else if (!strcmp(cmd, "device") && i < argc) {
		device_filter = argv[i];
		rc = call("ag.device", "list", NULL, show_device);
	} else if (!strcmp(cmd, "score") && i < argc) {
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "name", argv[i]);
		rc = call("ag.domain", "score", args.head, show_score);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "alerts")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 20);
		rc = call("ag.alert", "list", args.head, show_alerts);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "diagnose")) {
		blob_buf_init(&args, 0);
		if (i < argc)
			blobmsg_add_string(&args, "host", argv[i]);
		rc = call("ag", "diagnose", args.head, show_diag);
		blob_buf_free(&args);
		rc = rc || diag_failed;
#if AIGW_FEATURE_CARE
	} else if (!strcmp(cmd, "care")) {
		blob_buf_init(&args, 0);
		if (i < argc && (!strcmp(argv[i], "--check") || !strcmp(argv[i], "check"))) {
			blobmsg_add_u8(&args, "check", 1);
			care_asked_check = 1;
		} else if (i < argc && (!strcmp(argv[i], "--speed") || !strcmp(argv[i], "speed"))) {
			blobmsg_add_u8(&args, "speed", 1);
			if (i + 1 < argc && !strncmp(argv[i + 1], "http", 4))
				blobmsg_add_string(&args, "speed_url", argv[i + 1]);   /* a server of one's own */
			care_asked_check = 2;
		}
#if AIGW_FEATURE_PROACTIVE
		if (i < argc && !strcmp(argv[i], "proactive")) {
			if (i + 1 < argc && (!strcmp(argv[i + 1], "--run") || !strcmp(argv[i + 1], "run")))
				blobmsg_add_u8(&args, "run", 1);
			rc = call("ag", "proactive", args.head, show_proactive);
		} else
#endif
			rc = call("ag", "care", args.head, show_care);
		blob_buf_free(&args);
		rc = rc || diag_failed;
#endif
#if AIGW_FEATURE_PROVEN
	} else if (!strcmp(cmd, "repair")) {
		blob_buf_init(&args, 0);
		if (i < argc && (!strcmp(argv[i], "--stats") || !strcmp(argv[i], "stats")))
			blobmsg_add_u8(&args, "stats", 1);
		else if (i < argc && (!strcmp(argv[i], "--shadow") || !strcmp(argv[i], "shadow")))
			blobmsg_add_u8(&args, "shadow", 1);
		else if (i < argc) {
			usage();
			blob_buf_free(&args);
			return 1;
		}
		rc = call("ag", "repair", args.head, show_repair);
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_UPGRADE
	} else if (!strcmp(cmd, "upgrade")) {
		blob_buf_init(&args, 0);
		if (i < argc && (!strcmp(argv[i], "--forget") || !strcmp(argv[i], "forget")))
			blobmsg_add_u8(&args, "forget", 1);
		rc = call("ag", "upgrade", args.head, show_upgrade);
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_SQM
	} else if (!strcmp(cmd, "sqm")) {
		blob_buf_init(&args, 0);
		if (i < argc && (!strcmp(argv[i], "--on") || !strcmp(argv[i], "on")))
			blobmsg_add_u8(&args, "on", 1);
		else if (i < argc && (!strcmp(argv[i], "--off") || !strcmp(argv[i], "off")))
			blobmsg_add_u8(&args, "on", 0);
		rc = call("ag", "sqm", args.head, show_sqm);
		blob_buf_free(&args);
		rc = rc || sqm_failed;
#endif
#if AIGW_FEATURE_CALLS
	} else if (!strcmp(cmd, "calls")) {
		rc = call("ag", "calls", NULL, show_calls);
#endif
#if AIGW_FEATURE_WIFI_HEAL
	} else if (!strcmp(cmd, "channels")) {
		rc = call("ag", "channels", NULL, show_channels);
#endif
#if AIGW_FEATURE_MESH
	} else if (!strcmp(cmd, "mesh") && i + 2 < argc && (!strcmp(argv[i], "steer") || !strcmp(argv[i], "channel"))) {
		char target[128];
		int ttl = i + 3 < argc ? parse_time(argv[i + 3]) : 0;

		snprintf(target, sizeof(target), "%s%s%s", argv[i + 1], !strcmp(argv[i], "steer") ? "@" : ":", argv[i + 2]);
		if (i + 3 < argc && ttl <= 0) {
			fprintf(stderr, "agctl: usage: agctl mesh steer <device> <BSSID> [time] | mesh channel <radio> <ch> [time]\n");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", !strcmp(argv[i], "steer") ? "meshsteer" : "meshchan");
			blobmsg_add_string(&args, "target", target);
			if (ttl > 0)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			rc = call("ag.decision", "request", args.head, show_change) || change_failed;
			blob_buf_free(&args);
		}
	} else if (!strcmp(cmd, "mesh")) {
		blob_buf_init(&args, 0);
		if (i < argc && (!strcmp(argv[i], "--run") || !strcmp(argv[i], "run")))
			blobmsg_add_u8(&args, "run", 1);
		rc = call("ag", "mesh", args.head, show_mesh);
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_COVERAGE
	} else if (!strcmp(cmd, "coverage")) {
		rc = call("ag", "coverage", NULL, show_coverage);
#endif
#if AIGW_FEATURE_HOMEMODEL
	} else if (!strcmp(cmd, "home")) {
		blob_buf_init(&args, 0);
		if (i < argc && !strcmp(argv[i], "name")) {
			if (i + 2 >= argc) {
				fprintf(stderr, "agctl home name <device|place number> <zone> (\"\" clears)\n");
				return 1;
			}
			blobmsg_add_string(&args, "name", argv[i + 1]);
			blobmsg_add_string(&args, "zone", argv[i + 2]);
		}
		rc = call("ag", "home", args.head, show_home);
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_SENSING
	} else if (!strcmp(cmd, "sense")) {
		blob_buf_init(&args, 0);
		if (i < argc && !strcmp(argv[i], "on")) {
			char consent[96];
			time_t now = time(NULL);
			struct tm tm;

			if (i + 1 >= argc || strcmp(argv[i + 1], "--i-agree")) {
				printf("Wi-Fi sensing tells from the Wi-Fi signals whether someone is home and moving, by zone,\n"
				       "and (with the check-in) whether today's activity is as usual. It stays on this\n"
				       "gateway; the ISP gets nothing unless sensing.share_events is set, and then only\n"
				       "'home / away' and 'unusual inactivity'. Everyone who lives here should agree.\n"
				       "To turn it on: agctl sense on --i-agree   (off, and forget it all: agctl sense off)\n");
				blob_buf_free(&args);
				return 1;
			}
			gmtime_r(&now, &tm);
			strftime(consent, sizeof(consent), "agreed with agctl on %Y-%m-%d %H:%M UTC", &tm);
			blobmsg_add_u8(&args, "on", 1);
			blobmsg_add_string(&args, "consent", consent);
		} else if (i < argc && !strcmp(argv[i], "off"))
			blobmsg_add_u8(&args, "on", 0);
		sense_what = i < argc && !strcmp(argv[i], "what");
		rc = call("ag", "sense", args.head, show_sense);
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_OFFERS
	} else if (!strcmp(cmd, "offers")) {
		rc = call("ag", "offers", NULL, show_offers);
#endif
#if AIGW_FEATURE_BRIEF
	} else if (!strcmp(cmd, "brief")) {
		char period[96] = "";
		const char *host = NULL;
		int check = 1, speed = 0, propose = 0;
		size_t pl = 0;

		for (; i < argc; i++) {
			const char *o = argv[i];

			if (!strcmp(o, "--no-check"))
				check = 0;
			else if (!strcmp(o, "--speed"))
				speed = 1;
			else if (!strcmp(o, "--propose"))
				propose = 1;
			else if ((!strcmp(o, "--since") || !strcmp(o, "--period")) && i + 1 < argc) {
				pl = (size_t)snprintf(period, sizeof(period), "%s%s", !strcmp(o, "--since") ? "since " : "", argv[++i]);
			} else if (o[0] == '-') {
				usage();
				return 1;
			} else if (!host && !pl && !strchr(o, ':') && strcmp(o, "now") && strcmp(o, "tonight") && strcmp(o, "yesterday") &&
				   strcmp(o, "last") && strcmp(o, "since") && strcmp(o, "today") && !isdigit((unsigned char)o[0]) &&
				   strncmp(o, "last ", 5))
				host = o;
			else if (pl < sizeof(period))       /* the period's words, in any number of arguments */
				pl += (size_t)snprintf(period + pl, sizeof(period) - pl, "%s%s", pl ? " " : "", o);
		}
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "period", period);
		if (host)
			blobmsg_add_string(&args, "host", host);
		blobmsg_add_u8(&args, "check", (uint8_t)check);
		if (speed)
			blobmsg_add_u8(&args, "speed", 1);
		if (propose)
			blobmsg_add_u8(&args, "propose", 1);
		call_timeout = 150 * 1000;              /* a fresh check, and the speed test */
		rc = call("ag", "brief", args.head, show_brief);
		blob_buf_free(&args);
		rc = rc || brief_failed;
#endif
#if AIGW_FEATURE_WHY
	} else if (!strcmp(cmd, "why")) {
		long from = 0, to = 0, at = 0, last = 0, now = (long)time(NULL);
		const char *host = NULL;
		int raw = 0;

		for (; i < argc; i++) {
			const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

			if (!strcmp(o, "--raw")) {
				raw = 1;
				continue;
			}
			if (!strcmp(o, "--at") || !strcmp(o, "--from") || !strcmp(o, "--to") || !strcmp(o, "--last")) {
				long t = !v ? -1 : !strcmp(o, "--last") ? parse_time(v) : parse_clock(v);

				if (t < 0) {
					fprintf(stderr, "agctl why: %s %s: a time (20:00, \"yesterday 20:00\") or, for --last, "
						"a length (30m, 2h)\n", o, v ? v : "");
					return 1;
				}
				*(!strcmp(o, "--at") ? &at : !strcmp(o, "--from") ? &from : !strcmp(o, "--to") ? &to : &last) = t;
				i++;
				continue;
			}
			if (o[0] == '-' || host) {
				usage();
				return 1;
			}
			host = o;
		}
		if (at) {                       /* half an hour either side */
			from = at - 1800;
			to = at + 1800 > now ? now : at + 1800;
		} else if (last) {
			to = now;
			from = now - last;
		} else {
			if (!to)
				to = from && from + 3600 < now ? from + 3600 : now;
			if (!from)
				from = to - 3600;
		}
		blob_buf_init(&args, 0);
		blobmsg_add_u64(&args, "from", (uint64_t)from);
		blobmsg_add_u64(&args, "to", (uint64_t)to);
		if (host)
			blobmsg_add_string(&args, "host", host);
		if (raw)
			blobmsg_add_u8(&args, "raw", 1);
		rc = call("ag", "why", args.head, show_why);
		blob_buf_free(&args);
		rc = rc || why_failed;
#endif
#if AIGW_FEATURE_TR143
	} else if (!strcmp(cmd, "test") && i + 1 < argc) {
		static const struct { const char *what, *cmd, *arg, *extra; } T[] = {
			{ "ping", "Device.IP.Diagnostics.IPPing()", "Host", "NumberOfRepetitions" },
			{ "trace", "Device.IP.Diagnostics.TraceRoute()", "Host", NULL },
			{ "download", "Device.IP.Diagnostics.DownloadDiagnostics()", "DownloadURL", "TimeBasedTestDuration" },
			{ "upload", "Device.IP.Diagnostics.UploadDiagnostics()", "UploadURL", "TestFileLength" },
			{ "dns", "Device.DNS.Diagnostics.NSLookupDiagnostics()", "HostName", "DNSServer" },
		};
		const char *what = argv[i++];
		unsigned k;
		void *t;

		for (k = 0; k < sizeof(T) / sizeof(T[0]) && strcmp(T[k].what, what); k++)
			;
		if (k == sizeof(T) / sizeof(T[0]) || (i + 1 < argc && !T[k].extra) || i + 2 < argc) {
			usage();
			return 1;
		}
		test_kind = (int)k;
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "command", T[k].cmd);
		t = blobmsg_open_table(&args, "args");
		blobmsg_add_string(&args, T[k].arg, argv[i++]);
		if (i < argc) {
			size_t l = strlen(argv[i]);

			/* upload <url> 10s: time based */
			if (k == 3 && l > 1 && argv[i][l - 1] == 's') {
				char sec[16];

				snprintf(sec, sizeof(sec), "%.*s", (int)(l - 1), argv[i]);
				blobmsg_add_string(&args, "TimeBasedTestDuration", sec);
			} else
				blobmsg_add_string(&args, T[k].extra, argv[i]);
		}
		blobmsg_close_table(&args, t);
		call_timeout = 20 * 60 * 1000;
		if (!json_out)
			fprintf(stderr, "running %s (this can take a while)...\n", T[k].cmd);
		rc = call("ag.dm", "operate", args.head, show_test) || diag_failed;
		blob_buf_free(&args);
#endif
	} else if (!strcmp(cmd, "wifi")) {
#if AIGW_FEATURE_WIFI_HEAL
		rc = call("ag.wifi", "radios", NULL,
			  i < argc && (!strcmp(argv[i], "--report") || !strcmp(argv[i], "report")) ? show_report : show_radios);
#else
		rc = call("ag.wifi", "radios", NULL, show_radios);
#endif
	} else if (!strcmp(cmd, "dns") && i < argc && !strcmp(argv[i], "cache")) {
		rc = call("ag.dnscache", "stats", NULL, show_dnscache);
	} else if (!strcmp(cmd, "dns") || !strcmp(cmd, "flows")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 30);
		if (!strcmp(cmd, "dns"))
			rc = call("ag.dns", "log", args.head, show_dns);
		else
			rc = call("ag.flow", "recent", args.head, show_flows);
		blob_buf_free(&args);
	} else if (!strcmp(cmd, "watch")) {
		rc = watch(i < argc ? argv[i] : "dns");
	} else if (!strcmp(cmd, "decisions")) {
		blob_buf_init(&args, 0);
		blobmsg_add_u32(&args, "limit", i < argc ? (uint32_t)atoi(argv[i]) : 20);
		rc = call("ag.decision", "list", args.head, show_decisions);
		blob_buf_free(&args);
#if AIGW_FEATURE_WIFI_HEAL
	} else if (!strcmp(cmd, "channel") && i + 1 < argc) {
		char target[64];
		int ttl = i + 2 < argc ? parse_time(argv[i + 2]) : 0;

		snprintf(target, sizeof(target), "%s:%s", argv[i], argv[i + 1]);
		if (i + 2 < argc && ttl <= 0) {
			fprintf(stderr, "agctl: usage: agctl channel <access point|2.4|5> <channel> [time]\n");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", "channel");
			blobmsg_add_string(&args, "target", target);
			if (ttl > 0)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			rc = call("ag.decision", "request", args.head, show_change) || change_failed;
			blob_buf_free(&args);
		}
#endif
	} else if ((!strcmp(cmd, "quarantine") || !strcmp(cmd, "prioritize") || !strcmp(cmd, "block") ||
		    !strcmp(cmd, "deprioritize") || !strcmp(cmd, "throttle") || !strcmp(cmd, "disconnect") ||
		    (!strcmp(cmd, "do") && i + 1 < argc)) && i < argc) {
		const char *action = !strcmp(cmd, "do") ? argv[i++] : !strcmp(cmd, "prioritize") ? "priority" : cmd;
		const char *target = argv[i++], *device = NULL, *bad = NULL;
		int ttl = 0, rate = 0;

		/* optional, in this order: [rate] (throttle) [time] [device] (block) */
		if (!strcmp(cmd, "throttle") && i < argc && parse_rate(argv[i]) > 0 && parse_time(argv[i]) < 0)
			rate = parse_rate(argv[i++]);
		if (i < argc && parse_time(argv[i]) > 0)
			ttl = parse_time(argv[i++]);
		if (!strcmp(cmd, "block") && i < argc)
			device = argv[i++];
		if (i < argc)
			bad = argv[i];
		if (bad) {
			fprintf(stderr, "agctl: unexpected '%s' (time: 90s, 30m, 2h, 1d%s)\n", bad,
				!strcmp(cmd, "throttle") ? "; rate: 2mbit, 500kbit" : "");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "action", action);
			blobmsg_add_string(&args, "target", target);
			if (ttl)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			if (rate)
				blobmsg_add_u32(&args, "rate", (uint32_t)rate);
			if (device)
				blobmsg_add_string(&args, "device", device);
			rc = call("ag.decision", "request", args.head, show_change) || change_failed;
			blob_buf_free(&args);
		}
	} else if ((!strcmp(cmd, "approve") || !strcmp(cmd, "reject") || !strcmp(cmd, "undo")) && i < argc) {
		int ttl = (!strcmp(cmd, "approve") && i + 1 < argc) ? parse_time(argv[i + 1]) : 0;

		if (ttl < 0 || atoi(argv[i]) <= 0) {
			fprintf(stderr, "agctl: usage: agctl %s <id>%s\n", cmd, !strcmp(cmd, "approve") ? " [time]" : "");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_u32(&args, "id", (uint32_t)atoi(argv[i]));
			if (ttl)
				blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
			rc = call("ag.decision", !strcmp(cmd, "undo") ? "revert" : cmd, args.head, show_change) ||
			     change_failed;
			blob_buf_free(&args);
		}
#if AIGW_FEATURE_PLUGINS
	} else if (!strcmp(cmd, "plugins")) {
		rc = call("ag.plugin", "list", NULL, show_plugins);
	} else if (!strcmp(cmd, "plugin") && i < argc) {
		const char *sub = argv[i++];

		if (!strcmp(sub, "add") && i < argc) {
			rc = plugin_add(argv[i]);
		} else if (!strcmp(sub, "remove") && i < argc) {
			rc = plugin_remove(argv[i]);
		} else if ((!strcmp(sub, "allow") || !strcmp(sub, "deny") || !strcmp(sub, "enable") ||
			    !strcmp(sub, "disable")) && i < argc) {
			int allow = !strcmp(sub, "allow") || !strcmp(sub, "deny");

			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", argv[i]);
			blobmsg_add_u8(&args, allow ? "allow" : "enabled", !strcmp(sub, "allow") || !strcmp(sub, "enable"));
			rc = call("ag.plugin", "set", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		} else if (!strcmp(sub, "config") && i + 2 < argc) {
			rc = plugin_config(argv[i], argv[i + 1], argv[i + 2]);
		} else if (i == argc || !strcmp(sub, "show")) {
			device_filter = i < argc ? argv[i] : sub;
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", device_filter);
			rc = call("ag.plugin", "list", args.head, show_plugins);
			blob_buf_free(&args);
		} else {
			usage();
			rc = 1;
		}
#endif
	} else if (!strcmp(cmd, "models")) {
		rc = call("ag.model", "list", NULL, show_models);
	} else if (!strcmp(cmd, "model") && i < argc) {
		const char *sub = argv[i++];

		blob_buf_init(&args, 0);
		if (!strcmp(sub, "install") && i < argc) {
			char file[PATH_MAX], sig[PATH_MAX + 8] = "";
			int force = 0;

			if (!realpath(argv[i++], file)) {
				fprintf(stderr, "agctl: %s: %s\n", argv[i - 1], strerror(errno));
				rc = 1;
			}
			for (; i < argc && !rc; i++) {
				if (!strcmp(argv[i], "--force"))
					force = 1;
				else if (!realpath(argv[i], sig)) {
					fprintf(stderr, "agctl: %s: %s\n", argv[i], strerror(errno));
					rc = 1;
				}
			}
			if (!sig[0]) {
				snprintf(sig, sizeof(sig), "%s.sig", file);
				if (access(sig, F_OK))
					sig[0] = '\0';
			}
			if (!rc) {
				blobmsg_add_string(&args, "file", file);
				if (sig[0])
					blobmsg_add_string(&args, "sig", sig);
				blobmsg_add_u8(&args, "force", (uint8_t)force);
				rc = call("ag.model", "install", args.head, show_model_change) || change_failed;
			}
		} else if (!strcmp(sub, "rollback")) {
			if (i < argc)
				blobmsg_add_string(&args, "name", argv[i]);
			rc = call("ag.model", "rollback", args.head, show_model_change) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
#if AIGW_FEATURE_PLUGINS
	} else if (!strcmp(cmd, "bridge")) {
		if (i == argc) {
			device_filter = "bridge";
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", "bridge");
			rc = call("ag.plugin", "list", args.head, show_plugins);
			blob_buf_free(&args);
		} else if (!strcmp(argv[i], "token")) {
			rc = plugin_config("bridge", "token", "-");
		} else {
			rc = plugin_config("bridge", "url", argv[i]);
		}
#endif
#if AIGW_FEATURE_REMOTE
	} else if (!strcmp(cmd, "remote")) {
		const char *sub = i < argc ? argv[i++] : "";

		blob_buf_init(&args, 0);
		if (!*sub) {
			rc = call("ag.remote", "list", NULL, show_remote);
		} else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
			if (i < argc)
				blobmsg_add_string(&args, "endpoint", argv[i]);
			rc = call("ag.remote", sub, args.head, show_ok) || change_failed;
		} else if ((!strcmp(sub, "add") || !strcmp(sub, "revoke")) && i < argc) {
			blobmsg_add_string(&args, "name", argv[i++]);
			remote_conf = i < argc && !strcmp(argv[i], "--conf");
			rc = call("ag.remote", sub, args.head, !strcmp(sub, "add") ? show_remote_added : show_ok) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
#endif
#if AIGW_FEATURE_FED
	} else if (!strcmp(cmd, "fed") && (i == argc || !strcmp(argv[i], "forget"))) {
		if (i == argc) {
			rc = call("ag.fed", "status", NULL, show_fed);
		} else {
			blob_buf_init(&args, 0);
			if (i + 1 < argc)
				blobmsg_add_string(&args, "name", argv[i + 1]);
			rc = call("ag.fed", "forget", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		}
	} else if (!strcmp(cmd, "label") && i + 1 < argc) {
		if (strcmp(argv[i + 1], "safe") && strcmp(argv[i + 1], "threat")) {
			fprintf(stderr, "agctl: usage: agctl label <domain> safe|threat\n");
			rc = 1;
		} else {
			blob_buf_init(&args, 0);
			blobmsg_add_string(&args, "name", argv[i]);
			blobmsg_add_string(&args, "label", argv[i + 1]);
			rc = call("ag.fed", "label", args.head, show_ok) || change_failed;
			blob_buf_free(&args);
		}
#endif
#if AIGW_FEATURE_FLEETLEARN
	} else if (!strcmp(cmd, "fleet") && i == argc) {
		rc = call("ag.fleet", "status", NULL, show_fleet);
	} else if (!strcmp(cmd, "fleet") && !strcmp(argv[i], "rank") && i + 1 < argc) {
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "code", argv[i + 1]);
		if (i + 2 < argc)
			blobmsg_add_string(&args, "device", argv[i + 2]);
		rc = call("ag.fleet", "rank", args.head, show_fleet_rank);
		blob_buf_free(&args);
#endif
	} else if (!strcmp(cmd, "dm") && i < argc) {
		const char *sub = argv[i++];
		void *t;

		blob_buf_init(&args, 0);
		if (!strcmp(sub, "get") && i < argc) {
			t = blobmsg_open_array(&args, "paths");
			for (; i < argc; i++)
				blobmsg_add_string(&args, NULL, argv[i]);
			blobmsg_close_array(&args, t);
			rc = call("ag.dm", "get", args.head, show_dm_get) || change_failed;
		} else if (!strcmp(sub, "set") && i + 1 < argc && (argc - i) % 2 == 0) {
			t = blobmsg_open_table(&args, "params");
			for (; i + 1 < argc; i += 2)
				blobmsg_add_string(&args, argv[i], argv[i + 1]);
			blobmsg_close_table(&args, t);
			rc = call("ag.dm", "set", args.head, show_dm_result) || change_failed;
		} else if (!strcmp(sub, "do") && i < argc) {
			blobmsg_add_string(&args, "command", argv[i++]);
			t = blobmsg_open_table(&args, "args");
			for (; i < argc; i++) {
				char *eq = strchr(argv[i], '=');

				if (!eq) {
					fprintf(stderr, "agctl: command arguments are Name=value\n");
					return 1;
				}
				*eq = '\0';
				blobmsg_add_string(&args, argv[i], eq + 1);
			}
			blobmsg_close_table(&args, t);
			rc = call("ag.dm", "operate", args.head, show_dm_result) || change_failed;
		} else if (!strcmp(sub, "schema") && i == argc) {
			rc = call("ag.dm", "schema", NULL, show_dm_schema);
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
#if AIGW_FEATURE_KEYS
	} else if (!strcmp(cmd, "keys")) {
		rc = call("ag.wifi", "keys", NULL, show_keys);
	} else if (!strcmp(cmd, "key") && i + 1 < argc) {
		const char *sub = argv[i++], *name = argv[i++];

		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "name", name);
		if (!strcmp(sub, "add")) {
			const char *zone = NULL;
			int ttl = 0;

			if (i < argc && parse_time(argv[i]) < 0)
				zone = argv[i++];
			if (i < argc && (ttl = parse_time(argv[i])) > 0)
				i++;
			if (i < argc || ttl < 0) {
				fprintf(stderr, "agctl: usage: agctl key add <name> [zone] [time]\n");
				rc = 1;
			} else {
				if (zone && strcmp(zone, "none"))
					blobmsg_add_string(&args, "zone", zone);
				if (ttl > 0)
					blobmsg_add_u32(&args, "ttl", (uint32_t)ttl);
				rc = call("ag.wifi", "key_add", args.head, show_key_added) || change_failed;
			}
		} else if (!strcmp(sub, "qr")) {
			device_filter = name;
			rc = call("ag.wifi", "key_show", args.head, show_keys);
		} else if (!strcmp(sub, "revoke")) {
			rc = call("ag.wifi", "key_revoke", args.head, show_ok) || change_failed;
		} else {
			usage();
			rc = 1;
		}
		blob_buf_free(&args);
#endif
	} else if (!strcmp(cmd, "zone") && i + 1 < argc) {
		blob_buf_init(&args, 0);
		blobmsg_add_string(&args, "device", argv[i]);
		blobmsg_add_string(&args, "zone", strcmp(argv[i + 1], "none") ? argv[i + 1] : "");
		rc = call("ag.device", "zone", args.head, show_ok) || change_failed;
		blob_buf_free(&args);
	} else {
		usage();
		rc = 1;
	}
	ubus_free(ctx);
	return rc;
}
