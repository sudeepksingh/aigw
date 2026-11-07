/* SPDX-License-Identifier: Apache-2.0 */
/*
 * QR code encoder: byte mode, versions 1-10, error correction L/M.
 * Follows ISO/IEC 18004: data + Reed-Solomon blocks, interleaving, function
 * patterns, zig-zag placement, the mask with the lowest penalty, format and
 * version information.
 */
#include <stdio.h>
#include <string.h>

#include "ag/qr.h"

/* per version 1..10 and level: EC codewords per block, blocks in group 1
 * (with data codewords each), blocks in group 2 (one data codeword more) */
struct blocks { uint8_t ec, n1, d1, n2; };

static const struct blocks table[2][10] = {
	[AG_QR_ECC_L] = {
		{ 7, 1, 19, 0 }, { 10, 1, 34, 0 }, { 15, 1, 55, 0 }, { 20, 1, 80, 0 }, { 26, 1, 108, 0 },
		{ 18, 2, 68, 0 }, { 20, 2, 78, 0 }, { 24, 2, 97, 0 }, { 30, 2, 116, 0 }, { 18, 2, 68, 2 },
	},
	[AG_QR_ECC_M] = {
		{ 10, 1, 16, 0 }, { 16, 1, 28, 0 }, { 26, 1, 44, 0 }, { 18, 2, 32, 0 }, { 24, 2, 43, 0 },
		{ 16, 4, 27, 0 }, { 18, 4, 31, 0 }, { 22, 2, 38, 2 }, { 22, 3, 36, 2 }, { 26, 4, 43, 1 },
	},
};

/* alignment pattern centres (besides 6), versions 2..10 */
static const uint8_t align_pos[11][3] = {
	{ 0 }, { 0 }, { 18 }, { 22 }, { 26 }, { 30 }, { 34 }, { 22, 38 }, { 24, 42 }, { 26, 46 }, { 28, 50 },
};

static uint8_t fn[AG_QR_MAX_SIZE][AG_QR_MAX_SIZE];     /* function modules (not data) */

static int data_codewords(int ver, int ecc)
{
	const struct blocks *b = &table[ecc][ver - 1];

	return b->n1 * b->d1 + b->n2 * (b->d1 + 1);
}

/* ---------- Reed-Solomon over GF(256), polynomial 0x11d ---------- */

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	unsigned r = 0;
	int i;

	for (i = 7; i >= 0; i--) {
		r = (r << 1) ^ ((r >> 7) * 0x11d);
		r ^= ((b >> i) & 1u) * a;
	}
	return (uint8_t)r;
}

static void rs_remainder(const uint8_t *data, int len, int degree, uint8_t *out)
{
	uint8_t gen[32] = { 0 }, root = 1;
	int i, j;

	/* generator: product of (x - a^i), i = 0 .. degree-1; coefficients highest first, monic omitted */
	gen[degree - 1] = 1;
	for (i = 0; i < degree; i++) {
		for (j = 0; j < degree; j++) {
			gen[j] = gf_mul(gen[j], root);
			if (j + 1 < degree)
				gen[j] ^= gen[j + 1];
		}
		root = gf_mul(root, 2);
	}
	memset(out, 0, (size_t)degree);
	for (i = 0; i < len; i++) {
		uint8_t factor = data[i] ^ out[0];

		memmove(out, out + 1, (size_t)degree - 1);
		out[degree - 1] = 0;
		for (j = 0; j < degree; j++)
			out[j] ^= gf_mul(gen[j], factor);
	}
}

/* ---------- drawing ---------- */

static void set_fn(struct ag_qr *q, int x, int y, int dark)
{
	q->m[y][x] = (uint8_t)(dark != 0);
	fn[y][x] = 1;
}

static void finder(struct ag_qr *q, int cx, int cy)
{
	int dx, dy;

	for (dy = -4; dy <= 4; dy++)
		for (dx = -4; dx <= 4; dx++) {
			int x = cx + dx, y = cy + dy, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
			int dist = ax > ay ? ax : ay;

			if (x >= 0 && x < q->size && y >= 0 && y < q->size)
				set_fn(q, x, y, dist != 2 && dist != 4);
		}
}

static void alignment(struct ag_qr *q, int cx, int cy)
{
	int dx, dy;

	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++) {
			int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

			set_fn(q, cx + dx, cy + dy, (ax > ay ? ax : ay) != 1);
		}
}

