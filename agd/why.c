/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW "why was it slow?": the home's timeline (the rules: core/libag/why.c).
 *
 * Every why.interval (a minute) agd keeps a sample of the home:
 *   - the line: 3 echo requests to the next router and to the internet
 *     (diag.targets, the best one counts), their delays and losses
 *   - the WAN's download and upload, from the default route's interface
 *   - the gateway's DNS: lookups, failures (SERVFAIL), their time
 *   - each access point: Self-Healing Wi-Fi's verdict, its channel, stations
 *   - the two devices that downloaded most (the traffic meter, else flows)
 * and a log of what happened: devices joining, leaving, changing band, a
 * signal turning weak; Wi-Fi health; channel changes; AIGW's decisions;
 * proactive care's problems and warnings; speed tests; firmware upgrades;
 * agd starting (after a reboot, or only agd).
 *
 * why.hours (72) of it is kept, about 400 KB, in memory and in why.file: in
 * /tmp on a router (no flash writes; a reboot is itself the gap it leaves),
 * on disk on a PC. Asked about a stretch (ag why, agctl why,
 * Device.X_AIGW_.WhySlow()), the rules explain it against the home's normal
 * at those hours: proactive care's, else the timeline's own.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/care.h"
#include "ag/diag.h"
#include "ag/heal.h"
#include "ag/sqm.h"
#include "ag/why.h"
#include "agd.h"

#define EV_MAX      WHY_EV_MAX
#define RING_MAX    10080               /* a week of minutes */
#define TARGETS     3                   /* [0] the next router, then 2 on the internet */
#define ROUNDS      3
#define SAVE_S      600
#define MAX_ASK_S   86400               /* the longest stretch explained at once */
#define MAGIC       WHY_FILE_MAGIC


static struct why_min *ring;
static int R;
static struct why_ev evs[EV_MAX];
static unsigned ev_head, ev_n;
static char radio_if[WHY_RADIOS][24];
static int radio_ch[WHY_RADIOS];
static int64_t last_save;

/* the sample being taken */
static struct {
	long t;                         /* its start */
	uint64_t mono0;
	int dns_q, dns_fail;
	double dns_ms;
	int dns_n;
	char wan[32];
	uint64_t rx0, tx0;
	int have_wan;
	/* echo requests */
	int icmp;
	struct uloop_fd ufd;
	struct uloop_timeout round_t;
	int round;
	uint16_t ident;
	struct in_addr tgt[TARGETS];
	int ntgt;
	int route;
	int64_t sent[TARGETS][ROUNDS];
	int rtt[TARGETS][ROUNDS];
} S;

static struct uloop_timeout tick_t;

/* each device's counters at the last sample (by its slot in devs[]) */
static struct {
	char mac[18];
	uint64_t down;
	uint8_t weak;
} prev[DEV_MAX];

static long speed_seen;

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int mac_bytes(const char *s, uint8_t m[6])
{
	unsigned v[6];
	int i;

	if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++)
		m[i] = (uint8_t)v[i];
	return 0;
}

