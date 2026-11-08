/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Connection names from TLS and QUIC handshakes.
 *
 * aigw-nft copies the first packets of each new HTTPS connection from the
 * LAN (TCP 443: packets 3-6, the ClientHello; UDP 443: packets 1-3, the
 * QUIC Initial) to NFLOG group flow.sni_group. A copy, never a verdict:
 * forwarding never waits for agd. Here they are put back together per
 * connection, the server name is read (libag sni.c) and handed to the flow
 * sensor, which names the connection with it.
 *
 * Plain netlink (nfnetlink_log), no library: bind the group, ask for packet
 * copies, read NFULA_PAYLOAD.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_log.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "agd.h"
#include "ag/sni.h"

#define PEND_MAX     64              /* connections whose ClientHello is still arriving */
#define PEND_WAIT_US (5 * 1000000ull)
#define SNAPLEN      2048

struct pend {
	int used;
	uint64_t t0;
	char src[IP_LEN], dst[IP_LEN];
	uint16_t sport, dport;
	uint8_t proto;
	uint32_t seq0;                   /* TCP: sequence number of the first payload byte */
	size_t len;                      /* TCP: bytes kept, in order; QUIC: Initials read */
	union {
		uint8_t tcp[AG_HELLO_MAX];
		struct ag_quic_hello quic;
	} u;
};

static struct pend *pends;
static struct uloop_fd nl;
static int on;
static uint32_t seq;
static struct {
	uint64_t packets, tls, quic, ech, none;
} st;

/* ---------- netlink ---------- */

