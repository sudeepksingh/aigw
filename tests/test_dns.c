/* SPDX-License-Identifier: Apache-2.0 */
/* Unit tests for the DNS parser, including hostile inputs. */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "ag/dns.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* query: id 0x1234, RD, one question "www.Example.com" A IN */
static const unsigned char query[] = {
	0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	3, 'w', 'w', 'w', 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
	0x00, 0x01, 0x00, 0x01,
};

/* response: same question, answers: A 93.184.216.34 (ttl 300) and AAAA, both
 * using a compression pointer to offset 12 */
static const unsigned char response[] = {
	0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
	0x00, 0x01, 0x00, 0x01,
	0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x04, 93, 184, 216, 34,
	0xc0, 0x0c, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x10,
	0x26, 0x06, 0x28, 0x00, 0x02, 0x20, 0x00, 0x01, 0x02, 0x48, 0x18, 0x93, 0x25, 0xc8, 0x19, 0x46,
};

/* name that points at itself: must be rejected, not loop */
static const unsigned char ptr_loop[] = {
	0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
};

/* label length runs past the end of the packet */
static const unsigned char truncated[] = {
	0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	10, 'a', 'b', 'c',
};

/* answer count claims 5 but the records are cut short: question must still parse */
static const unsigned char short_answers[] = {
	0x00, 0x02, 0x81, 0x80, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
	1, 'a', 0, 0x00, 0x01, 0x00, 0x01,
	0xc0, 0x0c, 0x00, 0x01,
};

static void test_query(void)
{
	struct ag_dns_msg m;

	CHECK(ag_dns_parse(query, sizeof(query), &m) == 0);
	CHECK(m.id == 0x1234);
	CHECK(!AG_DNS_IS_RESPONSE(m.flags));
	CHECK(strcmp(m.qname, "www.example.com") == 0);
	CHECK(m.qtype == 1);
	CHECK(m.n_ans == 0);
}

static void test_response(void)
{
	struct ag_dns_msg m;

	CHECK(ag_dns_parse(response, sizeof(response), &m) == 0);
	CHECK(AG_DNS_IS_RESPONSE(m.flags));
	CHECK(AG_DNS_RCODE(m.flags) == 0);
	CHECK(m.n_ans == 2);
	CHECK(m.ans[0].family == AF_INET);
	CHECK(m.ans[0].addr[0] == 93 && m.ans[0].addr[3] == 34);
	CHECK(m.ans[0].ttl == 300);
	CHECK(m.ans[1].family == AF_INET6);
	CHECK(m.ans[1].addr[0] == 0x26 && m.ans[1].addr[15] == 0x46);
}

static void test_hostile(void)
{
	struct ag_dns_msg m;
	unsigned char big[600];
	size_t i, n;

	CHECK(ag_dns_parse(ptr_loop, sizeof(ptr_loop), &m) == -1);
	CHECK(ag_dns_parse(truncated, sizeof(truncated), &m) == -1);
	CHECK(ag_dns_parse(query, 5, &m) == -1);
	CHECK(ag_dns_parse(NULL, 0, &m) == -1);
	CHECK(ag_dns_parse(short_answers, sizeof(short_answers), &m) == 0);
	CHECK(strcmp(m.qname, "a") == 0 && m.n_ans == 0);

	/* name longer than the output buffer must be rejected, not overflow */
	memcpy(big, query, 12);
	n = 12;
	for (i = 0; i < 9; i++) {
		big[n++] = 60;
		memset(big + n, 'x', 60);
		n += 60;
	}
	big[n++] = 0;
	big[n++] = 0; big[n++] = 1; big[n++] = 0; big[n++] = 1;
	CHECK(ag_dns_parse(big, n, &m) == -1);

	/* every truncation of a valid response parses or fails cleanly */
	for (i = 0; i <= sizeof(response); i++)
		(void)ag_dns_parse(response, i, &m);
}

static void test_type_names(void)
{
	char tmp[16];

	CHECK(strcmp(ag_dns_type_str(1, tmp, sizeof(tmp)), "A") == 0);
	CHECK(strcmp(ag_dns_type_str(65, tmp, sizeof(tmp)), "HTTPS") == 0);
	CHECK(strcmp(ag_dns_type_str(4242, tmp, sizeof(tmp)), "TYPE4242") == 0);
}

/* the block actuator: NXDOMAIN answers and subdomain matching */
static void test_block(void)
{
	unsigned char q[sizeof(query) + 11];
	struct ag_dns_msg m, r;
	int n;

	/* the query plus an EDNS OPT record that must be dropped from the answer */
	memcpy(q, query, sizeof(query));
	q[11] = 1;
	memcpy(q + sizeof(query), "\x00\x00\x29\x10\x00\x00\x00\x00\x00\x00\x00", 11);
	CHECK(ag_dns_parse(q, sizeof(q), &m) == 0);
	CHECK(m.qend == sizeof(query));
	n = ag_dns_make_nxdomain(q, sizeof(q), &m);
	CHECK(n == (int)sizeof(query));
	CHECK(ag_dns_parse(q, (size_t)n, &r) == 0);
	CHECK(AG_DNS_IS_RESPONSE(r.flags) && AG_DNS_RCODE(r.flags) == 3);
	CHECK(r.id == 0x1234 && (r.flags & 0x0100));       /* same id, RD kept */
	CHECK(r.qdcount == 1 && r.ancount == 0 && strcmp(r.qname, "www.example.com") == 0);
	CHECK(ag_dns_make_nxdomain(q, 5, &m) == -1);

	CHECK(ag_dns_name_under("example.com", "example.com"));
	CHECK(ag_dns_name_under("ads.example.com", "example.com"));
	CHECK(ag_dns_name_under("a.b.example.com.", "example.com"));
	CHECK(!ag_dns_name_under("badexample.com", "example.com"));
	CHECK(!ag_dns_name_under("example.com.evil", "example.com"));
	CHECK(!ag_dns_name_under("com", "example.com"));
	CHECK(!ag_dns_name_under("example.com", ""));
}

int main(void)
{
	test_query();
	test_response();
	test_hostile();
	test_type_names();
	test_block();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("dns parser: all checks passed\n");
	return 0;
}