static void mac_str(const uint8_t m[6], char *s, size_t n)
{
	snprintf(s, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static const char *dev_name(const struct device *d)
{
	return d->hostname[0] ? d->hostname : d->mac;
}

static const char *name_cb(const uint8_t mac[6], void *ctx)
{
	char s[18];
	struct device *d;

	(void)ctx;
	mac_str(mac, s, sizeof(s));
	d = dev_find_mac(s, 0);
	return d && d->hostname[0] ? d->hostname : NULL;
}

/* ---------- the log ---------- */

static void note(int kind, const char *mac, int flag, long t, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void note(int kind, const char *mac, int flag, long t, const char *fmt, ...)
{
	struct why_ev *e;
	va_list ap;

	if (!cfg.why)
		return;
	e = &evs[ev_head];
	memset(e, 0, sizeof(*e));
	e->t = (uint32_t)(t ? t : (long)ag_wall_s());
	e->kind = (uint8_t)kind;
	e->flag = (uint8_t)flag;
	if (mac)
		mac_bytes(mac, e->mac);
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	ev_head = (ev_head + 1) % EV_MAX;
	if (ev_n < EV_MAX)
		ev_n++;
}

/* calls.c: a call or game that ended, and how it went (flag: enum call_verdict) */
void why_note_call(const char *mac, int verdict, const char *text)
{
	note(WE_CALL, mac && *mac ? mac : NULL, verdict, 0, "%s", text);
}

static int hide_calls;

/* dm.c: the ISP asks (WhySlow()) and calls.share is 0: the calls stay out of the story */
void why_hide_calls(int on)
{
	hide_calls = on;
}

/* a radio's slot (names kept for the whole timeline); -1 when all four are taken */
static int radio_slot(const char *iface)
{
	int i;

	for (i = 0; i < WHY_RADIOS; i++)
		if (radio_if[i][0] && !strcmp(radio_if[i], iface))
			return i;
	for (i = 0; i < WHY_RADIOS; i++)
		if (!radio_if[i][0]) {
			snprintf(radio_if[i], sizeof(radio_if[i]), "%.23s", iface);
			return i;
		}
	return -1;
}

static void radio_words(const char *iface, char *buf, size_t n)
{
	const struct wifi_ap *a = wifi_ap_find(iface);

	if (a && a->band)
		snprintf(buf, n, "%s (%.23s)", a->band == 5 ? "5 GHz" : "2.4 GHz", iface);
	else
		snprintf(buf, n, "%.23s", iface);
}

enum { P_EVENT, P_MAC, P_HOST, P_BAND, P_SIGNAL, P_IFACE, P_STATUS, P_CAUSE, P_ACTION, P_TARGET, P_DEVICE, P_STATE,
       P_ORIGIN, P_CODE, P_SUMMARY, P_FROM, P_TO, P_METRIC, __P_MAX };
static const struct blobmsg_policy pol[__P_MAX] = {
	[P_EVENT] = { "event", BLOBMSG_TYPE_STRING }, [P_MAC] = { "mac", BLOBMSG_TYPE_STRING },
	[P_HOST] = { "hostname", BLOBMSG_TYPE_STRING }, [P_BAND] = { "band", BLOBMSG_TYPE_STRING },
	[P_SIGNAL] = { "signal", BLOBMSG_TYPE_INT32 }, [P_IFACE] = { "iface", BLOBMSG_TYPE_STRING },
	[P_STATUS] = { "status", BLOBMSG_TYPE_STRING }, [P_CAUSE] = { "cause", BLOBMSG_TYPE_STRING },
	[P_ACTION] = { "action", BLOBMSG_TYPE_STRING }, [P_TARGET] = { "target", BLOBMSG_TYPE_STRING },
	[P_DEVICE] = { "device", BLOBMSG_TYPE_STRING }, [P_STATE] = { "state", BLOBMSG_TYPE_STRING },
	[P_ORIGIN] = { "origin", BLOBMSG_TYPE_STRING }, [P_CODE] = { "code", BLOBMSG_TYPE_STRING },
	[P_SUMMARY] = { "summary", BLOBMSG_TYPE_STRING }, [P_FROM] = { "from", BLOBMSG_TYPE_STRING },
	[P_TO] = { "to", BLOBMSG_TYPE_STRING }, [P_METRIC] = { "metric", BLOBMSG_TYPE_STRING },
};

static const char *s_(struct blob_attr **tb, int k)
{
	return tb[k] ? blobmsg_get_string(tb[k]) : "";
}

static void on_wifi(struct blob_attr **tb)
{
	const char *ev = s_(tb, P_EVENT), *mac = s_(tb, P_MAC);
	const char *who = *s_(tb, P_HOST) ? s_(tb, P_HOST) : mac;
	const char *band = s_(tb, P_BAND);
	int b = !strcmp(band, "5") ? 5 : !strcmp(band, "2.4") ? 2 : 0;
	int sig = tb[P_SIGNAL] ? (int)blobmsg_get_u32(tb[P_SIGNAL]) : 0;
	char sigs[24] = "", rw[48];

	if (sig)
		snprintf(sigs, sizeof(sigs), " (%d dBm)", sig);
	if (!strcmp(ev, "connected") || !strcmp(ev, "disconnected"))
		offers_on_wifi(mac, !strcmp(ev, "connected"));      /* short drops, for the signals */
	if (!strcmp(ev, "connected"))
		note(WE_JOIN, mac, b, 0, "%s joined Wi-Fi%s%s%s", who, b ? " on " : "", b == 5 ? "5 GHz" : b ? "2.4 GHz" : "",
		     sigs);
	else if (!strcmp(ev, "disconnected"))
		note(WE_LEAVE, mac, 0, 0, "%s left Wi-Fi", who);
	else if (!strcmp(ev, "roamed"))
		note(WE_ROAM, mac, b, 0, "%s moved to %s%s", who, b == 5 ? "5 GHz" : b ? "2.4 GHz" : "another access point",
		     sigs);
	else if (!strcmp(ev, "steer"))
		note(WE_STEER, mac, 0, 0, "AIGW moved %s to %s GHz", who, *s_(tb, P_TO) ? s_(tb, P_TO) : "another band");
	else if (!strcmp(ev, "health")) {
		const char *st = s_(tb, P_STATUS);
		int flag = !strcmp(st, "Good") ? HEAL_GOOD : !strcmp(st, "Degraded") ? HEAL_DEGRADED :
			   !strcmp(st, "Poor") ? HEAL_POOR : HEAL_UNKNOWN;

		radio_words(s_(tb, P_IFACE), rw, sizeof(rw));
		note(WE_HEALTH, NULL, flag, 0, "Wi-Fi on %s %s%s%s%s", rw, flag == HEAL_GOOD ? "fine again" : "turned ",
		     flag == HEAL_GOOD ? "" : st, *s_(tb, P_CAUSE) && strcmp(s_(tb, P_CAUSE), "None") ? ": " : "",
		     strcmp(s_(tb, P_CAUSE), "None") ? s_(tb, P_CAUSE) : "");
	}
}

static void on_decision(struct blob_attr **tb)
{
	const char *a = s_(tb, P_ACTION), *state = s_(tb, P_STATE), *target = s_(tb, P_TARGET);
	const char *who = *s_(tb, P_DEVICE) ? s_(tb, P_DEVICE) : target;
	int ended = !strcmp(state, "reverted") || !strcmp(state, "expired") || !strcmp(state, "tried");
	int flag = 0;
	struct device *d = NULL;

	if (strcmp(state, "active") && !ended)
		return;                         /* only what took effect, or ended */
	if (!strcmp(a, "quarantine") || !strcmp(a, "disconnect"))
		flag = WF_BLOCKS;
	else if (!strcmp(a, "throttle") || !strcmp(a, "deprioritize"))
		flag = WF_LIMITS;
	else if (!strcmp(a, "channel") || !strcmp(a, "stay24") || !strcmp(a, "sqm") || !strcmp(a, "meshsteer") ||
		 !strcmp(a, "meshchan"))
		flag = WF_WIFI;
	if (ended)
		flag |= WF_ENDED;
	if (!strcmp(a, "meshsteer")) {          /* "<station>@<access point>": the station */
		char mac[18];

		snprintf(mac, sizeof(mac), "%.17s", target);
		d = dev_find_mac(mac, 0);
	} else if (strcmp(a, "channel") && strcmp(a, "block") && strcmp(a, "sqm") && strcmp(a, "meshchan"))
		d = act_find_device(target);
	note(WE_DECISION, d ? d->mac : NULL, flag, 0, "AIGW %s: %s %s%s%s", ended ? "ended" : "acted", a, who,
	     *s_(tb, P_ORIGIN) ? ", by " : "", s_(tb, P_ORIGIN));
}

static void on_care(struct blob_attr **tb)
{
	const char *ev = s_(tb, P_EVENT);

	if (!strcmp(ev, "found") || !strcmp(ev, "changed"))
		note(WE_CARE, NULL, 0, 0, "Proactive care: %s (%s)", s_(tb, P_SUMMARY), s_(tb, P_CODE));
	else if (!strcmp(ev, "cleared"))
		note(WE_CARE, NULL, 0, 0, "Proactive care: the problem was over (%s)", s_(tb, P_CODE));
	else if (!strcmp(ev, "warning"))
		note(WE_CARE, NULL, 0, 0, "Early warning: %s", s_(tb, P_SUMMARY));
}

static void on_upgrade(struct blob_attr **tb)
{
	const char *ev = s_(tb, P_EVENT);

	if (!strcmp(ev, "activated") || !strcmp(ev, "kept") || !strcmp(ev, "rolled-back") || !strcmp(ev, "worse"))
		note(WE_UPGRADE, NULL, 0, 0, "Firmware %s to %s: %s", s_(tb, P_FROM), s_(tb, P_TO), ev);
}

/* mesh Wi-Fi: a device moved between access points (by itself or the controller), a backhaul found weak or fine
   again, a trial's verdict (AIGW's own steers and channel changes are on the timeline as decisions too) */
static void on_mesh(struct blob_attr **tb)
{
	const char *ev = s_(tb, P_EVENT), *mac = s_(tb, P_MAC), *sum = s_(tb, P_SUMMARY);

	if (!strcmp(ev, "roamed") || !strcmp(ev, "backhaul") || !strcmp(ev, "backhaul-ok") || !strcmp(ev, "kept") ||
	    !strcmp(ev, "undone") || !strcmp(ev, "missed") || !strcmp(ev, "channel-kept") || !strcmp(ev, "channel-undone") ||
	    !strcmp(ev, "controller-lost"))
		note(WE_MESH, *mac ? mac : NULL, 0, 0, "Mesh: %.140s", sum);
}

void why_on_emit(const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[__P_MAX];

	if (!cfg.why || !ring)
		return;
	blobmsg_parse(pol, __P_MAX, tb, blob_data(msg), blob_len(msg));
	if (!strcmp(type, AG_EV_MESH)) {
		on_mesh(tb);
		return;
	}
	if (!strcmp(type, AG_EV_WIFI))
		on_wifi(tb);
	else if (!strcmp(type, AG_EV_DECISION))
		on_decision(tb);
	else if (!strcmp(type, AG_EV_CARE))
		on_care(tb);
	else if (!strcmp(type, AG_EV_UPGRADE))
		on_upgrade(tb);
}

void why_on_dns(int rcode, uint32_t latency_us)
{
	if (!ring)
		return;
	S.dns_q++;
	if (rcode == 2)
		S.dns_fail++;
	else if (latency_us) {
		S.dns_ms += latency_us / 1000.0;
		S.dns_n++;
	}
}

/* ---------- the line: echo requests ---------- */

static uint16_t csum(const void *p, size_t n)
{
	const unsigned char *c = p;
	uint32_t s = 0;
	size_t i;

	for (i = 0; i + 1 < n; i += 2) {
		uint16_t w;

		memcpy(&w, c + i, 2);
		s += w;
	}
	if (n & 1)
		s += c[n - 1];
	s = (s >> 16) + (s & 0xffff);
	s += s >> 16;
	return (uint16_t)~s;
}

/* the default route: its next hop and interface */
static int default_route(struct in_addr *gw, char *ifc, size_t n)
{
	char line[256], name[32];
	unsigned long dst, via, flags;
	FILE *f = fopen("/proc/net/route", "r");
	int found = 0;

	gw->s_addr = 0;
	ifc[0] = '\0';
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %lx %lx %lx", name, &dst, &via, &flags) == 4 && dst == 0 && (flags & 1)) {
			found = 1;
			gw->s_addr = (in_addr_t)via;
			snprintf(ifc, n, "%s", name);
			break;
		}
	fclose(f);
	return found;
}

static void send_round(void)
{
	int t;

	for (t = 0; t < S.ntgt; t++) {
		struct { struct icmphdr h; uint32_t pad; } pkt;
		struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = S.tgt[t] };

		if (!S.tgt[t].s_addr)
			continue;
		memset(&pkt, 0, sizeof(pkt));
		pkt.h.type = ICMP_ECHO;
		pkt.h.un.echo.id = htons(S.ident);
		pkt.h.un.echo.sequence = htons((uint16_t)(t << 8 | S.round));
		pkt.h.checksum = csum(&pkt, sizeof(pkt));
		S.sent[t][S.round] = mono_ms();
		if (sendto(S.icmp, &pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) < 0)
			S.sent[t][S.round] = 0;
	}
}

static void on_icmp(struct uloop_fd *u, unsigned ev)
{
	char buf[256];
	ssize_t n;

	(void)ev;
	while ((n = recv(u->fd, buf, sizeof(buf), MSG_DONTWAIT)) >= 0) {
		struct iphdr *ip = (struct iphdr *)buf;
		struct icmphdr *ic;
		unsigned t, r;

		if ((size_t)n < sizeof(*ip) || (size_t)n < (size_t)ip->ihl * 4 + sizeof(*ic))
			continue;
		ic = (struct icmphdr *)(buf + ip->ihl * 4);
		if (ic->type != ICMP_ECHOREPLY || ntohs(ic->un.echo.id) != S.ident)
			continue;
		t = ntohs(ic->un.echo.sequence) >> 8;
		r = ntohs(ic->un.echo.sequence) & 0xff;
		if (t < TARGETS && r < ROUNDS && S.sent[t][r] && S.rtt[t][r] < 0 && ip->saddr == S.tgt[t].s_addr)
			S.rtt[t][r] = (int)(mono_ms() - S.sent[t][r]);
	}
}

static void round_cb(struct uloop_timeout *t)
{
	if (S.round >= ROUNDS || S.icmp < 0)
		return;
	send_round();
	if (++S.round < ROUNDS)
		uloop_timeout_set(t, cfg.why_interval >= 10 ? 1000 : 200);
}

static void ping_of(int t, uint8_t *sent, uint8_t *lost, uint16_t *ms)
{
	int r, v[ROUNDS], m;

	*sent = *lost = 0;
	for (r = 0; r < ROUNDS; r++) {
		v[r] = -1;
		if (!S.sent[t][r])
			continue;
		(*sent)++;
		if (S.rtt[t][r] < 0)
			(*lost)++;
		else
			v[r] = S.rtt[t][r];
	}
	m = diag_median(v, ROUNDS);
	*ms = m < 0 ? WHY_NONE : (uint16_t)(m > 60000 ? 60000 : m);
}

/* ---------- a sample ---------- */

static int wan_bytes(const char *ifc, uint64_t *rx, uint64_t *tx)
{
	char p[128];
	unsigned long long v;
	FILE *f;

	if (!ifc[0])
		return -1;
	snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/rx_bytes", ifc);
	if (!(f = fopen(p, "r")))
		return -1;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	*rx = v;
	snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/tx_bytes", ifc);
	if (!(f = fopen(p, "r")))
		return -1;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	*tx = v;
	return 0;
}

static uint64_t dev_down(const struct device *d)
{
	return meter_active() ? d->meter_down : d->bytes_down;
}

/* the devices' downloads, their signals (weak, fine again) */
static void devices(struct why_min *m, double secs)
{
	int i, j;

	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];
		uint64_t now, delta = 0;
		uint32_t kbps;

		if (!d->used) {
			prev[i].mac[0] = '\0';
			continue;
		}
		now = dev_down(d);
		if (!strcmp(prev[i].mac, d->mac) && now >= prev[i].down)
			delta = now - prev[i].down;
		else {
			snprintf(prev[i].mac, sizeof(prev[i].mac), "%s", d->mac);
			prev[i].weak = 0;
		}
		prev[i].down = now;
		kbps = secs > 0 ? (uint32_t)((double)delta * 8.0 / 1000.0 / secs) : 0;
		if (kbps && m) {
			for (j = 0; j < WHY_TOP && m->top[j].kbps >= kbps; j++)
				;
			if (j < WHY_TOP) {
				memmove(&m->top[j + 1], &m->top[j], sizeof(m->top[0]) * (size_t)(WHY_TOP - 1 - j));
				memset(&m->top[j], 0, sizeof(m->top[0]));
				mac_bytes(d->mac, m->top[j].mac);
				m->top[j].kbps = kbps;
			}
		}
		if (!d->wifi_connected || !d->wifi_sig_avg) {
			prev[i].weak = 0;
			continue;
		}
		if (!prev[i].weak && d->wifi_sig_avg <= cfg.heal_weak) {
			prev[i].weak = 1;
			note(WE_WEAK, d->mac, (int)-d->wifi_sig_avg, 0, "%s's signal turned weak (%.0f dBm)", dev_name(d),
			     d->wifi_sig_avg);
		} else if (prev[i].weak && d->wifi_sig_avg >= cfg.heal_weak + cfg.heal_weak_margin) {
			prev[i].weak = 0;
			note(WE_SIGNAL_OK, d->mac, (int)-d->wifi_sig_avg, 0, "%s's signal was fine again (%.0f dBm)", dev_name(d),
			     d->wifi_sig_avg);
		}
		if (prev[i].weak && m)
			offers_on_weak(d->mac, (long)(secs + 0.5));    /* the signals for the ISP: time on a weak signal */
	}
}

