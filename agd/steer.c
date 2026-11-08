/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Band steering: with a 2.4 GHz and a 5 GHz access point under one name
 * (two radios: a dual-band router, or two adapters on a PC), each device is
 * moved to the band that suits it.
 *
 *   up    on 2.4 GHz, can use 5 GHz, smoothed signal >= wifi.steer_up
 *         (-60 dBm) for wifi.steer_hold seconds: close enough for 5 GHz's
 *         speed
 *   down  on 5 GHz, signal <= wifi.steer_down (-75 dBm) for steer_hold:
 *         2.4 GHz reaches further
 *
 * How: a device that supports 802.11v gets a BSS Transition Management
 * request naming the other access point ("btm": it moves by itself, without
 * dropping). Others, when wifi.steer_kick allows, are refused for 30 s by
 * the access point they are on and disconnected, so they join the other
 * band ("kick"). 15 s later agd looks where the device is. Guardrails: never
 * protected devices (act.protect), never in its first 20 s on an access
 * point, wifi.steer_gap (300 s) between two moves of a device, at most
 * wifi.steer_rate (2) moves a minute in the home, and a device that missed three moves in a row
 * is left alone for a day.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "agd.h"

#define CHECK_S      15
#define KICK_S       30
#define SETTLE_S     20
#define MISSES_MAX   3
#define PAUSE_S      86400

uint64_t steer_total, steer_ok_total;
static int64_t minute_start;
static int minute_moves;

/* the access points to steer between: same name, one per band */
int steer_ready(const struct wifi_ap **a2, const struct wifi_ap **a5)
{
	int i, j;

	for (i = 0; i < wifi_n_aps; i++) {
		if (wifi_aps[i].band != 2 || !wifi_aps[i].bssid[0])
			continue;
		for (j = 0; j < wifi_n_aps; j++)
			if (wifi_aps[j].band == 5 && wifi_aps[j].bssid[0] &&
			    !strcmp(wifi_aps[i].ssid, wifi_aps[j].ssid)) {
				*a2 = &wifi_aps[i];
				*a5 = &wifi_aps[j];
				return 1;
			}
	}
	return 0;
}

/* IEEE 802.11 global operating class of a channel */
static int op_class(int band, int ch)
{
	if (band == 2)
		return 81;
	if (ch >= 36 && ch <= 48)
		return 115;
	if (ch >= 52 && ch <= 64)
		return 118;
	if (ch >= 100 && ch <= 144)
		return 121;
	if (ch >= 149 && ch <= 161)
		return 124;
	return 125;
}

static void emit(const char *what, const struct device *d, const struct wifi_ap *from,
		 const struct wifi_ap *to, const char *method, const char *why)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "mac", d->mac);
	blobmsg_add_string(&b, "ip", d->ip);
	blobmsg_add_string(&b, "hostname", d->hostname);
	if (from)
		blobmsg_add_string(&b, "from", from->band == 5 ? "5" : "2.4");
	if (to)
		blobmsg_add_string(&b, "to", to->band == 5 ? "5" : "2.4");
	if (method)
		blobmsg_add_string(&b, "method", method);
	if (why)
		blobmsg_add_string(&b, "reason", why);
	agd_emit(AG_EV_WIFI, &b);
	blob_buf_free(&b);
}

static int ok_reply(const char *r)
{
	return !strncmp(r, "OK", 2);
}

/* Ask d to move from access point `from` to `to`. 0 when asked. */
static int move(struct device *d, const struct wifi_ap *from, const struct wifi_ap *to, const char *why)
{
	char cmd[256], reply[512];
	int64_t now = ag_wall_s();

	if (d->wifi_btm) {
		/* neighbor=<bssid>,<bssid info>,<op class>,<channel>,<phy type: 7 HT, 9 VHT> */
		snprintf(cmd, sizeof(cmd), "BSS_TM_REQ %s pref=1 abridged=1 valid_int=255 neighbor=%s,0x0000000f,%d,%d,%d",
			 d->mac, to->bssid, op_class(to->band, to->channel), to->channel, to->band == 5 ? 9 : 7);
		if (wifi_cmd(from->iface, cmd, reply, sizeof(reply)) <= 0 || !ok_reply(reply)) {
			AG_DEBUG("steer %s: BSS_TM_REQ refused by %s", d->mac, from->iface);
			return -1;
		}
		snprintf(d->steer_method, sizeof(d->steer_method), "btm");
	} else if (cfg.steer_kick) {
		/* already refused somewhere (a "disconnect" action): not ours to touch */
		if (wifi_cmd(from->iface, "DENY_ACL SHOW", reply, sizeof(reply)) >= 0 && strcasestr(reply, d->mac))
			return -1;
		snprintf(cmd, sizeof(cmd), "DENY_ACL ADD_MAC %s", d->mac);
		if (wifi_cmd(from->iface, cmd, reply, sizeof(reply)) <= 0 || !ok_reply(reply))
			return -1;
		snprintf(cmd, sizeof(cmd), "DEAUTHENTICATE %s", d->mac);
		wifi_cmd(from->iface, cmd, reply, sizeof(reply));
		snprintf(d->steer_method, sizeof(d->steer_method), "kick");
		snprintf(d->steer_unban, sizeof(d->steer_unban), "%s", from->iface);
		d->steer_unban_at = now + KICK_S;
	} else {
		return -1;
	}
	d->steer_last = now;
	d->steer_check_at = now + CHECK_S;
	d->steer_from_band = from->band;
	d->steer_cond_since = 0;
	steer_total++;
	minute_moves++;
	AG_INFO("steer %s (%s) from %s GHz to %s GHz by %s: %s", d->hostname[0] ? d->hostname : d->mac, d->mac,
		from->band == 5 ? "5" : "2.4", to->band == 5 ? "5" : "2.4", d->steer_method, why);
	emit("steer", d, from, to, d->steer_method, why);
	return 0;
}