static void format_bits(struct ag_qr *q, int ecc, int mask)
{
	int data = ((ecc == AG_QR_ECC_L ? 1 : 0) << 3) | mask, rem = data, bits, i, s = q->size;

	for (i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	bits = ((data << 10) | rem) ^ 0x5412;
	for (i = 0; i <= 5; i++)
		set_fn(q, 8, i, (bits >> i) & 1);
	set_fn(q, 8, 7, (bits >> 6) & 1);
	set_fn(q, 8, 8, (bits >> 7) & 1);
	set_fn(q, 7, 8, (bits >> 8) & 1);
	for (i = 9; i < 15; i++)
		set_fn(q, 14 - i, 8, (bits >> i) & 1);
	for (i = 0; i < 8; i++)
		set_fn(q, s - 1 - i, 8, (bits >> i) & 1);
	for (i = 8; i < 15; i++)
		set_fn(q, 8, s - 15 + i, (bits >> i) & 1);
	set_fn(q, 8, s - 8, 1);         /* always dark */
}

static void version_bits(struct ag_qr *q)
{
	long rem = q->version, bits;
	int i;

	if (q->version < 7)
		return;
	for (i = 0; i < 12; i++)
		rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
	bits = ((long)q->version << 12) | rem;
	for (i = 0; i < 18; i++) {
		int bit = (int)((bits >> i) & 1), a = q->size - 11 + i % 3, b = i / 3;

		set_fn(q, a, b, bit);
		set_fn(q, b, a, bit);
	}
}

static void function_patterns(struct ag_qr *q, int ecc)
{
	int i, j, n = 1, pos[4] = { 6 };

	for (i = 0; i < q->size; i++) {
		set_fn(q, 6, i, i % 2 == 0);
		set_fn(q, i, 6, i % 2 == 0);
	}
	finder(q, 3, 3);
	finder(q, q->size - 4, 3);
	finder(q, 3, q->size - 4);
	for (i = 0; i < 3 && align_pos[q->version][i]; i++)
		pos[n++] = align_pos[q->version][i];
	if (q->version >= 2)
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++)
				if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
					alignment(q, pos[i], pos[j]);
	format_bits(q, ecc, 0);         /* reserve; drawn for real after masking */
	version_bits(q);
}

static void place(struct ag_qr *q, const uint8_t *cw, int ncw)
{
	int right, vert, j, i = 0, s = q->size;

	for (right = s - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (vert = 0; vert < s; vert++)
			for (j = 0; j < 2; j++) {
				int x = right - j, up = ((right + 1) & 2) == 0, y = up ? s - 1 - vert : vert;

				if (fn[y][x])
					continue;
				if (i < ncw * 8) {
					q->m[y][x] = (uint8_t)((cw[i >> 3] >> (7 - (i & 7))) & 1);
					i++;
				} else {
					q->m[y][x] = 0;  /* remainder bits */
				}
			}
	}
}