static void radios(struct why_min *m)
{
	int i;

	for (i = 0; i < wifi_n_aps; i++) {
		const struct wifi_ap *a = &wifi_aps[i];
		const struct heal_radio *h = heal_for(a->iface);
		int k = radio_slot(a->iface);
		char rw[48];

		if (k < 0)
			continue;
		m->r[k].used = 1;
		m->r[k].channel = (uint8_t)(a->channel > 255 ? 255 : a->channel);
		m->r[k].stations = (uint8_t)(a->stations > 255 ? 255 : a->stations);
		m->r[k].rtt = WHY_NONE;
		if (h) {
			m->r[k].status = (uint8_t)h->status;
			m->r[k].cause = (uint8_t)h->cause;
			m->r[k].loss = (uint8_t)(h->loss_pct < 0 ? 0 : h->loss_pct > 100 ? 100 : h->loss_pct);
			if (h->measured && h->rtt_ms >= 0)
				m->r[k].rtt = (uint16_t)(h->rtt_ms > 60000 ? 60000 : h->rtt_ms);
		}
		if (a->channel && radio_ch[k] && a->channel != radio_ch[k]) {
			radio_words(a->iface, rw, sizeof(rw));
			note(WE_CHANNEL, NULL, 0, 0, "%s moved from channel %d to %d", rw, radio_ch[k], a->channel);
		}
		if (a->channel)
			radio_ch[k] = a->channel;
	}
}

