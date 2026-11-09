/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fast path (phase 7): established connections of devices AIGW isn't acting
 * on skip the forwarding rules and go through the kernel's flowtable
 * (software, or the switch chip with fastpath.offload on routers that have
 * one). aigw-nft builds it in table "inet ag":
 *
 *   new connection ── forward chains: quarantine, connection marks, throttle,
 *        │            priority (DSCP), handshake copies for connection names
 *        ▼
 *   established, past its first packets, device under no action
 *        │            → "flow add @fast": the rest of it bypasses netfilter
 *        ▼
 *   an action starts on a device (quarantine, throttle, priority, ...) →
 *   agd removes that device's offloaded connections from conntrack, so their
 *   next packets go through the rules again (and, the device now being in a
 *   set, stay on the slow path)
 *
 * Traffic counts stay right: the flowtable keeps the connection's counters
 * (flowtable "counter"), which flow.c reads as before.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

#include "agd.h"

#ifndef IPS_OFFLOAD
#define IPS_OFFLOAD (1u << 14)
#endif
#ifndef IPS_HW_OFFLOAD
#define IPS_HW_OFFLOAD (1u << 15)
#endif
#define RELEASE_MAX 1024

static uint32_t offloaded, hw_offloaded;
static uint64_t released_total;
static int64_t counted_at;

struct scan {
	int family;
	uint8_t addr[16];                   /* the device whose connections go back; family 0 = count only */
	struct nf_conntrack *ct[RELEASE_MAX];
	int n;
	uint32_t off, hw;
};

static int has_addr(const struct nf_conntrack *ct, const struct scan *s)
{
	static const int v4[] = { ATTR_ORIG_IPV4_SRC, ATTR_ORIG_IPV4_DST, ATTR_REPL_IPV4_SRC, ATTR_REPL_IPV4_DST };
	static const int v6[] = { ATTR_ORIG_IPV6_SRC, ATTR_ORIG_IPV6_DST, ATTR_REPL_IPV6_SRC, ATTR_REPL_IPV6_DST };
	int i;

	if (nfct_get_attr_u8(ct, ATTR_L3PROTO) != s->family)
		return 0;
	for (i = 0; i < 4; i++) {
		const void *a = nfct_get_attr(ct, s->family == AF_INET ? v4[i] : v6[i]);

		if (a && !memcmp(a, s->addr, s->family == AF_INET ? 4 : 16))
			return 1;
	}
	return 0;
}

static int scan_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
	struct scan *s = data;
	uint32_t st = nfct_attr_is_set(ct, ATTR_STATUS) ? nfct_get_attr_u32(ct, ATTR_STATUS) : 0;

	(void)type;
	if (!(st & IPS_OFFLOAD))
		return NFCT_CB_CONTINUE;
	s->off++;
	if (st & IPS_HW_OFFLOAD)
		s->hw++;
	if (s->family && s->n < RELEASE_MAX && has_addr(ct, s))
		s->ct[s->n++] = nfct_clone(ct);
	return NFCT_CB_CONTINUE;
}

static int scan(struct scan *s)
{
	struct nfct_handle *h = nfct_open(CONNTRACK, 0);
	uint32_t fam = AF_UNSPEC;

	if (!h)
		return -1;
	nfct_callback_register(h, NFCT_T_ALL, scan_cb, s);
	if (nfct_query(h, NFCT_Q_DUMP, &fam) < 0) {
		nfct_close(h);
		return -1;
	}
	nfct_close(h);
	offloaded = s->off;
	hw_offloaded = s->hw;
	counted_at = ag_wall_s();
	return 0;
}

/* an action starts on this device: its offloaded connections go back through the rules */
int fastpath_release(const char *ip, const char *why)
{
	struct scan *s;
	struct nfct_handle *h;
	int i, done = 0;

	if (!cfg.fastpath_enabled || !ip || !*ip)
		return 0;
	s = calloc(1, sizeof(*s));
	if (!s)
		return -1;
	if (inet_pton(AF_INET, ip, s->addr) == 1)
		s->family = AF_INET;
	else if (inet_pton(AF_INET6, ip, s->addr) == 1)
		s->family = AF_INET6;
	if (!s->family || scan(s)) {
		free(s);
		return -1;
	}
	if (s->n && (h = nfct_open(CONNTRACK, 0))) {
		for (i = 0; i < s->n; i++)
			if (nfct_query(h, NFCT_Q_DESTROY, s->ct[i]) == 0)
				done++;
		nfct_close(h);
	}
	for (i = 0; i < s->n; i++)
		nfct_destroy(s->ct[i]);
	free(s);
	if (done) {
		released_total += (uint64_t)done;
		AG_INFO("fast path: %d connection%s of %s back through the rules (%s)", done, done == 1 ? "" : "s", ip, why);
	}
	return done;
}

/* quarantine is by MAC: the device's address */
int fastpath_release_mac(const char *mac, const char *why)
{
	struct device *d = dev_find_mac(mac, 0);

	return d && d->ip[0] ? fastpath_release(d->ip, why) : 0;
}

void fastpath_tick(void)
{
	struct scan *s;

	if (!cfg.fastpath_enabled || ag_wall_s() - counted_at < 10)
		return;
	s = calloc(1, sizeof(*s));
	if (s && scan(s))
		counted_at = ag_wall_s();       /* no conntrack here: try again later */
	free(s);
}

void fastpath_blob(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "fastpath");

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.fastpath_enabled);
	blobmsg_add_u32(b, "offloaded", offloaded);
	blobmsg_add_u32(b, "hw_offloaded", hw_offloaded);
	blobmsg_add_u64(b, "released", released_total);
	blobmsg_close_table(b, t);
}
