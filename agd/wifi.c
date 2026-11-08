/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wi-Fi sensor: connected stations with signal and link rate, read from
 * hostapd's control sockets (one per access point interface, in
 * cfg.wifi_ctrl). Works the same on OpenWRT and on a PC acting as AP.
 * Joins and leaves are reported as "ag.wifi" events.
 *
 * Also the Wi-Fi actuator (wifi_ban): hostapd's deny list plus a deauth.
 *
 * Each access point's band and channel come from hostapd's STATUS, and each
 * station's abilities from its STA entry: 802.11v BSS transition (extended
 * capabilities bit 19) and 5 GHz (operating classes 115+ it lists, VHT, or
 * having been seen there). A device found on another access point than
 * before has "roamed". Band steering (steer.c) builds on this.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "agd.h"

static int sock = -1;
static char local_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static unsigned poll_seq;
static int stations;

struct wifi_ap wifi_aps[WIFI_AP_MAX];
int wifi_n_aps;

static void wifi_cleanup(void)
{
	if (local_path[0])
		unlink(local_path);
}

void wifi_init(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	struct timeval tv = { 0, 300000 };

	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0)
		return;
	snprintf(local_path, sizeof(local_path), "/tmp/agd-wifi-%d", (int)getpid());
	unlink(local_path);
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", local_path);
	if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(sock);
		sock = -1;
		return;
	}
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	atexit(wifi_cleanup);
}

/* One request/response to hostapd. Returns reply length or -1. */
static int hapd_cmd(const char *path, const char *cmd, char *reply, size_t n)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	ssize_t r;

	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (sendto(sock, cmd, strlen(cmd), 0, (struct sockaddr *)&a, sizeof(a)) < 0)
		return -1;
	r = recv(sock, reply, n - 1, 0);
	if (r < 0)
		return -1;
	reply[r] = '\0';
	return (int)r;
}

int wifi_cmd(const char *iface, const char *cmd, char *reply, size_t n)
{
	char path[512];

	if (sock < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/%s", cfg.wifi_ctrl, iface);
	return hapd_cmd(path, cmd, reply, n);
}

const struct wifi_ap *wifi_ap_find(const char *iface)
{
	int i;

	for (i = 0; i < wifi_n_aps; i++)
		if (!strcmp(wifi_aps[i].iface, iface))
			return &wifi_aps[i];
	return NULL;
}

static long kv(const char *text, const char *key, long dflt)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\n%s=", key);
	p = strstr(text, pat);
	return p ? strtol(p + strlen(pat), NULL, 10) : dflt;
}

/* a "key=value" string out of a hostapd reply ("" when absent) */
static void kvs(const char *text, const char *key, char *out, size_t n)
{
	char pat[48];
	const char *p;
	size_t i = 0;

	snprintf(pat, sizeof(pat), "\n%s=", key);
	out[0] = '\0';
	p = strstr(text, pat);
	if (!p && !strncmp(text, pat + 1, strlen(pat) - 1))
		p = text - 1;                   /* the very first line */
	if (!p)
		return;
	p += strlen(pat);
	while (p[i] && p[i] != '\n' && i + 1 < n) {
		out[i] = p[i];
		i++;
	}
	out[i] = '\0';
}

static int hexbyte(const char *h, size_t i)
{
	unsigned v;

	if (strlen(h) < 2 * i + 2 || sscanf(h + 2 * i, "%2x", &v) != 1)
		return -1;
	return (int)v;
}

/* What a station's STA entry says it can do. */
static void station_abilities(struct device *d, const char *reply)
{
	char h[256], flags[256];
	int b, i;

	kvs(reply, "ext_capab", h, sizeof(h));
	b = hexbyte(h, 2);                      /* extended capabilities bit 19: BSS transition */
	if (b >= 0)
		d->wifi_btm = (b & 0x08) != 0;
	kvs(reply, "supp_op_classes", h, sizeof(h));
	for (i = 1; (b = hexbyte(h, (size_t)i)) >= 0; i++)
		if (b >= 115 && b <= 130)           /* 5 GHz operating classes */
			d->wifi_dual = 1;
	kvs(reply, "flags", flags, sizeof(flags));
	if (strstr(flags, "[VHT]"))             /* 802.11ac is 5 GHz only */
		d->wifi_dual = 1;
	if (d->wifi_band == 5)
		d->wifi_dual = 1;
}

