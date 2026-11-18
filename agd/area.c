/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Area outages, on the gateway (cortex finds them: cortex/aigw_cortex/outage.py;
 * docs/area-outages.md).
 *
 *   cortex   many homes behind this one's uplink went down, slow or lossy in the
 *            same few minutes: the ISP's network, not the homes
 *     │      the answer to the bridge's report carries "area": {incident, kind,
 *     ▼      since, homes, of, confidence, uplink, hold_s}; incident 0: none
 *   bridge   hands it to agd: {"area": {...}} (capability area.notice)
 *     │
 *     ▼
 *   agd      Device.X_AIGW_.AreaOutage. and the events AreaOutage! and
 *            AreaOutageCleared! for the ISP's ACS; the help-desk brief says
 *            "known outage in your area"; and AIGW holds its own fixes while it
 *            lasts: no smart-queueing trial started or judged, no band steering,
 *            no channel change, no device kept on 2.4 GHz (security actions are
 *            never held). It fixes nothing that isn't broken.
 *
 * The notice lapses by itself hold_s after the last one (cortex renews it with
 * every report), so a cortex that went away never holds fixes for long.
 *
 * What cortex groups homes by: the operator's id (area.uplink_id, written by the
 * ACS as Device.X_AIGW_.AreaOutage.UplinkID), else the WAN's next hop, else (a
 * point-to-point WAN: PPPoE) its peer, the BNG. The bridge sends all three.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "agd.h"

#define HOLD_MIN 60
#define HOLD_MAX 21600

static const char *const held_names[AH_N] = {
	"SmartQueueing", "SmartQueueingTrial", "BandSteering", "ChannelChange", "Stay24",
};
static const char *const held_words[AH_N] = {
	"turning on smart queueing", "judging a smart-queueing trial", "band steering", "a Wi-Fi channel change",
	"keeping a device on 2.4 GHz",
};

static struct {
	int active;
	uint32_t incident;
	char kind[8], confidence[16], uplink[64], who[48];
	long since, started, until;
	int homes, of;
	unsigned held;
	/* the last one, for the data model after it is over */
	uint32_t last_incident;
	long last_end;
} A;
static struct uloop_timeout lapse;

static long now_s(void)
{
	return (long)ag_wall_s();
}

static const char *kind_ok(const char *k)
{
	static const char *const kinds[] = { "DOWN", "LOSS", "SLOW", "DNS", NULL };
	int i;

	for (i = 0; k && kinds[i]; i++)
		if (!strcmp(k, kinds[i]))
			return kinds[i];
	return NULL;
}

/* printable, no quotes or separators that could confuse a log or a CSV list */
static void clean(char *dst, size_t n, const char *src)
{
	size_t i, k = 0;

	for (i = 0; src && src[i] && k + 1 < n; i++)
		if (isprint((unsigned char)src[i]) && !strchr("\"'\\,;", src[i]))
			dst[k++] = src[i];
	dst[k] = '\0';
}

void area_held_words(char *buf, size_t n)
{
	size_t len = 0;
	int i;

	buf[0] = '\0';
	for (i = 0; i < AH_N; i++)
		if (A.held & (1u << i))
			len += (size_t)snprintf(buf + len, len < n ? n - len : 0, "%s%s", len ? ", " : "", held_words[i]);
}

static void held_names_list(char *buf, size_t n)
{
	size_t len = 0;
	int i;

	buf[0] = '\0';
	for (i = 0; i < AH_N; i++)
		if (A.held & (1u << i))
			len += (size_t)snprintf(buf + len, len < n ? n - len : 0, "%s%s", len ? "," : "", held_names[i]);
}

