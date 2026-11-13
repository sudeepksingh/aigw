/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Diagnose: "my internet is slow", answered in about 3 seconds, for the
 * person at home (agctl diagnose) and the ISP's help desk (Device.X_AIGW_.
 * Diagnose() over USP; over TR-069 through Device.X_AIGW_.Request.).
 *
 * It measures what the gateway can see:
 *   - the line: the default route, and echo requests to its next hop (the
 *     modem or the ISP's router) and to the internet (diag.targets; the best
 *     one counts, as some don't answer echo requests)
 *   - names: one query to the gateway's own DNS (agdns), for diag.name
 *   - Wi-Fi: Self-Healing Wi-Fi's verdict on every access point, and what it
 *     did about it today
 *   - one device, when asked: is it connected, its signal, what it feels
 *     (Self-Healing Wi-Fi's probes), and whether AIGW itself keeps it off or
 *     slows it (an active decision)
 * and lets core/libag/diag.c name the cause.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/diag.h"
#include "ag/heal.h"
#include "agd.h"

#define TARGETS   5                     /* [0] the next hop, then up to 4 on the internet */
#define ROUNDS    10                    /* one echo request to each, every 200 ms */
#define ROUND_MS  200
#define LINGER_MS 1000                  /* after the last round, for late answers */
#define DNS_MS    2500
#define WAITERS   8

struct waiter {
	diag_cb cb;
	void *ctx;
};

static struct {
	int running;
	char host[64];
	char mac[18];                   /* the device asked about, once found */
	struct waiter w[WAITERS];
	int nw;
	int icmp, udp;
	struct uloop_fd ufd_icmp, ufd_udp;
	struct uloop_timeout timer;
	int round;
	uint16_t ident, dns_id;
	int64_t start_ms, dns_sent_ms;
	struct in_addr tgt[TARGETS];
	int ntgt;                       /* tgt[0] unused when there is no next hop */
	int64_t sent[TARGETS][ROUNDS];
	int rtt[TARGETS][ROUNDS];       /* -1 no answer */
	struct diag_input in;
} D;

static struct diag_input last_in;
static struct diag_result last_res;
static char last_host[64];
static int64_t last_at;

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the Internet checksum, read byte by byte: a uint16_t pointer into the packet
   breaks C's aliasing rules, and the optimiser may then sum it before the
   type is stored (seen here: every request dropped for a bad checksum) */
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

/* the default route's next hop, from the kernel's table ("" with a route but no next hop) */
static int default_route(char *gw, size_t n)
{
	char line[256], ifc[32];
	unsigned long dst, via, flags;
	FILE *f = fopen("/proc/net/route", "r");
	int found = 0;

	gw[0] = '\0';
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %lx %lx %lx", ifc, &dst, &via, &flags) == 4 && dst == 0 && (flags & 1)) {
			struct in_addr a = { .s_addr = (in_addr_t)via };

			found = 1;
			if (via)
				inet_ntop(AF_INET, &a, gw, (socklen_t)n);
			break;
		}
	fclose(f);
	return found;
}

static void send_round(void)
{
	int t;

	for (t = 0; t < D.ntgt; t++) {
		struct { struct icmphdr h; uint32_t pad; } pkt;
		struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = D.tgt[t] };

		if (!D.tgt[t].s_addr)
			continue;
		memset(&pkt, 0, sizeof(pkt));
		pkt.h.type = ICMP_ECHO;
		pkt.h.un.echo.id = htons(D.ident);
		pkt.h.un.echo.sequence = htons((uint16_t)(t << 8 | D.round));
		pkt.h.checksum = csum(&pkt, sizeof(pkt));
		D.sent[t][D.round] = mono_ms();
		if (sendto(D.icmp, &pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) < 0)
			D.sent[t][D.round] = 0;  /* not sent: no route there, say */
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
		if (ic->type != ICMP_ECHOREPLY || ntohs(ic->un.echo.id) != D.ident)
			continue;
		t = ntohs(ic->un.echo.sequence) >> 8;
		r = ntohs(ic->un.echo.sequence) & 0xff;
		if (t < TARGETS && r < ROUNDS && D.sent[t][r] && D.rtt[t][r] < 0 && ip->saddr == D.tgt[t].s_addr)
			D.rtt[t][r] = (int)(mono_ms() - D.sent[t][r]);
	}
}

