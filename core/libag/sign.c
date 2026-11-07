/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SHA-512 and Ed25519 signature verification, small and dependency-free so
 * it runs on every router CPU. The curve arithmetic follows TweetNaCl (public
 * domain, Bernstein et al.): 16 limbs of 16 bits, constant-time selects.
 * The curve constants are derived at first use instead of typed in.
 */
#include <string.h>

#include "ag/sign.h"

typedef int64_t gf[16];

/* ---------- SHA-512 (FIPS 180-4) ---------- */

static const uint64_t K[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static const uint64_t H0[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t h[8], const uint8_t *p)
{
	uint64_t w[80], a, b, c, d, e, f, g, hh, t1, t2;
	int i, j;

	for (i = 0; i < 16; i++) {
		w[i] = 0;
		for (j = 0; j < 8; j++)
			w[i] = (w[i] << 8) | p[i * 8 + j];
	}
	for (i = 16; i < 80; i++) {
		uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
		uint64_t s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (i = 0; i < 80; i++) {
		t1 = hh + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

struct sha512 {
	uint64_t h[8];
	uint8_t buf[128];
	size_t n;
	uint64_t total;
};

static void sha_init(struct sha512 *s)
{
	memcpy(s->h, H0, sizeof(H0));
	s->n = 0;
	s->total = 0;
}

static void sha_update(struct sha512 *s, const uint8_t *p, size_t len)
{
	s->total += len;
	while (len) {
		size_t take = 128 - s->n < len ? 128 - s->n : len;

		memcpy(s->buf + s->n, p, take);
		s->n += take;
		p += take;
		len -= take;
		if (s->n == 128) {
			sha512_block(s->h, s->buf);
			s->n = 0;
		}
	}
}

static void sha_final(struct sha512 *s, uint8_t out[64])
{
	uint64_t bits = s->total * 8;
	int i;

	s->buf[s->n++] = 0x80;
	if (s->n > 112) {
		memset(s->buf + s->n, 0, 128 - s->n);
		sha512_block(s->h, s->buf);
		s->n = 0;
	}
	memset(s->buf + s->n, 0, 128 - s->n);
	for (i = 0; i < 8; i++)
		s->buf[127 - i] = (uint8_t)(bits >> (8 * i));
	sha512_block(s->h, s->buf);
	for (i = 0; i < 64; i++)
		out[i] = (uint8_t)(s->h[i / 8] >> (8 * (7 - i % 8)));
}

void ag_sha512(const uint8_t *msg, size_t len, uint8_t out[64])
{
	struct sha512 s;

	sha_init(&s);
	sha_update(&s, msg, len);
	sha_final(&s, out);
}

/* ---------- field arithmetic mod p = 2^255 - 19 ---------- */

static const gf gf0;
static const gf gf1 = { 1 };
static gf D, D2, I, BX, BY;
static int ready;

static void set25519(gf r, const gf a)
{
	memcpy(r, a, sizeof(gf));
}

static void car25519(gf o)
{
	int i;
	int64_t c;

	for (i = 0; i < 16; i++) {
		o[i] += (int64_t)1 << 16;
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * ((int64_t)1 << 16);
	}
}

static void sel25519(gf p, gf q, int b)
{
	int64_t t, c = ~(int64_t)(b - 1);
	int i;

	for (i = 0; i < 16; i++) {
		t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

static void pack25519(uint8_t *o, const gf n)
{
	int i, j, b;
	gf m, t;

	set25519(t, n);
	car25519(t);
	car25519(t);
	car25519(t);
	for (j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		sel25519(t, m, 1 - b);
	}
	for (i = 0; i < 16; i++) {
		o[2 * i] = (uint8_t)(t[i] & 0xff);
		o[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}

static int verify32(const uint8_t *x, const uint8_t *y)
{
	unsigned d = 0;
	int i;

	for (i = 0; i < 32; i++)
		d |= x[i] ^ y[i];
	return (1 & ((d - 1) >> 8)) - 1;    /* 0 equal, -1 different */
}

static int neq25519(const gf a, const gf b)
{
	uint8_t c[32], d[32];

	pack25519(c, a);
	pack25519(d, b);
	return verify32(c, d);
}

static uint8_t par25519(const gf a)
{
	uint8_t d[32];

	pack25519(d, a);
	return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b)
{
	int i;

	for (i = 0; i < 16; i++)
		o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b)
{
	int64_t t[31];
	int i, j;

	memset(t, 0, sizeof(t));
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++)
			t[i + j] += a[i] * b[j];
	for (i = 0; i < 15; i++)
		t[i] += 38 * t[i + 16];
	for (i = 0; i < 16; i++)
		o[i] = t[i];
	car25519(o);
	car25519(o);
}

static void S(gf o, const gf a)
{
	M(o, a, a);
}

/* o = i^e where e has bits top..0 set except the bits listed in skip[] */
static void powskip(gf o, const gf i, int top, int skip1, int skip2)
{
	gf c;
	int a;

	set25519(c, i);
	for (a = top - 1; a >= 0; a--) {
		S(c, c);
		if (a != skip1 && a != skip2)
			M(c, c, i);
	}
	set25519(o, c);
}

static void inv25519(gf o, const gf i)
{
	powskip(o, i, 254, 2, 4);           /* i^(p-2) */
}

static void pow2523(gf o, const gf i)
{
	powskip(o, i, 251, 1, -1);          /* i^((p-5)/8) */
}

/* ---------- curve points (extended coordinates X, Y, Z, T) ---------- */

static void add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;

	Z(a, p[1], p[0]);
	Z(t, q[1], q[0]);
	M(a, a, t);
	A(b, p[0], p[1]);
	A(t, q[0], q[1]);
	M(b, b, t);
	M(c, p[3], q[3]);
	M(c, c, D2);
	M(d, p[2], q[2]);
	A(d, d, d);
	Z(e, b, a);
	Z(f, d, c);
	A(g, d, c);
	A(h, b, a);
	M(p[0], e, f);
	M(p[1], h, g);
	M(p[2], g, f);
	M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], int b)
{
	int i;

	for (i = 0; i < 4; i++)
		sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, gf p[4])
{
	gf tx, ty, zi;

	inv25519(zi, p[2]);
	M(tx, p[0], zi);
	M(ty, p[1], zi);
	pack25519(r, ty);
	r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
	int i;

	set25519(p[0], gf0);
	set25519(p[1], gf1);
	set25519(p[2], gf1);
	set25519(p[3], gf0);
	for (i = 255; i >= 0; --i) {
		int b = (s[i / 8] >> (i & 7)) & 1;

		cswap(p, q, b);
		add(q, p);
		add(p, p);
		cswap(p, q, b);
	}
}

static void scalarbase(gf p[4], const uint8_t *s)
{
	gf q[4];

	set25519(q[0], BX);
	set25519(q[1], BY);
	set25519(q[2], gf1);
	M(q[3], BX, BY);
	scalarmult(p, q, s);
}

/* decode a point and negate it: r = -P */
static int unpackneg(gf r[4], const uint8_t p[32])
{
	gf t, chk, num, den, den2, den4, den6;

	set25519(r[2], gf1);
	unpack25519(r[1], p);
	S(num, r[1]);
	M(den, num, D);
	Z(num, num, r[2]);
	A(den, r[2], den);
	S(den2, den);
	S(den4, den2);
	M(den6, den4, den2);
	M(t, den6, num);
	M(t, t, den);
	pow2523(t, t);
	M(t, t, num);
	M(t, t, den);
	M(t, t, den);
	M(r[0], t, den);
	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num))
		M(r[0], r[0], I);
	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num))
		return -1;
	if (par25519(r[0]) == (p[31] >> 7))
		Z(r[0], gf0, r[0]);
	M(r[3], r[0], r[1]);
	return 0;
}

/* d = -121665/121666, I = sqrt(-1) = 2^((p-1)/4), B = the point with y = 4/5, x even */
static void derive_constants(void)
{
	gf a = { 0 }, b = { 0 }, two = { 2 }, four = { 4 }, five = { 5 }, inv;
	gf bp[4];
	uint8_t enc[32];

	a[0] = 121665 & 0xffff; a[1] = 121665 >> 16;
	b[0] = 121666 & 0xffff; b[1] = 121666 >> 16;
	inv25519(inv, b);
	M(D, a, inv);
	Z(D, gf0, D);
	A(D2, D, D);
	powskip(I, two, 252, 2, -1);
	inv25519(inv, five);
	M(BY, four, inv);
	pack25519(enc, BY);                 /* sign bit 0: x even */
	unpackneg(bp, enc);                 /* gives -B */
	Z(BX, gf0, bp[0]);
	ready = 1;
}

/* ---------- scalars mod L = 2^252 + 27742317777372353535851937790883648493 ---------- */

static const int64_t L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

static void modL(uint8_t *r, int64_t x[64])
{
	int64_t carry;
	int i, j;

	for (i = 63; i >= 32; --i) {
		carry = 0;
		for (j = i - 32; j < i - 12; ++j) {
			x[j] += carry - 16 * x[i] * L[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry * 256;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	for (j = 0; j < 32; j++) {
		x[j] += carry - (x[31] >> 4) * L[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (j = 0; j < 32; j++)
		x[j] -= carry * L[j];
	for (i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = (uint8_t)(x[i] & 255);
	}
}

static void reduce(uint8_t *r)
{
	int64_t x[64];
	int i;

	for (i = 0; i < 64; i++)
		x[i] = r[i];
	memset(r, 0, 64);
	modL(r, x);
}

/* s < L, so a signature can't be re-encoded (RFC 8032 5.1.7) */
static int scalar_ok(const uint8_t *s)
{
	int i;

	for (i = 31; i >= 0; i--) {
		if (s[i] < L[i])
			return 1;
		if (s[i] > L[i])
			return 0;
	}
	return 0;
}

int ag_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32])
{
	struct sha512 s;
	uint8_t h[64], t[32];
	gf p[4], q[4];

	if (!ready)
		derive_constants();
	if (!scalar_ok(sig + 32) || unpackneg(q, pk))
		return -1;
	sha_init(&s);
	sha_update(&s, sig, 32);
	sha_update(&s, pk, 32);
	sha_update(&s, msg, len);
	sha_final(&s, h);
	reduce(h);
	scalarmult(p, q, h);                /* -hA */
	scalarbase(q, sig + 32);            /* sB */
	add(p, q);
	pack(t, p);
	return verify32(sig, t) ? -1 : 0;
}

/* ---------- usign files ---------- */

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* the base64 line after "untrusted comment: ..."; returns bytes decoded */
static int usign_body(const char *text, uint8_t *out, size_t max)
{
	const char *p = text;
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;

	if (strncmp(p, "untrusted comment:", 18))
		return -1;
	p = strchr(p, '\n');
	if (!p)
		return -1;
	for (p++; *p && *p != '\n' && *p != '\r' && *p != '='; p++) {
		int v = b64val((unsigned char)*p);

		if (v < 0)
			return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= max)
				return -1;
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)n;
}

int ag_usign_pubkey(const char *text, uint8_t fingerprint[8], uint8_t pk[32])
{
	uint8_t raw[64];

	if (usign_body(text, raw, sizeof(raw)) != 42 || raw[0] != 'E' || raw[1] != 'd')
		return -1;
	memcpy(fingerprint, raw + 2, 8);
	memcpy(pk, raw + 10, 32);
	return 0;
}

int ag_usign_sig(const char *text, uint8_t fingerprint[8], uint8_t sig[64])
{
	uint8_t raw[96];

	if (usign_body(text, raw, sizeof(raw)) != 74 || raw[0] != 'E' || raw[1] != 'd')
		return -1;
	memcpy(fingerprint, raw + 2, 8);
	memcpy(sig, raw + 10, 64);
	return 0;
}

void ag_fingerprint_hex(const uint8_t fingerprint[8], char out[17])
{
	static const char hx[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 8; i++) {
		out[2 * i] = hx[fingerprint[i] >> 4];
		out[2 * i + 1] = hx[fingerprint[i] & 15];
	}
	out[16] = '\0';
}
