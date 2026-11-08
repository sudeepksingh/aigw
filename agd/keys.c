/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-device Wi-Fi keys and one-scan onboarding (phase 4).
 *
 *   agctl key add "Living room plug" iot
 *      │  a fresh passphrase, a keyid, a line in hostapd's wpa_psk_file,
 *      │  RELOAD_WPA_PSK, and a QR code: WIFI:T:WPA;S:<ssid>;P:<key>;;
 *      ▼
 *   the device joins with it ── hostapd reports keyid=<id> for the station
 *      │
 *      ▼
 *   bound: the key now works only for that device's MAC, the device is
 *   named and placed in the key's zone ("iot"), and the key can be revoked
 *   on its own (agctl key revoke) without changing anyone else's password.
 *
 * Unused keys expire (wifi.key_ttl, default a day). The main Wi-Fi password
 * keeps working; per-device keys are additional. The file is the record:
 * metadata lives in comment lines hostapd ignores, so nothing is lost when
 * agd restarts.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agd.h"
#include "ag/qr.h"

#define KEY_MAX     64
#define FOREIGN_MAX 32
#define ANY_MAC     "00:00:00:00:00:00"

struct wkey {
	int used;
	char id[16];
	char name[64];
	char zone[16];
	char pass[64];
	char mac[18];               /* "" until a device joins with it */
	int64_t created, expires, bound;
};

static struct wkey keys[KEY_MAX];
static char foreign[FOREIGN_MAX][160];  /* lines someone else wrote: kept as they are */
static int n_foreign;

static int name_ok(const char *s)
{
	size_t i, n = strlen(s);

	if (n < 1 || n > 63 || s[0] == ' ' || s[n - 1] == ' ')
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && !strchr(" -_.'()", s[i]))
			return 0;
	return 1;
}

static int zone_ok(const char *s)
{
	size_t i, n = strlen(s);

	if (n > 15)
		return 0;
	for (i = 0; i < n; i++)
		if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && s[i] != '-')
			return 0;
	return 1;
}

static struct wkey *by_name(const char *q)
{
	int i;

	for (i = 0; i < KEY_MAX; i++)
		if (keys[i].used && (!strcasecmp(keys[i].name, q) || !strcmp(keys[i].id, q)))
			return &keys[i];
	return NULL;
}

static struct wkey *by_id(const char *id)
{
	int i;

	for (i = 0; i < KEY_MAX; i++)
		if (keys[i].used && !strcmp(keys[i].id, id))
			return &keys[i];
	return NULL;
}

static int random_bytes(uint8_t *buf, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n);
	close(fd);
	return r == (ssize_t)n ? 0 : -1;
}

static int write_file(char *err, size_t n)
{
	char tmp[300], dir[256], *slash;
	FILE *f;
	int i, fd;

	snprintf(dir, sizeof(dir), "%s", cfg.wifi_keys);
	if ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.wifi_keys);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		if (fd >= 0)
			close(fd);
		snprintf(err, n, "%s: %s", tmp, strerror(errno));
		return -1;
	}
	fprintf(f, "# AIGW per-device Wi-Fi keys (hostapd wpa_psk_file), managed by agd.\n"
		   "# Add and remove keys with: agctl key add <name> / agctl key revoke <name>\n");
	for (i = 0; i < n_foreign; i++)
		fprintf(f, "%s\n", foreign[i]);
	for (i = 0; i < KEY_MAX; i++) {
		const struct wkey *k = &keys[i];

		if (!k->used)
			continue;
		fprintf(f, "# aigw %s %s %lld %lld %lld %s\n", k->id, k->zone[0] ? k->zone : "-", (long long)k->created,
			(long long)k->expires, (long long)k->bound, k->name);
		fprintf(f, "keyid=%s %s %s\n", k->id, k->mac[0] ? k->mac : ANY_MAC, k->pass);
	}
	if (fclose(f) != 0 || rename(tmp, cfg.wifi_keys) != 0) {
		snprintf(err, n, "%s: %s", cfg.wifi_keys, strerror(errno));
		return -1;
	}
	return 0;
}

/* hostapd re-reads the file; 0 when at least one access point did */
static int reload(void)
{
	char reply[64];

	return wifi_all("RELOAD_WPA_PSK", reply, sizeof(reply)) > 0 ? 0 : -1;
}

