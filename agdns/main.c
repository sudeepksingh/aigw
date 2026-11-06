/* SPDX-License-Identifier: Apache-2.0 */
/*
 * agdns: the AIGW DNS sensor.
 *
 * Forwards client DNS queries to an upstream resolver and reports every
 * answered query to the brain as a ubus event ("ag.dns"). Forwarding never
 * depends on ubus or agd: if they are down, DNS keeps working (fail-open).
 *
 * UDP is forwarded in-process with randomized upstream query IDs.
 * TCP connections are relayed by a short-lived child process each.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "ag/ag.h"
#include "ag/dns.h"

#define PENDING_MAX   4096
#define QUERY_TTL_US  (5 * 1000000ull)
#define TCP_CHILD_MAX 32
#define TCP_IDLE_MS   10000
#define BUF_LEN       65536

struct pending {
	int used;
	uint16_t up_id;          /* id we sent upstream */
	uint16_t client_id;      /* id the client used */
	struct sockaddr_storage client;
	socklen_t client_len;
	uint64_t t0_us;
	uint16_t qtype;
	char qname[AG_DNS_NAME_MAX];
};

static struct ag_config cfg;
static struct pending pend[PENDING_MAX];
static uint16_t id_slot[65536];        /* upstream id -> slot + 1 */
static int free_list[PENDING_MAX], free_top;

static struct sockaddr_storage up_addr;
static socklen_t up_len;

static struct uloop_fd udp_listen, udp_up, tcp_listen;
static struct uloop_timeout sweep_timer;
static struct ubus_auto_conn conn;
static struct blob_buf eb;
static uint8_t buf[BUF_LEN];
static int tcp_children;

static struct {
	uint64_t queries, answers, dropped, timeouts, tcp;
} stats;

/* ---------- helpers ---------- */

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint16_t rand16(void)
{
	uint16_t v;

	if (getrandom(&v, sizeof(v), 0) != sizeof(v))
		v = (uint16_t)random();
	return v;
}

static void addr_str(const struct sockaddr_storage *ss, char *out, size_t n)
{
	if (ss->ss_family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, out, n);
	else if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;

		/* show v4-mapped clients of a dual-stack socket as plain IPv4 */
		if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr))
			inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], out, n);
		else
			inet_ntop(AF_INET6, &s6->sin6_addr, out, n);
	} else
		snprintf(out, n, "?");
}

static int parse_addr(const char *host, int port, struct sockaddr_storage *ss, socklen_t *len, int passive)
{
	struct addrinfo hints = { 0 }, *res = NULL;
	char ports[8];
	int rc;

	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
	snprintf(ports, sizeof(ports), "%d", port);
	rc = getaddrinfo(host, ports, &hints, &res);
	if (rc || !res)
		return -1;
	memcpy(ss, res->ai_addr, res->ai_addrlen);
	*len = res->ai_addrlen;
	freeaddrinfo(res);
	return 0;
}

/* "auto": dnsmasq on OpenWRT, else the first nameserver in resolv.conf.
 * Otherwise "ip" or "ip#port". */
static int resolve_upstream(const char *spec)
{
	char host[128] = "", line[256];
	int port = 53;
	char *hash;

	if (!strcmp(spec, "auto")) {
		if (ag_is_openwrt()) {
			snprintf(host, sizeof(host), "127.0.0.1");
		} else {
			FILE *f = fopen("/etc/resolv.conf", "r");

			while (f && fgets(line, sizeof(line), f)) {
				char ns[128];

				if (sscanf(line, " nameserver %127s", ns) == 1) {
					snprintf(host, sizeof(host), "%s", ns);
					break;
				}
			}
			if (f)
				fclose(f);
			if (!*host)
				snprintf(host, sizeof(host), "1.1.1.1");
		}
	} else {
		snprintf(host, sizeof(host), "%s", spec);
		hash = strchr(host, '#');
		if (hash) {
			*hash = '\0';
			port = atoi(hash + 1);
		}
	}
	if (parse_addr(host, port, &up_addr, &up_len, 0) < 0) {
		AG_ERR("invalid upstream '%s'", spec);
		return -1;
	}
	AG_INFO("upstream %s#%d", host, port);
	return 0;
}

/* ---------- pending queries ---------- */

static struct pending *slot_alloc(void)
{
	struct pending *p;
	int i, tries;
	uint16_t id;

	if (free_top == 0)
		return NULL;
	for (tries = 0; tries < 64; tries++) {
		id = rand16();
		if (!id_slot[id])
			break;
	}
	if (id_slot[id])
		return NULL;
	i = free_list[--free_top];
	p = &pend[i];
	memset(p, 0, sizeof(*p));
	p->used = 1;
	p->up_id = id;
	id_slot[id] = (uint16_t)(i + 1);
	return p;
}

