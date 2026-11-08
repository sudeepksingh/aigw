/* SPDX-License-Identifier: Apache-2.0 */
/*
 * agctl selftest: checks that run on the router itself, because its CPU,
 * C library and float unit are what matter (MIPS, ARM, x86; big or little
 * endian): signature verification, the model runtime, the QR encoder, and a
 * real DNS query through agdns. Prints PASS / FAIL / INFO lines; the exit
 * code is the number of failures. aigw-selftest runs it with its other checks.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ag/ag.h"
#include "ag/agm.h"
#include "ag/dns.h"
#include "ag/domfeat.h"
#include "ag/qr.h"
#include "ag/sign.h"

static int fails;

static void result(int ok, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void result(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("%s %s: ", ok ? "PASS" : "FAIL", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fails += !ok;
}

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;

	for (; h[0] && h[1]; h += 2)
		sscanf(h, "%2hhx", &out[n++]);
	return n;
}

static void check_crypto(void)
{
	static const char *const abc =
		"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
		"2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
	uint8_t h[64], want[64], pk[32], sig[64], msg[1] = { 0x72 };
	int ok;

	ag_sha512((const uint8_t *)"abc", 3, h);
	unhex(abc, want);
	result(!memcmp(h, want, 64), "sha512", "FIPS 180-4 vector");
	unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", pk);
	unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
	      "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", sig);
	ok = ag_ed25519_verify(sig, msg, 1, pk) == 0;
	sig[5] ^= 1;
	ok = ok && ag_ed25519_verify(sig, msg, 1, pk) < 0;
	result(ok, "ed25519", "RFC 8032 vector accepted, changed signature refused");
}

static double score(struct ag_model *m, const char *name)
{
	float x[AG_DOMFEAT_N];
	char label[256];
	const float *bg;
	int dim;

	bg = ag_model_bigram(m, &dim);
	if (ag_domain_label(name, label, sizeof(label)) < 0)
		return -1;
	ag_domain_features(name, label, bg, dim, x);
	return ag_model_prob(m, x);
}

static void check_model(const struct ag_config *c)
{
	char path[512], err[128];
	struct ag_model *m;
	double good, bad;

	snprintf(path, sizeof(path), "%s/domain-risk.agm", c->model_dir);
	m = ag_model_load(path, err, sizeof(err));
	if (!m) {
		result(0, "model", "%s: %s", path, err);
		return;
	}
	good = score(m, "wikipedia.org");
	bad = score(m, "xkqjzvbwptmr.com");
	result(good >= 0 && good < 0.5 && bad >= c->dga_threshold, "model",
	       "packaged domain-risk on this CPU: wikipedia.org %.3f, xkqjzvbwptmr.com %.3f (%d trees)", good, bad,
	       ag_model_ntrees(m));
	ag_model_free(m);
}

static void check_qr(void)
{
	struct ag_qr q;
	int ok = ag_qr_encode(&q, (const uint8_t *)"WIFI:T:WPA;S:AIGW;P:selftest;;", 30, AG_QR_ECC_M) == 0;

	ok = ok && q.size == 17 + 4 * q.version && q.m[0][0] && q.m[6][6] && !q.m[7][7];
	result(ok, "qr", "Wi-Fi QR code, version %d", ok ? q.version : 0);
}

/* a real A query to agdns on 127.0.0.1:<dns.port> */
static void check_dns(const struct ag_config *c)
{
	static const char *const name = "example.com";
	uint8_t q[512], r[1500];
	struct sockaddr_in sa = { .sin_family = AF_INET };
	struct pollfd p;
	struct ag_dns_msg m;
	size_t n = 12;
	const char *s = name;
	int fd, got;
	uint16_t id = (uint16_t)(time(NULL) ^ getpid());

	memset(q, 0, sizeof(q));
	q[0] = (uint8_t)(id >> 8);
	q[1] = (uint8_t)id;
	q[2] = 0x01;                        /* recursion desired */
	q[5] = 1;                           /* one question */
	while (*s) {
		const char *dot = strchr(s, '.');
		size_t l = dot ? (size_t)(dot - s) : strlen(s);

		q[n++] = (uint8_t)l;
		memcpy(q + n, s, l);
		n += l;
		s += l + (dot ? 1 : 0);
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 1;             /* A */
	q[n++] = 0; q[n++] = 1;             /* IN */
	sa.sin_port = htons((uint16_t)c->dns_port);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || sendto(fd, q, n, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		result(0, "dns", "cannot send to 127.0.0.1#%d", c->dns_port);
		if (fd >= 0)
			close(fd);
		return;
	}
	p.fd = fd;
	p.events = POLLIN;
	got = poll(&p, 1, 4000) == 1 ? (int)recv(fd, r, sizeof(r), 0) : -1;
	close(fd);
	if (got <= 0) {
		result(0, "dns", "agdns on 127.0.0.1#%d did not answer", c->dns_port);
		return;
	}
	if (ag_dns_parse(r, (size_t)got, &m) < 0 || m.id != id) {
		result(0, "dns", "agdns answered something that is not a DNS reply");
		return;
	}
	if ((m.flags & 0xf) == 2)               /* SERVFAIL: agdns works, the upstream doesn't */
		printf("INFO dns: agdns answers on 127.0.0.1#%d, but its upstream failed (no internet?)\n", c->dns_port);
	else
		result((m.flags & 0xf) == 0 && m.n_ans > 0, "dns", "%s through agdns: rcode %d, %d address%s", name,
		       m.flags & 0xf, m.n_ans, m.n_ans == 1 ? "" : "es");
}

int agctl_selftest(void)
{
	struct ag_config c;

	ag_config_load(&c);
	check_crypto();
	check_model(&c);
	check_qr();
	check_dns(&c);
	return fails;
}