static void load_file(void)
{
	FILE *f = fopen(cfg.wifi_keys, "r");
	char line[512], meta[512] = "";
	int n = 0;

	n_foreign = 0;
	memset(keys, 0, sizeof(keys));
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char id[32], mac[32], pass[128];

		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "# aigw ", 7)) {
			snprintf(meta, sizeof(meta), "%s", line + 7);
			continue;
		}
		if (!strncmp(line, "# AIGW", 6) || !strncmp(line, "# Add and remove", 16))
			continue;
		if (sscanf(line, "keyid=%31s %31s %127s", id, mac, pass) == 3 && meta[0] && n < KEY_MAX) {
			struct wkey *k = &keys[n];
			char mid[32], zone[32];
			long long c = 0, e = 0, b = 0;
			int off = 0;

			if (sscanf(meta, "%31s %31s %lld %lld %lld %n", mid, zone, &c, &e, &b, &off) >= 5 &&
			    !strcmp(mid, id) && off > 0) {
				k->used = 1;
				snprintf(k->id, sizeof(k->id), "%.15s", id);
				snprintf(k->zone, sizeof(k->zone), "%.15s", strcmp(zone, "-") ? zone : "");
				snprintf(k->name, sizeof(k->name), "%.63s", meta + off);
				snprintf(k->pass, sizeof(k->pass), "%.63s", pass);
				if (strcmp(mac, ANY_MAC))
					snprintf(k->mac, sizeof(k->mac), "%.17s", mac);
				k->created = c;
				k->expires = e;
				k->bound = b;
				n++;
				meta[0] = '\0';
				continue;
			}
		}
		meta[0] = '\0';
		if (line[0] && n_foreign < FOREIGN_MAX)
			snprintf(foreign[n_foreign++], sizeof(foreign[0]), "%.159s", line);
	}
	fclose(f);
	AG_INFO("Wi-Fi keys: %d per-device keys in %s", n, cfg.wifi_keys);
}

void keys_init(void)
{
	char err[200];
	struct stat st;

	load_file();
	/* hostapd refuses to start when its wpa_psk_file is missing: make sure it exists */
	if (stat(cfg.wifi_keys, &st) != 0 && write_file(err, sizeof(err)) < 0)
		AG_WARN("Wi-Fi keys: %s", err);
}

static void ssid_of_ap(char *out, size_t n)
{
	char reply[4096], *p;

	out[0] = '\0';
	if (wifi_all("GET_CONFIG", reply, sizeof(reply)) <= 0)
		return;
	p = strstr(reply, "ssid=");
	while (p && p != reply && p[-1] != '\n')
		p = strstr(p + 1, "ssid=");      /* not "bssid=" */
	if (p)
		snprintf(out, n, "%.*s", (int)strcspn(p + 5, "\n"), p + 5);
}

int keys_add(const char *name, const char *zone, int ttl, struct blob_buf *reply, char *err, size_t errlen)
{
	static const char alphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	char ssid[64], qr[300];
	uint8_t rnd[24];
	struct wkey *k = NULL;
	int i, ok;

	if (!name_ok(name)) {
		snprintf(err, errlen, "name must be 1-63 letters, digits, spaces or -_.'()");
		return -1;
	}
	if (zone && !zone_ok(zone)) {
		snprintf(err, errlen, "zone must be up to 15 of a-z, 0-9 and -");
		return -1;
	}
	if (by_name(name)) {
		snprintf(err, errlen, "there is already a key called '%s'", name);
		return -1;
	}
	ssid_of_ap(ssid, sizeof(ssid));
	if (!ssid[0]) {
		snprintf(err, errlen, "no Wi-Fi access point answers (hostapd control sockets in %s)", cfg.wifi_ctrl);
		return -1;
	}
	for (i = 0; i < KEY_MAX && !k; i++)
		if (!keys[i].used)
			k = &keys[i];
	if (!k) {
		snprintf(err, errlen, "too many keys (%d); revoke some first", KEY_MAX);
		return -1;
	}
	if (random_bytes(rnd, sizeof(rnd))) {
		snprintf(err, errlen, "no randomness available");
		return -1;
	}
	memset(k, 0, sizeof(*k));
	k->used = 1;
	do {
		snprintf(k->id, sizeof(k->id), "k%02x%02x%02x", rnd[0], rnd[1], rnd[2]);
		rnd[0]++;
	} while (by_id(k->id) != k);
	/* 16 characters from 57 unambiguous ones: ~93 bits */
	for (i = 0; i < 16; i++)
		k->pass[i] = alphabet[rnd[3 + i] % (sizeof(alphabet) - 1)];
	k->pass[16] = '\0';
	snprintf(k->name, sizeof(k->name), "%s", name);
	snprintf(k->zone, sizeof(k->zone), "%s", zone ? zone : "");
	k->created = ag_wall_s();
	k->expires = k->created + (ttl > 0 ? ttl : cfg.key_ttl);
	if (write_file(err, errlen) < 0) {
		k->used = 0;
		return -1;
	}
	ok = reload() == 0;
	ag_qr_wifi_text(qr, sizeof(qr), ssid, k->pass);
	AG_INFO("Wi-Fi key '%s' (%s%s%s) created; waiting for a device to join", k->name, k->id,
		k->zone[0] ? ", zone " : "", k->zone);
	blobmsg_add_string(reply, "name", k->name);
	blobmsg_add_string(reply, "id", k->id);
	blobmsg_add_string(reply, "zone", k->zone);
	blobmsg_add_string(reply, "ssid", ssid);
	blobmsg_add_string(reply, "passphrase", k->pass);
	blobmsg_add_string(reply, "qr", qr);
	agd_qr_rows(reply, "qr_rows", qr);
	blobmsg_add_u64(reply, "expires", (uint64_t)k->expires);
	blobmsg_add_u8(reply, "reloaded", (uint8_t)ok);
	if (!ok)
		snprintf(err, errlen, "key saved, but no access point re-read it (is wpa_psk_file=%s in its config?)",
			 cfg.wifi_keys);
	return 0;
}

