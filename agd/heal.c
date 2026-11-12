/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Self-Healing Wi-Fi, part 1: measure what devices feel, learn each
 * home's normal, name what is wrong, report it (to the home and, through the
 * data model, to the ISP).
 *
 *   measure  once a second, a small ICMP probe to each Wi-Fi device that is
 *            in use (hostapd: active in the last 300 ms, or passing traffic
 *            beyond our probes). A sleeping phone
 *            answers late or not at all on a perfect link, so its probes don't
 *            count. Loss and median delay over the last 2 minutes. Works on any
 *            radio, also those that report no channel load (chan.c needs that).
 *   learn    each device's usual delay (when it is well), and how often it
 *            changes band (wifi.c tells us when it roams).
 *   diagnose every 10 s per access point (core/libag/heal.c): Good, Degraded or
 *            Poor, and why: Congestion, WeakCoverage, BandHopping, Device, Radio.
 *            A new verdict must hold for two looks (20 s); "Good" again for three.
 *   report   a log line and an ag.wifi "health" event when it changes, the
 *            data model's Device.X_AIGW_.WiFiHealth.{i} and its
 *            WiFiHealthChanged! event, `agctl wifi`.
 *
 * Part 2 acts on it (a quieter channel, the 5 GHz floor) through the guardrails.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "ag/heal.h"
#include "agd.h"

#define RING        128                 /* probes kept per device (2 minutes at 1/s) */
#define WINDOW_MS   120000
#define TIMEOUT_MS  1000
#define AWAKE_MS    300                 /* hostapd inactive_msec below this: awake */
#define EVAL_EVERY  10                  /* seconds */
#define HOP_WINDOW  600                 /* seconds */
#define HOPS_KEPT   16

struct probe {
	int64_t sent_ms;
	int rtt_ms;                     /* -2 waiting, -1 lost */
	uint16_t seq;
};

struct hdev {
	char mac[18];
	struct probe ring[RING];
	int head;
	uint16_t seq;
	double base_rtt;                /* usual delay, learned while well; 0 = not yet */
	long pkts;                      /* hostapd's rx + tx packets at the last look */
	int64_t pkts_ms;
	int64_t hops[HOPS_KEPT];        /* when it changed band */
	int nhops;
};

static struct hdev hd[DEV_MAX];
static struct heal_radio radios[WIFI_AP_MAX];
static int nradios;
static int sock = -1;
static uint16_t ident;
static struct uloop_fd ufd;
static struct uloop_timeout timer;
static int ticks;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct hdev *state_of(int i)
{
	struct hdev *h = &hd[i];

	if (strcmp(h->mac, devs[i].mac)) {             /* a new device in this slot */
		memset(h, 0, sizeof(*h));
		snprintf(h->mac, sizeof(h->mac), "%s", devs[i].mac);
	}
	return h;
}

/* wifi.c: the device is on another access point now */
void heal_roamed(const struct device *d)
{
	struct hdev *h;
	int i = (int)(d - devs);

	if (i < 0 || i >= DEV_MAX)
		return;
	h = state_of(i);
	memmove(h->hops + 1, h->hops, sizeof(h->hops[0]) * (HOPS_KEPT - 1));
	h->hops[0] = ag_wall_s();
	if (h->nhops < HOPS_KEPT)
		h->nhops++;
}

static int roams(const struct hdev *h)
{
	int i, n = 0;

	for (i = 0; i < h->nhops; i++)
		if (ag_wall_s() - h->hops[i] <= HOP_WINDOW)
			n++;
	return n;
}

/* ---------- probing ---------- */

static uint16_t csum(const void *p, size_t n)
{
	const uint16_t *w = p;
	uint32_t s = 0;

	for (; n > 1; n -= 2)
		s += *w++;
	if (n)
		s += *(const uint8_t *)w;
	s = (s >> 16) + (s & 0xffff);
	s += s >> 16;
	return (uint16_t)~s;
}

/*
 * In use, and so awake: it sent or received in the last 300 ms, or it passes
 * traffic beyond our own probe (about 2 packets a second). A phone asleep in a
 * pocket answers a probe late, or not at all, on a perfect link: not counted.
 */
