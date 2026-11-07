/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Signatures: SHA-512 and Ed25519 verification, with OpenWRT's usign file
 * format, so a model (or anything else) signed with `usign -S` or with
 * AIGW's cortex is checked the same way on every gateway.
 *
 *   public key file   untrusted comment: <text>
 *                     base64("Ed" | fingerprint[8] | public key[32])
 *   signature file    untrusted comment: <text>
 *                     base64("Ed" | fingerprint[8] | signature[64])
 *
 * Verification only: gateways never hold a signing key.
 */
#ifndef AG_SIGN_H
#define AG_SIGN_H

#include <stddef.h>
#include <stdint.h>

void ag_sha512(const uint8_t *msg, size_t len, uint8_t out[64]);

/* 0 when sig is pk's Ed25519 signature of msg (RFC 8032), -1 otherwise */
int ag_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]);

/* Parse usign files (text as read from disk). 0 on success. */
int ag_usign_pubkey(const char *text, uint8_t fingerprint[8], uint8_t pk[32]);
int ag_usign_sig(const char *text, uint8_t fingerprint[8], uint8_t sig[64]);

/* fingerprint as 16 lowercase hex digits (usign -F prints the same) */
void ag_fingerprint_hex(const uint8_t fingerprint[8], char out[17]);

#endif
