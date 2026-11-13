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
 * Part 2 acts on it, always through the guardrails (act.mode, the policies,
 * the hourly limit, time limits, undo):
 *   Congestion  held for wifi.heal_hold (5 min): alert "wifi-congestion" for
 *               another channel; the wifi_congestion policy makes it a
 *               "channel" decision. A trial: measured for wifi.heal_trial
 *               (3 min) on the new channel, only probes sent after the switch.
 *               Clearly better: kept (decision "kept", the channel saved in the
 *               Wi-Fi config, nothing restarted). Not: switched back (decision
 *               "tried"), that channel avoided for a day, the next try after
 *               wifi.heal_retry (30 min).
 *   BandHopping alert "wifi-hopping" for the device (once a day); the
 *               wifi_hopping policy keeps it on 2.4 GHz for a day ("stay24").
 *   WeakCoverage, Device, Radio: advice, as an alert ("wifi-coverage",
 *               "wifi-device", "wifi-radio"); nothing a gateway could switch fixes them.
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
	int weak;                       /* heal_weak_step: 0 unknown, 1 weak, 2 strong */
};

#define CAUSE_LOOKS 6                   /* a problem's cause changing must hold this many looks (1 min) */

static struct hdev hd[DEV_MAX];
static struct heal_radio radios[WIFI_AP_MAX];
static struct heal_report reps[WIFI_AP_MAX];   /* the healing report, same index as radios */
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

/*
 * wifi.c: the device is on another access point now. It counts towards
 * "hopping" when it moved by itself with a marginal 5 GHz signal: not when
 * AIGW steered it (band steering), and not with a strong signal (an access
 * point restarted, say).
 */
#define HOP_STEERED_S 60
#define HOP_MARGINAL  -65               /* dBm on 5 GHz at or below which it is marginal */