/* ---------- names ---------- */

static void send_dns(void)
{
	unsigned char q[300];
	size_t k = 12;
	const char *p = cfg.diag_name;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg.dns_port) };

	if (!cfg.dns_listen[0] || !strcmp(cfg.dns_listen, "0.0.0.0") || inet_pton(AF_INET, cfg.dns_listen, &to.sin_addr) != 1)
		inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
	memset(q, 0, sizeof(q));
	D.dns_id = (uint16_t)(D.ident ^ (uint16_t)mono_ms());
	q[0] = (unsigned char)(D.dns_id >> 8);
	q[1] = (unsigned char)D.dns_id;
	q[2] = 0x01;                            /* recursion desired */
	q[5] = 1;                               /* one question */
	while (*p && k < sizeof(q) - 6) {
		size_t l = strcspn(p, ".");

		if (!l || l > 63 || k + l + 1 >= sizeof(q) - 6)
			return;
		q[k++] = (unsigned char)l;
		memcpy(q + k, p, l);
		k += l;
		p += l;
		if (*p == '.')
			p++;
	}
	q[k++] = 0;
	q[k++] = 0, q[k++] = 1;                 /* A */
	q[k++] = 0, q[k++] = 1;                 /* IN */
	D.dns_sent_ms = mono_ms();
	if (sendto(D.udp, q, k, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) < 0)
		D.dns_sent_ms = 0;
	D.in.dns = 0;
}

static void on_udp(struct uloop_fd *u, unsigned ev)
{
	unsigned char a[512];
	ssize_t n;

	(void)ev;
	while ((n = recv(u->fd, a, sizeof(a), MSG_DONTWAIT)) >= 0)
		if (n >= 12 && D.dns_sent_ms && (uint16_t)(a[0] << 8 | a[1]) == D.dns_id && D.in.dns != 1 &&
		    mono_ms() - D.dns_sent_ms <= DNS_MS) {
			D.in.dns = 1;
			D.in.dns_rcode = a[3] & 0x0f;
			D.in.dns_ms = (int)(mono_ms() - D.dns_sent_ms);
		}
}

/* ---------- what else the gateway knows ---------- */

static void ping_result(int t, struct diag_ping *p)
{
	int r, v[ROUNDS];

	memset(p, 0, sizeof(*p));
	p->rtt_ms = -1;
	for (r = 0; r < ROUNDS; r++) {
		v[r] = -1;
		if (!D.sent[t][r])
			continue;
		p->sent++;
		if (D.rtt[t][r] < 0)
			p->lost++;
		else
			v[r] = D.rtt[t][r];
	}
	p->rtt_ms = diag_median(v, ROUNDS);
}

