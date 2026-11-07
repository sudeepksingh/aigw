/* SPDX-License-Identifier: Apache-2.0 */
/* DNS parsing for AIGW. Every read is bounds-checked; compression pointers
 * are followed with a hop limit so malformed or hostile packets can't loop. */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "ag/dns.h"

#define MAX_PTR_HOPS 32

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Reads a (possibly compressed) name starting at *off. Writes the dotted,
 * lower-cased form to out (if non-NULL) and advances *off past the name as
 * it appears at the original position. */
static int read_name(const uint8_t *buf, size_t len, size_t *off, char *out, size_t outlen)
{
	size_t pos = *off, end = 0, o = 0;
	int jumped = 0, hops = 0;

	for (;;) {
		uint8_t l;

		if (pos >= len)
			return -1;
		l = buf[pos];
		if ((l & 0xc0) == 0xc0) {
			size_t ptr;

			if (pos + 1 >= len)
				return -1;
			ptr = ((size_t)(l & 0x3f) << 8) | buf[pos + 1];
			if (!jumped)
				end = pos + 2;
			if (++hops > MAX_PTR_HOPS || ptr >= len)
				return -1;
			pos = ptr;
			jumped = 1;
			continue;
		}
		if (l & 0xc0)
			return -1; /* reserved label types */
		pos++;
		if (l == 0)
			break;
		if (pos + l > len)
			return -1;
		if (out) {
			size_t i;

			if (o + l + 2 > outlen)
				return -1;
			if (o)
				out[o++] = '.';
			for (i = 0; i < l; i++) {
				unsigned char ch = buf[pos + i];

				if (ch >= 'A' && ch <= 'Z')
					ch = (unsigned char)(ch + 32);
				else if (ch < 0x21 || ch > 0x7e)
					ch = '?';
				out[o++] = (char)ch;
			}
		}
		pos += l;
	}
	if (out) {
		if (o == 0) {
			if (outlen < 2)
				return -1;
			out[o++] = '.';
		}
		out[o] = '\0';
	}
	*off = jumped ? end : pos;
	return 0;
}

int ag_dns_parse(const uint8_t *buf, size_t len, struct ag_dns_msg *m)
{
	size_t off = AG_DNS_HDR_LEN;
	unsigned i;

	memset(m, 0, sizeof(*m));
	if (!buf || len < AG_DNS_HDR_LEN)
		return -1;

	m->id = rd16(buf);
	m->flags = rd16(buf + 2);
	m->qdcount = rd16(buf + 4);
	m->ancount = rd16(buf + 6);

	if (m->qdcount == 0)
		return 0;

	/* first question: the one we report */
	if (read_name(buf, len, &off, m->qname, sizeof(m->qname)) < 0)
		return -1;
	if (off + 4 > len)
		return -1;
	m->qtype = rd16(buf + off);
	m->qclass = rd16(buf + off + 2);
	off += 4;
	m->qend = (uint16_t)off;

	/* skip any further questions */
	for (i = 1; i < m->qdcount; i++) {
		if (read_name(buf, len, &off, NULL, 0) < 0 || off + 4 > len)
			return 0;
		off += 4;
	}

	/* answers: collect A/AAAA, stop quietly on anything malformed */
	for (i = 0; i < m->ancount && m->n_ans < AG_DNS_MAX_ANS; i++) {
		uint16_t type, rdlen;
		uint32_t ttl;

		if (read_name(buf, len, &off, NULL, 0) < 0 || off + 10 > len)
			break;
		type = rd16(buf + off);
		ttl = rd32(buf + off + 4);
		rdlen = rd16(buf + off + 8);
		off += 10;
		if (off + rdlen > len)
			break;
		if (type == 1 && rdlen == 4) {
			m->ans[m->n_ans].family = AF_INET;
			memcpy(m->ans[m->n_ans].addr, buf + off, 4);
			m->ans[m->n_ans].ttl = ttl;
			m->n_ans++;
		} else if (type == 28 && rdlen == 16) {
			m->ans[m->n_ans].family = AF_INET6;
			memcpy(m->ans[m->n_ans].addr, buf + off, 16);
			m->ans[m->n_ans].ttl = ttl;
			m->n_ans++;
		}
		off += rdlen;
	}
	return 0;
}

int ag_dns_make_nxdomain(uint8_t *buf, size_t len, const struct ag_dns_msg *q)
{
	if (!buf || !q || q->qdcount == 0 || q->qend < AG_DNS_HDR_LEN || q->qend > len)
		return -1;
	/* QR=1, keep opcode and RD, RA=1, RCODE=3 (no such name) */
	buf[2] = (uint8_t)(0x80 | (buf[2] & 0x79));
	buf[3] = 0x80 | 3;
	buf[4] = 0; buf[5] = 1;                         /* one question */
	memset(buf + 6, 0, 6);                          /* no answers, authority, additional */
	return q->qend;
}

int ag_dns_name_under(const char *qname, const char *name)
{
	size_t lq, ln;

	if (!qname || !name || !*name)
		return 0;
	lq = strlen(qname);
	ln = strlen(name);
	if (lq && qname[lq - 1] == '.')
		lq--;
	if (lq < ln || strncmp(qname + lq - ln, name, ln))
		return 0;
	return lq == ln || qname[lq - ln - 1] == '.';
}

const char *ag_dns_type_str(uint16_t type, char *tmp, size_t tmplen)
{
	switch (type) {
	case 1: return "A";
	case 2: return "NS";
	case 5: return "CNAME";
	case 6: return "SOA";
	case 12: return "PTR";
	case 15: return "MX";
	case 16: return "TXT";
	case 28: return "AAAA";
	case 33: return "SRV";
	case 64: return "SVCB";
	case 65: return "HTTPS";
	case 255: return "ANY";
	default:
		snprintf(tmp, tmplen, "TYPE%u", type);
		return tmp;
	}
}