void heal_roamed(const struct device *d, int old_band, int old_signal)
{
	struct hdev *h;
	int i = (int)(d - devs), sig5;

	if (i < 0 || i >= DEV_MAX)
		return;
	if (d->steer_last && ag_wall_s() - d->steer_last <= HOP_STEERED_S)
		return;
	sig5 = d->wifi_band == 5 ? d->wifi_signal : old_band == 5 ? old_signal : 0;
	if (!sig5 || sig5 > HOP_MARGINAL)
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

/* read byte by byte: a uint16_t pointer into the packet breaks C's aliasing
   rules (diag.c's copy summed before the type was stored) */
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

/* the last 2 minutes, or only probes sent after since_ms (a channel trial) */
static void window(int i, struct heal_sta *s, int64_t since_ms)
{
	struct hdev *h = state_of(i);
	int64_t t = now_ms();
	int rtts[RING], nr = 0, k;

	memset(s, 0, sizeof(*s));
	for (k = 0; k < RING; k++) {
		struct probe *p = &h->ring[k];

		if (!p->sent_ms || t - p->sent_ms > WINDOW_MS || p->sent_ms < since_ms)
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
	/* weak with a margin, so a signal at the line doesn't flip the cause */
	h->weak = heal_weak_step(h->weak, s->signal, cfg.heal_weak, cfg.heal_weak_margin);
	s->weak = h->weak;
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
	memset(&reps[nradios], 0, sizeof(reps[nradios]));
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
	char where[64], who[160] = "", delay[48] = "";
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
	/* a delay worth naming (not "replies take 0 ms") */
	if (worst >= 0 && s[worst].rtt_ms >= 5)
		snprintf(delay, sizeof(delay), " and replies take %d ms", s[worst].rtt_ms);
	switch (res->cause) {
	case HC_CONGESTION:
		snprintf(r->explanation, sizeof(r->explanation),
			 "%s: %s %s %d%% of its traffic%s, though the signal is fine (%d dBm): "
			 "the channel is busy (neighbouring networks or interference). A quieter channel would help.",
			 where, who, nw > 1 ? "lose" : "loses", heal_sta_loss_pct(&s[worst]), delay, s[worst].signal);
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
			 "%s: only %s %s %d%% of its traffic%s (signal %d dBm); the other devices here "
			 "are fine: the trouble is likely the device itself.",
			 where, who, "loses", heal_sta_loss_pct(&s[worst]), delay, s[worst].signal);
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

/* ---------- part 2: acting on it ---------- */

#define TRIAL_GRACE_MS 20000            /* after a switch, devices reconnect: not counted */
#define BAD_FOR        86400            /* a channel that didn't help is avoided for a day */
#define BAD_KEPT       8
#define BEFORE_FOR_MS  180000           /* the last measured look stands for "before" this long */

struct trial {
	int64_t last_try;               /* wall s: last time a trial was asked for */
	int64_t start_ms;               /* monotonic: the switch took effect; 0 = no trial */
	uint32_t id;                    /* its decision */
	int from, to;                   /* channels */
	int before_loss, before_rtt;    /* what devices felt before */
	int64_t fresh_ms;               /* monotonic: the last trial was judged; the verdict counts only
	                                   probes after it (the window still held the old channel's) */
	int last_loss, last_rtt;        /* the last look that measured someone, and when (monotonic): */
	int64_t last_ms;                /* "before", when the switch itself left nobody to measure */
	struct { int ch; int64_t until; } bad[BAD_KEPT];
};

static struct trial trials[WIFI_AP_MAX];

static int is_bad(const struct trial *t, int ch)
{
	int i;

	for (i = 0; i < BAD_KEPT; i++)
		if (t->bad[i].ch == ch && t->bad[i].until > ag_wall_s())
			return 1;
	return 0;
}

static void mark_bad(struct trial *t, int ch)
{
	int i, k = 0;

	for (i = 0; i < BAD_KEPT; i++)
		if (t->bad[i].ch == ch || t->bad[i].until < t->bad[k].until)
			k = i;
	t->bad[k].ch = ch;
	t->bad[k].until = ag_wall_s() + BAD_FOR;
}

static void collect(const struct wifi_ap *ap, struct heal_sta *s, int *idx, int *n, int64_t since_ms)
{
	int i;

	*n = 0;
	for (i = 0; i < DEV_MAX && *n < 32; i++)
		if (devs[i].used && devs[i].wifi_connected && !strcmp(devs[i].wifi_iface, ap->iface)) {
			window(i, &s[*n], since_ms);
			idx[(*n)++] = i;
		}
}

static void heal_event(const char *what, const struct wifi_ap *ap, const char *text)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "iface", ap->iface);
	blobmsg_add_string(&b, "explanation", text);
	agd_emit(AG_EV_WIFI, &b);
	blob_buf_free(&b);
}

/* the trial on this access point: started, judged, or a new one asked for */
static void trial_step(int a, const struct wifi_ap *ap, struct heal_radio *r, const struct heal_result *now_res,
		       const struct heal_params *p)
{
	struct trial *t = &trials[a];
	uint32_t id = 0;
	int active = 0, ours = 0, from = 0,
	    has = act_channel_for(ap->iface, "wifi_congestion", &id, &active, &ours, &from) == 0;
	int64_t wall = ag_wall_s();

	if (t->start_ms) {                              /* a trial is running */
		struct heal_sta s[32];
		struct heal_result after;
		int idx[32], n, elapsed = (int)((now_ms() - t->start_ms) / 1000), keep;
		char note[200], text[400];

		if (!has || !active || id != t->id) {   /* undone or expired meanwhile: not ours to judge */
			t->start_ms = 0;
			return;
		}
		if (elapsed < cfg.heal_trial)
			return;
		collect(ap, s, idx, &n, t->start_ms + TRIAL_GRACE_MS);
		heal_diagnose(s, n, p, &after);
		if (!after.measured && elapsed < 2 * cfg.heal_trial)
			return;                         /* nobody awake to measure yet: a little longer */
		/* "before" unknown (-1): kept only if devices are now plainly fine */
		keep = after.measured &&
		       (t->before_loss < 0 ? after.status == HEAL_GOOD :
			after.loss_pct <= t->before_loss &&
			(after.status == HEAL_GOOD || (t->before_loss > 0 && after.loss_pct * 2 <= t->before_loss) ||
			 (t->before_rtt > 0 && after.rtt_ms >= 0 && after.rtt_ms * 2 <= t->before_rtt)));
		if (keep) {
			struct heal_day *day = heal_report_day(&reps[a], (long)wall);
			char ch[8];
			/* unknown before: counted as no change, so the report never claims a gain it didn't see */
			int bl = t->before_loss < 0 ? after.loss_pct : t->before_loss,
			    br = t->before_loss < 0 ? after.rtt_ms : t->before_rtt;

			day->kept++;
			day->fixes++;
			day->fix_secs += r->problem_since ? (unsigned)(wall - r->problem_since) : 0;
			day->loss_before += (unsigned)bl;
			day->loss_after += (unsigned)after.loss_pct;
			day->rtt_before += br > 0 ? (unsigned)br : 0;
			day->rtt_after += after.rtt_ms > 0 ? (unsigned)after.rtt_ms : 0;
			if (t->before_loss < 0)
				snprintf(note, sizeof(note), "kept: on channel %d devices lose %d%% (%d ms), "
					 "before not measured", t->to, after.loss_pct, after.rtt_ms < 0 ? 0 : after.rtt_ms);
			else
				snprintf(note, sizeof(note), "kept: on channel %d devices lose %d%% (%d ms), before %d%% (%d ms)",
					 t->to, after.loss_pct, after.rtt_ms < 0 ? 0 : after.rtt_ms, t->before_loss,
					 t->before_rtt < 0 ? 0 : t->before_rtt);
			act_keep(t->id, note);
			snprintf(ch, sizeof(ch), "%d", t->to);
			wifiset_save(ap->iface, WS_CHANNEL, ch, "Self-Healing Wi-Fi");
			if (t->before_loss < 0)
				snprintf(text, sizeof(text), "Channel %d -> %d worked (devices lose %d%%): kept and saved.",
					 t->from, t->to, after.loss_pct);
			else
				snprintf(text, sizeof(text), "Channel %d -> %d worked (devices lose %d%%, before %d%%): kept and saved.",
					 t->from, t->to, after.loss_pct, t->before_loss);
		} else {
			if (after.measured && t->before_loss < 0)
				snprintf(note, sizeof(note), "tried: on channel %d devices lose %d%% (%d ms), before not measured; "
					 "back to %d", t->to, after.loss_pct, after.rtt_ms < 0 ? 0 : after.rtt_ms, t->from);
			else if (after.measured)
				snprintf(note, sizeof(note), "tried: on channel %d devices lose %d%% (%d ms), before %d%% (%d ms); "
					 "back to %d", t->to, after.loss_pct, after.rtt_ms < 0 ? 0 : after.rtt_ms, t->before_loss,
					 t->before_rtt < 0 ? 0 : t->before_rtt, t->from);
			else
				snprintf(note, sizeof(note), "tried: no device could be measured on channel %d; back to %d", t->to,
					 t->from);
			act_undo_because(t->id, note);
			mark_bad(t, t->to);
			heal_report_day(&reps[a], (long)wall)->tried++;
			snprintf(text, sizeof(text), "Channel %d -> %d did not help: switched back; channel %d is avoided for a day.",
				 t->from, t->to, t->to);
		}
		AG_INFO("wifi health: %s %s", ap->iface, text);
		heal_event(keep ? "trial-kept" : "trial-tried", ap, text);
		t->start_ms = 0;
		t->last_try = wall;
		/* from here on, judged on what devices feel now: not the old channel's probes
		   still in the window, and a new hold before the next trial */
		t->fresh_ms = now_ms();
		if (r->cause_since)
			r->cause_since = wall;
		return;
	}
	if (has) {
		/* where it switches back to: the decision knows, even after agd restarted
		   between the proposal and a person's approval */
		if (from > 0)
			t->from = from;
		/* the switch is in effect: the trial starts now, against what devices felt before
		   (the 2-minute window is still nearly all before the switch). A radio that
		   restarts to switch drops everyone for a moment: then the last look that
		   measured someone, if recent, else "not measured" */
		if (active && ours && ap->channel != t->from) {
			int64_t now = now_ms();

			t->start_ms = now;
			t->id = id;
			t->to = ap->channel;
			if (now_res->measured) {
				t->before_loss = now_res->loss_pct;
				t->before_rtt = now_res->rtt_ms;
			} else if (t->last_ms && now - t->last_ms <= BEFORE_FOR_MS) {
				t->before_loss = t->last_loss;
				t->before_rtt = t->last_rtt;
			} else {
				t->before_loss = t->before_rtt = -1;
			}
			if (t->before_loss < 0)
				AG_INFO("wifi health: %s trial on channel %d (from %d): measuring for %d s (before: not measured)",
					ap->iface, t->to, t->from, cfg.heal_trial);
			else
				AG_INFO("wifi health: %s trial on channel %d (from %d): measuring for %d s (before: loss %d%%, %d ms)",
					ap->iface, t->to, t->from, cfg.heal_trial, t->before_loss,
					t->before_rtt < 0 ? 0 : t->before_rtt);
		}
		return;                                 /* proposed, waiting for a person, or someone else's */
	}
	if (r->status == HEAL_GOOD || r->status == HEAL_UNKNOWN || r->cause != HC_CONGESTION ||
	    wall - r->cause_since < cfg.heal_hold || (t->last_try && wall - t->last_try < cfg.heal_retry))
		return;
	{
		int opts[16], no = chan_options(ap, opts, 16), i, pick = 0;
		char target[40], reason[640];

		/* not one that failed in the last day, nor one proposed in it (its alert
		   wouldn't be raised again), even if agd restarted since */
		for (i = 0; i < no && !pick; i++) {
			snprintf(target, sizeof(target), "%.23s:%d", ap->iface, opts[i]);
			if (!is_bad(t, opts[i]) && !act_channel_recent(target, wall - BAD_FOR))
				pick = opts[i];
		}
		t->last_try = wall;
		if (!pick) {
			AG_INFO("wifi health: %s congested, but every other channel was tried in the last day", ap->iface);
			return;
		}
		t->from = ap->channel;
		snprintf(target, sizeof(target), "%.23s:%d", ap->iface, pick);
		snprintf(reason, sizeof(reason),
			 "%.400s For %d minutes now. A trial on channel %d: measured for %d s, kept if devices do clearly "
			 "better, else switched back.", r->explanation, (int)((wall - r->cause_since) / 60), pick, cfg.heal_trial);
		alerts_raise("wifi-congestion", "info", target, ap->iface, now_res->loss_pct / 100.0, reason);
	}
}

/* a new verdict: the alert that goes with it (once a day per device and kind, alerts.c) */
static void verdict_alerts(const struct wifi_ap *ap, const struct heal_radio *r, const struct heal_result *res,
			   const struct heal_sta *s, const int *idx, int n, const struct heal_params *p)
{
	const char *kind = r->cause == HC_COVERAGE ? "wifi-coverage" : r->cause == HC_DEVICE ? "wifi-device" :
			   r->cause == HC_RADIO ? "wifi-radio" : r->cause == HC_HOPPING ? "wifi-hopping" : NULL;
	int i;

	if (!kind || r->status == HEAL_GOOD)
		return;
	if (r->cause == HC_RADIO) {
		alerts_raise(kind, "warning", ap->iface, ap->iface, 1.0, r->explanation);
		return;
	}
	for (i = 0; i < n; i++) {
		const struct device *d = &devs[idx[i]];
		char reason[400];

		if (!(res->affected & (1u << i)) || !d->ip[0])
			continue;
		if (r->cause == HC_HOPPING) {
			if (!cfg.heal_act || s[i].roams < p->hops)
				continue;
			snprintf(reason, sizeof(reason),
				 "%s switched between 2.4 and 5 GHz %d times in 10 minutes, its 5 GHz signal marginal: keeping "
				 "it on 2.4 GHz for a day gives it a steady connection.", dname(d), s[i].roams);
			alerts_raise(kind, "info", d->ip, "5 GHz edge", s[i].roams, reason);
		} else {
			alerts_raise(kind, "info", d->ip, ap->iface, heal_sta_loss_pct(&s[i]) / 100.0, r->explanation);
		}
	}
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
		int idx[32], n = 0, i, need, was_bad, now_bad;

		if (!r)
			continue;
		/* the report: the last 10 s were in the verdict that held */
		heal_report_day(&reps[r - radios], (long)ag_wall_s())->secs[r->status & 3] += EVAL_EVERY;
		collect(ap, s, idx, &n, trials[r - radios].fresh_ms);
		heal_diagnose(s, n, &p, &res);
		if (cfg.heal_act)
			trial_step((int)(r - radios), ap, r, &res, &p);
		if (res.measured && !trials[r - radios].start_ms) {     /* "before" for a trial to come */
			trials[r - radios].last_loss = res.loss_pct;
			trials[r - radios].last_rtt = res.rtt_ms;
			trials[r - radios].last_ms = now_ms();
		}
		/* learn each well device's usual delay */
		for (i = 0; i < n; i++)
			if (s[i].samples >= p.min_samples && s[i].rtt_ms >= 0 && !heal_sta_degraded(&s[i], &p)) {
				struct hdev *h = state_of(idx[i]);

				h->base_rtt = h->base_rtt > 0 ? 0.8 * h->base_rtt + 0.2 * s[i].rtt_ms : s[i].rtt_ms;
			}
		/* just after a trial, too few new probes yet: the verdict stays as it is */
		if (res.status == HEAL_UNKNOWN && trials[r - radios].fresh_ms &&
		    now_ms() - trials[r - radios].fresh_ms < 60000)
			continue;
		r->measured = res.measured;
		r->loss_pct = res.loss_pct;
		r->rtt_ms = res.rtt_ms;
		/* a new verdict holds for two looks before it counts, "Good" again for three */
		if ((int)res.status != r->status || (int)res.cause != r->cause) {
			was_bad = r->status == HEAL_DEGRADED || r->status == HEAL_POOR;
			now_bad = res.status == HEAL_DEGRADED || res.status == HEAL_POOR;
			/* the same pending verdict again; for a new cause, Degraded or Poor alike */
			if ((int)res.cause == r->pend_cause &&
			    ((int)res.status == r->pend_status ||
			     (now_bad && (r->pend_status == HEAL_DEGRADED || r->pend_status == HEAL_POOR) &&
			      (int)res.cause != r->cause)))
				r->pend_n++;
			else
				r->pend_n = 1;
			r->pend_status = (int)res.status;
			r->pend_cause = (int)res.cause;
			need = res.status == HEAL_GOOD && r->status != HEAL_UNKNOWN ? 3 : 2;
			if (r->status == HEAL_UNKNOWN && res.status == HEAL_GOOD)
				need = 1;
			/* still a problem, another cause: only when it holds a minute (a signal
			   near the line, a burst of hops, shouldn't keep renaming it) */
			if (was_bad && now_bad && (int)res.cause != r->cause)
				need = CAUSE_LOOKS;
			if (r->pend_n < need)
				continue;
			if (now_bad && (!was_bad || (int)res.cause != r->cause)) {
				heal_report_day(&reps[r - radios], (long)ag_wall_s())->found[res.cause % HEAL_CAUSES]++;
				r->cause_since = ag_wall_s();
			}
			if (now_bad && !was_bad)
				r->problem_since = ag_wall_s();
			if (!now_bad)
				r->problem_since = r->cause_since = 0;
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
			verdict_alerts(ap, r, &res, s, idx, n, &p);
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
	{
		static const struct { const char *name; int days; } per[] = { { "day", 1 }, { "week", 7 }, { "month", 30 } };
		void *rt = blobmsg_open_table(b, "report");
		unsigned k;

		for (k = 0; k < sizeof(per) / sizeof(per[0]); k++) {
			struct heal_day s;
			void *pt;
			int c;

			heal_report_sum(&reps[r - radios], (long)ag_wall_s(), per[k].days, &s);
			pt = blobmsg_open_table(b, per[k].name);
			blobmsg_add_u32(b, "good_s", s.secs[HEAL_GOOD]);
			blobmsg_add_u32(b, "degraded_s", s.secs[HEAL_DEGRADED]);
			blobmsg_add_u32(b, "poor_s", s.secs[HEAL_POOR]);
			if (heal_day_good_pct(&s) >= 0)
				blobmsg_add_u32(b, "good_pct", (uint32_t)heal_day_good_pct(&s));
			blobmsg_add_u32(b, "problems", heal_day_problems(&s));
			for (c = 1; c < HEAL_CAUSES; c++)
				blobmsg_add_u32(b, heal_cause_name(c), s.found[c]);
			blobmsg_add_u32(b, "trials_kept", s.kept);
			blobmsg_add_u32(b, "trials_tried", s.tried);
			blobmsg_add_u32(b, "band_rules", s.band_rules);
			blobmsg_add_u32(b, "fixes", s.fixes);
			if (s.fixes)
				blobmsg_add_u32(b, "time_to_fix_s", s.fix_secs / s.fixes);
			if (s.kept) {
				blobmsg_add_u32(b, "loss_before_pct", s.loss_before / s.kept);
				blobmsg_add_u32(b, "loss_after_pct", s.loss_after / s.kept);
				blobmsg_add_u32(b, "rtt_before_ms", s.rtt_before / s.kept);
				blobmsg_add_u32(b, "rtt_after_ms", s.rtt_after / s.kept);
			}
			blobmsg_close_table(b, pt);
		}
		blobmsg_close_table(b, rt);
	}
	blobmsg_close_table(b, t);
}

/* ---------- the healing report ---------- */

/* Diagnose: what this device felt over the last 2 minutes (samples 0: not measured) */
void heal_device(const struct device *d, int *loss_pct, int *rtt_ms, int *samples)
{
	struct heal_sta s;
	int i = (int)(d - devs);

	*loss_pct = 0;
	*rtt_ms = -1;
	*samples = 0;
	if (sock < 0 || i < 0 || i >= DEV_MAX)
		return;
	window(i, &s, 0);
	*samples = s.samples;
	*loss_pct = heal_sta_loss_pct(&s);
	*rtt_ms = s.rtt_ms;
}

int heal_report_of(const char *iface, int days, struct heal_day *out)
{
	const struct heal_radio *r = heal_for(iface);

	if (!r)
		return -1;
	heal_report_sum(&reps[r - radios], (long)ag_wall_s(), days, out);
	return 0;
}

/* act.c: stay24 in effect for a device: counted on the access point whose hopping it fixes */
void heal_on_band_rule(const char *mac, uint32_t decision_id)
{
	static uint32_t seen[16];
	static int nseen;
	const struct device *d = dev_find_mac(mac, 0);
	struct heal_radio *r = NULL;
	struct heal_day *day;
	int64_t wall = ag_wall_s();
	int i;

	for (i = 0; i < 16; i++)
		if (seen[i] == decision_id)
			return;                         /* its time limit restarted: counted already */
	seen[nseen++ % 16] = decision_id;
	for (i = 0; i < nradios && !r; i++)
		if (radios[i].cause == HC_HOPPING && strcasestr(radios[i].affected, mac))
			r = &radios[i];
	if (!r && d && d->wifi_iface[0])
		r = radio_for(d->wifi_iface, 1);
	if (!r)
		return;
	day = heal_report_day(&reps[r - radios], (long)wall);
	day->band_rules++;
	day->fixes++;
	day->fix_secs += r->problem_since ? (unsigned)(wall - r->problem_since) : 0;
}

/* saved with agd's state (devices.c): per access point, the days with anything in them */
void heal_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "wifi_report");
	int i, k;

	for (i = 0; i < nradios; i++) {
		void *arr = blobmsg_open_array(b, radios[i].iface);

		for (k = 0; k < HEAL_DAYS; k++) {
			const struct heal_day *d = &reps[i].d[k];
			void *row;
			int c;

			if (!d->day)
				continue;
			row = blobmsg_open_array(b, NULL);
			blobmsg_add_u64(b, NULL, (uint64_t)d->day);
			for (c = 0; c < 4; c++)
				blobmsg_add_u32(b, NULL, d->secs[c]);
			for (c = 0; c < HEAL_CAUSES; c++)
				blobmsg_add_u32(b, NULL, d->found[c]);
			blobmsg_add_u32(b, NULL, d->kept);
			blobmsg_add_u32(b, NULL, d->tried);
			blobmsg_add_u32(b, NULL, d->band_rules);
			blobmsg_add_u32(b, NULL, d->fixes);
			blobmsg_add_u32(b, NULL, d->fix_secs);
			blobmsg_add_u32(b, NULL, d->loss_before);
			blobmsg_add_u32(b, NULL, d->loss_after);
			blobmsg_add_u32(b, NULL, d->rtt_before);
			blobmsg_add_u32(b, NULL, d->rtt_after);
			blobmsg_close_array(b, row);
		}
		blobmsg_close_array(b, arr);
	}
	blobmsg_close_table(b, t);
}