static void gather(void)
{
	struct diag_input *in = &D.in;
	struct device *d = NULL;
	int i, t;

	ping_result(0, &in->gw_ping);
	/* the internet: the target that did best (least lost, then fastest) */
	for (t = 1; t < D.ntgt; t++) {
		struct diag_ping p;

		ping_result(t, &p);
		if (!p.sent)
			continue;
		if (!in->inet.sent || p.lost < in->inet.lost ||
		    (p.lost == in->inet.lost && p.rtt_ms >= 0 && (in->inet.rtt_ms < 0 || p.rtt_ms < in->inet.rtt_ms)))
			in->inet = p;
	}
	if (!in->inet.sent && D.ntgt > 1)
		in->inet.sent = ROUNDS, in->inet.lost = ROUNDS, in->inet.rtt_ms = -1;  /* nothing could be sent: no way there */
	if (cfg.diag_name[0] && !D.dns_sent_ms)
		in->dns = 0;                    /* asked, but the query couldn't be sent */
	in->weak_signal = cfg.heal_weak;
	in->loss_pct = cfg.heal_loss;
	for (i = 0; i < wifi_n_aps && in->nradios < DIAG_RADIOS; i++) {
		const struct wifi_ap *ap = &wifi_aps[i];
		const struct heal_radio *h = heal_for(ap->iface);
		struct diag_radio *r = &in->radios[in->nradios++];
		struct heal_day day;

		snprintf(r->name, sizeof(r->name), "%.23s", ap->iface);
		r->band = ap->band;
		r->channel = ap->channel;
		if (h) {
			r->status = h->status;
			r->cause = h->cause;
			r->loss_pct = h->loss_pct;
			r->rtt_ms = h->rtt_ms;
			snprintf(r->explanation, sizeof(r->explanation), "%.299s", h->explanation);
		}
		if (heal_report_of(ap->iface, 1, &day) == 0) {
			r->kept_today = (int)day.kept;
			r->tried_today = (int)day.tried;
		}
	}
	in->host.radio = -1;
	in->host.seen_ago = -1;
	if (!D.host[0])
		return;
	in->host.given = 1;
	d = act_find_device(D.host);
	snprintf(in->host.name, sizeof(in->host.name), "%s", D.host);
	if (!d)
		return;
	in->host.found = 1;
	snprintf(D.mac, sizeof(D.mac), "%s", d->mac);
	snprintf(in->host.name, sizeof(in->host.name), "%s", d->hostname[0] ? d->hostname : d->mac);
	in->host.connected = d->wifi_connected;
	in->host.seen_ago = d->last_seen ? (int)(ag_wall_s() - d->last_seen) : -1;
	if (d->present && in->host.seen_ago > 300)
		in->host.seen_ago = 0;          /* on the wire (presence), if not on Wi-Fi */
	if (d->wifi_connected) {
		in->host.signal = (int)(d->wifi_sig_avg - (d->wifi_sig_avg < 0 ? 0.5 : -0.5));
		for (i = 0; i < in->nradios; i++)
			if (!strcmp(in->radios[i].name, d->wifi_iface))
				in->host.radio = i;
		heal_device(d, &in->host.loss_pct, &in->host.rtt_ms, &in->host.samples);
	}
	act_on_device(d, in->host.actions, sizeof(in->host.actions), &in->host.blocked, &in->host.limited,
		      &in->host.on24);
}

static void finish(void)
{
	struct diag_result res;
	struct diag_input in;
	int i, nw = D.nw;
	struct waiter w[WAITERS];

	uloop_timeout_cancel(&D.timer);
	if (D.icmp >= 0) {
		uloop_fd_delete(&D.ufd_icmp);
		close(D.icmp);
	}
	if (D.udp >= 0) {
		uloop_fd_delete(&D.ufd_udp);
		close(D.udp);
	}
	gather();
	diag_judge(&D.in, &res);
	in = D.in;
	memcpy(w, D.w, sizeof(w));
	last_in = in;
	last_res = res;
	snprintf(last_host, sizeof(last_host), "%s", D.host);
	last_at = ag_wall_s();
	AG_INFO("diagnose%s%s: %s -- %s", D.host[0] ? " " : "", D.host, res.n ? res.f[0].code : "OK",
		res.n ? res.f[0].text : "");
	D.running = 0;
	D.nw = 0;
	dm_on_diag(&res, &in, D.mac);
	for (i = 0; i < nw; i++)
		w[i].cb(w[i].ctx, &res, &in);
}

static void tick(struct uloop_timeout *t)
{
	if (D.round < ROUNDS) {
		send_round();
		D.round++;
		uloop_timeout_set(t, D.round < ROUNDS ? ROUND_MS : LINGER_MS);
		return;
	}
	finish();
}

