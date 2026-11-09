/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Flow sensor: what devices actually do, from the kernel's connection
 * tracking. Connection start/end arrive as conntrack events; traffic counters
 * are refreshed by a periodic table dump so long-lived flows (video calls,
 * streams) are counted while they run, not only when they end.
 *
 * Every new connection is joined with the endpoint table: its destination
 * gets the DNS name the device looked up, or is flagged "no DNS" when the
 * device connected to a public address it never resolved.
 *
 * HTTPS connections (TCP or UDP 443) also carry their server name in the
 * handshake (sni.c). A connection without a DNS name waits up to
 * SNI_JUDGE_MS for it before it counts as "no DNS"; when it comes, the
 * connection is named after it and the name is judged like a lookup.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <libubox/uloop.h>

#include "agd.h"
#include "ag/sni.h"

#define ACT_MAX      4096        /* active flows tracked (hash, linear probing) */
#define RECENT_MAX   256         /* finished flows kept for "agctl flows" */
#define SNI_JUDGE_MS 3000        /* how long an unnamed HTTPS connection waits for its handshake */
#define EARLY_MAX    16          /* names that arrived before their connection */

enum { BY_NONE, BY_DNS, BY_NEARBY, BY_SNI };
static const char *const by_name[] = { "", "dns", "nearby", "sni" };

struct aflow {
	int used;
	uint32_t id;
	char client[IP_LEN];
	char dst[IP_LEN];
	char domain[NAME_LEN];
	uint8_t proto;
	uint8_t named_by;
	uint16_t sport, dport;
	int no_dns;
	int64_t start;
	uint64_t judge_at;              /* ms: judge "no DNS" then, unless a name came; 0 = not waiting */
	uint64_t up_seen, down_seen;    /* counters already credited */
};

struct rflow {
	int64_t start, end;
	char client[IP_LEN];
	char dst[IP_LEN];
	char domain[NAME_LEN];
	uint8_t proto;
	uint8_t named_by;
	uint16_t dport;
	int no_dns;
	uint64_t up, down;
};

struct early {
	uint64_t at;                    /* ms */
	char client[IP_LEN], dst[IP_LEN];
	uint8_t proto;
	uint16_t sport, dport;
	struct ag_sni s;
};

uint64_t flows_total, flows_no_dns_total, flows_sni_total;

static struct nfct_handle *ev_h, *dump_h;
static struct uloop_fd ev_fd;
static struct uloop_timeout dump_timer, judge_timer;
static int judging;
static struct early early[EARLY_MAX];
static unsigned early_head;
static struct aflow act[ACT_MAX];
static struct rflow recent[RECENT_MAX];
static unsigned recent_head, recent_count;
static int active;

/* ---------- active flow table ---------- */

static struct aflow *act_find(uint32_t id, int create)
{
	unsigned i, h = (id * 2654435761u) % ACT_MAX;

	for (i = 0; i < ACT_MAX; i++) {
		struct aflow *f = &act[(h + i) % ACT_MAX];

		if (f->used && f->id == id)
			return f;
		if (!f->used) {
			if (!create)
				return NULL;
			memset(f, 0, sizeof(*f));
			f->used = 1;
			f->id = id;
			return f;
		}
	}
	return NULL; /* full: the flow is simply not tracked */
}

static void act_remove(struct aflow *f)
{
	unsigned i = (unsigned)(f - act), j, k;

	/* backward-shift deletion keeps the linear probe chains intact: an entry
	 * moves into the hole unless its home slot lies cyclically in (i, j] */
	f->used = 0;
	for (j = (i + 1) % ACT_MAX; act[j].used; j = (j + 1) % ACT_MAX) {
		int stays;

		k = (act[j].id * 2654435761u) % ACT_MAX;
		stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
		if (!stays) {
			act[i] = act[j];
			act[j].used = 0;
			i = j;
		}
	}
}

/* ---------- conntrack helpers ---------- */

struct tuple {
	char src[IP_LEN], dst[IP_LEN];
	uint8_t proto;
	uint16_t sport, dport;
	uint32_t id;
};

