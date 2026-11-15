/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TR-143 diagnostics: the standard tests an ISP's management system runs from
 * a gateway, for USP (Device.IP.Diagnostics.IPPing() and the others, as
 * asynchronous commands) and TR-069 (the same objects with DiagnosticsState:
 * the ACS writes the inputs and DiagnosticsState = Requested, icwmp runs the
 * command when the session ends and sends "8 DIAGNOSTICS COMPLETE"; the ACS
 * reads the results from the object).
 *
 *   IPPing                echo requests, one after the other
 *   TraceRoute            echo requests with a growing TTL (ICMP, not UDP)
 *   DownloadDiagnostics   HTTP GET, timed as TR-143 says (ROM, BOM, EOM, TCP open)
 *   UploadDiagnostics     HTTP PUT of TestFileLength bytes (chunked when time based)
 *   NSLookupDiagnostics   A queries to DNSServer, else the gateway's own DNS
 *
 * One test of each kind at a time, each in a child process (blocking code,
 * nothing on agd's loop), killed if it runs far past what it was given.
 * Proactive care's nightly speed test is a DownloadDiagnostics too.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "ag/dns.h"
#include "ag/tr143.h"
#include "agd.h"

#define CONNECT_MS 10000
#define IDLE_MS    10000                /* a transfer with nothing for this long has failed */
#define WHOLE_S    600                  /* a download or upload of a whole file, at most */

static int64_t wall_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ================= the tests (in the child) ================= */

/* the result's status and its reason in words; returns 0, so a test can end with it */
static int why(struct tr143_out *o, int status, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int why(struct tr143_out *o, int status, const char *fmt, ...)
{
	va_list ap;

	o->status = status;
	va_start(ap, fmt);
	vsnprintf(o->why, sizeof(o->why), fmt, ap);
	va_end(ap);
	return 0;
}

static int resolve4(const char *host, struct in_addr *a)
{
	struct addrinfo hints = { .ai_family = AF_INET }, *res;

	if (!host || !*host)
		return -1;
	if (inet_pton(AF_INET, host, a) == 1)
		return 0;
	if (getaddrinfo(host, NULL, &hints, &res) || !res)
		return -1;
	*a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	freeaddrinfo(res);
	return 0;
}

/* the gateway's own address toward dst (IPAddressUsed) */
static void src_for(struct in_addr dst, char *out, size_t n)
{
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr = dst }, me;
	socklen_t l = sizeof(me);
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	out[0] = '\0';
	if (s < 0)
		return;
	if (!connect(s, (struct sockaddr *)&to, sizeof(to)) && !getsockname(s, (struct sockaddr *)&me, &l))
		inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n);
	close(s);
}

static int ipv6_asked(const struct tr143_in *in, struct tr143_out *o)
{
	if (strcasecmp(in->proto, "IPv6"))
		return 0;
	why(o, TS_OTHER, "IPv6 is not supported (IPv4 only)");
	return 1;
}

/* the Internet checksum, read byte by byte (see diag.c) */
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

static unsigned char pkt[8 + 65000], rbuf[2048];

static int icmp_socket(unsigned dscp)
{
	int s = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP), tos = (int)(dscp << 2);

	if (s >= 0 && tos)
		setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	return s;
}

static int send_echo(int s, struct in_addr dst, uint16_t id, uint16_t seq, unsigned size)
{
	struct icmphdr h;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = dst };
	size_t i, len;

	if (size > sizeof(pkt) - 8)
		size = sizeof(pkt) - 8;
	len = 8 + size;
	memset(&h, 0, sizeof(h));
	h.type = ICMP_ECHO;
	h.un.echo.id = htons(id);
	h.un.echo.sequence = htons(seq);
	memcpy(pkt, &h, 8);
	for (i = 8; i < len; i++)
		pkt[i] = (unsigned char)('a' + i % 23);
	h.checksum = csum(pkt, len);
	memcpy(pkt, &h, 8);
	return sendto(s, pkt, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0 ? -1 : 0;
}

/*
 * The next ICMP answer to (id, seq) before the deadline: its type, sender and
 * code; -1 on timeout (an echo reply is type 0). Echo replies count only from
 * dst; time exceeded and unreachable when they quote our request.
 */
static int wait_answer(int s, struct in_addr dst, uint16_t id, uint16_t seq, int64_t deadline, struct in_addr *from,
		       int *code)
{
	for (;;) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		int64_t left = deadline - mono_us();
		struct sockaddr_in sa;
		socklen_t sl = sizeof(sa);
		struct iphdr ip, in_ip;
		struct icmphdr ic, in_ic;
		ssize_t n;
		size_t off;

		if (left <= 0)
			return -1;
		if (poll(&p, 1, (int)((left + 999) / 1000)) <= 0)
			continue;
		n = recvfrom(s, rbuf, sizeof(rbuf), MSG_DONTWAIT, (struct sockaddr *)&sa, &sl);
		if (n < (ssize_t)sizeof(ip))
			continue;
		memcpy(&ip, rbuf, sizeof(ip));
		off = (size_t)ip.ihl * 4;
		if ((size_t)n < off + 8)
			continue;
		memcpy(&ic, rbuf + off, 8);
		if (ic.type == ICMP_ECHOREPLY) {
			if (ntohs(ic.un.echo.id) != id || ntohs(ic.un.echo.sequence) != seq || ip.saddr != dst.s_addr)
				continue;
			from->s_addr = ip.saddr;
			*code = 0;
			return ICMP_ECHOREPLY;
		}
		if (ic.type != ICMP_TIME_EXCEEDED && ic.type != ICMP_DEST_UNREACH)
			continue;
		if ((size_t)n < off + 8 + sizeof(in_ip))
			continue;
		memcpy(&in_ip, rbuf + off + 8, sizeof(in_ip));
		if ((size_t)n < off + 8 + (size_t)in_ip.ihl * 4 + 8 || in_ip.daddr != dst.s_addr)
			continue;
		memcpy(&in_ic, rbuf + off + 8 + (size_t)in_ip.ihl * 4, 8);
		if (in_ic.type != ICMP_ECHO || ntohs(in_ic.un.echo.id) != id || ntohs(in_ic.un.echo.sequence) != seq)
			continue;
		from->s_addr = ip.saddr;
		*code = ic.code;
		return ic.type;
	}
}

