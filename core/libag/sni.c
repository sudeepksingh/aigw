/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Server names from TLS ClientHellos and QUIC Initial packets (see sni.h).
 *
 * SHA-256 and HKDF (RFC 5869) derive the QUIC Initial keys; AES-128 removes
 * the header protection and AES-128-GCM opens the payload (RFC 9001). The
 * AES S-box is computed at first use instead of typed in. Small and simple
 * rather than fast: it runs on the first packets of a connection only.
 */
#include <string.h>

#include "ag/sni.h"

/* ---------- SHA-256 ---------- */

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct sha256 {
	uint32_t h[8];
	uint8_t blk[64];
	size_t n;
	uint64_t total;
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	for (; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
	for (i = 0; i < 64; i++) {
		t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_init(struct sha256 *s)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, h0, sizeof(h0));
	s->n = 0;
	s->total = 0;
}

static void sha256_update(struct sha256 *s, const uint8_t *p, size_t len)
{
	s->total += len;
	while (len) {
		size_t take = 64 - s->n < len ? 64 - s->n : len;

		memcpy(s->blk + s->n, p, take);
		s->n += take;
		p += take;
		len -= take;
		if (s->n == 64) {
			sha256_block(s->h, s->blk);
			s->n = 0;
		}
	}
}

static void sha256_final(struct sha256 *s, uint8_t out[32])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80, zero = 0, len[8];
	int i;

	sha256_update(s, &pad, 1);
	while (s->n != 56)
		sha256_update(s, &zero, 1);
	for (i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(s, len, 8);
	for (i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

void ag_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
	struct sha256 s;

	sha256_init(&s);
	sha256_update(&s, msg, len);
	sha256_final(&s, out);
}

void ag_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t len, uint8_t out[32])
{
	uint8_t k[64] = { 0 }, pad[64], inner[32];
	struct sha256 s;
	int i;

	if (klen > 64)
		ag_sha256(key, klen, k);
	else
		memcpy(k, key, klen);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&s);
	sha256_update(&s, pad, 64);
	sha256_update(&s, msg, len);
	sha256_final(&s, inner);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha256_init(&s);
	sha256_update(&s, pad, 64);
	sha256_update(&s, inner, 32);
	sha256_final(&s, out);
}

void ag_hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32])
{
	ag_hmac_sha256(salt, slen, ikm, ilen, prk);
}

void ag_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t len)
{
	uint8_t t[32 + 255 + 1], blk[32];
	size_t tl = 0, done = 0;
	uint8_t ctr = 1;

	if (ilen > 255)
		return;
	while (done < len && ctr) {
		memcpy(t + tl, info, ilen);
		t[tl + ilen] = ctr++;
		ag_hmac_sha256(prk, 32, t, tl + ilen + 1, blk);
		memcpy(out + done, blk, len - done < 32 ? len - done : 32);
		done += 32;
		memcpy(t, blk, 32);
		tl = 32;
	}
}

/* TLS 1.3 HKDF-Expand-Label with an empty context */
static void expand_label(const uint8_t secret[32], const char *label, uint8_t *out, size_t len)
{
	uint8_t info[2 + 1 + 6 + 32 + 1];
	size_t ll = strlen(label), n = 0;

	info[n++] = (uint8_t)(len >> 8);
	info[n++] = (uint8_t)len;
	info[n++] = (uint8_t)(6 + ll);
	memcpy(info + n, "tls13 ", 6);
	n += 6;
	memcpy(info + n, label, ll);
	n += ll;
	info[n++] = 0;
	ag_hkdf_expand(secret, info, n, out, len);
}

/* ---------- AES-128 ---------- */

static uint8_t sbox[256];
static int sbox_ready;

static uint8_t xtime(uint8_t a)
{
	return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b) {
		if (b & 1)
			p ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return p;
}