/* After a move: lift a kick's refusal, and see whether the device moved. */
static void follow_up(struct device *d, int64_t now)
{
	char cmd[64], reply[256];

	if (d->steer_unban[0] && now >= d->steer_unban_at) {
		snprintf(cmd, sizeof(cmd), "DENY_ACL DEL_MAC %s", d->mac);
		wifi_cmd(d->steer_unban, cmd, reply, sizeof(reply));
		d->steer_unban[0] = '\0';
	}
	if (!d->steer_check_at || now < d->steer_check_at)
		return;
	d->steer_check_at = 0;
	if (d->wifi_connected && d->wifi_band && d->wifi_band != d->steer_from_band) {
		d->steer_moves++;
		d->steer_fails = 0;
		steer_ok_total++;
		emit("steered", d, NULL, NULL, d->steer_method, NULL);
		return;
	}
	d->steer_misses++;
	if (++d->steer_fails >= MISSES_MAX) {
		d->steer_pause_until = now + PAUSE_S;
		d->steer_fails = 0;
		AG_INFO("steer %s: stayed put %d times; leaving it on its band for a day", d->mac, MISSES_MAX);
	}
	emit("steer-missed", d, NULL, NULL, d->steer_method, NULL);
}

void steer_tick(void)
{
	const struct wifi_ap *a2, *a5;
	int64_t now = ag_wall_s();
	int i;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && (devs[i].steer_check_at || devs[i].steer_unban[0]))
			follow_up(&devs[i], now);
	if (!cfg.steer || !steer_ready(&a2, &a5))
		return;
	if (now - minute_start >= 60) {
		minute_start = now;
		minute_moves = 0;
	}
	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];
		const struct wifi_ap *from = NULL, *to = NULL;
		const char *why = NULL;
		char reason[96];
		int up, down;

		if (!d->used || !d->wifi_connected || !d->wifi_band || d->steer_check_at)
			continue;
		up = d->wifi_band == 2 && d->wifi_dual && d->wifi_sig_avg >= cfg.steer_up && !strcmp(d->wifi_iface, a2->iface);
		down = d->wifi_band == 5 && d->wifi_sig_avg && d->wifi_sig_avg <= cfg.steer_down &&
		       !strcmp(d->wifi_iface, a5->iface);
		if (!up && !down) {
			d->steer_cond_since = 0;
			continue;
		}
		if (!d->steer_cond_since)
			d->steer_cond_since = now;
		if (now - d->steer_cond_since < cfg.steer_hold || now - d->wifi_since < SETTLE_S ||
		    now - d->steer_last < cfg.steer_gap || now < d->steer_pause_until ||
		    minute_moves >= cfg.steer_rate ||(!d->wifi_btm && !cfg.steer_kick) || act_is_protected(d))
			continue;
		if (up) {
			from = a2;
			to = a5;
			snprintf(reason, sizeof(reason), "strong signal (%.0f dBm): 5 GHz is faster", d->wifi_sig_avg);
		} else {
			from = a5;
			to = a2;
			snprintf(reason, sizeof(reason), "weak signal (%.0f dBm): 2.4 GHz reaches further", d->wifi_sig_avg);
		}
		why = reason;
		move(d, from, to, why);
	}
}

void steer_blob(struct blob_buf *b)
{
	const struct wifi_ap *a2, *a5;
	void *t = blobmsg_open_table(b, "steering");
	int ready = steer_ready(&a2, &a5);

	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.steer);
	blobmsg_add_u8(b, "dual_band", (uint8_t)ready);
	blobmsg_add_u32(b, "up_dbm", (uint32_t)cfg.steer_up);
	blobmsg_add_u32(b, "down_dbm", (uint32_t)cfg.steer_down);
	blobmsg_add_u64(b, "moves_asked", steer_total);
	blobmsg_add_u64(b, "moves_done", steer_ok_total);
	blobmsg_close_table(b, t);
}
