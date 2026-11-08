/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Server names from the first packets of a connection, for connections a
 * device opened without a DNS lookup AIGW saw (DNS over HTTPS, cached or
 * hard-coded addresses):
 *
 *   TCP   the TLS ClientHello's server_name (SNI) extension
 *   QUIC  the same ClientHello inside the client's Initial packets. Their
 *         protection uses keys derived from the packet itself (RFC 9001
 *         section 5), so any on-path box can read them; nothing secret is
 *         decrypted.
 *
 * With Encrypted Client Hello the visible name is the provider's public
 * name (ech = 1), not the site. Every read is bounds-checked: the input is
 * whatever a device sends.
 */
#ifndef AG_SNI_H
#define AG_SNI_H

#include <stddef.h>
#include <stdint.h>

#define AG_SNI_MAX   256
#define AG_HELLO_MAX 4096   /* ClientHello bytes kept; post-quantum key shares make it ~1.8 KB */

enum {
	AG_SNI_FOUND = 1,   /* name filled in */
	AG_SNI_NONE = 0,    /* a complete ClientHello without a name */
	AG_SNI_BAD = -1,    /* not TLS / not a client Initial, or malformed */
	AG_SNI_MORE = -2,   /* cut short: the name may be in the next packet */
};

struct ag_sni {
	char name[AG_SNI_MAX];   /* lower-case host name */
	char alpn[32];           /* first offered protocol: h2, http/1.1, h3, ... */
	int ech;                 /* Encrypted Client Hello offered: name is the outer one */
};

/* building blocks (exported for the unit test) */
void ag_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
void ag_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t len, uint8_t out[32]);
void ag_hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32]);
void ag_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t len);
void ag_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* 0 and the plaintext in out when the tag is right, -1 otherwise */
int ag_aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t alen,
		       const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *out);
/* the client's Initial keys for QUIC version 1 or 2; -1 for other versions */
int ag_quic_client_keys(uint32_t version, const uint8_t *dcid, size_t dlen,
			uint8_t key[16], uint8_t iv[12], uint8_t hp[16]);

/* TLS: a ClientHello handshake message from its first byte, possibly cut short */
int ag_tls_hello_sni(const uint8_t *hs, size_t len, struct ag_sni *out);
/* TLS: the start of a TCP stream (records) */
int ag_tls_stream_sni(const uint8_t *data, size_t len, struct ag_sni *out);

/* QUIC: the client's Initial packets of one connection, one UDP payload at a time */
struct ag_quic_hello {
	uint8_t have[AG_HELLO_MAX / 8];  /* which CRYPTO bytes arrived */
	uint8_t buf[AG_HELLO_MAX];
};
/* 1 when a client Initial was decrypted and its CRYPTO data kept, 0 when the
 * datagram holds none, -1 when malformed */
int ag_quic_feed(struct ag_quic_hello *q, const uint8_t *dgram, size_t len);
int ag_quic_sni(const struct ag_quic_hello *q, struct ag_sni *out);

#endif