/* This access point's band, channel and name, from hostapd's STATUS. */
static void ap_status(const char *iface)
{
	char reply[4096], v[64];
	struct wifi_ap *a;

	if (wifi_n_aps >= WIFI_AP_MAX)
		return;
	a = &wifi_aps[wifi_n_aps];
	memset(a, 0, sizeof(*a));
	snprintf(a->iface, sizeof(a->iface), "%.23s", iface);
	wifi_n_aps++;
	if (wifi_cmd(iface, "STATUS", reply, sizeof(reply)) <= 0 || !strncmp(reply, "UNKNOWN", 7))
		return;                             /* band unknown: never steered */
	a->freq = (int)kv(reply, "freq", 0);
	a->channel = (int)kv(reply, "channel", 0);
	a->band = a->freq >= 2400 && a->freq < 2500 ? 2 : a->freq >= 5000 && a->freq < 5900 ? 5 : 0;
	a->width = kv(reply, "vht_oper_chwidth", 0) >= 1 ? 80 : kv(reply, "secondary_channel", 0) ? 40 : 20;
	kvs(reply, "bssid[0]", v, sizeof(v));
	snprintf(a->bssid, sizeof(a->bssid), "%.17s", v);
	kvs(reply, "ssid[0]", a->ssid, sizeof(a->ssid));
}

static void emit(const char *what, const struct device *d)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "mac", d->mac);
	blobmsg_add_string(&b, "ip", d->ip);
	blobmsg_add_string(&b, "hostname", d->hostname);
	blobmsg_add_string(&b, "iface", d->wifi_iface);
	if (d->wifi_band)
		blobmsg_add_string(&b, "band", d->wifi_band == 5 ? "5" : "2.4");
	if (!strcmp(what, "connected") || !strcmp(what, "roamed"))
		blobmsg_add_u32(&b, "signal", (uint32_t)d->wifi_signal);
	agd_emit(AG_EV_WIFI, &b);
	blob_buf_free(&b);
}

static void poll_iface(const char *dir, const char *iface)
{
	char path[512], cmd[64], reply[4096];
	const struct wifi_ap *ap;
	int guard = 0, band;

	snprintf(path, sizeof(path), "%s/%s", dir, iface);
	ap_status(iface);
	ap = wifi_ap_find(iface);
	band = ap ? ap->band : 0;
	if (hapd_cmd(path, "STA-FIRST", reply, sizeof(reply)) <= 0)
		return;
	while (guard++ < 256) {
		char mac[18];
		struct device *d;
		long rate;

		/* reply: "<mac>\nkey=value\n..." ; empty or FAIL at the end */
		if (sscanf(reply, "%17[0-9a-fA-F:]", mac) != 1 || strlen(mac) != 17)
			break;
		d = dev_find_mac(mac, 1);
		if (d->wifi_connected && d->wifi_seen == poll_seq && strcmp(d->wifi_iface, iface) &&
		    kv(reply, "connected_time", 0) >= ag_wall_s() - d->wifi_since) {
			/* listed by two access points for a moment while it moves: the newer join counts */
			goto next;
		}
		if (!d->wifi_connected) {
			d->wifi_connected = 1;
			d->wifi_since = ag_wall_s() - kv(reply, "connected_time", 0);
			snprintf(d->wifi_iface, sizeof(d->wifi_iface), "%.23s", iface);
			d->wifi_band = band;
			d->wifi_signal = (int)kv(reply, "signal", 0);
			d->wifi_sig_avg = d->wifi_signal;
			emit("connected", d);
		} else if (strcmp(d->wifi_iface, iface)) {
			/* on another access point now (another band, or a mesh node) */
			snprintf(d->wifi_iface, sizeof(d->wifi_iface), "%.23s", iface);
			d->wifi_band = band;
			d->wifi_since = ag_wall_s() - kv(reply, "connected_time", 0);
			d->wifi_signal = (int)kv(reply, "signal", d->wifi_signal);
			d->wifi_sig_avg = d->wifi_signal;
			d->steer_cond_since = 0;
			emit("roamed", d);
		}
		d->wifi_signal = (int)kv(reply, "signal", d->wifi_signal);
		if (d->wifi_signal)
			d->wifi_sig_avg = d->wifi_sig_avg ? 0.7 * d->wifi_sig_avg + 0.3 * d->wifi_signal : d->wifi_signal;
		station_abilities(d, reply);
		if (ap)
			((struct wifi_ap *)ap)->stations++;
		/* tx_rate_info is in units of 100 kbit/s */
		rate = kv(reply, "tx_rate_info", -1);
		if (rate >= 0)
			d->wifi_rate_kbps = (uint32_t)rate * 100;
		d->wifi_seen = poll_seq;
		d->last_seen = ag_wall_s();
		stations++;
		{
			/* joined with its own key (wpa_psk_file keyid): see keys.c */
			const char *k = strstr(reply, "\nkeyid=");
			char kid[32];

			if (k && sscanf(k + 7, "%31[^\n]", kid) == 1)
				keys_seen(d, kid);
		}
next:
		snprintf(cmd, sizeof(cmd), "STA-NEXT %s", mac);
		if (hapd_cmd(path, cmd, reply, sizeof(reply)) <= 0)
			break;
	}
}