static long sta_num(const char *reply, const char *key)
{
	const char *p = strstr(reply, key);

	return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static int awake(const struct device *d, struct hdev *h)
{
	char cmd[40], reply[2048];
	long ms, pk, rate = 0;
	int64_t t = now_ms();

	snprintf(cmd, sizeof(cmd), "STA %s", d->mac);
	if (wifi_cmd(d->wifi_iface, cmd, reply, sizeof(reply)) <= 0)
		return 0;
	ms = sta_num(reply, "\ninactive_msec=");
	pk = sta_num(reply, "\nrx_packets=") + sta_num(reply, "\ntx_packets=");
	if (h->pkts_ms && pk >= h->pkts && t > h->pkts_ms)
		rate = (pk - h->pkts) * 1000 / (t - h->pkts_ms);
	h->pkts = pk;
	h->pkts_ms = t;
	return (ms >= 0 && ms < AWAKE_MS) || rate >= 4;
}

static void send_probe(int i, struct hdev *h)
{
	struct {
		struct icmphdr h;
		int64_t sent;
	} pkt;
	struct sockaddr_in to = { .sin_family = AF_INET };
	struct probe *pr;

	if (inet_pton(AF_INET, devs[i].ip, &to.sin_addr) != 1)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.h.type = ICMP_ECHO;
	pkt.h.un.echo.id = htons(ident);
	pkt.h.un.echo.sequence = htons(++h->seq);
	pkt.sent = now_ms();
	pkt.h.checksum = csum(&pkt, sizeof(pkt));
	if (sendto(sock, &pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) < 0)
		return;
	pr = &h->ring[h->head];
	h->head = (h->head + 1) % RING;
	pr->sent_ms = pkt.sent;
	pr->rtt_ms = -2;
	pr->seq = h->seq;
}

static void on_reply(struct uloop_fd *u, unsigned events)
{
	char buf[256];
	struct sockaddr_in from;
	socklen_t fl;
	ssize_t n;

	(void)events;
	for (;;) {
		struct iphdr *ip = (struct iphdr *)buf;
		struct icmphdr *ic;
		char addr[INET_ADDRSTRLEN];
		struct device *d;
		struct hdev *h;
		uint16_t seq;
		int k;

		fl = sizeof(from);
		n = recvfrom(u->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		if ((size_t)n < sizeof(*ip) || (size_t)n < (size_t)ip->ihl * 4 + sizeof(*ic))
			continue;
		ic = (struct icmphdr *)(buf + ip->ihl * 4);
		if (ic->type != ICMP_ECHOREPLY || ntohs(ic->un.echo.id) != ident)
			continue;
		inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
		if (!(d = dev_find_ip(addr, 0)))
			continue;
		h = state_of((int)(d - devs));
		seq = ntohs(ic->un.echo.sequence);
		for (k = 0; k < RING; k++)
			if (h->ring[k].seq == seq && h->ring[k].rtt_ms == -2 && h->ring[k].sent_ms) {
				h->ring[k].rtt_ms = (int)(now_ms() - h->ring[k].sent_ms);
				break;
			}
	}
}

/* ---------- one device's window ---------- */

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static void window(int i, struct heal_sta *s)
{
	struct hdev *h = state_of(i);
	int64_t t = now_ms();
	int rtts[RING], nr = 0, k;

	memset(s, 0, sizeof(*s));
	for (k = 0; k < RING; k++) {
		struct probe *p = &h->ring[k];

		if (!p->sent_ms || t - p->sent_ms > WINDOW_MS)
			continue;
		if (p->rtt_ms == -2 && t - p->sent_ms > TIMEOUT_MS)
			p->rtt_ms = -1;                 /* no answer in time */
		if (p->rtt_ms == -2)
			continue;                       /* still waiting */
		s->samples++;
		if (p->rtt_ms < 0)
			s->lost++;
		else
			rtts[nr++] = p->rtt_ms;
	}
	qsort(rtts, (size_t)nr, sizeof(int), cmp_int);
	s->rtt_ms = nr ? rtts[nr / 2] : -1;
	s->base_rtt_ms = (int)(h->base_rtt + 0.5);
	s->signal = devs[i].wifi_sig_avg ? (int)(devs[i].wifi_sig_avg - 0.5) : devs[i].wifi_signal;
	s->roams = roams(h);
}

/* ---------- diagnosis per access point ---------- */

static struct heal_radio *radio_for(const char *iface, int create)
{
	int i;

	for (i = 0; i < nradios; i++)
		if (!strcmp(radios[i].iface, iface))
			return &radios[i];
	if (!create || nradios >= WIFI_AP_MAX)
		return NULL;
	memset(&radios[nradios], 0, sizeof(radios[nradios]));
	snprintf(radios[nradios].iface, sizeof(radios[nradios].iface), "%.23s", iface);
	radios[nradios].since = ag_wall_s();
	return &radios[nradios++];
}

const struct heal_radio *heal_for(const char *iface)
{
	return radio_for(iface, 0);
}

static const char *dname(const struct device *d)
{
	return d->hostname[0] ? d->hostname : d->ip[0] ? d->ip : d->mac;
}

/* for people: what is wrong, on whom, and what would help */
static void explain(struct heal_radio *r, const struct wifi_ap *ap, const struct heal_result *res,
		    const struct heal_sta *s, int *idx, int n)
{
	char where[64], who[160] = "";
	int i, worst = -1, nw = 0;

	snprintf(where, sizeof(where), "%s GHz (channel %d)", ap->band == 5 ? "5" : "2.4", ap->channel);
	for (i = 0; i < n; i++)
		if (res->affected & (1u << i)) {
			if (worst < 0 || heal_sta_loss_pct(&s[i]) > heal_sta_loss_pct(&s[worst]) || s[i].roams > s[worst].roams)
				worst = i;
			if (nw++ < 3)
				snprintf(who + strlen(who), sizeof(who) - strlen(who), "%s%s", nw > 1 ? ", " : "",
					 dname(&devs[idx[i]]));
		}
	if (nw > 3)
		snprintf(who + strlen(who), sizeof(who) - strlen(who), " and %d more", nw - 3);
	switch (res->cause) {
	case HC_CONGESTION:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: %s %s %d%% of its traffic and replies take %d ms, though the signal is fine (%d dBm): "
			 "the channel is busy (neighbouring networks or interference). A quieter channel would help.",
			 where, who, nw > 1 ? "lose" : "loses", heal_sta_loss_pct(&s[worst]), s[worst].rtt_ms, s[worst].signal);
		break;
	case HC_COVERAGE:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: %s %s %d%% of its traffic with a weak signal (%d dBm): too far from the access point, "
			 "or walls in the way. Moving the access point or its antennas would help.",
			 where, who, nw > 1 ? "lose" : "loses", heal_sta_loss_pct(&s[worst]), s[worst].signal);
		break;
	case HC_HOPPING:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: %s switched between 2.4 and 5 GHz %d times in 10 minutes: it is at the edge of 5 GHz "
			 "coverage, and its signal drops each time it moves there. Keeping it on 2.4 GHz would help.",
			 where, who, s[worst].roams);
		break;
	case HC_DEVICE:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: only %s %s %d%% of its traffic (replies %d ms, signal %d dBm); the other devices here "
			 "are fine: the trouble is likely the device itself.",
			 where, who, "loses", heal_sta_loss_pct(&s[worst]), s[worst].rtt_ms, s[worst].signal);
		break;
	case HC_RADIO:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: none of the %d devices on it answers: the radio itself may have stopped working.",
			 where, res->measured);
		break;
	default:
		if (res->status == HEAL_GOOD)
			snprintf(r->explanation, sizeof(r->explanation),
				 "%s: %d device%s answer%s well (loss %d%%, %d ms).", where, res->measured,
				 res->measured == 1 ? "" : "s", res->measured == 1 ? "s" : "", res->loss_pct,
				 res->rtt_ms < 0 ? 0 : res->rtt_ms);
		else
			snprintf(r->explanation, sizeof(r->explanation), "%s: no awake device to measure yet.", where);
	}
	/* the affected devices, as data model references */
	r->affected[0] = '\0';
	for (i = 0; i < n; i++)
		if (res->affected & (1u << i))
			snprintf(r->affected + strlen(r->affected), sizeof(r->affected) - strlen(r->affected), "%s%s",
				 r->affected[0] ? "," : "", devs[idx[i]].mac);
}