static void clear(const char *why)
{
	char held[200];

	if (!A.active)
		return;
	area_held_words(held, sizeof(held));
	AG_INFO("area outage %u (%s on %s) over for this home: %s; %s%s", A.incident, A.kind, A.uplink, why,
		held[0] ? "AIGW's own fixes go on again, after holding " : "nothing was held", held);
	held_names_list(held, sizeof(held));
	dm_on_area(0, A.incident, A.kind, A.since, now_s() - A.started, A.homes, A.of, A.confidence, A.uplink, held, why);
	A.last_incident = A.incident;
	A.last_end = now_s();
	A.active = 0;
	uloop_timeout_cancel(&lapse);
}

static void lapse_cb(struct uloop_timeout *t)
{
	char why[96];

	(void)t;
	if (!A.active)
		return;
	if (now_s() < A.until) {
		uloop_timeout_set(&lapse, (int)(A.until - now_s()) * 1000 + 500);
		return;
	}
	snprintf(why, sizeof(why), "no word from cortex since (it lapsed)");
	clear(why);
}

int area_notice(struct blob_attr *a, const char *who, char *err, size_t en)
{
	struct blob_attr *cur;
	size_t rem;
	const char *kind = NULL, *conf = "", *uplink = "";
	long incident = -1, since = 0, hold = 1800, homes = 0, of = 0, now = now_s();
	int fresh = 0;

	blobmsg_for_each_attr(cur, a, rem) {
		const char *k = blobmsg_name(cur);
		long v = 0;
		int is_num = 1;

		switch (blobmsg_type(cur)) {
		case BLOBMSG_TYPE_INT64: v = (long)blobmsg_get_u64(cur); break;
		case BLOBMSG_TYPE_INT32: v = (long)(int32_t)blobmsg_get_u32(cur); break;
		case BLOBMSG_TYPE_INT16: v = (long)blobmsg_get_u16(cur); break;
		case BLOBMSG_TYPE_INT8: v = (long)blobmsg_get_u8(cur); break;
		case BLOBMSG_TYPE_DOUBLE: v = (long)blobmsg_get_double(cur); break;
		default: is_num = 0;
		}
		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			if (!strcmp(k, "kind"))
				kind = blobmsg_get_string(cur);
			else if (!strcmp(k, "confidence"))
				conf = blobmsg_get_string(cur);
			else if (!strcmp(k, "uplink"))
				uplink = blobmsg_get_string(cur);
		} else if (is_num) {
			if (!strcmp(k, "incident"))
				incident = v;
			else if (!strcmp(k, "since"))
				since = v;
			else if (!strcmp(k, "hold_s"))
				hold = v;
			else if (!strcmp(k, "homes"))
				homes = v;
			else if (!strcmp(k, "of"))
				of = v;
		}
	}
	if (incident < 0 || incident > 0x7fffffffL) {
		snprintf(err, en, "an area notice needs its incident (0: none)");
		return -1;
	}
	if (incident == 0) {
		if (A.active)
			clear("cortex says it is over");
		return 0;
	}
	if (!cfg.area) {
		snprintf(err, en, "area outages are not taken here (area.enabled 0)");
		return -1;
	}
	if (!(kind = kind_ok(kind))) {
		snprintf(err, en, "an area outage's kind is DOWN, LOSS, SLOW or DNS");
		return -1;
	}
	{
		/* tests only: a shorter least hold, to see a notice lapse without waiting a minute */
		const char *m = getenv("AIGW_TEST_AREA_HOLD_MIN");
		long least = m && atol(m) > 0 && atol(m) < HOLD_MIN ? atol(m) : HOLD_MIN;

		if (hold < least)
			hold = least;
	}
	if (hold > HOLD_MAX)
		hold = HOLD_MAX;
	if (since <= 0 || since > now + 300)
		since = now;
	if (A.active && A.incident != (uint32_t)incident)
		clear("another one took its place");
	if (!A.active) {
		fresh = 1;
		A.active = 1;
		A.incident = (uint32_t)incident;
		A.started = now;
		A.held = 0;
		snprintf(A.who, sizeof(A.who), "%s", who ? who : "");
	}
	snprintf(A.kind, sizeof(A.kind), "%s", kind);
	clean(A.confidence, sizeof(A.confidence), conf);
	clean(A.uplink, sizeof(A.uplink), uplink);
	A.since = since;
	A.homes = homes < 0 || homes > 1000000 ? 0 : (int)homes;
	A.of = of < 0 || of > 1000000 ? 0 : (int)of;
	A.until = now + hold;
	if (fresh) {
		AG_WARN("area outage %u: %d of %d homes on %s %s since %ld s ago (%s confidence): the ISP's network; AIGW "
			"holds its own fixes while it lasts", A.incident, A.homes, A.of, A.uplink, A.kind, now - A.since,
			A.confidence[0] ? A.confidence : "?");
		dm_on_area(1, A.incident, A.kind, A.since, 0, A.homes, A.of, A.confidence, A.uplink, "", "");
	}
	lapse.cb = lapse_cb;
	uloop_timeout_set(&lapse, (int)hold * 1000 + 500);
	return 0;
}