static unsigned long long jnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (unsigned long long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (unsigned long long)blobmsg_get_double(a);
	default: return 0;
	}
}

void heal_load(struct blob_attr *t)
{
	struct blob_attr *ap, *row, *v;
	size_t r1, r2, r3;
	int n = 0;

	blobmsg_for_each_attr(ap, t, r1) {
		struct heal_radio *r;

		if (blobmsg_type(ap) != BLOBMSG_TYPE_ARRAY || !(r = radio_for(blobmsg_name(ap), 1)))
			continue;
		blobmsg_for_each_attr(row, ap, r2) {
			unsigned long long x[4 + HEAL_CAUSES + 10];
			struct heal_day *d;
			int k = 0, c;

			if (blobmsg_type(row) != BLOBMSG_TYPE_ARRAY)
				continue;
			memset(x, 0, sizeof(x));
			blobmsg_for_each_attr(v, row, r3)
				if (k < (int)(sizeof(x) / sizeof(x[0])))
					x[k++] = jnum(v);
			if (!x[0])
				continue;
			d = &reps[r - radios].d[x[0] % HEAL_DAYS];
			memset(d, 0, sizeof(*d));
			d->day = (long)x[0];
			for (c = 0; c < 4; c++)
				d->secs[c] = (unsigned)x[1 + c];
			for (c = 0; c < HEAL_CAUSES; c++)
				d->found[c] = (unsigned)x[5 + c];
			k = 5 + HEAL_CAUSES;
			d->kept = (unsigned)x[k++];
			d->tried = (unsigned)x[k++];
			d->band_rules = (unsigned)x[k++];
			d->fixes = (unsigned)x[k++];
			d->fix_secs = (unsigned)x[k++];
			d->loss_before = (unsigned)x[k++];
			d->loss_after = (unsigned)x[k++];
			d->rtt_before = (unsigned)x[k++];
			d->rtt_after = (unsigned)x[k++];
			n++;
		}
	}
	if (n)
		AG_INFO("wifi health: restored %d days of the healing report", n);
}