static void slot_free(struct pending *p)
{
	id_slot[p->up_id] = 0;
	p->used = 0;
	free_list[free_top++] = (int)(p - pend);
}

static void sweep_cb(struct uloop_timeout *t)
{
	uint64_t now = ag_mono_us();
	int i;

	for (i = 0; i < PENDING_MAX; i++) {
		if (pend[i].used && now - pend[i].t0_us > QUERY_TTL_US) {
			stats.timeouts++;
			slot_free(&pend[i]);
		}
	}
	uloop_timeout_set(t, 1000);
}

/* ---------- events to the brain ---------- */

static void emit_event(const struct pending *p, const struct ag_dns_msg *resp, uint64_t lat_us)
{
	char ip[INET6_ADDRSTRLEN];
	void *arr;
	int i;

	if (conn.ctx.sock.fd < 0)
		return; /* not connected: forwarding continues regardless */

	blob_buf_init(&eb, 0);
	addr_str(&p->client, ip, sizeof(ip));
	blobmsg_add_string(&eb, "client", ip);
	blobmsg_add_string(&eb, "qname", p->qname);
	blobmsg_add_u32(&eb, "qtype", p->qtype);
	blobmsg_add_u32(&eb, "rcode", AG_DNS_RCODE(resp->flags));
	blobmsg_add_u32(&eb, "latency_us", (uint32_t)lat_us);
	arr = blobmsg_open_array(&eb, "answers");
	for (i = 0; i < resp->n_ans; i++) {
		char a[INET6_ADDRSTRLEN];

		inet_ntop(resp->ans[i].family, resp->ans[i].addr, a, sizeof(a));
		blobmsg_add_string(&eb, NULL, a);
	}
	blobmsg_close_array(&eb, arr);
	if (resp->n_ans)
		blobmsg_add_u32(&eb, "ttl", resp->ans[0].ttl);
	ubus_send_event(&conn.ctx, AG_EV_DNS, eb.head);
}

/* ---------- UDP path ---------- */

static void client_cb(struct uloop_fd *u, unsigned int events)
{
	for (;;) {
		struct sockaddr_storage from;
		socklen_t flen = sizeof(from);
		struct ag_dns_msg q;
		struct pending *p;
		ssize_t n;

		n = recvfrom(u->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
		if (n < 0)
			return; /* EAGAIN or error: wait for the next readiness */
		if (n < AG_DNS_HDR_LEN || AG_DNS_IS_RESPONSE((buf[2] << 8) | buf[3])) {
			stats.dropped++;
			continue;
		}
		stats.queries++;
		if (ag_dns_parse(buf, (size_t)n, &q) < 0)
			q.qname[0] = '\0'; /* still forward: upstream decides */

		p = slot_alloc();
		if (!p) {
			stats.dropped++;
			continue;
		}
		p->client_id = (uint16_t)((buf[0] << 8) | buf[1]);
		memcpy(&p->client, &from, flen);
		p->client_len = flen;
		p->t0_us = ag_mono_us();
		p->qtype = q.qtype;
		snprintf(p->qname, sizeof(p->qname), "%s", q.qname[0] ? q.qname : "?");

		buf[0] = (uint8_t)(p->up_id >> 8);
		buf[1] = (uint8_t)(p->up_id & 0xff);
		if (send(udp_up.fd, buf, (size_t)n, 0) < 0) {
			AG_DEBUG("send upstream: %s", strerror(errno));
			stats.dropped++;
			slot_free(p);
		}
	}
}

static void upstream_cb(struct uloop_fd *u, unsigned int events)
{
	for (;;) {
		struct ag_dns_msg r;
		struct pending *p;
		uint16_t id, s;
		ssize_t n;

		n = recv(u->fd, buf, sizeof(buf), 0);
		if (n < 0)
			return;
		if (n < AG_DNS_HDR_LEN)
			continue;
		id = (uint16_t)((buf[0] << 8) | buf[1]);
		s = id_slot[id];
		if (!s)
			continue; /* late or unknown answer */
		p = &pend[s - 1];

		buf[0] = (uint8_t)(p->client_id >> 8);
		buf[1] = (uint8_t)(p->client_id & 0xff);
		sendto(udp_listen.fd, buf, (size_t)n, 0, (struct sockaddr *)&p->client, p->client_len);
		stats.answers++;

		if (ag_dns_parse(buf, (size_t)n, &r) == 0)
			emit_event(p, &r, ag_mono_us() - p->t0_us);
		slot_free(p);
	}
}

/* ---------- TCP path (rare: truncated answers, zone transfers) ---------- */

static void tcp_relay(int cfd)
{
	struct pollfd pfd[2];
	struct timeval tv = { 3, 0 };
	int ufd;

	ufd = socket(up_addr.ss_family, SOCK_STREAM, 0);
	if (ufd < 0)
		_exit(1);
	setsockopt(ufd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(ufd, (struct sockaddr *)&up_addr, up_len) < 0)
		_exit(1);

	pfd[0].fd = cfd;
	pfd[1].fd = ufd;
	pfd[0].events = pfd[1].events = POLLIN;
	for (;;) {
		int i, rc = poll(pfd, 2, TCP_IDLE_MS);

		if (rc <= 0)
			_exit(0);
		for (i = 0; i < 2; i++) {
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				ssize_t n = read(pfd[i].fd, buf, sizeof(buf));

				if (n <= 0 || write(pfd[!i].fd, buf, (size_t)n) != n)
					_exit(0);
			}
		}
	}
}