int area_hold(int what)
{
	if (!A.active || what < 0 || what >= AH_N)
		return 0;
	if (now_s() >= A.until) {
		clear("no word from cortex since (it lapsed)");
		return 0;
	}
	if (!(A.held & (1u << what)))
		AG_INFO("area outage %u: holding %s while the area is down", A.incident, held_words[what]);
	A.held |= 1u << what;
	return 1;
}

int area_holds_action(const char *action)
{
	if (!action)
		return 0;
	if (!strcmp(action, "sqm"))
		return area_hold(AH_SQM);
	if (!strcmp(action, "channel"))
		return area_hold(AH_CHANNEL);
	if (!strcmp(action, "stay24"))
		return area_hold(AH_STAY24);
	return 0;               /* security and a person's own actions are never held */
}

int area_get(struct area_now *o)
{
	memset(o, 0, sizeof(*o));
	if (!A.active || now_s() >= A.until)
		return 0;
	o->incident = A.incident;
	o->kind = A.kind;
	o->confidence = A.confidence;
	o->uplink = A.uplink;
	o->since = A.since;
	o->until = A.until;
	o->homes = A.homes;
	o->of = A.of;
	o->held = A.held;
	return 1;
}

/* the WAN's next hop (IPv4 default route), else a point-to-point WAN's peer (the BNG) */
void area_wan(char *gw, size_t gn, char *peer, size_t pn)
{
	char line[256], ifc[32] = "", dev[IF_NAMESIZE + 1] = "";
	unsigned long dst, via, flags;
	struct ifaddrs *ifa, *i;
	FILE *f = fopen("/proc/net/route", "r");

	gw[0] = peer[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %lx %lx %lx", ifc, &dst, &via, &flags) == 4 && dst == 0 && (flags & 1)) {
			struct in_addr a = { .s_addr = (in_addr_t)via };

			snprintf(dev, sizeof(dev), "%s", ifc);
			if (via)
				inet_ntop(AF_INET, &a, gw, (socklen_t)gn);
			break;
		}
	fclose(f);
	if (gw[0] || !dev[0] || getifaddrs(&ifa) < 0)
		return;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_name && !strcmp(i->ifa_name, dev) && (i->ifa_flags & IFF_POINTOPOINT) && i->ifa_dstaddr &&
		    i->ifa_dstaddr->sa_family == AF_INET) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)i->ifa_dstaddr)->sin_addr, peer, (socklen_t)pn);
			break;
		}
	freeifaddrs(ifa);
}

/* for the bridge's report: what cortex groups this home by */
void area_status_blob(struct blob_buf *b)
{
	char gw[64], peer[64];

	area_wan(gw, sizeof(gw), peer, sizeof(peer));
	blobmsg_add_string(b, "uplink_id", cfg.area_uplink_id);
	blobmsg_add_string(b, "wan_gateway", gw);
	blobmsg_add_string(b, "bng", peer);
}