static void store(const struct why_min *m)
{
	ring[(m->t / (uint32_t)cfg.why_interval) % (uint32_t)R] = *m;
}

/* the most the timeline has seen on the line (Mbit/s) */
static void peaks(float *down, float *up)
{
	uint32_t d = 0, u = 0;
	int i;

	for (i = 0; i < R; i++)
		if (ring[i].t) {
			if (ring[i].down_kbps > d)
				d = ring[i].down_kbps;
			if (ring[i].up_kbps > u)
				u = ring[i].up_kbps;
		}
	*down = d / 1000.0f;
	*up = u / 1000.0f;
}

/* the sample that ends now; then the next one starts */
static void finish(long now)
{
	struct why_min m;
	uint64_t rx, tx;
	double secs = (double)(ag_mono_us() - S.mono0) / 1e6;
	int t, best = -1;
	double mbps;
	long at;

	memset(&m, 0, sizeof(m));
	m.t = (uint32_t)S.t;
	m.route = (uint8_t)S.route;
	m.gw_ms = m.in_ms = m.dns_ms = WHY_NONE;
	if (S.icmp >= 0) {
		if (S.tgt[0].s_addr)
			ping_of(0, &m.gw_sent, &m.gw_lost, &m.gw_ms);
		for (t = 1; t < S.ntgt; t++) {
			uint8_t s, l;
			uint16_t ms;

			ping_of(t, &s, &l, &ms);
			if (!s)
				continue;
			if (best < 0 || l < m.in_lost || (l == m.in_lost && ms < m.in_ms)) {
				best = t;
				m.in_sent = s, m.in_lost = l, m.in_ms = ms;
			}
		}
		uloop_fd_delete(&S.ufd);
		close(S.icmp);
		S.icmp = -1;
	}
	m.dns_q = (uint16_t)(S.dns_q > 65535 ? 65535 : S.dns_q);
	m.dns_fail = (uint16_t)(S.dns_fail > 65535 ? 65535 : S.dns_fail);
	if (S.dns_n)
		m.dns_ms = (uint16_t)(S.dns_ms / S.dns_n > 60000 ? 60000 : S.dns_ms / S.dns_n);
	if (S.have_wan && wan_bytes(S.wan, &rx, &tx) == 0 && rx >= S.rx0 && tx >= S.tx0 && secs > 0.5) {
		m.down_kbps = (uint32_t)((double)(rx - S.rx0) * 8.0 / 1000.0 / secs);
		m.up_kbps = (uint32_t)((double)(tx - S.tx0) * 8.0 / 1000.0 / secs);
	}
	devices(&m, secs);
	radios(&m);
	if (S.t) {
		float pd, pu;

		store(&m);
		peaks(&pd, &pu);
		offers_on_sample(&m, (long)(secs + 0.5), pd, pu);
		sqm_on_sample();                /* bufferbloat: smart queueing found, tried, judged */
	}
	/* a speed test that ran since */
	at = care_speed_last(&mbps);
	if (at > speed_seen) {
		if (speed_seen)
			note(WE_SPEED, NULL, 0, at, "Speed test: %.1f Mbit/s%s", mbps,
			     cfg.care_speed_mbps ? " (the line's speed is set)" : "");
		speed_seen = at;
	}
	(void)now;
}