int keys_revoke(const char *name, char *err, size_t errlen)
{
	struct wkey *k = by_name(name);
	char mac[18];
	int i;

	if (!k) {
		snprintf(err, errlen, "no key '%s' (agctl keys)", name);
		return -1;
	}
	snprintf(mac, sizeof(mac), "%s", k->mac);
	k->used = 0;
	if (write_file(err, errlen) < 0) {
		k->used = 1;
		return -1;
	}
	reload();
	if (mac[0]) {
		wifi_kick(mac);              /* off now; it can't come back with that key */
		for (i = 0; i < DEV_MAX; i++)
			if (devs[i].used && !strcasecmp(devs[i].mac, mac))
				devs[i].wifi_key[0] = '\0';
	}
	AG_INFO("Wi-Fi key '%s' revoked%s%s", k->name, mac[0] ? ", disconnected " : "", mac);
	return 0;
}

void keys_seen(struct device *d, const char *keyid)
{
	struct wkey *k = by_id(keyid);
	char err[200];

	if (!k || !d->mac[0])
		return;
	if (!k->mac[0]) {
		struct blob_buf b = { 0 };

		/* first join: from now on this key works for this device only */
		snprintf(k->mac, sizeof(k->mac), "%s", d->mac);
		k->bound = ag_wall_s();
		k->expires = 0;
		if (write_file(err, sizeof(err)) < 0)
			AG_WARN("Wi-Fi keys: %s", err);
		else
			reload();
		if (!d->hostname[0])
			snprintf(d->hostname, sizeof(d->hostname), "%s", k->name);
		AG_INFO("Wi-Fi key '%s' is now bound to %s%s%s", k->name, d->mac, k->zone[0] ? ", zone " : "", k->zone);
		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "event", "joined-with-key");
		blobmsg_add_string(&b, "mac", d->mac);
		blobmsg_add_string(&b, "ip", d->ip);
		blobmsg_add_string(&b, "hostname", d->hostname);
		blobmsg_add_string(&b, "key", k->name);
		blobmsg_add_string(&b, "zone", k->zone);
		agd_emit(AG_EV_WIFI, &b);
		blob_buf_free(&b);
	} else if (strcasecmp(k->mac, d->mac)) {
		return;                      /* hostapd only lets the bound MAC use it */
	}
	snprintf(d->wifi_key, sizeof(d->wifi_key), "%.31s", k->name);
	if (k->zone[0])
		snprintf(d->zone, sizeof(d->zone), "%s", k->zone);
}

void keys_tick(void)
{
	int64_t now = ag_wall_s();
	char err[200];
	int i, changed = 0;

	for (i = 0; i < KEY_MAX; i++)
		if (keys[i].used && !keys[i].mac[0] && keys[i].expires && now >= keys[i].expires) {
			AG_INFO("Wi-Fi key '%s' expired unused", keys[i].name);
			keys[i].used = 0;
			changed = 1;
		}
	if (changed) {
		if (write_file(err, sizeof(err)) < 0)
			AG_WARN("Wi-Fi keys: %s", err);
		else
			reload();
	}
}

int keys_count(void)
{
	int i, n = 0;

	for (i = 0; i < KEY_MAX; i++)
		n += keys[i].used;
	return n;
}

void keys_blob(struct blob_buf *b, const char *only)
{
	void *arr = blobmsg_open_array(b, "keys"), *t;
	char ssid[64] = "", qr[300];
	int i;

	if (only)
		ssid_of_ap(ssid, sizeof(ssid));
	for (i = 0; i < KEY_MAX; i++) {
		const struct wkey *k = &keys[i];
		const struct device *d;

		if (!k->used || (only && strcasecmp(only, k->name) && strcmp(only, k->id)))
			continue;
		d = k->mac[0] ? dev_find_mac(k->mac, 0) : NULL;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", k->name);
		blobmsg_add_string(b, "id", k->id);
		blobmsg_add_string(b, "zone", k->zone);
		blobmsg_add_string(b, "state", k->mac[0] ? "in use" : "waiting for a device");
		blobmsg_add_string(b, "mac", k->mac);
		blobmsg_add_string(b, "device", d ? (d->hostname[0] ? d->hostname : d->ip) : "");
		blobmsg_add_u8(b, "connected", (uint8_t)(d && d->wifi_connected));
		blobmsg_add_u64(b, "created", (uint64_t)k->created);
		blobmsg_add_u64(b, "expires", (uint64_t)k->expires);
		blobmsg_add_u64(b, "bound", (uint64_t)k->bound);
		if (only) {                  /* one key asked for by name: enough to show its QR again */
			blobmsg_add_string(b, "ssid", ssid);
			blobmsg_add_string(b, "passphrase", k->pass);
			if (ssid[0] && ag_qr_wifi_text(qr, sizeof(qr), ssid, k->pass) > 0) {
				blobmsg_add_string(b, "qr", qr);
				agd_qr_rows(b, "qr_rows", qr);
			}
		}
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}
