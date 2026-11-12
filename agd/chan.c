/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Channel self-healing: is each access point's channel crowded, and would
 * another one be quieter?
 *
 *   measure  every wifi.chan_poll seconds (30), the radio's survey of the
 *            channel in use (`iw dev <ap> survey dump`): active, busy and
 *            transmit time. Air used by others = (busy - our transmit) /
 *            active, smoothed.
 *   decide   busier than wifi.chan_busy (60 %) for wifi.chan_hold (30 min):
 *            scan for neighbouring networks (`iw dev <ap> scan ap-force`; at
 *            most every wifi.chan_scan_gap, 6 h: the radio leaves its channel
 *            for a moment) and score each channel the band allows here by
 *            the networks on it or overlapping it, stronger ones counting
 *            more. A channel scoring at most half of the current one raises
 *            alert "wifi-busy"; the wifi_busy policy turns it into a
 *            "channel" decision (act.c).
 *   switch   hostapd CHAN_SWITCH: a channel switch announcement, so clients
 *            follow without dropping. Where the radio can't (no CSA), the PC
 *            restarts that access point on the new channel (aigw-ap channel).
 *            The decision's undo, or its time limit, switches back.
 *
 * `iw` is $AIGW_IW when set (the tests use a stand-in).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agd.h"

#define SCAN_MAX 128

struct chan_state {
	char iface[24];
	int works;                      /* the radio reports channel time */
	uint64_t active, busy, tx;      /* last survey, ms */
	double util;                    /* smoothed % of air used by others; -1 = not yet */
	int64_t busy_since;
	int64_t last_scan, last_sample;
	char seen[200];                 /* last scan, for people: "1: 2 networks, 6: 9, 11: 4" */
	int suggest;                    /* quieter channel found by the last scan, 0 = none */
};

static struct chan_state cs[WIFI_AP_MAX];

static const char *iw(void)
{
	const char *p = getenv("AIGW_IW");

	return p && *p ? p : "iw";
}

static struct chan_state *state_for(const char *iface, int create)
{
	int i;

	for (i = 0; i < WIFI_AP_MAX; i++)
		if (cs[i].iface[0] && !strcmp(cs[i].iface, iface))
			return &cs[i];
	if (!create)
		return NULL;
	for (i = 0; i < WIFI_AP_MAX; i++)
		if (!cs[i].iface[0]) {
			memset(&cs[i], 0, sizeof(cs[i]));
			snprintf(cs[i].iface, sizeof(cs[i].iface), "%.23s", iface);
			cs[i].util = -1;
			return &cs[i];
		}
	return NULL;
}