static void begin(long now)
{
	char tl[sizeof(cfg.diag_targets)], *s, *save;
	struct in_addr g;
	int t, r;

	memset(&S, 0, sizeof(S));
	S.t = now - now % cfg.why_interval;
	S.mono0 = ag_mono_us();
	S.icmp = -1;
	S.route = default_route(&g, S.wan, sizeof(S.wan));
	if (cfg.why_wan[0])                     /* the WAN set by hand (several WANs, or tests) */
		snprintf(S.wan, sizeof(S.wan), "%.31s", cfg.why_wan);
	S.have_wan = wan_bytes(S.wan, &S.rx0, &S.tx0) == 0;
	S.ident = (uint16_t)(getpid() ^ 0x5759);
	for (t = 0; t < TARGETS; t++)
		for (r = 0; r < ROUNDS; r++)
			S.rtt[t][r] = -1;
	if (!cfg.why_probe || !S.route)
		return;
	S.tgt[0] = g;
	S.ntgt = 1;
	snprintf(tl, sizeof(tl), "%s", cfg.diag_targets);
	for (s = strtok_r(tl, " ,", &save); s && S.ntgt < TARGETS; s = strtok_r(NULL, " ,", &save))
		if (inet_pton(AF_INET, s, &S.tgt[S.ntgt]) == 1)
			S.ntgt++;
	if ((S.icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
		static int warned;

		if (!warned++)
			AG_WARN("why: no ICMP socket (%s): the line is not sampled", strerror(errno));
		return;
	}
	S.ufd.fd = S.icmp;
	S.ufd.cb = on_icmp;
	uloop_fd_add(&S.ufd, ULOOP_READ);
	S.round_t.cb = round_cb;
	uloop_timeout_set(&S.round_t, 1);
}

static void tick(struct uloop_timeout *t)
{
	long now = (long)ag_wall_s();
	int next = cfg.why_interval - (int)(now % cfg.why_interval);

	uloop_timeout_cancel(&S.round_t);
	finish(now);
	begin(now);
	if (now - (long)last_save >= SAVE_S) {
		why_save();
		last_save = now;
	}
	offers_tick();
	uloop_timeout_set(t, next * 1000);
}

/* ---------- kept over a restart ---------- */

void why_save(void)
{
	struct why_file_hdr h;
	char tmp[300];
	FILE *f;
	int ok;

	if (!ring || !cfg.why_file[0])
		return;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, MAGIC, 8);
	h.min_size = sizeof(struct why_min);
	h.ev_size = sizeof(struct why_ev);
	h.ring = (uint32_t)R;
	h.interval = (uint32_t)cfg.why_interval;
	h.ev_n = ev_n;
	h.ev_head = ev_head;
	memcpy(h.radio, radio_if, sizeof(h.radio));
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.why_file);
	if (!(f = fopen(tmp, "wb"))) {
		char dir[256], *sl;

		snprintf(dir, sizeof(dir), "%s", cfg.why_file);
		if ((sl = strrchr(dir, '/')) && sl != dir) {
			*sl = '\0';
			mkdir(dir, 0755);
		}
		if (!(f = fopen(tmp, "wb"))) {
			AG_WARN("why: can't save %s: %s", tmp, strerror(errno));
			return;
		}
	}
	ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(ring, sizeof(*ring), (size_t)R, f) == (size_t)R &&
	     fwrite(evs, sizeof(evs[0]), EV_MAX, f) == EV_MAX;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, cfg.why_file) < 0) {
		AG_WARN("why: can't save %s", cfg.why_file);
		unlink(tmp);
	}
}