static int read_tuple(struct nf_conntrack *ct, struct tuple *t)
{
	uint8_t fam = nfct_get_attr_u8(ct, ATTR_L3PROTO);

	memset(t, 0, sizeof(*t));
	if (fam == AF_INET) {
		uint32_t s = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_SRC);
		uint32_t d = nfct_get_attr_u32(ct, ATTR_ORIG_IPV4_DST);

		inet_ntop(AF_INET, &s, t->src, sizeof(t->src));
		inet_ntop(AF_INET, &d, t->dst, sizeof(t->dst));
	} else if (fam == AF_INET6) {
		inet_ntop(AF_INET6, nfct_get_attr(ct, ATTR_ORIG_IPV6_SRC), t->src, sizeof(t->src));
		inet_ntop(AF_INET6, nfct_get_attr(ct, ATTR_ORIG_IPV6_DST), t->dst, sizeof(t->dst));
	} else {
		return -1;
	}
	t->proto = nfct_get_attr_u8(ct, ATTR_ORIG_L4PROTO);
	if ((t->proto == IPPROTO_TCP || t->proto == IPPROTO_UDP) && nfct_attr_is_set(ct, ATTR_ORIG_PORT_DST)) {
		t->dport = ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_DST));
		if (nfct_attr_is_set(ct, ATTR_ORIG_PORT_SRC))
			t->sport = ntohs(nfct_get_attr_u16(ct, ATTR_ORIG_PORT_SRC));
	}
	t->id = nfct_attr_is_set(ct, ATTR_ID) ? nfct_get_attr_u32(ct, ATTR_ID) : 0;
	return 0;
}

/* Only connections a LAN device starts towards somewhere else. DNS itself is
 * covered by agdns, so port 53 is skipped. */
static int interesting(const struct tuple *t)
{
	if (is_local(t->src) || is_local(t->dst) || !is_lan_client(t->src))
		return 0;
	if (t->dport == 53)
		return 0;
	if (!strncmp(t->dst, "224.", 4) || !strncmp(t->dst, "239.", 4) ||
	    !strcmp(t->dst, "255.255.255.255") || !strncmp(t->dst, "ff", 2))
		return 0;
	return 1;
}

static void counters(struct nf_conntrack *ct, uint64_t *up, uint64_t *down)
{
	*up = nfct_attr_is_set(ct, ATTR_ORIG_COUNTER_BYTES) ? nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_BYTES) : 0;
	*down = nfct_attr_is_set(ct, ATTR_REPL_COUNTER_BYTES) ? nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_BYTES) : 0;
}

/* Credit traffic since the last look to the device and its destination. */
static void credit(struct aflow *f, uint64_t up, uint64_t down)
{
	uint64_t du = up > f->up_seen ? up - f->up_seen : 0;
	uint64_t dd = down > f->down_seen ? down - f->down_seen : 0;
	struct device *d;

	f->up_seen = up > f->up_seen ? up : f->up_seen;
	f->down_seen = down > f->down_seen ? down : f->down_seen;
	if (!du && !dd)
		return;
	d = dev_find_ip(f->client, 1);
	d->bytes_up += du;
	d->bytes_down += dd;
	top_add(d->dsts, DEV_DST_MAX, f->domain[0] ? f->domain : f->dst, du + dd);
	baseline_traffic(d, du, dd);
}

static const char *proto_name(uint8_t p)
{
	return p == IPPROTO_TCP ? "tcp" : p == IPPROTO_UDP ? "udp" : p == IPPROTO_ICMP ? "icmp" :
	       p == IPPROTO_ICMPV6 ? "icmpv6" : "other";
}

static void emit(const char *what, const struct aflow *f, uint64_t up, uint64_t down)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "client", f->client);
	blobmsg_add_string(&b, "dst", f->dst);
	blobmsg_add_string(&b, "domain", f->domain);
	blobmsg_add_string(&b, "proto", proto_name(f->proto));
	blobmsg_add_u32(&b, "dport", f->dport);
	blobmsg_add_u8(&b, "no_dns", f->no_dns ? 1 : 0);
	blobmsg_add_string(&b, "named_by", by_name[f->named_by]);
	if (!strcmp(what, "end")) {
		blobmsg_add_u64(&b, "bytes_up", up);
		blobmsg_add_u64(&b, "bytes_down", down);
		blobmsg_add_u32(&b, "duration_s", f->start ? (uint32_t)(ag_wall_s() - f->start) : 0);
	}
	agd_emit(AG_EV_FLOW, &b);
	blob_buf_free(&b);
}

