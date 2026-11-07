/* SPDX-License-Identifier: Apache-2.0 */
/* QR encoder: structure checks always; a real decode when zbarimg is installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ag/qr.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void write_pbm(const struct ag_qr *q, const char *path)
{
	FILE *f = fopen(path, "w");
	int x, y, scale = 6, quiet = 4, side;

	if (!f)
		return;
	side = (q->size + 2 * quiet) * scale;
	fprintf(f, "P1\n%d %d\n", side, side);
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++) {
			int mx = x / scale - quiet, my = y / scale - quiet;

			fputc(mx >= 0 && my >= 0 && mx < q->size && my < q->size && q->m[my][mx] ? '1' : '0', f);
		}
		fputc('\n', f);
	}
	fclose(f);
}

static int finder_ok(const struct ag_qr *q, int x0, int y0)
{
	int x, y;

	for (y = 0; y < 7; y++)
		for (x = 0; x < 7; x++) {
			int ring = x == 0 || y == 0 || x == 6 || y == 6, core = x >= 2 && x <= 4 && y >= 2 && y <= 4;

			if (q->m[y0 + y][x0 + x] != (ring || core))
				return 0;
		}
	return 1;
}

static int have_zbar(void)
{
	return system("command -v zbarimg >/dev/null 2>&1") == 0;
}

static void roundtrip(const char *text, enum ag_qr_ecc ecc, int want_version)
{
	struct ag_qr q;
	char path[64], cmd[160], got[512] = "";
	FILE *p;

	CHECK(ag_qr_encode(&q, (const uint8_t *)text, strlen(text), ecc) == 0, "encode '%s'", text);
	CHECK(q.size == 17 + 4 * q.version, "size %d for version %d", q.size, q.version);
	if (want_version)
		CHECK(q.version == want_version, "'%.20s...' version %d, expected %d", text, q.version, want_version);
	CHECK(finder_ok(&q, 0, 0) && finder_ok(&q, q.size - 7, 0) && finder_ok(&q, 0, q.size - 7),
	      "finder patterns (version %d)", q.version);
	CHECK(q.m[q.size - 8][8] == 1, "dark module");
	if (!have_zbar())
		return;
	snprintf(path, sizeof(path), "/tmp/test_qr_%d.pbm", (int)getpid());
	write_pbm(&q, path);
	snprintf(cmd, sizeof(cmd), "zbarimg -q --raw %s 2>/dev/null", path);
	p = popen(cmd, "r");
	if (p) {
		size_t n = fread(got, 1, sizeof(got) - 1, p);

		got[n] = '\0';
		got[strcspn(got, "\n")] = '\0';
		pclose(p);
	}
	unlink(path);
	CHECK(!strcmp(got, text), "zbarimg read '%s', expected '%s' (version %d, ecc %c)", got, text, q.version,
	      ecc == AG_QR_ECC_M ? 'M' : 'L');
}

int main(void)
{
	char wifi[256], longtext[260];
	struct ag_qr q;
	int i;

	roundtrip("AIGW", AG_QR_ECC_M, 1);
	roundtrip("hello, world 0123456789", AG_QR_ECC_L, 2);

	CHECK(ag_qr_wifi_text(wifi, sizeof(wifi), "AIGW-BSP8", "k3y;with:odd\\chars") > 0, "wifi text");
	CHECK(!strcmp(wifi, "WIFI:T:WPA;S:AIGW-BSP8;P:k3y\\;with\\:odd\\\\chars;;"), "wifi escaping: %s", wifi);
	roundtrip(wifi, AG_QR_ECC_M, 0);

	/* every version and both levels */
	for (i = 1; i <= 210; i += 11) {
		memset(longtext, 0, sizeof(longtext));
		memset(longtext, 'a' + i % 26, (size_t)i);
		roundtrip(longtext, AG_QR_ECC_M, 0);
		if (i <= 200)
			roundtrip(longtext, AG_QR_ECC_L, 0);
	}
	memset(longtext, 'x', 250);
	longtext[250] = '\0';
	CHECK(ag_qr_encode(&q, (const uint8_t *)longtext, 250, AG_QR_ECC_M) < 0, "too long for version 10 M");
	CHECK(ag_qr_encode(&q, (const uint8_t *)longtext, 213, AG_QR_ECC_M) == 0 && q.version == 10, "213 bytes = v10 M");

	printf("qr: %s%s\n", fails ? "FAILED" : "ok", have_zbar() ? " (decoded with zbarimg)" : " (zbarimg not installed: structure only)");
	return fails ? 1 : 0;
}
