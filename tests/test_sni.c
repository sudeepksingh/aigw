/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Server names from TLS and QUIC: the crypto against published vectors
 * (FIPS 180-4, RFC 4231, RFC 5869, FIPS 197, the GCM spec, RFC 9001 A.1),
 * then real ClientHellos (tests/data/sni-*.bin, made by make_sni_fixtures.py
 * with OpenSSL and Python's cryptography), cut short, reordered and damaged.
 *
 *   test_sni <tests/data directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/sni.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;

	for (; h[0] && h[1]; h += 2)
		sscanf(h, "%2hhx", &out[n++]);
	return n;
}

static int same_hex(const uint8_t *b, size_t n, const char *hex)
{
	uint8_t want[128];

	return unhex(hex, want) == n && !memcmp(b, want, n);
}

static size_t load(const char *dir, const char *name, uint8_t *buf, size_t max)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f) {
		printf("FAIL: cannot read %s\n", path);
		fails++;
		return 0;
	}
	n = fread(buf, 1, max, f);
	fclose(f);
	return n;
}

static void crypto_vectors(void)
{
	uint8_t out[64], k[64], m[64], ct[16], tag[16], pt[16];
	size_t kl, ml;

	ag_sha256((const uint8_t *)"abc", 3, out);
	CHECK(same_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 abc");
	ag_sha256((const uint8_t *)"", 0, out);
	CHECK(same_hex(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 empty");
	ag_sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
	CHECK(same_hex(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 two blocks");

	ag_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
	CHECK(same_hex(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "HMAC-SHA256 RFC 4231 #2");

	memset(k, 0x0b, 22);
	ml = unhex("000102030405060708090a0b0c", m);
	ag_hkdf_extract(m, ml, k, 22, out);
	CHECK(same_hex(out, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"), "HKDF-Extract RFC 5869 #1");
	ml = unhex("f0f1f2f3f4f5f6f7f8f9", m);
	{
		uint8_t prk[32], okm[42];

		memcpy(prk, out, 32);
		ag_hkdf_expand(prk, m, ml, okm, 42);
		CHECK(same_hex(okm, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
		      "HKDF-Expand RFC 5869 #1");
	}

	kl = unhex("000102030405060708090a0b0c0d0e0f", k);
	unhex("00112233445566778899aabbccddeeff", m);
	ag_aes128_encrypt(k, m, out);
	CHECK(kl == 16 && same_hex(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"), "AES-128 FIPS 197 C.1");

	memset(k, 0, 16);
	memset(m, 0, 12);
	unhex("0388dace60b6a392f328c2b971b2fe78", ct);
	unhex("ab6e47d42cec13bdf53a67b21257bddf", tag);
	CHECK(ag_aes128_gcm_open(k, m, NULL, 0, ct, 16, tag, pt) == 0 && same_hex(pt, 16, "00000000000000000000000000000000"),
	      "AES-128-GCM test case 2");
	tag[0] ^= 1;
	CHECK(ag_aes128_gcm_open(k, m, NULL, 0, ct, 16, tag, pt) < 0, "AES-128-GCM accepted a wrong tag");

	{
		uint8_t dcid[8], key[16], iv[12], hp[16];

		unhex("8394c8f03e515708", dcid);
		CHECK(ag_quic_client_keys(1, dcid, 8, key, iv, hp) == 0 &&
		      same_hex(key, 16, "1f369613dd76d5467730efcbe3b1a22d") && same_hex(iv, 12, "fa044b2f42a3fd3b46fb255c") &&
		      same_hex(hp, 16, "9f50449e04a0e810283a1e9933adedd2"), "QUIC v1 client Initial keys, RFC 9001 A.1");
		CHECK(ag_quic_client_keys(0xff00001d, dcid, 8, key, iv, hp) < 0, "unknown QUIC version accepted");
	}
}

static void tls(const char *dir)
{
	static uint8_t rec[4096];
	struct ag_sni s;
	size_t n = load(dir, "sni-tls.bin", rec, sizeof(rec)), cut;
	int rc;

	rc = ag_tls_stream_sni(rec, n, &s);
	CHECK(rc == AG_SNI_FOUND && !strcmp(s.name, "www.example.com") && !strcmp(s.alpn, "h2") && !s.ech,
	      "TLS ClientHello: rc %d name '%s' alpn '%s'", rc, s.name, s.alpn);

	/* every cut: the name, or "more", never a wrong name or a crash */
	for (cut = 0; cut < n; cut++) {
		rc = ag_tls_stream_sni(rec, cut, &s);
		if (!(rc == AG_SNI_MORE || (rc == AG_SNI_FOUND && !strcmp(s.name, "www.example.com")))) {
			CHECK(0, "TLS cut at %zu: rc %d name '%s'", cut, rc, s.name);
			break;
		}
	}
	/* damage anywhere: no crash, and a name only ever made of host-name characters */
	{
		static uint8_t d[4096];
		size_t i;

		for (i = 0; i < n; i++) {
			memcpy(d, rec, n);
			d[i] ^= 0xff;
			rc = ag_tls_stream_sni(d, n, &s);
			if (rc == AG_SNI_FOUND && strspn(s.name, "abcdefghijklmnopqrstuvwxyz0123456789-._") != strlen(s.name)) {
				CHECK(0, "TLS damaged at %zu gave '%s'", i, s.name);
				break;
			}
		}
	}
	CHECK(ag_tls_stream_sni((const uint8_t *)"GET / HTTP/1.1\r\n", 16, &s) == AG_SNI_BAD, "plain HTTP taken for TLS");
}

static void quic(const char *dir)
{
	static uint8_t a[2048], b[2048], v2[2048];
	static struct ag_quic_hello q;
	struct ag_sni s;
	size_t na = load(dir, "sni-quic1-a.bin", a, sizeof(a));
	size_t nb = load(dir, "sni-quic1-b.bin", b, sizeof(b));
	size_t n2 = load(dir, "sni-quic2.bin", v2, sizeof(v2));
	int rc;

	memset(&q, 0, sizeof(q));
	CHECK(ag_quic_feed(&q, a, na) == 1, "QUIC v1 first Initial not decrypted");
	rc = ag_quic_sni(&q, &s);
	CHECK(rc == AG_SNI_MORE, "QUIC: start of the ClientHello missing, yet rc %d", rc);
	CHECK(ag_quic_feed(&q, b, nb) == 1, "QUIC v1 second Initial not decrypted");
	rc = ag_quic_sni(&q, &s);
	CHECK(rc == AG_SNI_FOUND && !strcmp(s.name, "video.example.net") && !strcmp(s.alpn, "h3"),
	      "QUIC v1 reassembled: rc %d name '%s' alpn '%s'", rc, s.name, s.alpn);

	memset(&q, 0, sizeof(q));
	CHECK(ag_quic_feed(&q, v2, n2) == 1 && ag_quic_sni(&q, &s) == AG_SNI_FOUND && !strcmp(s.name, "www.example.com"),
	      "QUIC v2: name '%s'", s.name);

	memset(&q, 0, sizeof(q));
	b[nb - 1] ^= 1; /* the tag no longer matches */
	CHECK(ag_quic_feed(&q, b, nb) == 0, "QUIC: a damaged packet was accepted");
	CHECK(ag_quic_feed(&q, (const uint8_t *)"\x40hello world....", 16) == 0, "a short-header packet was taken");
	{
		size_t cut;

		for (cut = 0; cut < na; cut += 7)
			ag_quic_feed(&q, a, cut); /* no crash on any truncation */
	}
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";

	crypto_vectors();
	tls(dir);
	quic(dir);
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("sni: all checks passed\n");
	return 0;
}