static long load(void)
{
	struct why_file_hdr h;
	struct why_min m;
	FILE *f;
	uint32_t i;
	long last = 0;

	if (!cfg.why_file[0] || !(f = fopen(cfg.why_file, "rb")))
		return 0;
	if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, MAGIC, 8) || h.min_size != sizeof(struct why_min) ||
	    h.ev_size != sizeof(struct why_ev) || !h.interval || h.ring > RING_MAX * 30) {
		fclose(f);
		AG_WARN("why: %s isn't a timeline this agd knows: starting a new one", cfg.why_file);
		return 0;
	}
	/* a sample in the new ring by its time (the length or the interval may have changed) */
	for (i = 0; i < h.ring && fread(&m, sizeof(m), 1, f) == 1; i++)
		if (m.t) {
			if ((long)m.t > last)
				last = (long)m.t;
			if ((long)m.t >= (long)ag_wall_s() - (long)R * cfg.why_interval)
				store(&m);
		}
	if (i == h.ring && fread(evs, sizeof(evs[0]), EV_MAX, f) == EV_MAX) {
		ev_n = h.ev_n > EV_MAX ? EV_MAX : h.ev_n;
		ev_head = h.ev_head % EV_MAX;
	} else {
		memset(evs, 0, sizeof(evs));
	}
	memcpy(radio_if, h.radio, sizeof(radio_if));
	for (i = 0; i < WHY_RADIOS; i++)
		radio_if[i][sizeof(radio_if[i]) - 1] = '\0';
	fclose(f);
	return last;
}

static long uptime_s(void)
{
	double up = 0;
	FILE *f = fopen("/proc/uptime", "r");

	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0;
		fclose(f);
	}
	return (long)up;
}

void why_init(void)
{
	long now = (long)ag_wall_s(), last, off;
	char d[48] = "";

	if (!cfg.why)
		return;
	R = cfg.why_hours * 3600 / cfg.why_interval;
	if (R > RING_MAX)
		R = RING_MAX;
	if (R < 10)
		R = 10;
	ring = calloc((size_t)R, sizeof(*ring));
	if (!ring) {
		AG_ERR("why: no memory for the timeline");
		return;
	}
	last = load();
	off = last ? now - last - cfg.why_interval : 0;
	if (off >= 2L * cfg.why_interval) {
		if (off < 7200)
			snprintf(d, sizeof(d), "%ld min", (off + 30) / 60);
		else
			snprintf(d, sizeof(d), "%ld h %ld min", off / 3600, off % 3600 / 60);
	}
	/* the system came up after the last sample: the gateway was off (or rebooting) */
	if (last && uptime_s() < now - last)
		note(WE_START, NULL, 1, now, "The gateway started%s%s%s", off >= 2L * cfg.why_interval ? " (it was off for " : "",
		     off >= 2L * cfg.why_interval ? d : "", off >= 2L * cfg.why_interval ? ")" : "");
	else if (last)
		note(WE_START, NULL, 0, now, "AIGW restarted%s%s%s", off >= 2L * cfg.why_interval ? " (not recording for " : "",
		     off >= 2L * cfg.why_interval ? d : "", off >= 2L * cfg.why_interval ? ")" : "");
	else
		note(WE_START, NULL, uptime_s() < 600, now, "AIGW started recording the home's timeline");
	speed_seen = care_speed_last(&(double){ 0 });
	devices(NULL, 0);                       /* the counters as they are */
	begin(now);
	tick_t.cb = tick;
	uloop_timeout_set(&tick_t, (cfg.why_interval - (int)(now % cfg.why_interval)) * 1000);
	AG_INFO("why: a sample every %d s, %d h kept (%d samples)%s%s", cfg.why_interval, cfg.why_hours, R,
		cfg.why_file[0] ? ", in " : "", cfg.why_file);
}

/* ---------- asking ---------- */

/* ---------- for smart queueing (sqm.c) ---------- */

static int cmp_min(const void *a, const void *b);

const char *why_wan(void)
{
	return S.wan;
}

/* the samples with from <= t < to, oldest first; how many */
int why_samples(long from, long to, struct why_min *out, int max)
{
	int i, n = 0;

	for (i = 0; ring && i < R && n < max; i++)
		if (ring[i].t && (long)ring[i].t >= from && (long)ring[i].t < to)
			out[n++] = ring[i];
	qsort(out, (size_t)n, sizeof(*out), cmp_min);
	return n;
}

/* the line as it is now: its speed (sold, else tested), the most seen, the internet's normal at this hour */
void why_line_now(struct sqm_line *l)
{
	double mbps = 0;
	float v;
	time_t now = (time_t)ag_wall_s();
	struct tm tm;

	memset(l, 0, sizeof(*l));
	if (cfg.care_speed_mbps > 0)
		l->down_mbps = (float)cfg.care_speed_mbps;
	else if (care_speed_last(&mbps) > 0 && mbps > 0)
		l->down_mbps = (float)mbps;
	if (ring)
		peaks(&l->peak_down_mbps, &l->peak_up_mbps);
	localtime_r(&now, &tm);
	if (care_normal_hour(CM_INET_MS, tm.tm_hour, &v) == 0) {
		l->normal_ms = v;
		l->have_normal = 1;
	} else if (ring) {
		float bn[WN_N];
		int bh[WN_N];

		why_baseline(ring, R, (long)now - 3600, (long)now, bn, bh);
		if (bh[WN_INET]) {
			l->normal_ms = bn[WN_INET];
			l->have_normal = 1;
		}
	}
}
static long recorded_since(void)
{
	long s = 0;
	int i;

	for (i = 0; i < R; i++)
		if (ring[i].t && (!s || (long)ring[i].t < s))
			s = (long)ring[i].t;
	return s;
}