static uint64_t now_ms(void)
{
	return ag_mono_us() / 1000;
}

/* No DNS name, and no handshake name in time: "no DNS" after all. */
static void judge_no_dns(struct aflow *f)
{
	struct device *d = dev_find_ip(f->client, 1);

	if (f->judge_at)
		judging--;
	f->judge_at = 0;
	f->no_dns = 1;
	d->flows_no_dns++;
	flows_no_dns_total++;
	alerts_no_dns(f->client, f->dst, proto_name(f->proto), f->dport);
}

static void judge_cb(struct uloop_timeout *t)
{
	uint64_t now = now_ms();
	int i;

	for (i = 0; i < ACT_MAX && judging > 0; i++)
		if (act[i].used && act[i].judge_at && now >= act[i].judge_at)
			judge_no_dns(&act[i]);
	if (judging > 0)
		uloop_timeout_set(t, 500);
}

/* The handshake named the connection. A DNS name stays when the handshake
 * shows only an Encrypted Client Hello's outer name, or the same name. */
static void apply_name(struct aflow *f, const struct ag_sni *s)
{
	int had_dns = f->named_by == BY_DNS;

	if (had_dns && (s->ech || !strcmp(f->domain, s->name)))
		return;
	snprintf(f->domain, sizeof(f->domain), "%.*s", (int)sizeof(f->domain) - 1, s->name);
	f->named_by = BY_SNI;
	flows_sni_total++;
	if (f->judge_at) {          /* it had a name after all: not "no DNS" */
		f->judge_at = 0;
		judging--;
	}
	/* not added to the endpoint table: other connections to this address
	 * still need a lookup (or their own handshake) to count as named */
	emit("named", f, 0, 0);
	alerts_on_sni(f->client, f->dst, s->name, f->dport, s->ech);
}

void flow_named(const char *client, const char *dst, uint8_t proto, uint16_t sport, uint16_t dport,
		const struct ag_sni *s)
{
	struct early *e;
	int i;

	for (i = 0; i < ACT_MAX; i++) {
		struct aflow *f = &act[i];

		if (f->used && f->proto == proto && f->sport == sport && f->dport == dport &&
		    !strcmp(f->client, client) && !strcmp(f->dst, dst)) {
			apply_name(f, s);
			return;
		}
	}
	/* QUIC: the first packet is the handshake, and its copy can beat the
	 * conntrack event here. Keep the name for the connection's start. */
	e = &early[early_head];
	early_head = (early_head + 1) % EARLY_MAX;
	e->at = now_ms();
	snprintf(e->client, sizeof(e->client), "%s", client);
	snprintf(e->dst, sizeof(e->dst), "%s", dst);
	e->proto = proto;
	e->sport = sport;
	e->dport = dport;
	e->s = *s;
}

static struct early *early_take(const struct tuple *t)
{
	uint64_t now = now_ms();
	int i;

	for (i = 0; i < EARLY_MAX; i++) {
		struct early *e = &early[i];

		if (e->at && now - e->at < 5000 && e->proto == t->proto && e->sport == t->sport &&
		    e->dport == t->dport && !strcmp(e->client, t->src) && !strcmp(e->dst, t->dst)) {
			e->at = 0;
			return e;
		}
	}
	return NULL;
}