static void tcp_child_done(struct uloop_process *p, int ret)
{
	tcp_children--;
	free(p);
}

static void tcp_accept_cb(struct uloop_fd *u, unsigned int events)
{
	for (;;) {
		int cfd = accept(u->fd, NULL, NULL);
		pid_t pid;

		if (cfd < 0)
			return;
		if (tcp_children >= TCP_CHILD_MAX) {
			close(cfd);
			continue;
		}
		stats.tcp++;
		pid = fork();
		if (pid == 0) {
			close(udp_listen.fd);
			close(udp_up.fd);
			close(tcp_listen.fd);
			tcp_relay(cfd);
			_exit(0);
		}
		if (pid > 0) {
			struct uloop_process *pr = calloc(1, sizeof(*pr));

			if (pr) {
				pr->pid = pid;
				pr->cb = tcp_child_done;
				uloop_process_add(pr);
				tcp_children++;
			}
		}
		close(cfd);
	}
}

/* ---------- setup ---------- */

static int open_listen(int type)
{
	struct sockaddr_storage ss;
	socklen_t len;
	int fd, one = 1, zero = 0;

	if (parse_addr(cfg.dns_listen, cfg.dns_port, &ss, &len, 1) < 0) {
		AG_ERR("invalid listen address '%s'", cfg.dns_listen);
		return -1;
	}
	fd = socket(ss.ss_family, type, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ss.ss_family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	if (bind(fd, (struct sockaddr *)&ss, len) < 0) {
		AG_ERR("bind %s#%d (%s): %s", cfg.dns_listen, cfg.dns_port,
		       type == SOCK_STREAM ? "tcp" : "udp", strerror(errno));
		close(fd);
		return -1;
	}
	if (type == SOCK_STREAM && listen(fd, 32) < 0) {
		close(fd);
		return -1;
	}
	set_nonblock(fd);
	return fd;
}

static void on_ubus_connect(struct ubus_context *ctx)
{
	AG_INFO("connected to ubus");
}

int main(int argc, char **argv)
{
	int i;

	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		printf("agdns %s\n", AG_VERSION);
		return 0;
	}

	ag_config_load(&cfg);
	ag_log_init("agdns", ag_log_level_parse(cfg.log_level));

	if (resolve_upstream(cfg.dns_upstream) < 0)
		return 1;

	for (i = 0; i < PENDING_MAX; i++)
		free_list[i] = PENDING_MAX - 1 - i;
	free_top = PENDING_MAX;

	uloop_init();

	udp_listen.fd = open_listen(SOCK_DGRAM);
	tcp_listen.fd = open_listen(SOCK_STREAM);
	if (udp_listen.fd < 0)
		return 1;

	udp_up.fd = socket(up_addr.ss_family, SOCK_DGRAM, 0);
	if (udp_up.fd < 0 || connect(udp_up.fd, (struct sockaddr *)&up_addr, up_len) < 0) {
		AG_ERR("upstream socket: %s", strerror(errno));
		return 1;
	}
	set_nonblock(udp_up.fd);

	udp_listen.cb = client_cb;
	udp_up.cb = upstream_cb;
	uloop_fd_add(&udp_listen, ULOOP_READ);
	uloop_fd_add(&udp_up, ULOOP_READ);
	if (tcp_listen.fd >= 0) {
		tcp_listen.cb = tcp_accept_cb;
		uloop_fd_add(&tcp_listen, ULOOP_READ);
	}

	sweep_timer.cb = sweep_cb;
	uloop_timeout_set(&sweep_timer, 1000);

	conn.path = ag_ubus_path();
	conn.cb = on_ubus_connect;
	conn.ctx.sock.fd = -1;
	ubus_auto_connect(&conn);

	signal(SIGPIPE, SIG_IGN);
	AG_INFO("listening on %s#%d", cfg.dns_listen, cfg.dns_port);
	uloop_run();

	AG_INFO("stopping: %llu queries, %llu answers, %llu timeouts, %llu dropped, %llu tcp",
		(unsigned long long)stats.queries, (unsigned long long)stats.answers,
		(unsigned long long)stats.timeouts, (unsigned long long)stats.dropped,
		(unsigned long long)stats.tcp);
	uloop_done();
	return 0;
}