/* S(x) = affine(x^-1) over GF(2^8) */
static void sbox_init(void)
{
	int i, j;

	for (i = 0; i < 256; i++) {
		uint8_t inv = 0, s;

		for (j = 1; i && j < 256; j++)
			if (gmul((uint8_t)i, (uint8_t)j) == 1) {
				inv = (uint8_t)j;
				break;
			}
		s = inv;
		for (j = 1; j <= 4; j++)
			s ^= (uint8_t)((inv << j) | (inv >> (8 - j)));
		sbox[i] = s ^ 0x63;
	}
	sbox_ready = 1;
}

void ag_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t rk[176], s[16], t[16], rcon = 1;
	int i, r, c;

	if (!sbox_ready)
		sbox_init();
	memcpy(rk, key, 16);
	for (i = 16; i < 176; i += 4) {
		uint8_t w[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };

		if (i % 16 == 0) {
			uint8_t w0 = w[0];

			w[0] = sbox[w[1]] ^ rcon;
			w[1] = sbox[w[2]];
			w[2] = sbox[w[3]];
			w[3] = sbox[w0];
			rcon = xtime(rcon);
		}
		for (c = 0; c < 4; c++)
			rk[i + c] = rk[i - 16 + c] ^ w[c];
	}
	for (i = 0; i < 16; i++)
		s[i] = in[i] ^ rk[i];
	for (r = 1; r <= 10; r++) {
		for (i = 0; i < 16; i++)                    /* SubBytes + ShiftRows */
			t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
		if (r < 10) {
			for (c = 0; c < 4; c++) {               /* MixColumns */
				uint8_t *a = t + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];

				a[0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
				a[1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
				a[2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
				a[3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
			}
		}
		for (i = 0; i < 16; i++)
			s[i] = t[i] ^ rk[16 * r + i];
	}
	memcpy(out, s, 16);
}

/* ---------- AES-128-GCM (open only) ---------- */

static void gf_mul(uint8_t x[16], const uint8_t h[16])
{
	uint8_t z[16] = { 0 }, v[16];
	int i, j;

	memcpy(v, h, 16);
	for (i = 0; i < 128; i++) {
		int lsb;

		if (x[i / 8] & (0x80 >> (i % 8)))
			for (j = 0; j < 16; j++)
				z[j] ^= v[j];
		lsb = v[15] & 1;
		for (j = 15; j > 0; j--)
			v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
		v[0] >>= 1;
		if (lsb)
			v[0] ^= 0xe1;
	}
	memcpy(x, z, 16);
}

static void ghash(uint8_t x[16], const uint8_t h[16], const uint8_t *p, size_t len)
{
	size_t i, j;

	for (i = 0; i < len; i += 16) {
		for (j = 0; j < 16 && i + j < len; j++)
			x[j] ^= p[i + j];
		gf_mul(x, h);
	}
}

int ag_aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t alen,
		       const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *out)
{
	uint8_t h[16] = { 0 }, j0[16], ctr[16], ks[16], x[16] = { 0 }, lens[16], diff = 0;
	size_t i, k;
	int b;

	ag_aes128_encrypt(key, h, h);
	memcpy(j0, iv, 12);
	j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
	ghash(x, h, aad, alen);
	ghash(x, h, ct, len);
	for (b = 0; b < 8; b++) {
		lens[b] = (uint8_t)(((uint64_t)alen * 8) >> (56 - 8 * b));
		lens[8 + b] = (uint8_t)(((uint64_t)len * 8) >> (56 - 8 * b));
	}
	ghash(x, h, lens, 16);
	ag_aes128_encrypt(key, j0, ks);
	for (i = 0; i < 16; i++)
		diff |= (uint8_t)(ks[i] ^ x[i] ^ tag[i]);
	if (diff)
		return -1;
	memcpy(ctr, j0, 16);
	for (i = 0; i < len; i += 16) {
		for (b = 15; b >= 12 && ++ctr[b] == 0; b--)
			;
		ag_aes128_encrypt(key, ctr, ks);
		for (k = 0; k < 16 && i + k < len; k++)
			out[i + k] = ct[i + k] ^ ks[k];
	}
	return 0;
}

/* ---------- QUIC Initial keys (RFC 9001 section 5.2, RFC 9369) ---------- */

#define QUIC_V1 0x00000001u
#define QUIC_V2 0x6b3343cfu

int ag_quic_client_keys(uint32_t version, const uint8_t *dcid, size_t dlen,
			uint8_t key[16], uint8_t iv[12], uint8_t hp[16])
{
	static const uint8_t salt1[20] = {
		0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
		0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a,
	};
	static const uint8_t salt2[20] = {
		0x0d, 0xed, 0xe3, 0xde, 0xf7, 0x00, 0xa6, 0xdb, 0x81, 0x93,
		0x81, 0xbe, 0x6e, 0x26, 0x9d, 0xcb, 0xf9, 0xbd, 0x2e, 0xd9,
	};
	uint8_t initial[32], client[32];
	int v2 = version == QUIC_V2;

	if (version != QUIC_V1 && !v2)
		return -1;
	ag_hkdf_extract(v2 ? salt2 : salt1, 20, dcid, dlen, initial);
	expand_label(initial, "client in", client, 32);
	expand_label(client, v2 ? "quicv2 key" : "quic key", key, 16);
	expand_label(client, v2 ? "quicv2 iv" : "quic iv", iv, 12);
	expand_label(client, v2 ? "quicv2 hp" : "quic hp", hp, 16);
	return 0;
}

/* ---------- TLS ClientHello ---------- */

static uint32_t be16(const uint8_t *p)
{
	return ((uint32_t)p[0] << 8) | p[1];
}

/* a plausible host name, copied lower-case */
static int host_ok(const uint8_t *p, size_t n, char *out)
{
	size_t i;

	if (!n || n >= AG_SNI_MAX || p[0] == '.')
		return 0;
	for (i = 0; i < n; i++) {
		uint8_t c = p[i];

		if (c >= 'A' && c <= 'Z')
			c = (uint8_t)(c + 32);
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
			return 0;
		out[i] = (char)c;
	}
	out[n] = '\0';
	return 1;
}

int ag_tls_hello_sni(const uint8_t *h, size_t n, struct ag_sni *o)
{
	size_t mlen, avail, p, eend;
	int cut;

	memset(o, 0, sizeof(*o));
	if (n < 4)
		return AG_SNI_MORE;
	if (h[0] != 1)
		return AG_SNI_BAD;
	mlen = ((size_t)h[1] << 16) | ((size_t)h[2] << 8) | h[3];
	if (mlen < 38)
		return AG_SNI_BAD;
	cut = n < 4 + mlen;
	avail = cut ? n : 4 + mlen;
#define NEED(k) do { if (p + (k) > avail) return cut ? AG_SNI_MORE : AG_SNI_BAD; } while (0)
	p = 4;
	NEED(34);                                   /* version, random */
	p += 34;
	NEED(1);                                    /* session id */
	if (h[p] > 32)
		return AG_SNI_BAD;
	p += 1 + h[p];
	NEED(2);                                    /* cipher suites */
	p += 2 + be16(h + p);
	NEED(1);                                    /* compression methods */
	p += 1 + h[p];
	if (p == 4 + mlen)
		return AG_SNI_NONE;                     /* no extensions at all */
	NEED(2);
	eend = p + 2 + be16(h + p);
	p += 2;
	if (eend > 4 + mlen)
		return AG_SNI_BAD;
	while (p < eend) {
		uint32_t t, l;

		if (p + 4 > avail)
			return o->name[0] ? AG_SNI_FOUND : cut ? AG_SNI_MORE : AG_SNI_BAD;
		t = be16(h + p);
		l = be16(h + p + 2);
		p += 4;
		if (p + l > eend)
			return AG_SNI_BAD;
		if (t == 0xfe0d)
			o->ech = 1;
		if ((t == 0 || t == 16) && p + l > avail) /* this extension is cut short */
			return o->name[0] ? AG_SNI_FOUND : AG_SNI_MORE;
		if (t == 0 && l >= 5) {                 /* server_name: list, type 0 = host_name */
			size_t q = p + 2, end = p + 2 + be16(h + p);

			if (end > p + l)
				return AG_SNI_BAD;
			while (q + 3 <= end) {
				uint32_t nl = be16(h + q + 1);

				if (q + 3 + nl > end)
					return AG_SNI_BAD;
				if (h[q] == 0) {
					if (!host_ok(h + q + 3, nl, o->name))
						return AG_SNI_BAD;
					break;
				}
				q += 3 + nl;
			}
		} else if (t == 16 && l >= 3) {         /* ALPN: list of length-prefixed names */
			size_t pl = h[p + 2];

			if (pl && pl < sizeof(o->alpn) && 3 + pl <= l) {
				size_t i;

				for (i = 0; i < pl; i++)
					o->alpn[i] = (h[p + 3 + i] >= 0x21 && h[p + 3 + i] <= 0x7e) ? (char)h[p + 3 + i] : '?';
				o->alpn[pl] = '\0';
			}
		}
		p += l;
	}
#undef NEED
	return o->name[0] ? AG_SNI_FOUND : cut ? AG_SNI_MORE : AG_SNI_NONE;
}

int ag_tls_stream_sni(const uint8_t *d, size_t n, struct ag_sni *o)
{
	uint8_t hs[AG_HELLO_MAX];
	size_t hn = 0, p = 0;

	memset(o, 0, sizeof(*o));
	while (p < n) {
		size_t rl, take;

		if (n - p < 5)
			break;
		if (d[p] != 0x16 || d[p + 1] != 3)
			return AG_SNI_BAD;
		rl = be16(d + p + 3);
		if (!rl || rl > 16384 + 256)
			return AG_SNI_BAD;
		take = rl < n - p - 5 ? rl : n - p - 5;
		if (take > sizeof(hs) - hn)
			take = sizeof(hs) - hn;
		memcpy(hs + hn, d + p + 5, take);
		hn += take;
		if (p + 5 + rl > n || hn == sizeof(hs))
			break; /* this record continues in the next segment */
		p += 5 + rl;
	}
	if (!hn)
		return AG_SNI_MORE;
	return ag_tls_hello_sni(hs, hn, o);
}

/* ---------- QUIC client Initial ---------- */

static int varint(const uint8_t *p, size_t len, size_t *off, uint64_t *v)
{
	size_t l, i;

	if (*off >= len)
		return -1;
	l = (size_t)1 << (p[*off] >> 6);
	if (*off + l > len)
		return -1;
	*v = p[*off] & 0x3f;
	for (i = 1; i < l; i++)
		*v = (*v << 8) | p[*off + i];
	*off += l;
	return 0;
}

static void keep_crypto(struct ag_quic_hello *q, uint64_t off, const uint8_t *data, uint64_t len)
{
	uint64_t i;

	for (i = 0; i < len && off + i < AG_HELLO_MAX; i++) {
		q->buf[off + i] = data[i];
		q->have[(off + i) / 8] |= (uint8_t)(1 << ((off + i) % 8));
	}
}

/* Frames of a decrypted Initial: CRYPTO data is kept, the rest skipped. */
static int frames(struct ag_quic_hello *q, const uint8_t *p, size_t len)
{
	size_t off = 0;

	while (off < len) {
		uint64_t t, a, b, c, i;

		if (varint(p, len, &off, &t) < 0)
			return -1;
		switch (t) {
		case 0x00: /* PADDING */
		case 0x01: /* PING */
			break;
		case 0x02: case 0x03: /* ACK: largest, delay, ranges, first range [, ECN counts] */
			if (varint(p, len, &off, &a) || varint(p, len, &off, &b) || varint(p, len, &off, &c) ||
			    varint(p, len, &off, &a))
				return -1;
			for (i = 0; i < c; i++)
				if (varint(p, len, &off, &a) || varint(p, len, &off, &b))
					return -1;
			if (t == 0x03 && (varint(p, len, &off, &a) || varint(p, len, &off, &b) ||
					  varint(p, len, &off, &c)))
				return -1;
			break;
		case 0x06: /* CRYPTO: offset, length, data */
			if (varint(p, len, &off, &a) || varint(p, len, &off, &b) || b > len - off)
				return -1;
			keep_crypto(q, a, p + off, b);
			off += (size_t)b;
			break;
		default:   /* CONNECTION_CLOSE or anything else: nothing more for us */
			return 0;
		}
	}
	return 0;
}

int ag_quic_feed(struct ag_quic_hello *q, const uint8_t *dg, size_t n)
{
	static uint8_t pkt[2048], plain[2048];
	size_t off = 0;
	int got = 0;

	while (off < n) {
		const uint8_t *p = dg + off;
		size_t rem = n - off, pos = 5, pnoff, end, pnlen, ctlen, i;
		uint8_t key[16], iv[12], hp[16], mask[16], dcid[20], dcl;
		uint64_t toklen, length, pn = 0;
		uint32_t ver;
		int type, initial;

		if (rem < 7 || !(p[0] & 0x80))
			break;                                  /* short header: no Initial after it */
		ver = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
		if (!ver)
			break;                                  /* version negotiation */
		type = (p[0] >> 4) & 3;
		initial = (ver == QUIC_V1 && type == 0) || (ver == QUIC_V2 && type == 1);
		dcl = p[pos++];
		if (dcl > 20 || pos + dcl + 1 > rem)
			return got ? 1 : AG_SNI_BAD;
		memcpy(dcid, p + pos, dcl);
		pos += dcl;
		pos += 1 + p[pos];                          /* source connection id */
		if (pos > rem)
			return got ? 1 : AG_SNI_BAD;
		if (initial && (varint(p, rem, &pos, &toklen) < 0 || toklen > rem - pos))
			return got ? 1 : AG_SNI_BAD;
		if (initial)
			pos += (size_t)toklen;
		if (varint(p, rem, &pos, &length) < 0 || length > rem - pos)
			return got ? 1 : AG_SNI_BAD;
		pnoff = pos;
		end = pnoff + (size_t)length;
		off += end;
		if (!initial)
			continue;                               /* 0-RTT, Handshake: not ours */
		if (ag_quic_client_keys(ver, dcid, dcl, key, iv, hp) < 0 || pnoff + 4 + 16 > end ||
		    end > sizeof(pkt))
			continue;
		memcpy(pkt, p, end);
		ag_aes128_encrypt(hp, pkt + pnoff + 4, mask); /* header protection */
		pkt[0] ^= mask[0] & 0x0f;
		pnlen = (pkt[0] & 3) + 1;
		for (i = 0; i < pnlen; i++) {
			pkt[pnoff + i] ^= mask[1 + i];
			pn = (pn << 8) | pkt[pnoff + i];
		}
		if (end < pnoff + pnlen + 16 + 1)
			continue;
		ctlen = end - pnoff - pnlen - 16;
		for (i = 0; i < 8; i++)
			iv[11 - i] ^= (uint8_t)(pn >> (8 * i));
		if (ag_aes128_gcm_open(key, iv, pkt, pnoff + pnlen, pkt + pnoff + pnlen, ctlen,
				       pkt + pnoff + pnlen + ctlen, plain) < 0)
			continue;                               /* the server's, or not QUIC at all */
		if (frames(q, plain, ctlen) == 0)
			got = 1;
	}
	return got;
}

int ag_quic_sni(const struct ag_quic_hello *q, struct ag_sni *o)
{
	size_t n = 0;

	while (n < AG_HELLO_MAX && (q->have[n / 8] & (1 << (n % 8))))
		n++;
	if (!n) {
		memset(o, 0, sizeof(*o));
		return AG_SNI_MORE;
	}
	return ag_tls_hello_sni(q->buf, n, o);
}