int area_set_uplink(const char *v, char *err, size_t en)
{
	char id[sizeof(cfg.area_uplink_id)];
	size_t i;

	if (strlen(v) >= sizeof(id)) {
		snprintf(err, en, "UplinkID is at most %zu characters", sizeof(id) - 1);
		return -1;
	}
	for (i = 0; v[i]; i++)
		if (!isprint((unsigned char)v[i]) || strchr("\"'\\,;", v[i])) {
			snprintf(err, en, "UplinkID: letters, digits and - _ . / : # space only");
			return -1;
		}
	snprintf(id, sizeof(id), "%s", v);
	snprintf(cfg.area_uplink_id, sizeof(cfg.area_uplink_id), "%s", id);
	if (ag_config_ensure("area") < 0 || ag_config_set("area", "uplink_id", id) < 0)
		AG_WARN("area: uplink id %s set, but not saved in the config", id);
	return 0;
}

/* Device.X_AIGW_.AreaOutage.: 0, or -1 for a field it doesn't know */
int area_field(const char *f, char *buf, size_t n)
{
	int on = A.active && now_s() < A.until;
	char held[200];

	if (!strcmp(f, "Enable"))
		snprintf(buf, n, "%s", cfg.area ? "true" : "false");
	else if (!strcmp(f, "UplinkID"))
		snprintf(buf, n, "%s", cfg.area_uplink_id);
	else if (!strcmp(f, "Status"))
		snprintf(buf, n, "%s", on ? "Active" : "None");
	else if (!strcmp(f, "Incident"))
		snprintf(buf, n, "%u", on ? A.incident : 0);
	else if (!strcmp(f, "Kind"))
		snprintf(buf, n, "%s", on ? A.kind : "");
	else if (!strcmp(f, "Since"))
		snprintf(buf, n, "%ld", on ? A.since : 0);
	else if (!strcmp(f, "Homes"))
		snprintf(buf, n, "%d", on ? A.homes : 0);
	else if (!strcmp(f, "HomesInArea"))
		snprintf(buf, n, "%d", on ? A.of : 0);
	else if (!strcmp(f, "Confidence"))
		snprintf(buf, n, "%s", on ? A.confidence : "");
	else if (!strcmp(f, "Uplink"))
		snprintf(buf, n, "%s", on ? A.uplink : "");
	else if (!strcmp(f, "HeldFixes")) {
		held_names_list(held, sizeof(held));
		snprintf(buf, n, "%s", on ? held : "");
	} else if (!strcmp(f, "Until"))
		snprintf(buf, n, "%ld", on ? A.until : 0);
	else if (!strcmp(f, "LastIncident"))
		snprintf(buf, n, "%u", A.last_incident);
	else if (!strcmp(f, "LastEnded"))
		snprintf(buf, n, "%ld", A.last_end);
	else
		return -1;
	return 0;
}

void area_blob(struct blob_buf *b)
{
	struct area_now a;
	char gw[64], peer[64], held[200];
	void *t = blobmsg_open_table(b, "area");

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.area);
	blobmsg_add_string(b, "uplink_id", cfg.area_uplink_id);
	area_wan(gw, sizeof(gw), peer, sizeof(peer));
	blobmsg_add_string(b, "wan_gateway", gw);
	blobmsg_add_string(b, "bng", peer);
	blobmsg_add_string(b, "status", area_get(&a) ? "active" : "none");
	if (a.incident) {
		blobmsg_add_u32(b, "incident", a.incident);
		blobmsg_add_string(b, "kind", a.kind);
		blobmsg_add_u64(b, "since", (uint64_t)a.since);
		blobmsg_add_u32(b, "homes", (uint32_t)a.homes);
		blobmsg_add_u32(b, "of", (uint32_t)a.of);
		blobmsg_add_string(b, "confidence", a.confidence);
		blobmsg_add_string(b, "uplink", a.uplink);
		blobmsg_add_u64(b, "until", (uint64_t)a.until);
		area_held_words(held, sizeof(held));
		blobmsg_add_string(b, "held", held);
		blobmsg_add_string(b, "from", A.who);
	}
	blobmsg_close_table(b, t);
}