static int run_ping(const struct tr143_in *in, struct tr143_out *o)
{
	struct in_addr dst, from;
	uint16_t id = (uint16_t)(getpid() ^ 0x5431);
	unsigned long long sum = 0;
	unsigned i;
	int s, code;

	if (ipv6_asked(in, o))
		return 0;
	if (resolve4(in->host, &dst) < 0)
		return why(o, TS_CANNOT_RESOLVE, "cannot resolve %.100s", in->host);
	src_for(dst, o->ip_used, sizeof(o->ip_used));
	if ((s = icmp_socket(in->dscp)) < 0)
		return why(o, TS_INTERNAL, "no ICMP socket: %s", strerror(errno));
	o->min_us = ~0u;
	for (i = 0; i < in->reps; i++) {
		int64_t t0 = mono_us();
		unsigned us;

		if (send_echo(s, dst, id, (uint16_t)i, in->size) < 0 ||
		    wait_answer(s, dst, id, (uint16_t)i, t0 + (int64_t)in->timeout_ms * 1000, &from, &code) != ICMP_ECHOREPLY) {
			o->fail++;
			continue;
		}
		us = (unsigned)(mono_us() - t0);
		o->ok++;
		sum += us;
		if (us < o->min_us)
			o->min_us = us;
		if (us > o->max_us)
			o->max_us = us;
		/* answers come fast; a pause keeps them echo requests, not a flood */
		if (i + 1 < in->reps && us < 200000)
			usleep(200000 - us);
	}
	close(s);
	if (!o->ok)
		o->min_us = 0;
	o->avg_us = o->ok ? (unsigned)(sum / o->ok) : 0;
	o->avg_ms = (o->avg_us + 500) / 1000;
	o->min_ms = (o->min_us + 500) / 1000;
	o->max_ms = (o->max_us + 500) / 1000;
	return why(o, TS_COMPLETE, "%u of %u answered%s", o->ok, o->ok + o->fail, o->ok ? "" : " (none)");
}

static int run_trace(const struct tr143_in *in, struct tr143_out *o)
{
	struct in_addr dst;
	uint16_t id = (uint16_t)(getpid() ^ 0x5452);
	unsigned ttl;
	int s, reached = 0, last_ms = 0;

	if (ipv6_asked(in, o))
		return 0;
	if (resolve4(in->host, &dst) < 0)
		return why(o, TS_CANNOT_RESOLVE, "cannot resolve %.100s", in->host);
	src_for(dst, o->ip_used, sizeof(o->ip_used));
	if ((s = icmp_socket(in->dscp)) < 0)
		return why(o, TS_INTERNAL, "no ICMP socket: %s", strerror(errno));
	for (ttl = 1; ttl <= in->max_hops && !reached && o->nhops < TR143_HOPS; ttl++) {
		struct tr143_hop *h = &o->hop[o->nhops++];
		unsigned t;
		size_t len = 0;
		int t_ttl = (int)ttl;

		setsockopt(s, IPPROTO_IP, IP_TTL, &t_ttl, sizeof(t_ttl));
		for (t = 0; t < in->reps; t++) {
			uint16_t seq = (uint16_t)(ttl << 8 | t);
			int64_t t0 = mono_us();
			struct in_addr from;
			int code = 0, type;

			if (send_echo(s, dst, id, seq, in->size) < 0)
				continue;
			type = wait_answer(s, dst, id, seq, t0 + (int64_t)in->timeout_ms * 1000, &from, &code);
			if (type < 0)
				continue;
			last_ms = (int)((mono_us() - t0 + 500) / 1000);
			inet_ntop(AF_INET, &from, h->addr, sizeof(h->addr));
			h->code = code;
			if (len < sizeof(h->rtts))
				len += (size_t)snprintf(h->rtts + len, sizeof(h->rtts) - len, "%s%d", len ? "," : "", last_ms);
			if (type == ICMP_ECHOREPLY || type == ICMP_DEST_UNREACH)
				reached = 1;
		}
	}
	close(s);
	if (!reached)
		return why(o, TS_MAX_HOPS, "%.100s not reached in %u hops", in->host, in->max_hops);
	o->resp_ms = (unsigned)last_ms;
	return why(o, TS_COMPLETE, "%d hop%s to %.100s", o->nhops, o->nhops == 1 ? "" : "s", in->host);
}

/* ---------- HTTP ---------- */

/* the interface a connected socket goes out of, and its byte counters */
static int sock_iface(int s, char *ifn, size_t n)
{
	struct sockaddr_in me;
	socklen_t l = sizeof(me);
	struct ifaddrs *ifa, *i;
	int rc = -1;

	if (getsockname(s, (struct sockaddr *)&me, &l) || getifaddrs(&ifa))
		return -1;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
		    ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == me.sin_addr.s_addr) {
			snprintf(ifn, n, "%s", i->ifa_name);
			rc = 0;
			break;
		}
	freeifaddrs(ifa);
	return rc;
}

static void if_bytes(const char *ifn, unsigned long long *rx, unsigned long long *tx)
{
	char line[512];
	FILE *f = fopen("/proc/net/dev", "r");

	*rx = *tx = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':'), *name = line;
		unsigned long long v[9];

		if (!c)
			continue;
		*c = '\0';
		while (*name == ' ')
			name++;
		if (strcmp(name, ifn))
			continue;
		if (sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
			   &v[6], &v[7], &v[8]) == 9) {
			*rx = v[0];
			*tx = v[8];
		}
		break;
	}
	fclose(f);
}