static void evaluate(void)
{
	struct heal_params p;
	int a;

	heal_params_default(&p);
	p.loss_pct = cfg.heal_loss;
	p.delay_ms = cfg.heal_delay;
	p.weak_signal = cfg.heal_weak;
	p.hops = cfg.heal_hops;
	for (a = 0; a < wifi_n_aps; a++) {
		const struct wifi_ap *ap = &wifi_aps[a];
		struct heal_radio *r = radio_for(ap->iface, 1);
		struct heal_sta s[32];
		struct heal_result res;
		int idx[32], n = 0, i, need;

		if (!r)
			continue;
		for (i = 0; i < DEV_MAX && n < 32; i++)
			if (devs[i].used && devs[i].wifi_connected && !strcmp(devs[i].wifi_iface, ap->iface)) {
				window(i, &s[n]);
				idx[n++] = i;
			}
		heal_diagnose(s, n, &p, &res);
		/* learn each well device's usual delay */
		for (i = 0; i < n; i++)
			if (s[i].samples >= p.min_samples && s[i].rtt_ms >= 0 && !heal_sta_degraded(&s[i], &p)) {
				struct hdev *h = state_of(idx[i]);

				h->base_rtt = h->base_rtt > 0 ? 0.8 * h->base_rtt + 0.2 * s[i].rtt_ms : s[i].rtt_ms;
			}
		r->measured = res.measured;
		r->loss_pct = res.loss_pct;
		r->rtt_ms = res.rtt_ms;
		/* a new verdict holds for two looks before it counts, "Good" again for three */
		if ((int)res.status != r->status || (int)res.cause != r->cause) {
			if ((int)res.status == r->pend_status && (int)res.cause == r->pend_cause)
				r->pend_n++;
			else {
				r->pend_status = (int)res.status;
				r->pend_cause = (int)res.cause;
				r->pend_n = 1;
			}
			need = res.status == HEAL_GOOD && r->status != HEAL_UNKNOWN ? 3 : 2;
			if (r->status == HEAL_UNKNOWN && res.status == HEAL_GOOD)
				need = 1;
			if (r->pend_n < need)
				continue;
			r->status = (int)res.status;
			r->cause = (int)res.cause;
			r->since = ag_wall_s();
			r->pend_n = 0;
			explain(r, ap, &res, s, idx, n);
			AG_INFO("wifi health: %s %s%s%s -- %s", ap->iface, heal_status_name(r->status),
				r->cause ? ", " : "", r->cause ? heal_cause_name(r->cause) : "", r->explanation);
			{
				struct blob_buf b = { 0 };

				blob_buf_init(&b, 0);
				blobmsg_add_string(&b, "event", "health");
				blobmsg_add_string(&b, "iface", ap->iface);
				blobmsg_add_string(&b, "status", heal_status_name(r->status));
				blobmsg_add_string(&b, "cause", heal_cause_name(r->cause));
				blobmsg_add_string(&b, "explanation", r->explanation);
				agd_emit(AG_EV_WIFI, &b);
				blob_buf_free(&b);
			}
			dm_on_heal(ap->iface);
		} else {
			r->pend_n = 0;
			explain(r, ap, &res, s, idx, n);        /* same verdict, today's numbers */
		}
	}
}