static int safe_iface(const char *s)
{
	size_t i;

	if (!*s || strlen(s) > 23)
		return 0;
	for (i = 0; s[i]; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_' && s[i] != '.')
			return 0;
	return 1;
}

static int ch_freq(int band, int ch)
{
	if (band == 2)
		return ch == 14 ? 2484 : 2407 + 5 * ch;
	return 5000 + 5 * ch;
}

static int freq_ch(int f)
{
	if (f == 2484)
		return 14;
	if (f >= 2412 && f < 2484)
		return (f - 2407) / 5;
	if (f >= 5000 && f < 5900)
		return (f - 5000) / 5;
	return 0;
}

/* channels a band offers for a width, before the country is asked */
static const int *candidates(int band, int width, int *n)
{
	static const int c24[] = { 1, 6, 11 };
	static const int c5_20[] = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
	static const int c5_40[] = { 36, 44, 149, 157 };
	static const int c5_80[] = { 36, 149 };

	if (band == 2) {
		*n = 3;
		return c24;
	}
	if (width >= 80) {
		*n = 2;
		return c5_80;
	}
	if (width == 40) {
		*n = 4;
		return c5_40;
	}
	*n = 9;
	return c5_20;
}

int chan_valid(int band, int ch)
{
	int n, i;
	const int *c = candidates(band, 20, &n);

	if (band == 2)
		return ch >= 1 && ch <= 13;
	for (i = 0; i < n; i++)
		if (c[i] == ch)
			return 1;
	return 0;
}

/* The 5 GHz block (80 MHz) or pair (40 MHz) a channel belongs to */
static int block_of(int ch, int width)
{
	int base = ch >= 149 ? 149 : 36, span = width >= 80 ? 16 : width == 40 ? 8 : 4;

	return base * 100 + (ch - base) / span;
}

/* ---------- survey ---------- */

/* The channel in use: active, busy and transmit time (ms). 0 when found. */
static int survey(const char *iface, uint64_t *active, uint64_t *busy, uint64_t *tx)
{
	char cmd[128], line[256];
	FILE *p;
	int inuse = 0, found = 0;

	if (!safe_iface(iface))
		return -1;
	snprintf(cmd, sizeof(cmd), "%s dev %s survey dump 2>/dev/null", iw(), iface);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	*active = *busy = *tx = 0;
	while (fgets(line, sizeof(line), p)) {
		unsigned long long v;
		char *c;

		if (strstr(line, "Survey data from")) {
			if (inuse)
				break;              /* the block in use is over */
			continue;
		}
		if (strstr(line, "frequency:")) {
			inuse = strstr(line, "[in use]") != NULL;
			continue;
		}
		if (!inuse || !(c = strchr(line, ':')) || sscanf(c + 1, "%llu", &v) != 1)
			continue;
		if (strstr(line, "channel active time"))
			*active = v, found = 1;
		else if (strstr(line, "channel busy time"))
			*busy = v;
		else if (strstr(line, "channel transmit time"))
			*tx = v;
	}
	pclose(p);
	return found ? 0 : -1;
}

/* ---------- scan ---------- */

struct bss {
	int freq;
	int signal;
};

static int our_bssid(const char *b)
{
	int i;

	for (i = 0; i < wifi_n_aps; i++)
		if (!strcasecmp(wifi_aps[i].bssid, b))
			return 1;
	return 0;
}

static int scan(const char *iface, struct bss *out, int max)
{
	char cmd[128], line[256], bssid[18] = "";
	FILE *p;
	int n = 0, cur = -1;

	if (!safe_iface(iface))
		return -1;
	snprintf(cmd, sizeof(cmd), "%s dev %s scan ap-force 2>/dev/null", iw(), iface);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		double f, s;

		if (!strncmp(line, "BSS ", 4)) {
			cur = -1;
			if (sscanf(line + 4, "%17[0-9a-fA-F:]", bssid) == 1 && !our_bssid(bssid) && n < max) {
				cur = n++;
				out[cur].freq = 0;
				out[cur].signal = -100;
			}
		} else if (cur >= 0 && sscanf(line, " freq: %lf", &f) == 1) {
			out[cur].freq = (int)f;
		} else if (cur >= 0 && sscanf(line, " signal: %lf", &s) == 1) {
			out[cur].signal = (int)s;
		}
	}
	if (pclose(p) != 0 && n == 0)
		return -1;
	return n;
}

/* Whether this radio may run an access point on channel ch here
 * (`iw phy`: not disabled, no "no IR", no radar detection). */
static int allowed_here(const char *iface, int band, int ch);

/* for Self-Healing Wi-Fi's trials: the channels AIGW would use on this access point */
int chan_options(const struct wifi_ap *a, int *out, int max)
{
	int n, i, k = 0;
	const int *c = candidates(a->band, a->width ? a->width : 20, &n);

	for (i = 0; i < n && k < max; i++)
		if (c[i] != a->channel && allowed_here(a->iface, a->band, c[i]))
			out[k++] = c[i];
	return k;
}

static int allowed_here(const char *iface, int band, int ch)
{
	char cmd[160], line[256], want[24];
	FILE *p;
	int phy = -1, ok = 0;

	snprintf(cmd, sizeof(cmd), "%s dev %s info 2>/dev/null", iw(), iface);
	if ((p = popen(cmd, "r"))) {
		while (fgets(line, sizeof(line), p))
			if (sscanf(line, " wiphy %d", &phy) == 1)
				break;
		pclose(p);
	}
	if (phy < 0)
		return 1;                       /* can't tell: hostapd will */
	snprintf(cmd, sizeof(cmd), "%s phy phy%d info 2>/dev/null", iw(), phy);
	snprintf(want, sizeof(want), "[%d]", ch);
	if ((p = popen(cmd, "r"))) {
		while (fgets(line, sizeof(line), p)) {
			char mhz[16];

			snprintf(mhz, sizeof(mhz), "%d", ch_freq(band, ch));
			if (strstr(line, " MHz ") && strstr(line, want) && strstr(line, mhz)) {
				ok = !strstr(line, "disabled") && !strstr(line, "no IR") && !strstr(line, "radar");
				break;
			}
		}
		pclose(p);
	}
	return ok;
}