static int tcp_open(const struct tr143_in *in, const char *host, unsigned port, struct tr143_out *o)
{
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	struct pollfd p;
	int s, e = 0, tos = (int)(in->dscp << 2);
	socklen_t el = sizeof(e);

	if (resolve4(host, &to.sin_addr) < 0) {
		why(o, TS_CANNOT_RESOLVE, "cannot resolve %.100s", host);
		return -1;
	}
	if ((s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) {
		why(o, TS_INTERNAL, "no TCP socket: %s", strerror(errno));
		return -1;
	}
	if (tos)
		setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	o->tcp_req_us = wall_us();
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EINPROGRESS)
		e = errno;
	if (!e) {
		p.fd = s;
		p.events = POLLOUT;
		if (poll(&p, 1, CONNECT_MS) <= 0)
			e = ETIMEDOUT;
		else if (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0)
			e = errno;
	}
	if (e) {
		close(s);
		why(o, e == ENETUNREACH || e == EHOSTUNREACH ? TS_NO_ROUTE : TS_INIT_CONN, "cannot connect to %.100s:%u: %s",
		    host, port, strerror(e));
		return -1;
	}
	o->tcp_resp_us = wall_us();
	{
		struct sockaddr_in me;
		socklen_t l = sizeof(me);

		if (!getsockname(s, (struct sockaddr *)&me, &l))
			inet_ntop(AF_INET, &me.sin_addr, o->ip_used, sizeof(o->ip_used));
	}
	return s;
}