/* A connection we saw start: count it, judge "no DNS", tell the alerts. */
static void flow_started(struct aflow *f, const struct tuple *t)
{
	struct early *e;
	struct device *d;
	int judge;

	f->start = ag_wall_s();
	/* only judge connections we saw start, once the endpoint table is warm */
	judge = f->named_by == BY_NONE && !is_private(t->dst) && ag_wall_s() - started >= cfg.flow_warmup;
	/* HTTPS: its handshake may name it in a moment */
	if (judge && sni_active() && t->dport == 443 && (t->proto == IPPROTO_TCP || t->proto == IPPROTO_UDP)) {
		judge = 0;
		f->judge_at = now_ms() + SNI_JUDGE_MS;
		if (judging++ == 0)
			uloop_timeout_set(&judge_timer, 500);
	}
	f->no_dns = judge;

	d = dev_find_ip(t->src, 1);
	d->flows++;
	flows_total++;
	if (f->no_dns) {
		d->flows_no_dns++;
		flows_no_dns_total++;
	}
	emit("new", f, 0, 0);
	alerts_on_flow(f->client, f->dst, f->domain, proto_name(f->proto), f->dport, f->no_dns);
	if ((e = early_take(t)))
		apply_name(f, &e->s);
}

/* A flow we first meet (event or dump): set it up and join it with DNS. */
static struct aflow *flow_begin(const struct tuple *t, int seen_start)
{
	struct aflow *f = act_find(t->id, 1);
	struct device *d;
	const char *dom;

	if (!f)
		return NULL;
	snprintf(f->client, sizeof(f->client), "%s", t->src);
	snprintf(f->dst, sizeof(f->dst), "%s", t->dst);
	f->proto = t->proto;
	f->sport = t->sport;
	f->dport = t->dport;
	f->start = 0;
	dom = ep_lookup(t->dst);
	if (dom) {
		snprintf(f->domain, sizeof(f->domain), "%s", dom);
		f->named_by = BY_DNS;
	} else if ((dom = ep_lookup_near(t->dst))) {
		/* same /24 as a name the home looked up: an app's own server list */
		snprintf(f->domain, sizeof(f->domain), "%.100s (nearby)", dom);
		f->named_by = BY_NEARBY;
	}
	d = dev_find_ip(t->src, 1);
	d->last_seen = ag_wall_s();
	if (seen_start)
		flow_started(f, t);
	return f;
}

static void flow_end(struct aflow *f, uint64_t up, uint64_t down)
{
	struct rflow *r = &recent[recent_head];

	if (f->judge_at)
		judge_no_dns(f);        /* ended before its handshake named it */
	credit(f, up, down);
	memset(r, 0, sizeof(*r));
	r->start = f->start;
	r->end = ag_wall_s();
	snprintf(r->client, sizeof(r->client), "%s", f->client);
	snprintf(r->dst, sizeof(r->dst), "%s", f->dst);
	snprintf(r->domain, sizeof(r->domain), "%s", f->domain);
	r->proto = f->proto;
	r->dport = f->dport;
	r->no_dns = f->no_dns;
	r->named_by = f->named_by;
	r->up = up;
	r->down = down;
	recent_head = (recent_head + 1) % RECENT_MAX;
	if (recent_count < RECENT_MAX)
		recent_count++;
	emit("end", f, up, down);
	act_remove(f);
}

/* ---------- conntrack events ---------- */

static int event_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
	struct tuple t;
	struct aflow *f;
	uint64_t up, down;

	if (read_tuple(ct, &t) < 0 || !interesting(&t))
		return NFCT_CB_CONTINUE;

	if (type == NFCT_T_NEW) {
		f = act_find(t.id, 0);
		if (!f)
			flow_begin(&t, 1);
		else if (!f->start)
			/* the counter refresh met it first (its dump ran before this
			 * event was read): it did start now, so judge it as new */
			flow_started(f, &t);
	} else if (type == NFCT_T_DESTROY) {
		f = act_find(t.id, 0);
		if (!f)
			f = flow_begin(&t, 0); /* started before agd: count its traffic */
		if (f) {
			counters(ct, &up, &down);
			flow_end(f, up, down);
		}
	}
	return NFCT_CB_CONTINUE;
}

static void ev_cb(struct uloop_fd *u, unsigned int events)
{
	if (nfct_catch(ev_h) < 0 && errno == ENOBUFS)
		AG_DEBUG("conntrack events lost (busy); counters catch up on the next refresh");
}

/* ---------- periodic counter refresh ---------- */