/* how crowded channel ch would be: networks on it or overlapping, stronger ones weigh more */
static int crowd(const struct bss *b, int n, int band, int ch, int width)
{
	int i, score = 0;

	for (i = 0; i < n; i++) {
		int c = freq_ch(b[i].freq), w = b[i].signal >= -60 ? 3 : b[i].signal >= -75 ? 2 : 1;

		if (!c || (band == 2) != (b[i].freq < 3000))
			continue;
		if (band == 2 ? abs(c - ch) < 5 : block_of(c, width) == block_of(ch, width))
			score += w;
	}
	return score;
}

static void judge(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	struct bss b[SCAN_MAX];
	const int *cand;
	int n, nc, i, cur, best = 0, best_score = 1 << 30, len = 0, width = a->width ? a->width : 20;
	char reason[400];

	s->last_scan = now;
	s->suggest = 0;
	n = scan(a->iface, b, SCAN_MAX);
	if (n < 0) {
		snprintf(s->seen, sizeof(s->seen), "this radio can't scan while it is an access point");
		AG_INFO("%s: channel %d busy (%.0f%%), but the radio can't scan for a quieter one", a->iface, a->channel,
			s->util);
		return;
	}
	cur = crowd(b, n, a->band, a->channel, width);
	cand = candidates(a->band, width, &nc);
	s->seen[0] = '\0';
	for (i = 0; i < nc; i++) {
		int sc = crowd(b, n, a->band, cand[i], width);

		if (len < (int)sizeof(s->seen) - 24)
			len += snprintf(s->seen + len, sizeof(s->seen) - len, "%s%d: %d", len ? ", " : "", cand[i], sc);
		if (cand[i] == a->channel || !allowed_here(a->iface, a->band, cand[i]))
			continue;
		if (sc < best_score) {
			best_score = sc;
			best = cand[i];
		}
	}
	AG_INFO("%s: channel %d busy (%.0f%%); neighbours by channel (weighted): %s", a->iface, a->channel, s->util,
		s->seen);
	if (!best || best_score * 2 > cur) {
		AG_INFO("%s: no clearly quieter channel; staying on %d", a->iface, a->channel);
		return;
	}
	s->suggest = best;
	{
		char target[40];

		snprintf(target, sizeof(target), "%s:%d", a->iface, best);
		snprintf(reason, sizeof(reason),
			 "The %s GHz Wi-Fi (channel %d) has been busy with other traffic %.0f%% of the time for %d minutes. "
			 "Nearby networks weigh %d on it, %d on channel %d, which looks quieter. Devices follow the switch "
			 "without dropping.", a->band == 5 ? "5" : "2.4", a->channel, s->util,
			 (int)((now - s->busy_since) / 60), cur, best_score, best);
		alerts_raise("wifi-busy", "info", target, a->iface, s->util / 100.0, reason);
	}
}

void chan_tick(void)
{
	int64_t now = ag_wall_s();
	int i;

	if (!cfg.chan_heal)
		return;
	for (i = 0; i < wifi_n_aps; i++) {
		const struct wifi_ap *a = &wifi_aps[i];
		struct chan_state *s;
		uint64_t act, busy, tx;

		if (!a->band || !a->channel || !(s = state_for(a->iface, 1)) || now - s->last_sample < cfg.chan_poll)
			continue;
		s->last_sample = now;
		if (survey(a->iface, &act, &busy, &tx) < 0) {
			s->works = 0;
			continue;
		}
		if (s->works && act > s->active && busy >= s->busy) {
			uint64_t da = act - s->active, db = busy - s->busy, dt = tx > s->tx ? tx - s->tx : 0;
			double u = db > dt ? (double)(db - dt) * 100.0 / (double)da : 0;

			if (u > 100)
				u = 100;
			s->util = s->util < 0 ? u : 0.7 * s->util + 0.3 * u;
		}
		s->works = 1;
		s->active = act;
		s->busy = busy;
		s->tx = tx;
		if (s->util < 0)
			continue;
		if (s->util >= cfg.chan_busy) {
			if (!s->busy_since)
				s->busy_since = now;
		} else {
			s->busy_since = 0;
		}
		if (s->busy_since && now - s->busy_since >= cfg.chan_hold &&
		    (!s->last_scan || now - s->last_scan >= cfg.chan_scan_gap))
			judge(a, s, now);
	}
}