static int cfg_msg(uint16_t group, uint16_t type, const void *data, uint16_t dlen)
{
	char buf[256], ack[512];
	struct nlmsghdr *nh = (struct nlmsghdr *)buf;
	struct nfgenmsg *g;
	struct nlattr *a;
	ssize_t n;

	memset(buf, 0, sizeof(buf));
	nh->nlmsg_len = NLMSG_LENGTH(sizeof(*g));
	nh->nlmsg_type = (NFNL_SUBSYS_ULOG << 8) | NFULNL_MSG_CONFIG;
	nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nh->nlmsg_seq = ++seq;
	g = NLMSG_DATA(nh);
	g->nfgen_family = AF_UNSPEC;
	g->version = NFNETLINK_V0;
	g->res_id = htons(group);
	a = (struct nlattr *)(buf + NLMSG_ALIGN(nh->nlmsg_len));
	a->nla_type = type;
	a->nla_len = (uint16_t)(NLA_HDRLEN + dlen);
	memcpy((char *)a + NLA_HDRLEN, data, dlen);
	nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + NLA_ALIGN(a->nla_len);
	if (send(nl.fd, buf, nh->nlmsg_len, 0) < 0)
		return -errno;
	n = recv(nl.fd, ack, sizeof(ack), 0);
	if (n >= (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
		struct nlmsghdr *r = (struct nlmsghdr *)ack;

		if (r->nlmsg_type == NLMSG_ERROR)
			return ((struct nlmsgerr *)NLMSG_DATA(r))->error;
	}
	return 0;
}

/* ---------- connections in progress ---------- */

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static struct pend *pend_find(const char *src, const char *dst, uint8_t proto, uint16_t sport, uint16_t dport)
{
	int i;

	for (i = 0; i < PEND_MAX; i++) {
		struct pend *p = &pends[i];

		if (p->used && p->proto == proto && p->sport == sport && p->dport == dport &&
		    !strcmp(p->src, src) && !strcmp(p->dst, dst))
			return p;
	}
	return NULL;
}

static struct pend *pend_new(const char *src, const char *dst, uint8_t proto, uint16_t sport, uint16_t dport)
{
	uint64_t now = ag_mono_us();
	struct pend *p = NULL;
	int i;

	for (i = 0; i < PEND_MAX; i++) {
		struct pend *q = &pends[i];

		if (q->used && now - q->t0 > PEND_WAIT_US)
			q->used = 0;                    /* never finished: give up on it */
		if (!q->used && !p)
			p = q;
	}
	if (!p) { /* all busy: replace the oldest */
		p = &pends[0];
		for (i = 1; i < PEND_MAX; i++)
			if (pends[i].t0 < p->t0)
				p = &pends[i];
	}
	memset(p, 0, offsetof(struct pend, u));
	if (proto == IPPROTO_UDP)
		memset(&p->u.quic.have, 0, sizeof(p->u.quic.have));
	p->used = 1;
	p->t0 = now;
	snprintf(p->src, sizeof(p->src), "%s", src);
	snprintf(p->dst, sizeof(p->dst), "%s", dst);
	p->proto = proto;
	p->sport = sport;
	p->dport = dport;
	return p;
}

static void found(struct pend *p, const struct ag_sni *s)
{
	if (p->proto == IPPROTO_TCP)
		st.tls++;
	else
		st.quic++;
	if (s->ech)
		st.ech++;
	AG_DEBUG("sni %s:%u -> %s:%u %s %s%s", p->src, p->sport, p->dst, p->dport,
		 p->proto == IPPROTO_TCP ? "tls" : "quic", s->name, s->ech ? " (ech)" : "");
	flow_named(p->src, p->dst, p->proto, p->sport, p->dport, s);
	p->used = 0;
}

static void tcp_payload(const char *src, const char *dst, uint16_t sport, uint16_t dport,
			uint32_t tseq, const uint8_t *d, size_t n)
{
	struct pend *p = pend_find(src, dst, IPPROTO_TCP, sport, dport);
	struct ag_sni s;
	size_t off, take;
	int rc;

	if (!p) {
		/* a TLS handshake record holding a ClientHello starts here */
		if (n < 6 || d[0] != 0x16 || d[1] != 3 || d[5] != 1)
			return;
		p = pend_new(src, dst, IPPROTO_TCP, sport, dport);
		p->seq0 = tseq;
	}
	off = (size_t)(tseq - p->seq0);
	if (off != p->len)
		return;                                 /* retransmitted or out of order: wait */
	take = n < AG_HELLO_MAX - p->len ? n : AG_HELLO_MAX - p->len;
	memcpy(p->u.tcp + p->len, d, take);
	p->len += take;
	rc = ag_tls_stream_sni(p->u.tcp, p->len, &s);
	if (rc == AG_SNI_FOUND)
		found(p, &s);
	else if (rc != AG_SNI_MORE || p->len >= AG_HELLO_MAX) {
		st.none++;
		p->used = 0;                            /* no name in it, or not TLS after all */
	}
}

static void udp_payload(const char *src, const char *dst, uint16_t sport, uint16_t dport,
			const uint8_t *d, size_t n)
{
	struct pend *p = pend_find(src, dst, IPPROTO_UDP, sport, dport);
	struct ag_sni s;
	int rc, fed;

	if (!p) {
		if (n < 7 || !(d[0] & 0x80))
			return;                             /* QUIC Initials have a long header */
		p = pend_new(src, dst, IPPROTO_UDP, sport, dport);
	}
	fed = ag_quic_feed(&p->u.quic, d, n);
	if (fed <= 0) {
		if (!p->len)
			p->used = 0;                        /* nothing we could read: not QUIC, or not v1/v2 */
		return;
	}
	p->len++;
	rc = ag_quic_sni(&p->u.quic, &s);
	if (rc == AG_SNI_FOUND)
		found(p, &s);
	else if (rc != AG_SNI_MORE) {
		st.none++;
		p->used = 0;
	}
}

/* one copied packet, from its IP header on */
static void packet(const uint8_t *pk, size_t n)
{
	char src[IP_LEN], dst[IP_LEN];
	size_t l3, l4len;
	uint8_t proto;
	const uint8_t *l4;

	if (n < 20)
		return;
	if ((pk[0] >> 4) == 4) {
		size_t ihl = (size_t)(pk[0] & 15) * 4, tot = be16(pk + 2);

		if (ihl < 20 || n < ihl || (be16(pk + 6) & 0x1fff))
			return;                             /* later fragments carry no header */
		if (tot >= ihl && tot < n)
			n = tot;
		proto = pk[9];
		inet_ntop(AF_INET, pk + 12, src, sizeof(src));
		inet_ntop(AF_INET, pk + 16, dst, sizeof(dst));
		l3 = ihl;
	} else if ((pk[0] >> 4) == 6) {
		size_t plen;

		if (n < 40)
			return;
		plen = be16(pk + 4);
		if (40 + plen < n)
			n = 40 + plen;
		proto = pk[6];                          /* extension headers: not followed */
		inet_ntop(AF_INET6, pk + 8, src, sizeof(src));
		inet_ntop(AF_INET6, pk + 24, dst, sizeof(dst));
		l3 = 40;
	} else {
		return;
	}
	l4 = pk + l3;
	l4len = n - l3;
	if (proto == IPPROTO_TCP && l4len >= 20) {
		size_t doff = (size_t)(l4[12] >> 4) * 4;

		if (doff >= 20 && doff < l4len)
			tcp_payload(src, dst, be16(l4), be16(l4 + 2), be32(l4 + 4),l4 + doff, l4len - doff);
	} else if (proto == IPPROTO_UDP && l4len > 8) {
		udp_payload(src, dst, be16(l4), be16(l4 + 2), l4 + 8, l4len - 8);
	}
}

static void nl_cb(struct uloop_fd *u, unsigned int events)
{
	static uint8_t buf[65536];

	for (;;) {
		ssize_t n = recv(u->fd, buf, sizeof(buf), MSG_DONTWAIT);
		struct nlmsghdr *nh;
		int len;

		if (n <= 0) {
			if (n < 0 && errno == ENOBUFS)
				AG_DEBUG("sni: packet copies lost (busy)");
			return;
		}
		len = (int)n;
		for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
			struct nlattr *a;
			int alen;

			if (nh->nlmsg_type != ((NFNL_SUBSYS_ULOG << 8) | NFULNL_MSG_PACKET))
				continue;
			a = (struct nlattr *)((char *)NLMSG_DATA(nh) + NLMSG_ALIGN(sizeof(struct nfgenmsg)));
			alen = (int)nh->nlmsg_len - (int)NLMSG_LENGTH(NLMSG_ALIGN(sizeof(struct nfgenmsg)));
			while (alen >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= alen) {
				if ((a->nla_type & NLA_TYPE_MASK) == NFULA_PAYLOAD) {
					st.packets++;
					packet((const uint8_t *)a + NLA_HDRLEN, a->nla_len - NLA_HDRLEN);
				}
				alen -= NLA_ALIGN(a->nla_len);
				a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
			}
		}
	}
}