static int send_all(int s, const char *buf, size_t n)
{
	while (n) {
		struct pollfd p = { .fd = s, .events = POLLOUT };
		ssize_t w = send(s, buf, n, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (w > 0) {
			buf += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		if (poll(&p, 1, IDLE_MS) <= 0)
			return -1;
	}
	return 0;
}

static void host_header(char *buf, size_t n, const char *host, unsigned port)
{
	if (port == 80)
		snprintf(buf, n, "%s", host);
	else
		snprintf(buf, n, "%s:%u", host, port);
}

static int run_download(const struct tr143_in *in, struct tr143_out *o)
{
	char host[256], path[256], hh[300], req[700], head[16384], ifn[32] = "";
	unsigned long long rx0 = 0, tx0 = 0, rx1, tx1, body = 0;
	unsigned port;
	size_t hl = 0, hdr = 0;
	long long clen = -1;
	int s, code = 0, have_head = 0, timed = 0;

	if (ipv6_asked(in, o))
		return 0;
	if (tr143_url(in->url, host, sizeof(host), &port, path, sizeof(path)) < 0)
		return why(o, TS_INIT_CONN, "not an http:// URL: %.100s", in->url);
	if ((s = tcp_open(in, host, port, o)) < 0)
		return 0;
	host_header(hh, sizeof(hh), host, port);
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AIGW-OS-TR143/%s\r\nAccept: */*\r\n"
		 "Connection: close\r\n\r\n", path, hh, AG_VERSION);
	if (!sock_iface(s, ifn, sizeof(ifn)))
		if_bytes(ifn, &rx0, &tx0);
	o->rom_us = wall_us();
	if (send_all(s, req, strlen(req)) < 0) {
		close(s);
		return why(o, TS_NO_RESPONSE, "the request could not be sent");
	}
	for (;;) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		char buf[65536];
		ssize_t n;
		int pr = poll(&p, 1, IDLE_MS);

		if (pr <= 0) {
			close(s);
			if (!o->test_rx)
				return why(o, TS_NO_RESPONSE, "no answer from %.100s", host);
			return why(o, TS_TIMEOUT, "nothing for %d s after %llu bytes", IDLE_MS / 1000, o->test_rx);
		}
		n = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n < 0) {
			close(s);
			return why(o, TS_TRANSFER, "the transfer failed after %llu bytes: %s", o->test_rx, strerror(errno));
		}
		if (n == 0)
			break;
		if (!o->bom_us)
			o->bom_us = wall_us();
		o->eom_us = wall_us();
		o->test_rx += (unsigned long long)n;
		if (!have_head) {
			size_t k = (size_t)n < sizeof(head) - hl ? (size_t)n : sizeof(head) - hl;
			int r;

			memcpy(head + hl, buf, k);
			hl += k;
			r = tr143_http_head(head, hl, &code, &clen, &hdr);
			if (r < 0) {
				close(s);
				return why(o, TS_TRANSFER, "not an HTTP answer");
			}
			if (r == 1) {
				have_head = 1;
				if (code < 200 || code > 299) {
					close(s);
					return why(o, TS_TRANSFER, "the server answered HTTP %d", code);
				}
				body = hl - hdr + ((size_t)n - k);
			}
		} else
			body += (unsigned long long)n;
		if (have_head && clen >= 0 && body >= (unsigned long long)clen)
			break;
		if (in->duration_s && o->eom_us - o->bom_us >= (int64_t)in->duration_s * 1000000) {
			timed = 1;
			break;
		}
	}
	close(s);
	if (!have_head)
		return why(o, TS_NO_RESPONSE, "no HTTP answer");
	if (!timed && clen >= 0 && body < (unsigned long long)clen)
		return why(o, TS_TRANSFER, "the server stopped after %llu of %lld bytes", body, clen);
	if (ifn[0]) {
		if_bytes(ifn, &rx1, &tx1);
		o->total_rx = rx1 >= rx0 ? rx1 - rx0 : 0;
		o->total_tx = tx1 >= tx0 ? tx1 - tx0 : 0;
	}
	if (o->total_rx < o->test_rx)
		o->total_rx = o->test_rx;
	if (!o->total_tx)
		o->total_tx = strlen(req);
	return why(o, TS_COMPLETE, "%llu bytes in %.2f s: %llu kbit/s%s", o->test_rx, (o->eom_us - o->bom_us) / 1e6,
	    tr143_kbps(o->test_rx, o->bom_us, o->eom_us), timed ? " (time based)" : "");
}

static int run_upload(const struct tr143_in *in, struct tr143_out *o)
{
	char host[256], path[256], hh[300], req[800], head[4096], ifn[32] = "", chunk[16384 + 16];
	unsigned long long rx0 = 0, tx0 = 0, rx1, tx1, sent = 0;
	unsigned port;
	size_t hl = 0, hdr;
	long long clen;
	int s, code = 0, chunked = in->duration_s > 0;
	int64_t until = 0;

	if (ipv6_asked(in, o))
		return 0;
	if (tr143_url(in->url, host, sizeof(host), &port, path, sizeof(path)) < 0)
		return why(o, TS_INIT_CONN, "not an http:// URL: %.100s", in->url);
	if ((s = tcp_open(in, host, port, o)) < 0)
		return 0;
	host_header(hh, sizeof(hh), host, port);
	if (chunked)
		snprintf(req, sizeof(req), "PUT %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AIGW-OS-TR143/%s\r\n"
			 "Content-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
			 path, hh, AG_VERSION);
	else
		snprintf(req, sizeof(req), "PUT %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AIGW-OS-TR143/%s\r\n"
			 "Content-Type: application/octet-stream\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
			 path, hh, AG_VERSION, in->file_len);
	if (!sock_iface(s, ifn, sizeof(ifn)))
		if_bytes(ifn, &rx0, &tx0);
	o->rom_us = wall_us();
	if (send_all(s, req, strlen(req)) < 0) {
		close(s);
		return why(o, TS_NO_RESPONSE, "the request could not be sent");
	}
	o->test_tx = strlen(req);
	memset(chunk, 'x', sizeof(chunk));
	o->bom_us = wall_us();
	until = chunked ? o->bom_us + (int64_t)in->duration_s * 1000000 : 0;
	while (chunked ? wall_us() < until : sent < in->file_len) {
		size_t k = 16384;

		if (!chunked && in->file_len - sent < k)
			k = (size_t)(in->file_len - sent);
		if (chunked) {
			char lenl[16];
			int ll = snprintf(lenl, sizeof(lenl), "%zx\r\n", k);

			if (send_all(s, lenl, (size_t)ll) < 0 || send_all(s, chunk, k) < 0 || send_all(s, "\r\n", 2) < 0)
				break;
			o->test_tx += (unsigned long long)ll + 2;
		} else if (send_all(s, chunk, k) < 0)
			break;
		sent += k;
		o->test_tx += k;
	}
	if (chunked && send_all(s, "0\r\n\r\n", 5) == 0)
		o->test_tx += 5;
	if (!chunked && sent < in->file_len) {
		close(s);
		return why(o, TS_TRANSFER, "the upload stopped after %llu of %llu bytes", sent, in->file_len);
	}
	/* the server's answer ends it */
	for (;;) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		ssize_t n;
		int r;

		if (poll(&p, 1, IDLE_MS) <= 0) {
			close(s);
			return why(o, TS_NO_RESPONSE, "no answer after the upload");
		}
		n = recv(s, head + hl, sizeof(head) - 1 - hl, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0) {
			close(s);
			return why(o, TS_NO_RESPONSE, "the server closed without an answer");
		}
		hl += (size_t)n;
		r = tr143_http_head(head, hl, &code, &clen, &hdr);
		if (r < 0 || (r == 0 && hl >= sizeof(head) - 1)) {
			close(s);
			return why(o, TS_OTHER, "not an HTTP answer");
		}
		if (r == 1)
			break;
	}
	o->eom_us = wall_us();
	close(s);
	if (code < 200 || code > 299)
		return why(o, TS_OTHER, "the server answered HTTP %d", code);
	if (ifn[0]) {
		if_bytes(ifn, &rx1, &tx1);
		o->total_rx = rx1 >= rx0 ? rx1 - rx0 : 0;
		o->total_tx = tx1 >= tx0 ? tx1 - tx0 : 0;
	}
	if (o->total_tx < o->test_tx)
		o->total_tx = o->test_tx;
	if (!o->total_rx)
		o->total_rx = hl;
	return why(o, TS_COMPLETE, "%llu bytes in %.2f s: %llu kbit/s%s", sent, (o->eom_us - o->bom_us) / 1e6,
	    tr143_kbps(sent, o->bom_us, o->eom_us), chunked ? " (time based)" : "");
}

/* ---------- NSLookup ---------- */

static int dns_query(const char *name, uint16_t id, unsigned char *q, size_t qn)
{
	const char *p = name;
	size_t k = 12;

	memset(q, 0, 12);
	q[0] = (unsigned char)(id >> 8);
	q[1] = (unsigned char)id;
	q[2] = 0x01;                    /* recursion desired */
	q[5] = 1;
	while (*p) {
		size_t l = strcspn(p, ".");

		if (!l || l > 63 || k + l + 1 + 5 >= qn)
			return -1;
		q[k++] = (unsigned char)l;
		memcpy(q + k, p, l);
		k += l;
		p += l;
		if (*p == '.')
			p++;
	}
	q[k++] = 0;
	q[k++] = 0, q[k++] = 1;         /* A */
	q[k++] = 0, q[k++] = 1;         /* IN */
	return (int)k;
}

static int run_nslookup(const struct tr143_in *in, struct tr143_out *o)
{
	struct sockaddr_in to = { .sin_family = AF_INET };
	char server[48];
	unsigned i;

	if (in->server[0]) {
		if (resolve4(in->server, &to.sin_addr) < 0)
			return why(o, TS_DNS_SERVER, "cannot resolve the DNS server %.100s", in->server);
		to.sin_port = htons(53);
	} else {
		if (inet_pton(AF_INET, in->dns_ip, &to.sin_addr) != 1 || !to.sin_addr.s_addr)
			inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
		to.sin_port = htons((uint16_t)(in->dns_port ? in->dns_port : 53));
	}
	inet_ntop(AF_INET, &to.sin_addr, server, sizeof(server));
	if (!in->host[0])
		return why(o, TS_OTHER, "no HostName");
	for (i = 0; i < in->reps; i++) {
		struct tr143_ns tmp, *r = o->nns < TR143_NSRES ? &o->ns[o->nns++] : &tmp;
		unsigned char q[300], a[1500];
		uint16_t id = (uint16_t)(mono_us() ^ (i * 7919));
		int64_t t0, deadline;
		int s, ql = dns_query(in->host, id, q, sizeof(q)), got = 0;

		memset(r, 0, sizeof(*r));
		r->authoritative = -1;
		snprintf(r->server, sizeof(r->server), "%s", server);
		if (ql < 0) {
			r->status = NS_OTHER;
			continue;
		}
		if ((s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) {
			r->status = NS_OTHER;
			continue;
		}
		t0 = mono_us();
		deadline = t0 + (int64_t)in->timeout_ms * 1000;
		if (connect(s, (struct sockaddr *)&to, sizeof(to)) < 0 || send(s, q, (size_t)ql, 0) < 0) {
			close(s);
			r->status = NS_SERVER_NA;
			continue;
		}
		while (!got) {
			struct pollfd p = { .fd = s, .events = POLLIN };
			struct ag_dns_msg m;
			int64_t left = deadline - mono_us();
			ssize_t n;

			if (left <= 0 || poll(&p, 1, (int)((left + 999) / 1000)) <= 0)
				break;
			n = recv(s, a, sizeof(a), MSG_DONTWAIT);
			if (n < 0 && errno == ECONNREFUSED) {
				got = -1;
				break;
			}
			if (n < AG_DNS_HDR_LEN || ag_dns_parse(a, (size_t)n, &m) < 0 || m.id != id ||
			    !AG_DNS_IS_RESPONSE(m.flags))
				continue;
			got = 1;
			r->ms = (unsigned)((mono_us() - t0 + 500) / 1000);
			if (AG_DNS_RCODE(m.flags) == 0 && m.n_ans > 0) {
				size_t len = 0;
				int k;

				r->status = NS_SUCCESS;
				r->authoritative = (m.flags & 0x0400) ? 1 : 0;
				snprintf(r->name, sizeof(r->name), "%.127s", in->host);
				for (k = 0; k < m.n_ans && len < sizeof(r->ips); k++) {
					char ip[48];

					if (m.ans[k].family != AF_INET)
						continue;
					inet_ntop(AF_INET, m.ans[k].addr, ip, sizeof(ip));
					len += (size_t)snprintf(r->ips + len, sizeof(r->ips) - len, "%s%s", len ? "," : "", ip);
				}
				o->ns_ok++;
			} else if (AG_DNS_RCODE(m.flags) == 0 || AG_DNS_RCODE(m.flags) == 3) {
				r->status = NS_NOT_RESOLVED;
			} else {
				r->status = NS_SERVER_NA;
			}
		}
		close(s);
		if (got == 0)
			r->status = NS_TIMEOUT;
		else if (got < 0)
			r->status = NS_SERVER_NA;
	}
	return why(o, TS_COMPLETE, "%u of %u lookups of %.100s answered by %s", o->ns_ok, in->reps, in->host, server);
}

static void run(int kind, const struct tr143_in *in, struct tr143_out *o)
{
	switch (kind) {
	case TK_PING: run_ping(in, o); break;
	case TK_TRACE: run_trace(in, o); break;
	case TK_DOWNLOAD: run_download(in, o); break;
	case TK_UPLOAD: run_upload(in, o); break;
	case TK_NSLOOKUP: run_nslookup(in, o); break;
	default: why(o, TS_INTERNAL, "no such test");
	}
}

/* ================= the runner (in agd) ================= */

static struct job {
	int running, requested, reset, killed, canceled;
	struct uloop_process proc;
	struct uloop_timeout kill;
	int fd;
	struct tr143_in in;             /* the inputs of the run */
	struct tr143_out out;           /* the last result */
	int64_t at;                     /* when it ended (wall us) */
	char who[48];
	tr143_cb cb;
	void *ctx;
} J[TK_N];

static struct tr143_in stored[TK_N];    /* TR-069's writable inputs (the objects' own) */

static int limit_s(int kind, const struct tr143_in *in)
{
	switch (kind) {
	case TK_PING: return (int)(in->reps * (in->timeout_ms + 200) / 1000) + 10;
	case TK_TRACE: return (int)(in->max_hops * in->reps * (in->timeout_ms / 1000 + 1)) + 10;
	case TK_NSLOOKUP: return (int)(in->reps * (in->timeout_ms / 1000 + 1)) + 10;
	default: return in->duration_s ? (int)in->duration_s + 3 * IDLE_MS / 1000 + 10 : WHOLE_S;
	}
}

static void job_exit(struct uloop_process *p, int status)
{
	struct job *j = container_of(p, struct job, proc);
	int kind = (int)(j - J);
	struct tr143_out o;
	size_t got = 0;

	(void)status;
	uloop_timeout_cancel(&j->kill);
	memset(&o, 0, sizeof(o));
	for (;;) {
		ssize_t n = read(j->fd, (char *)&o + got, sizeof(o) - got);

		if (n > 0 && (got += (size_t)n) < sizeof(o))
			continue;
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	close(j->fd);
	j->fd = -1;
	if (got != sizeof(o)) {
		memset(&o, 0, sizeof(o));
		if (j->killed)
			why(&o, TS_TIMEOUT, "stopped after %d s", limit_s(kind, &j->in));
		else
			why(&o, TS_INTERNAL, "the test ended without a result");
	}
	if (j->canceled)
		o.status = TS_NONE;
	j->out = o;
	j->at = wall_us();
	j->running = j->requested = j->killed = j->canceled = 0;
	AG_INFO("tr143: %s by %s: %s -- %s", tr143_kind_name(kind), j->who, tr143_status_name(o.status), o.why);
	if (j->cb) {
		tr143_cb cb = j->cb;

		j->cb = NULL;
		cb(j->ctx, kind, &j->out);
	}
}

static void kill_cb(struct uloop_timeout *t)
{
	struct job *j = container_of(t, struct job, kill);

	j->killed = 1;
	if (j->proc.pid > 0)
		kill(-j->proc.pid, SIGKILL);
}

int tr143_start(int kind, const struct tr143_in *in, const char *who, tr143_cb cb, void *ctx, char *err, size_t en)
{
	struct job *j;
	int p[2];
	pid_t pid;

	if (kind < 0 || kind >= TK_N) {
		snprintf(err, en, "no such test");
		return -1;
	}
	j = &J[kind];
	if (j->running) {
		snprintf(err, en, "a %s test is running; ask again when it is done", tr143_kind_name(kind));
		return -1;
	}
	if (pipe2(p, O_CLOEXEC) < 0) {
		snprintf(err, en, "pipe: %s", strerror(errno));
		return -1;
	}
	j->in = *in;
	snprintf(j->in.dns_ip, sizeof(j->in.dns_ip), "%.47s", cfg.dns_listen);
	j->in.dns_port = (unsigned)cfg.dns_port;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		snprintf(err, en, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		static struct tr143_out o;
		size_t off = 0;

		close(p[0]);
		setpgid(0, 0);
		signal(SIGPIPE, SIG_IGN);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		run(kind, &j->in, &o);
		while (off < sizeof(o)) {
			ssize_t w = write(p[1], (char *)&o + off, sizeof(o) - off);

			if (w <= 0 && errno != EINTR)
				break;
			if (w > 0)
				off += (size_t)w;
		}
		_exit(0);
	}
	close(p[1]);
	j->fd = p[0];
	j->running = 1;
	j->reset = j->killed = j->canceled = 0;
	j->cb = cb;
	j->ctx = ctx;
	snprintf(j->who, sizeof(j->who), "%s", who && *who ? who : "local");
	j->proc.pid = pid;
	j->proc.cb = job_exit;
	uloop_process_add(&j->proc);
	j->kill.cb = kill_cb;
	uloop_timeout_set(&j->kill, limit_s(kind, &j->in) * 1000);
	AG_INFO("tr143: %s by %s: started (%s)", tr143_kind_name(kind), j->who,
		kind == TK_DOWNLOAD || kind == TK_UPLOAD ? j->in.url : j->in.host);
	return 0;
}

int tr143_running(int kind)
{
	return kind >= 0 && kind < TK_N && J[kind].running;
}

const struct tr143_out *tr143_last(int kind, int64_t *at)
{
	*at = J[kind].reset ? 0 : J[kind].at;
	return &J[kind].out;
}

void tr143_init(void)
{
	int k;

	for (k = 0; k < TK_N; k++) {
		tr143_defaults(k, &stored[k]);
		J[k].fd = -1;
	}
}

/* ================= inputs (command arguments, TR-069's writable parameters) ================= */

enum { F_STR, F_UINT, F_BOOL, F_PROTO, F_ULL };
#define KB(x) (1u << (x))
#define ALLK (KB(TK_PING) | KB(TK_TRACE) | KB(TK_DOWNLOAD) | KB(TK_UPLOAD) | KB(TK_NSLOOKUP))
#define IPK  (KB(TK_PING) | KB(TK_TRACE) | KB(TK_DOWNLOAD) | KB(TK_UPLOAD))
#define HTTPK (KB(TK_DOWNLOAD) | KB(TK_UPLOAD))
#define OFF(f) offsetof(struct tr143_in, f), sizeof(((struct tr143_in *)0)->f)

static const struct fld {
	unsigned kinds;
	const char *name;
	int type;
	unsigned min, max;
	size_t off, len;
} flds[] = {
	{ ALLK, "Interface", F_STR, 0, 0, OFF(iface) },
	{ IPK, "ProtocolVersion", F_PROTO, 0, 0, OFF(proto) },
	{ KB(TK_PING) | KB(TK_TRACE), "Host", F_STR, 0, 0, OFF(host) },
	{ KB(TK_NSLOOKUP), "HostName", F_STR, 0, 0, OFF(host) },
	{ KB(TK_NSLOOKUP), "DNSServer", F_STR, 0, 0, OFF(server) },
	{ KB(TK_DOWNLOAD), "DownloadURL", F_STR, 0, 0, OFF(url) },
	{ KB(TK_UPLOAD), "UploadURL", F_STR, 0, 0, OFF(url) },
	{ KB(TK_PING) | KB(TK_NSLOOKUP), "NumberOfRepetitions", F_UINT, 1, 100, OFF(reps) },
	{ KB(TK_TRACE), "NumberOfTries", F_UINT, 1, 3, OFF(reps) },
	{ KB(TK_PING) | KB(TK_TRACE) | KB(TK_NSLOOKUP), "Timeout", F_UINT, 1, 60000, OFF(timeout_ms) },
	{ KB(TK_PING) | KB(TK_TRACE), "DataBlockSize", F_UINT, 1, 65535, OFF(size) },
	{ IPK, "DSCP", F_UINT, 0, 63, OFF(dscp) },
	{ KB(TK_TRACE), "MaxHopCount", F_UINT, 1, 64, OFF(max_hops) },
	{ HTTPK, "EthernetPriority", F_UINT, 0, 7, OFF(eprio) },
	{ HTTPK, "NumberOfConnections", F_UINT, 1, 1, OFF(nconn) },
	{ HTTPK, "EnablePerConnectionResults", F_BOOL, 0, 0, OFF(perconn) },
	{ HTTPK, "TimeBasedTestDuration", F_UINT, 0, 999, OFF(duration_s) },
	{ KB(TK_UPLOAD), "TestFileLength", F_ULL, 0, 0, OFF(file_len) },
};
#define N_FLDS ((int)(sizeof(flds) / sizeof(flds[0])))

static const struct fld *fld_of(int kind, const char *name)
{
	int i;

	for (i = 0; i < N_FLDS; i++)
		if ((flds[i].kinds & KB(kind)) && !strcmp(flds[i].name, name))
			return &flds[i];
	return NULL;
}

/* one input into in; 0 taken, -1 not a valid value, 1 not an input of this test */
static int put(int kind, const char *name, const char *v, struct tr143_in *in, char *err, size_t en)
{
	const struct fld *f = fld_of(kind, name);
	char *p;
	unsigned u;

	if (!f)
		return 1;
	p = (char *)in + f->off;
	switch (f->type) {
	case F_STR:
		if (strlen(v) >= f->len) {
			snprintf(err, en, "%s: at most %zu characters", name, f->len - 1);
			return -1;
		}
		snprintf(p, f->len, "%s", v);
		return 0;
	case F_PROTO:
		if (strcasecmp(v, "Any") && strcasecmp(v, "IPv4") && strcasecmp(v, "IPv6")) {
			snprintf(err, en, "%s: Any, IPv4 or IPv6", name);
			return -1;
		}
		snprintf(p, f->len, "%s", !strcasecmp(v, "Any") ? "Any" : !strcasecmp(v, "IPv4") ? "IPv4" : "IPv6");
		return 0;
	case F_BOOL:
		if (strcmp(v, "true") && strcmp(v, "false") && strcmp(v, "1") && strcmp(v, "0")) {
			snprintf(err, en, "%s: true or false", name);
			return -1;
		}
		*(int *)p = !strcmp(v, "true") || !strcmp(v, "1");
		return 0;
	case F_ULL: {
		char *end;
		unsigned long long x;

		if (!*v || *v == '-' || (x = strtoull(v, &end, 10), *end) || x > 4294967295ULL) {
			snprintf(err, en, "%s: a number of bytes", name);
			return -1;
		}
		*(unsigned long long *)p = x;
		return 0;
	}
	default:
		if (tr143_uint(v, f->min, f->max, &u) < 0) {
			snprintf(err, en, "%s: a number from %u to %u", name, f->min, f->max);
			return -1;
		}
		*(unsigned *)p = u;
		return 0;
	}
}

static void show_input(const struct fld *f, const struct tr143_in *in, char *buf, size_t n)
{
	const char *p = (const char *)in + f->off;

	switch (f->type) {
	case F_STR:
	case F_PROTO: snprintf(buf, n, "%s", p); break;
	case F_BOOL: snprintf(buf, n, "%s", *(const int *)p ? "true" : "false"); break;
	case F_ULL: snprintf(buf, n, "%llu", *(const unsigned long long *)p); break;
	default: snprintf(buf, n, "%u", *(const unsigned *)p);
	}
}

int tr143_args(int kind, struct blob_attr *args, struct tr143_in *in, char *err, size_t en)
{
	struct blob_attr *cur;
	size_t rem;

	tr143_defaults(kind, in);
	if (args)
		blobmsg_for_each_attr(cur, args, rem)
			if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING &&
			    put(kind, blobmsg_name(cur), blobmsg_get_string(cur), in, err, en) < 0)
				return -1;
	if ((kind == TK_PING || kind == TK_TRACE || kind == TK_NSLOOKUP) && !in->host[0]) {
		snprintf(err, en, "%s is required", kind == TK_NSLOOKUP ? "HostName" : "Host");
		return -1;
	}
	if ((kind == TK_DOWNLOAD || kind == TK_UPLOAD) && !in->url[0]) {
		snprintf(err, en, "%s is required", kind == TK_DOWNLOAD ? "DownloadURL" : "UploadURL");
		return -1;
	}
	return 0;
}

/* a test that couldn't start (bad arguments): its result says so (TR-069 reads DiagnosticsState) */
void tr143_failed(int kind, int status, const char *msg)
{
	struct job *j = &J[kind];

	if (j->running)
		return;
	memset(&j->out, 0, sizeof(j->out));
	why(&j->out, status, "%s", msg);
	j->at = wall_us();
	j->requested = j->reset = 0;
}

int tr143_set(int kind, const char *name, const char *v, int apply, char *err, size_t en)
{
	struct tr143_in tmp = stored[kind];
	int rc;

	if (!strcmp(name, "DiagnosticsState")) {
		if (strcmp(v, "Requested") && strcmp(v, "Canceled") && strcmp(v, "None")) {
			snprintf(err, en, "DiagnosticsState: Requested or Canceled");
			return -1;
		}
		if (!apply)
			return 0;
		if (!strcmp(v, "Requested")) {
			J[kind].requested = 1;      /* the TR-069 client runs it when its session ends */
		} else {
			J[kind].requested = 0;
			if (J[kind].running) {
				J[kind].canceled = 1;
				kill(-J[kind].proc.pid, SIGKILL);
			}
			J[kind].reset = 1;
		}
		return 0;
	}
	rc = put(kind, name, v, &tmp, err, en);
	if (rc)
		return -1;
	if (apply) {
		stored[kind] = tmp;
		/* TR-181: changing an input makes the results stale */
		J[kind].reset = 1;
		J[kind].requested = 0;
	}
	return 0;
}

/* ================= results (parameters, command outputs) ================= */

static void tm(int64_t us, char *buf, size_t n)
{
	tr143_time(us, buf, n);
}

int tr143_value(int kind, const char *name, uint32_t inst, char *buf, size_t n)
{
	const struct fld *f = fld_of(kind, name);
	const struct job *j = &J[kind];
	const struct tr143_out *o = &j->out;
	int have = j->at && !j->reset;

	buf[0] = '\0';
	if (!inst && f) {
		show_input(f, &stored[kind], buf, n);
		return 0;
	}
	if (!inst && !strcmp(name, "DiagnosticsState")) {
		snprintf(buf, n, "%s", j->running || j->requested ? "Requested" : have ? tr143_status_name(o->status) : "None");
		return 0;
	}
	if (!have)
		o = NULL;
	if (inst && kind == TK_TRACE) {
		const struct tr143_hop *h;

		if (!o || inst > (uint32_t)o->nhops)
			return -1;
		h = &o->hop[inst - 1];
		if (!strcmp(name, "Host") || !strcmp(name, "HostAddress"))
			snprintf(buf, n, "%s", h->addr);
		else if (!strcmp(name, "ErrorCode"))
			snprintf(buf, n, "%d", h->code);
		else if (!strcmp(name, "RTTimes"))
			snprintf(buf, n, "%s", h->rtts);
		else
			return -1;
		return 0;
	}
	if (inst && kind == TK_NSLOOKUP) {
		const struct tr143_ns *r;

		if (!o || inst > (uint32_t)o->nns)
			return -1;
		r = &o->ns[inst - 1];
		if (!strcmp(name, "Status"))
			snprintf(buf, n, "%s", tr143_ns_status_name(r->status));
		else if (!strcmp(name, "AnswerType"))
			snprintf(buf, n, "%s", r->authoritative < 0 ? "None" : r->authoritative ? "Authoritative" : "NonAuthoritative");
		else if (!strcmp(name, "HostNameReturned"))
			snprintf(buf, n, "%s", r->name);
		else if (!strcmp(name, "IPAddresses"))
			snprintf(buf, n, "%s", r->ips);
		else if (!strcmp(name, "DNSServerIP"))
			snprintf(buf, n, "%s", r->server);
		else if (!strcmp(name, "ResponseTime"))
			snprintf(buf, n, "%u", r->ms);
		else
			return -1;
		return 0;
	}
	if (inst)
		return -1;
#define U(nm, v) if (!strcmp(name, nm)) { snprintf(buf, n, "%llu", o ? (unsigned long long)(v) : 0ULL); return 0; }
#define T(nm, v) if (!strcmp(name, nm)) { tm(o ? (v) : 0, buf, n); return 0; }
	if (!strcmp(name, "Status")) {
		snprintf(buf, n, "%s", o ? tr143_status_name(o->status) : "None");
		return 0;
	}
	if (!strcmp(name, "IPAddressUsed")) {
		snprintf(buf, n, "%s", o ? o->ip_used : "");
		return 0;
	}
	U("SuccessCount", kind == TK_NSLOOKUP ? o->ns_ok : o->ok)
	U("FailureCount", o->fail)
	U("AverageResponseTime", o->avg_ms)
	U("MinimumResponseTime", o->min_ms)
	U("MaximumResponseTime", o->max_ms)
	U("AverageResponseTimeDetailed", o->avg_us)
	U("MinimumResponseTimeDetailed", o->min_us)
	U("MaximumResponseTimeDetailed", o->max_us)
	U("ResponseTime", o->resp_ms)
	U("RouteHopsNumberOfEntries", o->nhops)
	U("ResultNumberOfEntries", o->nns)
	T("ROMTime", o->rom_us)
	T("BOMTime", o->bom_us)
	T("EOMTime", o->eom_us)
	T("TCPOpenRequestTime", o->tcp_req_us)
	T("TCPOpenResponseTime", o->tcp_resp_us)
	U("TestBytesReceived", o->test_rx)
	U("TestBytesSent", o->test_tx)
	U("TotalBytesReceived", o->total_rx)
	U("TotalBytesSent", o->total_tx)
	/* one connection, no incremental results: the whole test is the full-loading period */
	U("TestBytesReceivedUnderFullLoading", o->test_rx)
	U("TestBytesSentUnderFullLoading", o->test_tx)
	U("TotalBytesReceivedUnderFullLoading", o->total_rx)
	U("TotalBytesSentUnderFullLoading", o->total_tx)
	U("PeriodOfFullLoading", o->eom_us > o->bom_us ? o->eom_us - o->bom_us : 0)
#undef U
#undef T
	return -1;
}

int tr143_rows(int kind)
{
	int64_t at;
	const struct tr143_out *o = tr143_last(kind, &at);

	if (!at)
		return 0;
	return kind == TK_TRACE ? o->nhops : kind == TK_NSLOOKUP ? o->nns : 0;
}

/* the command's outputs (USP), as TR-181 names them */
void tr143_out_blob(struct blob_buf *b, int kind, const struct tr143_out *o)
{
	static const char *const ping[] = { "IPAddressUsed", "SuccessCount", "FailureCount", "AverageResponseTime",
		"MinimumResponseTime", "MaximumResponseTime", "AverageResponseTimeDetailed", "MinimumResponseTimeDetailed",
		"MaximumResponseTimeDetailed", NULL };
	static const char *const trace[] = { "IPAddressUsed", "ResponseTime", NULL };
	static const char *const down[] = { "IPAddressUsed", "ROMTime", "BOMTime", "EOMTime", "TestBytesReceived",
		"TotalBytesReceived", "TotalBytesSent", "TestBytesReceivedUnderFullLoading",
		"TotalBytesReceivedUnderFullLoading", "TotalBytesSentUnderFullLoading", "PeriodOfFullLoading",
		"TCPOpenRequestTime", "TCPOpenResponseTime", NULL };
	static const char *const up[] = { "IPAddressUsed", "ROMTime", "BOMTime", "EOMTime", "TestBytesSent",
		"TotalBytesReceived", "TotalBytesSent", "TestBytesSentUnderFullLoading", "TotalBytesReceivedUnderFullLoading",
		"TotalBytesSentUnderFullLoading", "PeriodOfFullLoading", "TCPOpenRequestTime", "TCPOpenResponseTime", NULL };
	static const char *const ns[] = { "SuccessCount", NULL };
	const char *const *names = kind == TK_PING ? ping : kind == TK_TRACE ? trace : kind == TK_DOWNLOAD ? down :
				   kind == TK_UPLOAD ? up : ns;
	char v[256], key[64];
	int i;

	(void)o;                        /* the values are the kind's last result, which o is */
	blobmsg_add_string(b, "Status", tr143_status_name(J[kind].out.status));
	for (i = 0; names[i]; i++) {
		tr143_value(kind, names[i], 0, v, sizeof(v));
		blobmsg_add_string(b, names[i], v);
	}
	if (kind == TK_TRACE) {
		static const char *const hf[] = { "Host", "HostAddress", "ErrorCode", "RTTimes" };

		for (i = 0; i < J[kind].out.nhops; i++) {
			unsigned k;

			for (k = 0; k < 4; k++) {
				snprintf(key, sizeof(key), "RouteHops.%d.%s", i + 1, hf[k]);
				tr143_value(kind, hf[k], (uint32_t)i + 1, v, sizeof(v));
				blobmsg_add_string(b, key, v);
			}
		}
	}
	if (kind == TK_NSLOOKUP) {
		static const char *const rf[] = { "Status", "AnswerType", "HostNameReturned", "IPAddresses", "DNSServerIP",
						  "ResponseTime" };

		for (i = 0; i < J[kind].out.nns; i++) {
			unsigned k;

			for (k = 0; k < 6; k++) {
				snprintf(key, sizeof(key), "Result.%d.%s", i + 1, rf[k]);
				tr143_value(kind, rf[k], (uint32_t)i + 1, v, sizeof(v));
				blobmsg_add_string(b, key, v);
			}
		}
	}
}