static int dump_cb(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
	struct tuple t;
	struct aflow *f;
	uint64_t up, down;

	if (read_tuple(ct, &t) < 0 || !interesting(&t))
		return NFCT_CB_CONTINUE;
	f = act_find(t.id, 0);
	if (!f)
		f = flow_begin(&t, 0);
	if (f) {
		counters(ct, &up, &down);
		credit(f, up, down);
	}
	return NFCT_CB_CONTINUE;
}

static void dump_cb_timer(struct uloop_timeout *tmo)
{
	uint32_t fam = AF_UNSPEC;

	if (nfct_query(dump_h, NFCT_Q_DUMP, &fam) < 0)
		AG_DEBUG("conntrack dump failed: %s", strerror(errno));
	uloop_timeout_set(tmo, cfg.flow_poll * 1000);
}

/* ---------- setup / API ---------- */

static void enable_accounting(void)
{
	/* byte counters per connection; off by default on most kernels */
	FILE *f = fopen("/proc/sys/net/netfilter/nf_conntrack_acct", "w");

	if (f) {
		fputs("1", f);
		fclose(f);
	} else {
		AG_WARN("cannot enable conntrack accounting: traffic counters will be zero");
	}
}

int flow_init(void)
{
	if (!cfg.flow_enabled) {
		AG_INFO("flow sensor disabled in config");
		return -1;
	}
	enable_accounting();

	ev_h = nfct_open(CONNTRACK, NF_NETLINK_CONNTRACK_NEW | NF_NETLINK_CONNTRACK_DESTROY);
	dump_h = nfct_open(CONNTRACK, 0);
	if (!ev_h || !dump_h) {
		AG_WARN("flow sensor off: conntrack netlink unavailable (%s). "
			"Load nf_conntrack_netlink and run agd as root.", strerror(errno));
		return -1;
	}
	nfct_callback_register(ev_h, NFCT_T_NEW | NFCT_T_DESTROY, event_cb, NULL);
	nfct_callback_register(dump_h, NFCT_T_ALL, dump_cb, NULL);

	ev_fd.fd = nfct_fd(ev_h);
	fcntl(ev_fd.fd, F_SETFL, fcntl(ev_fd.fd, F_GETFL) | O_NONBLOCK);
	{
		int sz = 4 * 1024 * 1024;

		/* bigger receive buffer: bursts of new connections when a page loads */
		setsockopt(ev_fd.fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz));
	}
	ev_fd.cb = ev_cb;
	uloop_fd_add(&ev_fd, ULOOP_READ);

	judge_timer.cb = judge_cb;
	dump_timer.cb = dump_cb_timer;
	uloop_timeout_set(&dump_timer, 1000);
	active = 1;
	AG_INFO("flow sensor on (counters every %ds, no-DNS checks after %ds)", cfg.flow_poll, cfg.flow_warmup);
	return 0;
}

int flow_active(void)
{
	return active;
}

void flow_blob_recent(struct blob_buf *b, unsigned limit)
{
	void *arr, *t;
	unsigned i;

	if (limit > recent_count)
		limit = recent_count;
	arr = blobmsg_open_array(b, "flows");
	for (i = 0; i < limit; i++) {
		struct rflow *r = &recent[(recent_head + RECENT_MAX - 1 - i) % RECENT_MAX];
		struct device *d = dev_find_ip(r->client, 0);

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_u64(b, "end", (uint64_t)r->end);
		blobmsg_add_u32(b, "duration_s", r->start ? (uint32_t)(r->end - r->start) : 0);
		blobmsg_add_string(b, "client", r->client);
		blobmsg_add_string(b, "hostname", d ? d->hostname : "");
		blobmsg_add_string(b, "dst", r->dst);
		blobmsg_add_string(b, "domain", r->domain);
		blobmsg_add_string(b, "proto", proto_name(r->proto));
		blobmsg_add_u32(b, "dport", r->dport);
		blobmsg_add_u8(b, "no_dns", r->no_dns ? 1 : 0);
		blobmsg_add_string(b, "named_by", by_name[r->named_by]);
		blobmsg_add_u64(b, "bytes_up", r->up);
		blobmsg_add_u64(b, "bytes_down", r->down);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}