/* ---------- the clock ---------- */

static void tick(struct uloop_timeout *t)
{
	int i;

	if (cfg.heal && sock >= 0)
		for (i = 0; i < DEV_MAX; i++) {
			struct device *d = &devs[i];

			if (d->used && d->wifi_connected && d->ip[0] && d->wifi_iface[0] && awake(d, state_of(i)))
				send_probe(i, state_of(i));
		}
	if (++ticks % EVAL_EVERY == 0)
		evaluate();
	uloop_timeout_set(t, 1000);
}

void heal_init(void)
{
	if (!cfg.heal) {
		AG_INFO("wifi health: off in config (wifi.heal)");
		return;
	}
	sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (sock < 0) {
		AG_WARN("wifi health: no ICMP socket (%s): Wi-Fi devices are not measured", strerror(errno));
		return;
	}
	ident = (uint16_t)(getpid() ^ 0x4147);
	ufd.fd = sock;
	ufd.cb = on_reply;
	uloop_fd_add(&ufd, ULOOP_READ);
	timer.cb = tick;
	uloop_timeout_set(&timer, 1000);
	AG_INFO("wifi health: measuring what Wi-Fi devices feel (loss %d%%, delay %d ms, weak at %d dBm)",
		cfg.heal_loss, cfg.heal_delay, cfg.heal_weak);
}

void heal_blob(struct blob_buf *b, const char *iface)
{
	const struct heal_radio *r = heal_for(iface);
	void *t;

	if (!r)
		return;
	t = blobmsg_open_table(b, "health");
	blobmsg_add_string(b, "status", heal_status_name(r->status));
	blobmsg_add_string(b, "cause", heal_cause_name(r->cause));
	blobmsg_add_string(b, "explanation", r->explanation);
	blobmsg_add_u32(b, "measured", (uint32_t)r->measured);
	blobmsg_add_u32(b, "loss_pct", (uint32_t)r->loss_pct);
	blobmsg_add_u32(b, "rtt_ms", (uint32_t)(r->rtt_ms < 0 ? 0 : r->rtt_ms));
	blobmsg_add_u64(b, "since", (uint64_t)r->since);
	blobmsg_close_table(b, t);
}