void chan_blob(struct blob_buf *b, const char *iface)
{
	const struct chan_state *s = state_for(iface, 0);

	if (!s || !s->works) {
		blobmsg_add_string(b, "air", s ? "the radio does not report channel time" : "");
		return;
	}
	if (s->util >= 0)
		blobmsg_add_u32(b, "busy_pct", (uint32_t)(s->util + 0.5));
	if (s->busy_since)
		blobmsg_add_u64(b, "busy_since", (uint64_t)s->busy_since);
	if (s->seen[0])
		blobmsg_add_string(b, "neighbours", s->seen);
	if (s->suggest)
		blobmsg_add_u32(b, "suggest", (uint32_t)s->suggest);
}

/* ---------- the actuator ---------- */

/* "<iface or band>:<channel>" -> access point and channel. 0 when valid. */
int chan_parse(const char *target, const struct wifi_ap **ap, int *ch)
{
	char name[32];
	const char *colon = strrchr(target, ':');
	int i;

	if (!colon || colon == target || (size_t)(colon - target) >= sizeof(name))
		return -1;
	snprintf(name, sizeof(name), "%.*s", (int)(colon - target), target);
	*ch = atoi(colon + 1);
	*ap = wifi_ap_find(name);
	for (i = 0; !*ap && i < wifi_n_aps; i++)
		if ((!strcmp(name, "2.4") && wifi_aps[i].band == 2) || (!strcmp(name, "5") && wifi_aps[i].band == 5))
			*ap = &wifi_aps[i];
	return *ap && (*ap)->band && chan_valid((*ap)->band, *ch) ? 0 : -1;
}

/* Moves an access point to channel ch, keeping its width. 0 when done or
 * already there. */
int chan_switch(const char *iface, int ch, char *err, size_t errlen)
{
	const struct wifi_ap *a = wifi_ap_find(iface);
	char cmd[256], reply[256];
	int f, w, sec = 0, c1 = 0;

	if (!a || !a->band) {
		snprintf(err, errlen, "no access point %s (agctl wifi)", iface);
		return -1;
	}
	if (a->channel == ch)
		return 0;
	if (!chan_valid(a->band, ch)) {
		snprintf(err, errlen, "channel %d is not one AIGW uses on %s GHz", ch, a->band == 5 ? "5" : "2.4");
		return -1;
	}
	f = ch_freq(a->band, ch);
	w = a->width ? a->width : 20;
	if (a->band == 2 && w > 40)
		w = 40;
	if (w == 40) {
		if (a->band == 2)
			sec = ch <= 7 ? 1 : -1;
		else
			sec = ((ch - (ch >= 149 ? 149 : 36)) / 4) % 2 == 0 ? 1 : -1;
		c1 = f + 10 * sec;
	} else if (w >= 80) {
		int base = ch >= 149 ? 149 : 36;

		sec = ((ch - base) / 4) % 2 == 0 ? 1 : -1;
		c1 = ch_freq(5, base + (ch - base) / 16 * 16 + 6);
	}
	if (w == 20)
		snprintf(cmd, sizeof(cmd), "CHAN_SWITCH 5 %d ht", f);
	else
		snprintf(cmd, sizeof(cmd), "CHAN_SWITCH 5 %d sec_channel_offset=%d center_freq1=%d bandwidth=%d ht%s",
			 f, sec, c1, w, a->band == 5 && w >= 80 ? " vht" : "");
	if (wifi_cmd(iface, cmd, reply, sizeof(reply)) > 0 && !strncmp(reply, "OK", 2)) {
		AG_INFO("%s: switching from channel %d to %d (%d MHz), announced to devices", iface, a->channel, ch, w);
		return 0;
	}
	/* no channel switch announcement on this radio: the PC restarts that access point */
	{
		const char *ap = !access("/usr/local/sbin/aigw-ap", X_OK) ? "/usr/local/sbin/aigw-ap" :
				 !access("/usr/sbin/aigw-ap", X_OK) ? "/usr/sbin/aigw-ap" : NULL;

		snprintf(cmd, sizeof(cmd), "%s channel %s %d >/dev/null 2>&1", ap ? ap : "false", iface, ch);
		if (ap && safe_iface(iface) && system(cmd) == 0) {
			AG_INFO("%s: restarted on channel %d (the radio can't announce a switch; devices rejoin)", iface, ch);
			return 0;
		}
	}
	snprintf(err, errlen, "%s can't switch to channel %d on the fly (hostapd: %.60s); change it in the Wi-Fi settings",
		 iface, ch, reply);
	return -1;
}
