/* SPDX-License-Identifier: Apache-2.0 */
/* SHA-512 and Ed25519 verification against FIPS 180-4 and RFC 8032 vectors; usign file parsing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/sign.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;

	for (; h[0] && h[1]; h += 2)
		sscanf(h, "%2hhx", &out[n++]);
	return n;
}

static void hexs(const uint8_t *b, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
}

struct vec { const char *pk, *msg, *sig; };

static const struct vec rfc8032[] = {
	{ "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
	  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
	{ "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
	  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
	{ "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
	  "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

int main(void)
{
	uint8_t out[64], pk[32], sig[64], msg[64], fp[8], fp2[8], big[1000];
	char hx[129], fph[17];
	size_t i, mlen;

	ag_sha512((const uint8_t *)"abc", 3, out);
	hexs(out, 64, hx);
	CHECK(!strcmp(hx, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
			  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"), "sha512(abc) = %s", hx);
	ag_sha512((const uint8_t *)"", 0, out);
	hexs(out, 64, hx);
	CHECK(!strncmp(hx, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce", 64), "sha512('') = %s", hx);
	/* 112 bytes: the padding spills into a second block */
	ag_sha512((const uint8_t *)"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
		  112, out);
	hexs(out, 64, hx);
	CHECK(!strncmp(hx, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018", 64), "sha512(112 bytes) = %s", hx);

	for (i = 0; i < sizeof(rfc8032) / sizeof(rfc8032[0]); i++) {
		unhex(rfc8032[i].pk, pk);
		unhex(rfc8032[i].sig, sig);
		mlen = unhex(rfc8032[i].msg, msg);
		CHECK(ag_ed25519_verify(sig, msg, mlen, pk) == 0, "RFC 8032 test %zu verifies", i + 1);
		sig[10] ^= 1;
		CHECK(ag_ed25519_verify(sig, msg, mlen, pk) < 0, "RFC 8032 test %zu: changed signature refused", i + 1);
		sig[10] ^= 1;
		msg[0] ^= 0x80;
		CHECK(ag_ed25519_verify(sig, msg, mlen ? mlen : 1, pk) < 0, "RFC 8032 test %zu: changed message refused", i + 1);
		msg[0] ^= 0x80;
		pk[3] ^= 4;
		CHECK(ag_ed25519_verify(sig, msg, mlen, pk) < 0, "RFC 8032 test %zu: other key refused", i + 1);
		pk[3] ^= 4;
		sig[63] |= 0xf0;                /* s >= L: malleated */
		CHECK(ag_ed25519_verify(sig, msg, mlen, pk) < 0, "RFC 8032 test %zu: s >= L refused", i + 1);
	}

	/* usign format: "Ed" | fingerprint | key, base64 under a comment line */
	CHECK(ag_usign_pubkey("untrusted comment: test key\nRWQBAgMEBQYHCNdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea\n", fp, pk) == 0,
	      "usign public key parses");
	ag_fingerprint_hex(fp, fph);
	CHECK(!strcmp(fph, "0102030405060708"), "fingerprint %s", fph);
	hexs(pk, 32, hx);
	CHECK(!strcmp(hx, rfc8032[0].pk), "public key from usign file: %s", hx);
	CHECK(ag_usign_sig("untrusted comment: signed by key 0102030405060708\n"
			   "RWQBAgMEBQYHCOVWQwDDYKxykIbizIBugoqEh38euOXZdNhz4GUiSQFVX7iCFZCjO6zGHjlwHPm0a9Jb9fBZW74kZVFBQ456EAs=\n",
			   fp2, sig) == 0, "usign signature parses");
	CHECK(!memcmp(fp, fp2, 8), "signature names the key");
	CHECK(ag_ed25519_verify(sig, (const uint8_t *)"", 0, pk) == 0, "usign signature verifies");
	CHECK(ag_usign_pubkey("RWQBAgMEBQYHCNdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea\n", fp, pk) < 0, "comment line required");
	CHECK(ag_usign_pubkey("untrusted comment: x\nRWQBAgME\n", fp, pk) < 0, "short key refused");
	CHECK(ag_usign_sig("untrusted comment: x\nRWQBAgMEBQYHCNdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea\n", fp, sig) < 0,
	      "a key is not a signature");

	/* longer message, unsigned */
	memset(big, 'z', sizeof(big));
	CHECK(ag_ed25519_verify(sig, big, sizeof(big), pk) < 0, "unrelated message refused");

	printf("sign: %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