static int cmp_min(const void *a, const void *b)
{
	const struct why_min *x = a, *y = b;

	return x->t < y->t ? -1 : x->t > y->t;
}

/* events oldest first: [from - 10 min, to), and for a device, its last state before that */
static int events_of(long from, long to, const uint8_t *host, struct why_ev *out, int max)
{
	int i, n = 0;
	unsigned k;

	if (host) {
		/* the device's state at `from`: its last join/leave/move, weak/fine, and decisions (newest first) */
		int seen[WE_N] = { 0 };

		for (k = 0; k < ev_n && n < max / 4; k++) {
			const struct why_ev *e = &evs[(ev_head + EV_MAX - 1 - k) % EV_MAX];
			int slot = e->kind == WE_JOIN || e->kind == WE_LEAVE || e->kind == WE_ROAM ? WE_JOIN :
				   e->kind == WE_WEAK || e->kind == WE_SIGNAL_OK ? WE_WEAK : e->kind;

			if ((long)e->t >= from - 600 || memcmp(e->mac, host, 6) || seen[slot] ||
			    (slot != WE_JOIN && slot != WE_WEAK && slot != WE_DECISION))
				continue;
			seen[slot] = 1;
			out[n++] = *e;
		}
		/* newest first so far: oldest first */
		for (i = 0; i < n / 2; i++) {
			struct why_ev t = out[i];

			out[i] = out[n - 1 - i];
			out[n - 1 - i] = t;
		}
	}
	for (k = 0; k < ev_n && n < max; k++) {
		const struct why_ev *e = &evs[(ev_head + EV_MAX - ev_n + k) % EV_MAX];

		if ((long)e->t >= from - 600 && (long)e->t < to && !(hide_calls && e->kind == WE_CALL))
			out[n++] = *e;
	}
	return n;
}

static int hour_of(long t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	localtime_r(&tt, &tm);
	return tm.tm_hour;
}

static struct why_min *pick;            /* the stretch's samples (sorted), for raw output */
static int npick;
static struct why_ev pick_ev[EV_MAX + 32];
static int npick_ev;

int why_ask(long from, long to, const char *host, struct why_result *res, struct why_in *in, char *err, size_t en)
{
	static uint8_t hmac[6];
	static char hname[64];
	struct device *d = NULL;
	float bn[WN_N];
	int bh[WN_N], i, w;
	double mbps = 0;
	uint32_t pd = 0, pu = 0;

	if (!cfg.why || !ring) {
		snprintf(err, en, "the timeline is off (why.enabled)");
		return -1;
	}
	if (to <= from || to - from > MAX_ASK_S) {
		snprintf(err, en, "a stretch of up to 24 hours, its start before its end");
		return -1;
	}
	if ((to - from) / cfg.why_interval > WHY_MAX_SAMPLES)  /* (short intervals, in tests): the latest part */
		from = to - (long)WHY_MAX_SAMPLES * cfg.why_interval;
	if (host && *host) {
		d = act_find_device(host);
		if (!d || mac_bytes(d->mac, hmac) < 0) {
			snprintf(err, en, "no device called %s", host);
			return -1;
		}
		snprintf(hname, sizeof(hname), "%s", dev_name(d));
	}
	free(pick);
	pick = calloc((size_t)R, sizeof(*pick));
	npick = 0;
	if (!pick) {
		snprintf(err, en, "no memory");
		return -1;
	}
	for (i = 0; i < R; i++) {
		if (ring[i].t && (long)ring[i].t >= from && (long)ring[i].t < to)
			pick[npick++] = ring[i];
		if (ring[i].t) {
			if (ring[i].down_kbps > pd)
				pd = ring[i].down_kbps;
			if (ring[i].up_kbps > pu)
				pu = ring[i].up_kbps;
		}
	}
	qsort(pick, (size_t)npick, sizeof(*pick), cmp_min);
	npick_ev = events_of(from, to, d ? hmac : NULL, pick_ev, (int)(sizeof(pick_ev) / sizeof(pick_ev[0])));

	memset(in, 0, sizeof(*in));
	in->from = from;
	in->to = to;
	in->step = cfg.why_interval;
	in->m = pick;
	in->n = npick;
	in->e = pick_ev;
	in->ne = npick_ev;
	in->recorded_since = recorded_since();
	/* the normal at those hours: proactive care's, else the timeline's own */
	why_baseline(ring, R, from, to, bn, bh);
	for (w = 0; w < WN_N; w++) {
		static const int cm[WN_N] = { CM_GW_MS, CM_INET_MS, CM_LOSS, CM_DNS_MS };
		float v;

		if (care_normal_hour(cm[w], hour_of(from + (to - from) / 2), &v) == 0) {
			in->norm[w] = v;
			in->have_norm[w] = 1;
		} else if (bh[w]) {
			in->norm[w] = bn[w];
			in->have_norm[w] = 1;
		}
	}
	if (cfg.care_speed_mbps)
		in->down_mbps = (float)cfg.care_speed_mbps;
	else if (care_speed_last(&mbps) > 0 && mbps > 0)
		in->down_mbps = (float)mbps;
	in->peak_down_mbps = pd / 1000.0f;
	in->peak_up_mbps = pu / 1000.0f;
	for (i = 0; i < WHY_RADIOS; i++) {
		static char rn[WHY_RADIOS][48];

		if (!radio_if[i][0])
			continue;
		radio_words(radio_if[i], rn[i], sizeof(rn[i]));
		in->radio[i] = rn[i];
	}
	if (d) {
		in->host = hmac;
		in->host_name = hname;
	}
	in->name = name_cb;
	why_judge(in, res);
	return 0;
}

static void add_time(struct blob_buf *b, const char *k, long t)
{
	blobmsg_add_u64(b, k, (uint64_t)(t < 0 ? 0 : t));
}

