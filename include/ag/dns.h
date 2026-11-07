/* SPDX-License-Identifier: Apache-2.0 */
/* Strict, bounds-checked DNS message parser (untrusted input). */
#ifndef AG_DNS_H
#define AG_DNS_H

#include <stdint.h>
#include <stddef.h>

#define AG_DNS_NAME_MAX 256
#define AG_DNS_MAX_ANS  8
#define AG_DNS_HDR_LEN  12

#define AG_DNS_IS_RESPONSE(flags) (((flags) & 0x8000) != 0)
#define AG_DNS_RCODE(flags)       ((flags) & 0x000f)

struct ag_dns_addr {
	int family;          /* AF_INET or AF_INET6 */
	uint8_t addr[16];
	uint32_t ttl;
};

struct ag_dns_msg {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount, ancount;
	uint16_t qtype, qclass;
	uint16_t qend;                      /* offset just past the first question */
	char qname[AG_DNS_NAME_MAX];        /* lower-case, dotted; "." for root */
	int n_ans;                          /* A / AAAA records found */
	struct ag_dns_addr ans[AG_DNS_MAX_ANS];
};

/* Returns 0 when the header and first question parse cleanly, -1 otherwise.
 * Answer records are collected best-effort and never cause a failure. */
int ag_dns_parse(const uint8_t *buf, size_t len, struct ag_dns_msg *m);

/* Turns the query in buf (already parsed into q) into an NXDOMAIN answer to
 * it, in place: header flags set, the first question kept, everything after
 * it dropped. Returns the answer's length, or -1. */
int ag_dns_make_nxdomain(uint8_t *buf, size_t len, const struct ag_dns_msg *q);

/* 1 when qname is name or one of its subdomains ("ads.example.com" matches
 * "example.com"); both lower-case, without a trailing dot. */
int ag_dns_name_under(const char *qname, const char *name);

/* "A", "AAAA", "HTTPS", ... or the number. */
const char *ag_dns_type_str(uint16_t type, char *tmp, size_t tmplen);

#endif