/* ---------- setup / API ---------- */

int sni_init(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct nfulnl_msg_config_cmd cmd = { .command = NFULNL_CFG_CMD_BIND };
	struct nfulnl_msg_config_mode mode = { .copy_range = htonl(SNAPLEN), .copy_mode = NFULNL_COPY_PACKET };
	uint32_t qthresh = htonl(1);
	uint16_t g = (uint16_t)cfg.flow_sni_group;
	int rc, sz = 1024 * 1024;

	if (!cfg.flow_sni || !flow_active())
		return -1;
	pends = calloc(PEND_MAX, sizeof(*pends));
	nl.fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
	if (!pends || nl.fd < 0 || bind(nl.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		AG_WARN("connection names off: netlink unavailable (%s)", strerror(errno));
		goto fail;
	}
	rc = cfg_msg(g, NFULA_CFG_CMD, &cmd, sizeof(cmd));
	if (rc < 0) {
		AG_WARN("connection names off: cannot listen on NFLOG group %u (%s)%s", g, strerror(-rc),
			rc == -EBUSY ? ": another program uses it; set flow.sni_group" : "");
		goto fail;
	}
	cfg_msg(g, NFULA_CFG_MODE, &mode, sizeof(mode));
	cfg_msg(g, NFULA_CFG_QTHRESH, &qthresh, sizeof(qthresh));
	setsockopt(nl.fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz));
	fcntl(nl.fd, F_SETFL, fcntl(nl.fd, F_GETFL) | O_NONBLOCK);
	nl.cb = nl_cb;
	uloop_fd_add(&nl, ULOOP_READ);
	on = 1;
	AG_INFO("connection names on: TLS and QUIC handshakes via NFLOG group %u", g);
	return 0;
fail:
	if (nl.fd >= 0)
		close(nl.fd);
	nl.fd = -1;
	free(pends);
	pends = NULL;
	return -1;
}

int sni_active(void)
{
	return on;
}

void sni_blob(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "sni");

	blobmsg_add_u8(b, "active", (uint8_t)on);
	blobmsg_add_u32(b, "group", (uint32_t)cfg.flow_sni_group);
	blobmsg_add_u64(b, "packets", st.packets);
	blobmsg_add_u64(b, "tls", st.tls);
	blobmsg_add_u64(b, "quic", st.quic);
	blobmsg_add_u64(b, "ech", st.ech);
	blobmsg_add_u64(b, "without_name", st.none);
	blobmsg_close_table(b, t);
}
