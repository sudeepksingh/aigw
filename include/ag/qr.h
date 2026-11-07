/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal QR code encoder (ISO/IEC 18004): byte mode, versions 1-10,
 * error correction L or M. Enough for Wi-Fi join codes
 * ("WIFI:T:WPA;S:<ssid>;P:<key>;;", at most 213 bytes at level M).
 */
#ifndef AG_QR_H
#define AG_QR_H

#include <stddef.h>
#include <stdint.h>

#define AG_QR_MAX_SIZE 57       /* version 10: 17 + 4 * 10 */

enum ag_qr_ecc { AG_QR_ECC_L, AG_QR_ECC_M };

struct ag_qr {
	int size;                   /* modules per side */
	int version;
	uint8_t m[AG_QR_MAX_SIZE][AG_QR_MAX_SIZE];   /* [y][x], 1 = dark */
};

/* Encodes len bytes; picks the smallest version that fits. 0 or -1 (too long). */
int ag_qr_encode(struct ag_qr *q, const uint8_t *data, size_t len, enum ag_qr_ecc ecc);

/* Wi-Fi join text, with ; , : \ " escaped. Returns length or -1 when it doesn't fit. */
int ag_qr_wifi_text(char *out, size_t n, const char *ssid, const char *passphrase);

#endif