static int mask_bit(int mask, int x, int y)
{
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

static void apply_mask(struct ag_qr *q, int mask)
{
	int x, y;

	for (y = 0; y < q->size; y++)
		for (x = 0; x < q->size; x++)
			if (!fn[y][x] && mask_bit(mask, x, y))
				q->m[y][x] ^= 1;
}

static int module_at(const struct ag_qr *q, int x, int y, int horizontal)
{
	return horizontal ? q->m[y][x] : q->m[x][y];
}

static int penalty(const struct ag_qr *q)
{
	static const uint8_t pat[2][11] = {
		{ 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 },
	};
	int s = q->size, p = 0, dark = 0, x, y, h, k;

	for (h = 0; h < 2; h++)
		for (y = 0; y < s; y++) {
			int run = 1;

			for (x = 1; x <= s; x++) {
				if (x < s && module_at(q, x, y, h) == module_at(q, x - 1, y, h)) {
					run++;
					continue;
				}
				if (run >= 5)
					p += 3 + run - 5;
				run = 1;
			}
			for (x = 0; x + 11 <= s; x++)
				for (k = 0; k < 2; k++) {
					int i, ok = 1;

					for (i = 0; i < 11 && ok; i++)
						ok = module_at(q, x + i, y, h) == pat[k][i];
					p += ok ? 40 : 0;
				}
		}
	for (y = 0; y + 1 < s; y++)
		for (x = 0; x + 1 < s; x++) {
			int c = q->m[y][x];

			if (c == q->m[y][x + 1] && c == q->m[y + 1][x] && c == q->m[y + 1][x + 1])
				p += 3;
		}
	for (y = 0; y < s; y++)
		for (x = 0; x < s; x++)
			dark += q->m[y][x];
	k = s * s;
	p += (((dark * 20 - k * 10) < 0 ? -(dark * 20 - k * 10) : dark * 20 - k * 10) + k - 1) / k * 10 - 10;
	return p;
}

int ag_qr_encode(struct ag_qr *q, const uint8_t *data, size_t len, enum ag_qr_ecc ecc)
{
	uint8_t buf[400], cw[400], ecw[32];
	int ver, cap = 0, nbits = 0, i, j, k, best = 0, best_p = -1, mask;
	const struct blocks *b;

	for (ver = 1; ver <= 10; ver++) {
		cap = data_codewords(ver, ecc);
		if (4 + (ver < 10 ? 8 : 16) + 8 * (int)len <= cap * 8)
			break;
	}
	if (ver > 10)
		return -1;
	memset(q, 0, sizeof(*q));
	memset(fn, 0, sizeof(fn));
	q->version = ver;
	q->size = 17 + 4 * ver;

	/* data bits: mode 0100 (byte), count, bytes, terminator, padding */
	memset(buf, 0, sizeof(buf));
#define PUT(val, n) do { for (int _i = (n) - 1; _i >= 0; _i--, nbits++) \
		if (((val) >> _i) & 1) buf[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7)); } while (0)
	PUT(4u, 4);
	PUT((unsigned)len, ver < 10 ? 8 : 16);
	for (i = 0; i < (int)len; i++)
		PUT(data[i], 8);
	for (i = 0; i < 4 && nbits < cap * 8; i++)
		nbits++;
	nbits = (nbits + 7) & ~7;
	for (i = nbits / 8, k = 0; i < cap; i++, k++)
		buf[i] = k % 2 ? 0x11 : 0xec;
#undef PUT

	/* split into blocks, add error correction, interleave */
	b = &table[ecc][ver - 1];
	{
		int nblocks = b->n1 + b->n2, off = 0, n = 0, maxd = b->d1 + (b->n2 ? 1 : 0);
		uint8_t ecc_all[10][32];
		int dlen[10], dstart[10];

		for (i = 0; i < nblocks; i++) {
			dlen[i] = b->d1 + (i >= b->n1 ? 1 : 0);
			dstart[i] = off;
			rs_remainder(buf + off, dlen[i], b->ec, ecw);
			memcpy(ecc_all[i], ecw, b->ec);
			off += dlen[i];
		}
		for (j = 0; j < maxd; j++)
			for (i = 0; i < nblocks; i++)
				if (j < dlen[i])
					cw[n++] = buf[dstart[i] + j];
		for (j = 0; j < b->ec; j++)
			for (i = 0; i < nblocks; i++)
				cw[n++] = ecc_all[i][j];

		function_patterns(q, ecc);
		place(q, cw, n);
	}

	for (mask = 0; mask < 8; mask++) {
		int p;

		apply_mask(q, mask);
		format_bits(q, ecc, mask);
		p = penalty(q);
		if (best_p < 0 || p < best_p) {
			best_p = p;
			best = mask;
		}
		apply_mask(q, mask);        /* XOR again: undo */
	}
	apply_mask(q, best);
	format_bits(q, ecc, best);
	return 0;
}

int ag_qr_wifi_text(char *out, size_t n, const char *ssid, const char *passphrase)
{
	size_t len = 0;
	int part;

	for (part = 0; part < 3; part++) {
		const char *s = part == 0 ? "WIFI:T:WPA;S:" : part == 1 ? ssid : passphrase;
		const char *p;

		if (part == 2 && len + 3 < n) {
			memcpy(out + len, ";P:", 3);
			len += 3;
		}
		for (p = s; *p; p++) {
			int esc = part > 0 && strchr("\\;,:\"", *p);

			if (len + (size_t)esc + 1 >= n)
				return -1;
			if (esc)
				out[len++] = '\\';
			out[len++] = *p;
		}
	}
	if (len + 3 > n)
		return -1;
	memcpy(out + len, ";;", 3);
	return (int)len + 2;
}