void why_blob(struct blob_buf *b, long from, long to, const char *host, int raw)
{
	static struct why_result r;
	struct why_in in;
	char err[160], c[16];
	void *a, *t;
	int i, j;

	if (why_ask(from, to, host, &r, &in, err, sizeof(err)) < 0) {
		blobmsg_add_string(b, "error", err);
		return;
	}
	blobmsg_add_string(b, "code", r.f[0].code);
	blobmsg_add_string(b, "area", r.f[0].area);
	blobmsg_add_string(b, "severity", diag_sev_name(r.f[0].sev));
	blobmsg_add_string(b, "summary", r.f[0].text);
	blobmsg_add_string(b, "advice", why_advice(r.f[0].code));
	if (in.host_name)
		blobmsg_add_string(b, "host", in.host_name);
	add_time(b, "from", from);
	add_time(b, "to", to);
	blobmsg_add_u32(b, "interval", (uint32_t)cfg.why_interval);
	blobmsg_add_u32(b, "recorded", (uint32_t)r.recorded);
	blobmsg_add_u32(b, "expected", (uint32_t)r.expected);
	add_time(b, "recorded_since", in.recorded_since);
	t = blobmsg_open_table(b, "normal");
	if (in.have_norm[WN_GW])
		blobmsg_add_u32(b, "next_hop_ms", (uint32_t)(in.norm[WN_GW] + 0.5f));
	if (in.have_norm[WN_INET])
		blobmsg_add_u32(b, "internet_ms", (uint32_t)(in.norm[WN_INET] + 0.5f));
	if (in.have_norm[WN_DNS])
		blobmsg_add_u32(b, "dns_ms", (uint32_t)(in.norm[WN_DNS] + 0.5f));
	if (in.down_mbps > 0)
		blobmsg_add_u32(b, "line_mbps", (uint32_t)(in.down_mbps + 0.5f));
	blobmsg_close_table(b, t);
	a = blobmsg_open_array(b, "story");
	for (i = 0; i < r.nstory; i++) {
		t = blobmsg_open_table(b, NULL);
		add_time(b, "time", r.story[i].t);
		why_clock(r.story[i].t, to, c, sizeof(c));
		blobmsg_add_string(b, "clock", c);
		blobmsg_add_string(b, "text", r.story[i].text);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "findings");
	for (i = 0; i < r.n; i++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "code", r.f[i].code);
		blobmsg_add_string(b, "area", r.f[i].area);
		blobmsg_add_string(b, "severity", diag_sev_name(r.f[i].sev));
		blobmsg_add_string(b, "text", r.f[i].text);
		if (r.f[i].samples)
			blobmsg_add_u32(b, "seconds", (uint32_t)(r.f[i].samples * cfg.why_interval));
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	if (!raw)
		return;
	a = blobmsg_open_array(b, "samples");
	for (i = 0; i < in.n; i++) {
		const struct why_min *m = &in.m[i];
		char mac[18];

		t = blobmsg_open_table(b, NULL);
		add_time(b, "t", (long)m->t);
		blobmsg_add_u8(b, "route", m->route);
		blobmsg_add_u32(b, "gw_sent", m->gw_sent);
		blobmsg_add_u32(b, "gw_lost", m->gw_lost);
		if (m->gw_ms != WHY_NONE)
			blobmsg_add_u32(b, "gw_ms", m->gw_ms);
		blobmsg_add_u32(b, "in_sent", m->in_sent);
		blobmsg_add_u32(b, "in_lost", m->in_lost);
		if (m->in_ms != WHY_NONE)
			blobmsg_add_u32(b, "in_ms", m->in_ms);
		blobmsg_add_u32(b, "dns_q", m->dns_q);
		blobmsg_add_u32(b, "dns_fail", m->dns_fail);
		if (m->dns_ms != WHY_NONE)
			blobmsg_add_u32(b, "dns_ms", m->dns_ms);
		blobmsg_add_u32(b, "down_kbps", m->down_kbps);
		blobmsg_add_u32(b, "up_kbps", m->up_kbps);
		for (j = 0; j < WHY_TOP; j++)
			if (m->top[j].kbps) {
				mac_str(m->top[j].mac, mac, sizeof(mac));
				blobmsg_add_string(b, j ? "top2" : "top", mac);
				blobmsg_add_u32(b, j ? "top2_kbps" : "top_kbps", m->top[j].kbps);
			}
		for (j = 0; j < WHY_RADIOS; j++)
			if (m->r[j].used) {
				char k[40];

				snprintf(k, sizeof(k), "wifi_%.23s", radio_if[j]);
				blobmsg_add_string(b, k, heal_status_name((enum heal_status)m->r[j].status));
			}
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
	a = blobmsg_open_array(b, "events");
	for (i = 0; i < in.ne; i++) {
		const struct why_ev *e = &in.e[i];

		t = blobmsg_open_table(b, NULL);
		add_time(b, "t", (long)e->t);
		blobmsg_add_string(b, "kind", why_kind_name(e->kind));
		blobmsg_add_string(b, "text", e->text);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

/* for the data model (Device.X_AIGW_.Timeline.) */
int why_field(const char *f, char *buf, size_t n)
{
	int i, k = 0;

	if (!strcmp(f, "Enable")) {
		snprintf(buf, n, "%s", cfg.why && ring ? "true" : "false");
		return 0;
	}
	if (!strcmp(f, "Hours")) {
		snprintf(buf, n, "%d", cfg.why_hours);
		return 0;
	}
	if (!strcmp(f, "RecordedSince")) {
		snprintf(buf, n, "%ld", ring ? recorded_since() : 0L);
		return 0;
	}
	if (!strcmp(f, "Samples")) {
		for (i = 0; ring && i < R; i++)
			k += ring[i].t != 0;
		snprintf(buf, n, "%d", k);
		return 0;
	}
	if (!strcmp(f, "Events")) {
		snprintf(buf, n, "%u", ring ? ev_n : 0);
		return 0;
	}
	return -1;
}
