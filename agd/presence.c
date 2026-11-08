/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Presence: who is home. A device is present while it is on Wi-Fi, or was
 * seen (DHCP, ARP, traffic) in the last wifi.presence_away seconds (600).
 * Phones drop Wi-Fi to save power, so leaving takes that long; arriving is
 * immediate. Changes are "ag.presence" events (arrived / left, with how long
 * the device was away or home), for plug-ins such as Home Assistant.
 *
 * How near the device is to the access point comes from its smoothed
 * signal: near (>= -55 dBm), mid, far (< -70 dBm). A rough distance, not a
 * room: room-level presence and motion from signal patterns come later.
 */
#include <stdio.h>
#include <string.h>

#include "agd.h"

static int warmed;              /* the first look sets the state without events */

const char *presence_near(const struct device *d)
{
	if (!d->wifi_connected || !d->wifi_sig_avg)
		return "";
	return d->wifi_sig_avg >= -55 ? "near" : d->wifi_sig_avg >= -70 ? "mid" : "far";
}

static void emit(const char *what, const struct device *d, int64_t for_s)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "mac", d->mac);
	blobmsg_add_string(&b, "ip", d->ip);
	blobmsg_add_string(&b, "hostname", d->hostname);
	blobmsg_add_string(&b, "type", d->type);
	if (for_s > 0)
		blobmsg_add_u32(&b, !strcmp(what, "arrived") ? "away_s" : "home_s", (uint32_t)for_s);
	agd_emit(AG_EV_PRESENCE, &b);
	blob_buf_free(&b);
}

void presence_tick(void)
{
	int64_t now = ag_wall_s();
	int i;

	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];
		int here;

		if (!d->used || !d->mac[0])
			continue;
		here = d->wifi_connected || (d->last_seen && now - d->last_seen < cfg.presence_away);
		if (here == d->present)
			continue;
		if (warmed)
			emit(here ? "arrived" : "left", d, d->present_since ? now - d->present_since : 0);
		d->present = (uint8_t)here;
		d->present_since = now;
	}
	warmed = 1;
}

int presence_count(void)
{
	int i, n = 0;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].present)
			n++;
	return n;
}
