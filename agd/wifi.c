/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wi-Fi sensor: connected stations with signal and link rate, read from
 * hostapd's control sockets (one per access point interface, in
 * cfg.wifi_ctrl). Works the same on OpenWRT and on a PC acting as AP.
 * Joins and leaves are reported as "ag.wifi" events.
 */
#define _GNU_SOURCE
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

static long kv(const char *text, const char *key, long dflt)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\n%s=", key);
	p = strstr(text, pat);
	return p ? strtol(p + strlen(pat), NULL, 10) : dflt;
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
	if (!strcmp(what, "connected"))
		blobmsg_add_u32(&b, "signal", (uint32_t)d->wifi_signal);
	agd_emit(AG_EV_WIFI, &b);
	blob_buf_free(&b);
}

static void poll_iface(const char *dir, const char *iface)
{
	char path[512], cmd[64], reply[4096];
	int guard = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, iface);
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
		if (!d->wifi_connected) {
			d->wifi_connected = 1;
			d->wifi_since = ag_wall_s() - kv(reply, "connected_time", 0);
			snprintf(d->wifi_iface, sizeof(d->wifi_iface), "%.23s", iface);
			d->wifi_signal = (int)kv(reply, "signal", 0);
			emit("connected", d);
		}
		d->wifi_signal = (int)kv(reply, "signal", d->wifi_signal);
		/* tx_rate_info is in units of 100 kbit/s */
		rate = kv(reply, "tx_rate_info", -1);
		if (rate >= 0)
			d->wifi_rate_kbps = (uint32_t)rate * 100;
		d->wifi_seen = poll_seq;
		d->last_seen = ag_wall_s();
		stations++;

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
			d->wifi_rate_kbps = 0;
		}
	}
}

int wifi_station_count(void)
{
	return stations;
}