void wifi_poll(void)
{
	DIR *dir;
	struct dirent *e;
	int i;

	if (sock < 0)
		return;
	dir = opendir(cfg.wifi_ctrl);
	if (!dir)
		return; /* no access point here (yet) */
	poll_seq++;
	stations = 0;
	wifi_n_aps = 0;
	while ((e = readdir(dir))) {
		char p[512];
		struct stat st;

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "global"))
			continue;
		snprintf(p, sizeof(p), "%s/%s", cfg.wifi_ctrl, e->d_name);
		if (stat(p, &st) == 0 && S_ISSOCK(st.st_mode))
			poll_iface(cfg.wifi_ctrl, e->d_name);
	}
	closedir(dir);

	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];

		if (d->used && d->wifi_connected && d->wifi_seen != poll_seq) {
			emit("disconnected", d);
			d->wifi_connected = 0;
			d->wifi_signal = 0;
			d->wifi_sig_avg = 0;
			d->wifi_rate_kbps = 0;
			d->steer_cond_since = 0;
		}
	}
	steer_tick();
	chan_tick();
}

int wifi_station_count(void)
{
	return stations;
}

/* Sends a command to every access point; returns how many answered (not FAIL). */
int wifi_all(const char *cmd, char *reply, size_t n)
{
	char path[512], buf[4096];
	DIR *dir;
	struct dirent *e;
	int ok = 0;

	if (reply && n)
		reply[0] = '\0';
	if (sock < 0 || !(dir = opendir(cfg.wifi_ctrl)))
		return 0;
	while ((e = readdir(dir))) {
		struct stat st;

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "global"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", cfg.wifi_ctrl, e->d_name);
		if (stat(path, &st) || !S_ISSOCK(st.st_mode))
			continue;
		if (hapd_cmd(path, cmd, buf, sizeof(buf)) <= 0 || !strncmp(buf, "FAIL", 4) ||
		    !strncmp(buf, "UNKNOWN COMMAND", 15))
			continue;
		if (!ok && reply && n)
			snprintf(reply, n, "%s", buf);
		ok++;
	}
	closedir(dir);
	return ok;
}

int wifi_kick(const char *mac)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "DEAUTHENTICATE %s", mac);
	return wifi_all(cmd, NULL, 0);
}

/*
 * Wi-Fi actuator: on = refuse the MAC on every access point (hostapd deny
 * list) and kick it off; off = let it join again. Returns 0 when at least
 * one access point did it.
 */
int wifi_ban(const char *mac, int on, char *err, size_t errlen)
{
	char lmac[18], path[512], cmd[64], reply[4096];
	DIR *dir;
	struct dirent *e;
	int i, done = 0, aps = 0;

	for (i = 0; i < 17 && mac[i]; i++)
		lmac[i] = (char)tolower((unsigned char)mac[i]);
	lmac[i] = '\0';
	if (sock < 0 || !(dir = opendir(cfg.wifi_ctrl))) {
		snprintf(err, errlen, "no Wi-Fi access point here (hostapd control sockets in %s)", cfg.wifi_ctrl);
		return -1;
	}
	while ((e = readdir(dir))) {
		struct stat st;

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "global"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", cfg.wifi_ctrl, e->d_name);
		if (stat(path, &st) || !S_ISSOCK(st.st_mode))
			continue;
		aps++;
		if (on) {
			/* already refused (e.g. re-applied after a minute): leave it be */
			if (hapd_cmd(path, "DENY_ACL SHOW", reply, sizeof(reply)) >= 0 && strcasestr(reply, lmac)) {
				done++;
				continue;
			}
			snprintf(cmd, sizeof(cmd), "DENY_ACL ADD_MAC %s", lmac);
			if (hapd_cmd(path, cmd, reply, sizeof(reply)) <= 0 || strncmp(reply, "OK", 2))
				continue;
			snprintf(cmd, sizeof(cmd), "DEAUTHENTICATE %s", lmac);
			hapd_cmd(path, cmd, reply, sizeof(reply));
			done++;
		} else {
			snprintf(cmd, sizeof(cmd), "DENY_ACL DEL_MAC %s", lmac);
			if (hapd_cmd(path, cmd, reply, sizeof(reply)) > 0 && !strncmp(reply, "OK", 2))
				done++;
		}
	}
	closedir(dir);
	if (done)
		return 0;
	snprintf(err, errlen, aps ? "hostapd refused the request" :
		 "no Wi-Fi access point here (hostapd control sockets in %s)", cfg.wifi_ctrl);
	return -1;
}