int diag_start(const char *host, diag_cb cb, void *ctx, char *err, size_t errlen)
{
	char gw[48], tl[sizeof(cfg.diag_targets)], *s, *save;
	int t, r;

	host = host ? host : "";
	if (D.running) {
		if (strcmp(D.host, host) || D.nw >= WAITERS) {
			snprintf(err, errlen, "a diagnosis is running; ask again in a few seconds");
			return -1;
		}
		D.w[D.nw].cb = cb;
		D.w[D.nw++].ctx = ctx;  /* the same question: the same answer */
		return 0;
	}
	memset(&D, 0, sizeof(D));
	D.icmp = D.udp = -1;
	snprintf(D.host, sizeof(D.host), "%s", host);
	D.w[0].cb = cb;
	D.w[0].ctx = ctx;
	D.nw = 1;
	D.ident = (uint16_t)(getpid() ^ 0x4447);
	D.in.dns = -1;
	D.in.route = default_route(gw, sizeof(gw));
	snprintf(D.in.gw, sizeof(D.in.gw), "%s", gw);
	if (gw[0])
		inet_pton(AF_INET, gw, &D.tgt[0]);
	D.ntgt = 1;
	snprintf(tl, sizeof(tl), "%s", cfg.diag_targets);
	for (s = strtok_r(tl, " ,", &save); s && D.ntgt < TARGETS; s = strtok_r(NULL, " ,", &save))
		if (inet_pton(AF_INET, s, &D.tgt[D.ntgt]) == 1)
			D.ntgt++;
	for (t = 0; t < TARGETS; t++)
		for (r = 0; r < ROUNDS; r++)
			D.rtt[t][r] = -1;
	if (D.in.route && (D.icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) >= 0) {
		D.ufd_icmp.fd = D.icmp;
		D.ufd_icmp.cb = on_icmp;
		uloop_fd_add(&D.ufd_icmp, ULOOP_READ);
	} else if (D.in.route) {
		AG_WARN("diagnose: no ICMP socket (%s): the line is not measured", strerror(errno));
		D.ntgt = 0;
	}
	if (cfg.diag_name[0] && (D.udp = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
		D.ufd_udp.fd = D.udp;
		D.ufd_udp.cb = on_udp;
		uloop_fd_add(&D.ufd_udp, ULOOP_READ);
		send_dns();
	}
	if (D.icmp < 0)
		D.ntgt = 0;
	D.running = 1;
	D.start_ms = mono_ms();
	D.timer.cb = tick;
	uloop_timeout_set(&D.timer, 1);
	AG_INFO("diagnose%s%s: measuring (next hop %s, %d internet targets, DNS %s)", host[0] ? " " : "", host,
		gw[0] ? gw : D.in.route ? "none" : "no route", D.ntgt > 1 ? D.ntgt - 1 : 0,
		cfg.diag_name[0] ? cfg.diag_name : "not checked");
	return 0;
}

void diag_blob(struct blob_buf *b, const struct diag_result *r, const struct diag_input *in)
{
	const struct diag_finding *v = r->n ? &r->f[0] : NULL;
	void *a, *t;
	int i;

	blobmsg_add_string(b, "code", v ? v->code : "OK");
	blobmsg_add_string(b, "area", v ? v->area : "All");
	blobmsg_add_string(b, "severity", diag_sev_name(v ? v->sev : DS_OK));
	blobmsg_add_string(b, "summary", v ? v->text : "");
	blobmsg_add_string(b, "advice", diag_advice(v ? v->code : "OK"));
	if (in->host.given)
		blobmsg_add_string(b, "host", in->host.name);
	t = blobmsg_open_table(b, "measured");
	blobmsg_add_u8(b, "route", (uint8_t)in->route);
	blobmsg_add_string(b, "next_hop", in->gw);
	blobmsg_add_u32(b, "next_hop_loss_pct", (uint32_t)(in->gw_ping.sent ? in->gw_ping.lost * 100 / in->gw_ping.sent : 0));
	blobmsg_add_u32(b, "next_hop_ms", (uint32_t)(in->gw_ping.rtt_ms < 0 ? 0 : in->gw_ping.rtt_ms));
	blobmsg_add_u32(b, "internet_loss_pct", (uint32_t)(in->inet.sent ? in->inet.lost * 100 / in->inet.sent : 100));
	blobmsg_add_u32(b, "internet_ms", (uint32_t)(in->inet.rtt_ms < 0 ? 0 : in->inet.rtt_ms));
	blobmsg_add_string(b, "dns", in->dns < 0 ? "not checked" : !in->dns ? "no answer" :
				     in->dns_rcode == 0 ? "ok" : in->dns_rcode == 2 ? "failed" :
				     in->dns_rcode == 3 ? "no such name" : "refused");
	blobmsg_add_u32(b, "dns_ms", (uint32_t)(in->dns == 1 ? in->dns_ms : 0));
	blobmsg_close_table(b, t);
	a = blobmsg_open_array(b, "findings");
	for (i = 0; i < r->n; i++) {
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "code", r->f[i].code);
		blobmsg_add_string(b, "area", r->f[i].area);
		blobmsg_add_string(b, "severity", diag_sev_name(r->f[i].sev));
		blobmsg_add_string(b, "text", r->f[i].text);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, a);
}

int64_t diag_last(const struct diag_result **r, const struct diag_input **in, const char **host)
{
	*r = &last_res;
	*in = &last_in;
	*host = last_host;
	return last_at;
}
