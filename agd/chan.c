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
 * Neighbour-aware channels (wifi.chan_plan, ag/chanplan.h): each channel
 * AIGW may use (2.4 GHz: 1, 6, 11; 5 GHz: no radar channels) learned hour by
 * hour of the day, never from one moment:
 *   learn    with every look, the air others use on the channel in use, the
 *            home's own use of it (its busy hours) and what its devices feel
 *            (loss, replies, link rate). Neighbours from scan results already
 *            there (`iw scan dump`: anyone's scan, hostapd's own), the other
 *            channels' air from the survey after a scan, and now and then a
 *            scan of our own (wifi.chan_learn_gap, 3 h), only when the home is
 *            quiet (no call or game, hardly any traffic), in hours not learned
 *            yet: a scan takes the radio off its channel for a moment.
 *   choose   every wifi.chan_plan_every (1 h), after wifi.chan_plan_days (3)
 *            of history: another channel clearly better over the busy hours
 *            (wifi.chan_plan_better, 30 % less load), for chan_plan_hold looks
 *            in a row, at most once per wifi.chan_plan_gap (7 days): alert
 *            "wifi-plan" at a quiet moment; the wifi_plan policy makes it a
 *            "channel" decision (act.mode, the hourly limit, area outages).
 *            Never while a call or game is on, nor with a protected device
 *            (act.protect) on that access point.
 *   trial    judged over wifi.chan_plan_trial (1 h) of busy hours on the new
 *            channel against what the old one gave in the same hours: what
 *            devices feel, and the air. Kept (saved in the Wi-Fi config), or
 *            back, that channel avoided for wifi.chan_avoid (7 days).
 *
 * `iw` is $AIGW_IW when set (the tests use a stand-in).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ag/chanplan.h"
#include "agd.h"

#define SCAN_MAX  128
#define SURV_MAX  48
#define AVOID_MAX 8
#define DUMP_EVERY 600                  /* s between two looks at the scan results already there */

struct chan_state {
	char iface[24];
	int works;                      /* the radio reports channel time */
	uint64_t active, busy, tx;      /* last survey, ms */
	double util;                    /* smoothed % of air used by others; -1 = not yet */
	int64_t busy_since;
	int64_t last_scan, last_sample;
	char seen[200];                 /* last scan, for people: "1: 2 networks, 6: 9, 11: 4" */
	int suggest;                    /* quieter channel found by the last scan, 0 = none */

	/* neighbour-aware channels */
	struct cp_table t;
	uint64_t off_act[CP_CH], off_busy[CP_CH];       /* the other channels' last survey */
	double own;                     /* the home's own share of the air at the last look; -1 not known */
	int64_t learn_scan, dump_at, dump_seen, look_at, last_move, asked_at;
	int pick, hold;                 /* the better channel the looks agree on, how many in a row */
	struct cp_choice choice;
	char waits[120];                /* why a planned move waits, in words ("" none) */
	struct { int ch; int64_t until; } avoid[AVOID_MAX];
	uint32_t tid;                   /* the planned move's decision in trial (0 none) */
	int from, to;
	int64_t tstart;
	struct cp_feel before;
	double s_loss, s_rtt, s_rate, s_air;
	int n_dev, n_rate, n_air, n_poll;
	int64_t meas_s, trial_last;     /* busy-hour seconds measured on the new channel, the last look counted */
	uint32_t proposed, kept, tried;
	char last[300];                 /* the last trial's verdict, in words */
	int64_t last_at;
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
			cs[i].own = -1;
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

static int local_hour(int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	localtime_r(&tt, &tm);
	return tm.tm_hour;
}

/* ---------- survey ---------- */

struct surv {
	int freq, inuse;
	uint64_t active, busy, tx;      /* ms */
};

/* every channel the radio reports on (the one in use, and the others a scan visited) */
static int survey(const char *iface, struct surv *o, int max)
{
	char cmd[128], line[256];
	FILE *p;
	int n = -1, found = 0;

	if (!safe_iface(iface))
		return -1;
	snprintf(cmd, sizeof(cmd), "%s dev %s survey dump 2>/dev/null", iw(), iface);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		unsigned long long v;
		char *c;
		int f;

		if (sscanf(line, " frequency: %d", &f) == 1) {
			if (n + 1 >= max)
				break;
			n++;
			memset(&o[n], 0, sizeof(o[n]));
			o[n].freq = f;
			o[n].inuse = strstr(line, "[in use]") != NULL;
			continue;
		}
		if (n < 0 || !(c = strchr(line, ':')) || sscanf(c + 1, "%llu", &v) != 1)
			continue;
		if (strstr(line, "channel active time"))
			o[n].active = v, found += o[n].inuse;
		else if (strstr(line, "channel busy time"))
			o[n].busy = v;
		else if (strstr(line, "channel transmit time"))
			o[n].tx = v;
	}
	pclose(p);
	return found ? n + 1 : -1;
}

/* ---------- scan ---------- */

struct bss {
	int freq;
	int signal;
	int util;                       /* BSS Load channel utilisation 0-255; -1 not said */
	int age_ms;                     /* scan dump: last seen this long ago */
};

static int our_bssid(const char *b)
{
	int i;

	for (i = 0; i < wifi_n_aps; i++)
		if (!strcasecmp(wifi_aps[i].bssid, b))
			return 1;
	return 0;
}

/* how: "scan ap-force" (a scan now) or "scan dump" (the results already there) */
static int scan(const char *iface, const char *how, struct bss *out, int max)
{
	char cmd[128], line[256], bssid[18] = "";
	FILE *p;
	int n = 0, cur = -1;

	if (!safe_iface(iface))
		return -1;
	snprintf(cmd, sizeof(cmd), "%s dev %s %s 2>/dev/null", iw(), iface, how);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		double f, s;
		int v;

		if (!strncmp(line, "BSS ", 4)) {
			cur = -1;
			if (sscanf(line + 4, "%17[0-9a-fA-F:]", bssid) == 1 && !our_bssid(bssid) && n < max) {
				cur = n++;
				out[cur].freq = 0;
				out[cur].signal = -100;
				out[cur].util = -1;
				out[cur].age_ms = -1;
			}
		} else if (cur >= 0 && sscanf(line, " freq: %lf", &f) == 1) {
			out[cur].freq = (int)f;
		} else if (cur >= 0 && sscanf(line, " signal: %lf", &s) == 1) {
			out[cur].signal = (int)s;
		} else if (cur >= 0 && sscanf(line, " last seen: %d ms", &v) == 1) {
			out[cur].age_ms = v;
		} else if (cur >= 0 && (strstr(line, "channel utilisation:") || strstr(line, "channel utilization:")) &&
			   sscanf(strchr(line, ':') + 1, "%d", &v) == 1) {
			out[cur].util = v < 0 ? 0 : v > 255 ? 255 : v;
		}
	}
	if (pclose(p) != 0 && n == 0)
		return -1;
	return n;
}

/* Whether this radio may run an access point on channel ch here
 * (`iw phy`: not disabled, no "no IR", no radar detection). */
static int allowed_here(const char *iface, int band, int ch);

static int avoided(const struct chan_state *s, int ch)
{
	int i;

	for (i = 0; s && i < AVOID_MAX; i++)
		if (s->avoid[i].ch == ch && s->avoid[i].until > ag_wall_s())
			return 1;
	return 0;
}

/* a channel that didn't help: neither a planned move nor a healing trial goes there for a while */
void chan_avoid(const char *iface, int ch, int secs)
{
	struct chan_state *s = state_for(iface, 1);
	int i, k = 0;

	if (!s || secs <= 0)
		return;
	for (i = 0; i < AVOID_MAX; i++)
		if (s->avoid[i].ch == ch || s->avoid[i].until < s->avoid[k].until)
			k = i;
	if (s->avoid[k].ch == ch && s->avoid[k].until > ag_wall_s() + secs)
		return;
	s->avoid[k].ch = ch;
	s->avoid[k].until = ag_wall_s() + secs;
}

/* for Self-Healing Wi-Fi's trials: the channels AIGW would use on this access point,
   the quietest known at this hour first */
int chan_options(const struct wifi_ap *a, int *out, int max)
{
	const struct chan_state *s = state_for(a->iface, 0);
	int n, i, j, k = 0, hour = local_hour(ag_wall_s());
	const int *c = candidates(a->band, a->width ? a->width : 20, &n);
	float key[16];

	for (i = 0; i < n && k < max && k < 16; i++)
		if (c[i] != a->channel && allowed_here(a->iface, a->band, c[i]) && !avoided(s, c[i])) {
			float l = s && s->t.nch ? cp_load(&s->t, cp_slot(&s->t, c[i]), hour, CP_NB) : -1;

			/* insertion by what is known of it now (unknown last, in the band's order) */
			for (j = k; j > 0 && l >= 0 && (key[j - 1] < 0 || key[j - 1] > l); j--) {
				out[j] = out[j - 1];
				key[j] = key[j - 1];
			}
			out[j] = c[i];
			key[j] = l;
			k++;
		}
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
static float crowd(const struct bss *b, int n, int band, int ch, int width)
{
	int i;
	float score = 0;

	for (i = 0; i < n; i++) {
		int c = freq_ch(b[i].freq);

		if (c && (band == 2) == (b[i].freq < 3000))
			score += cp_nb_weight(band, ch, width, c, b[i].signal, b[i].util);
	}
	return score;
}

/* neighbour-aware channels: one scan's neighbours, learned for this hour */
static void learn_scan(const struct wifi_ap *a, struct chan_state *s, const struct bss *b, int n, int64_t now)
{
	int k, hour = local_hour(now), width = a->width ? a->width : 20;

	if (!s->t.nch)
		return;
	for (k = 0; k < s->t.nch; k++) {
		struct cp_cell *c = &s->t.c[k][hour];

		cp_add(&c->nb, &c->n_nb, crowd(b, n, a->band, s->t.ch[k], width));
	}
	s->t.scans++;
}

static void judge(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	struct bss b[SCAN_MAX];
	const int *cand;
	int n, nc, i, best = 0, len = 0, width = a->width ? a->width : 20;
	float cur, best_score = 1e9f;
	char reason[400];

	s->last_scan = s->learn_scan = now;
	s->suggest = 0;
	n = scan(a->iface, "scan ap-force", b, SCAN_MAX);
	if (n < 0) {
		snprintf(s->seen, sizeof(s->seen), "this radio can't scan while it is an access point");
		AG_INFO("%s: channel %d busy (%.0f%%), but the radio can't scan for a quieter one", a->iface, a->channel,
			s->util);
		return;
	}
	if (cfg.chan_plan) {
		learn_scan(a, s, b, n, now);
		s->dump_seen = now;
	}
	cur = crowd(b, n, a->band, a->channel, width);
	cand = candidates(a->band, width, &nc);
	s->seen[0] = '\0';
	for (i = 0; i < nc; i++) {
		float sc = crowd(b, n, a->band, cand[i], width);

		if (len < (int)sizeof(s->seen) - 24)
			len += snprintf(s->seen + len, sizeof(s->seen) - len, "%s%d: %.0f", len ? ", " : "", cand[i], sc);
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
			 "Nearby networks weigh %.0f on it, %.0f on channel %d, which looks quieter. Devices follow the switch "
			 "without dropping.", a->band == 5 ? "5" : "2.4", a->channel, s->util,
			 (int)((now - s->busy_since) / 60), cur, best_score, best);
		alerts_raise("wifi-busy", "info", target, a->iface, s->util / 100.0, reason);
	}
}

/* ---------- neighbour-aware channels ---------- */

static void plan_event(const char *what, const struct chan_state *s, int from, int to, const char *text)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "event", what);
	blobmsg_add_string(&b, "iface", s->iface);
	blobmsg_add_u32(&b, "from_ch", (uint32_t)from);
	blobmsg_add_u32(&b, "to_ch", (uint32_t)to);
	blobmsg_add_string(&b, "explanation", text);
	agd_emit(AG_EV_WIFI, &b);
	blob_buf_free(&b);
}

static double traffic_mbps(const char *iface);

/* the devices on this access point: what they feel now (heal.c), their mean link rate (Mbit/s) */
static int feel_now(const struct wifi_ap *a, float *loss, float *rtt, float *rate, int *protected_on)
{
	const struct heal_radio *r = heal_for(a->iface);
	double sum = 0;
	int i, n = 0, have = 0;

	*protected_on = 0;
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].wifi_connected && !strcmp(devs[i].wifi_iface, a->iface)) {
			if (devs[i].wifi_rate_kbps)
				sum += devs[i].wifi_rate_kbps / 1000.0, n++;
			*protected_on |= act_is_protected(&devs[i]);
		}
	*rate = n ? (float)(sum / n) : -1;
	*loss = *rtt = -1;
	if (r && r->measured) {
		*loss = (float)r->loss_pct;
		*rtt = (float)(r->rtt_ms < 0 ? 0 : r->rtt_ms);
		have = 1;
	}
	return have;
}

static void plan_setup(const struct wifi_ap *a, struct chan_state *s)
{
	int n;
	const int *c = candidates(a->band, 20, &n);

	if (s->t.nch && s->t.band == a->band)
		return;
	cp_init(&s->t, a->band, c, n);
}

/* what the look and the trial measure: every chan_poll */
static void plan_learn(const struct wifi_ap *a, struct chan_state *s, const struct surv *sv, int nsv, double others,
		       int64_t now)
{
	int hour = local_hour(now), slot = cp_slot(&s->t, a->channel), i, prot;
	float loss, rtt, rate;
	int dev = feel_now(a, &loss, &rtt, &rate, &prot);

	if (!s->t.first)
		s->t.first = (long)now;
	if (slot >= 0) {
		struct cp_cell *c = &s->t.c[slot][hour];

		if (others >= 0)
			cp_add_n(&c->air, &c->n_air, (float)others, CP_SLOW);
		if (dev) {
			uint16_t n = c->n_dev;

			cp_add_n(&c->loss, &n, loss, CP_SLOW);
			cp_add_n(&c->rtt, &c->n_dev, rtt, CP_SLOW);
		}
		if (rate > 0)
			cp_add_n(&c->rate, &c->n_rate, rate, CP_SLOW);
	}
	/* the home's use: its share of the air; a radio without a survey: its devices' traffic (Mbit/s) */
	cp_add_n(&s->t.use[hour], &s->t.n_use[hour], (float)(s->own >= 0 ? s->own : traffic_mbps(a->iface)), CP_SLOW);
	/* the other channels' air, from the survey after a scan visited them */
	for (i = 0; i < nsv; i++) {
		int k = cp_slot(&s->t, freq_ch(sv[i].freq));
		uint64_t da, db;

		if (sv[i].inuse || k < 0 || !sv[i].active || sv[i].active == s->off_act[k])
			continue;
		da = sv[i].active > s->off_act[k] ? sv[i].active - s->off_act[k] : sv[i].active;
		db = sv[i].active > s->off_act[k] && sv[i].busy >= s->off_busy[k] ? sv[i].busy - s->off_busy[k] :
		     sv[i].busy;
		s->off_act[k] = sv[i].active;
		s->off_busy[k] = sv[i].busy;
		if (da >= 10 && db <= da)
			cp_add(&s->t.c[k][hour].air, &s->t.c[k][hour].n_air, (float)(db * 100.0 / (double)da));
	}
	/* a trial: what the new channel gives in the hours that matter, after a minute to settle */
	if (s->tid && now - s->tstart >= (cfg.chan_plan_trial / 10 < 60 ? cfg.chan_plan_trial / 10 : 60)) {
		float w[CP_HOURS];

		cp_busy_hours(&s->t, w);
		if (w[hour] >= 1) {
			int64_t dt = s->trial_last ? now - s->trial_last : cfg.chan_poll;

			s->meas_s += dt < 0 ? 0 : dt > 60 + cfg.chan_poll ? cfg.chan_poll : dt;     /* not across a gap */
			s->trial_last = now;
			s->n_poll++;
			if (dev)
				s->s_loss += loss, s->s_rtt += rtt, s->n_dev++;
			if (rate > 0)
				s->s_rate += rate, s->n_rate++;
			if (others >= 0)
				s->s_air += others, s->n_air++;
		} else {
			s->trial_last = 0;
		}
	}
}

/* the scan results already there (anyone's scan, hostapd's own), learned once each */
static void plan_dump(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	struct bss b[SCAN_MAX];
	int n, i, k = 0, newest = 1 << 30;
	int64_t at;

	s->dump_at = now;
	n = scan(a->iface, "scan dump", b, SCAN_MAX);
	for (i = 0; i < n; i++)
		if (b[i].age_ms >= 0 && b[i].age_ms < newest)
			newest = b[i].age_ms;
	if (n <= 0 || newest > DUMP_EVERY * 1000)
		return;                         /* nothing fresh, or no age said (an old iw): can't tell a new scan */
	at = now - newest / 1000;
	if (at <= s->dump_seen + 30)
		return;                         /* the same scan, learned already */
	s->dump_seen = at;
	for (i = 0; i < n; i++)                 /* this scan's: seen within a minute of the newest */
		if (b[i].age_ms >= 0 && b[i].age_ms <= newest + 60000)
			b[k++] = b[i];
	learn_scan(a, s, b, k, at);
}

/* the traffic of the devices on this access point now, Mbit/s (the traffic meter; 0 without it) */
static double traffic_mbps(const char *iface)
{
	double sum = 0;
	int i;

	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].wifi_connected && !strcmp(devs[i].wifi_iface, iface))
			sum += (double)(devs[i].rate_up + devs[i].rate_down) / 1e6;
	return sum;
}

/* no call or game, and the home hardly uses the air: its own share of it (the survey), else its devices'
   traffic under 2 Mbit/s */
static int quiet_now(const struct chan_state *s)
{
	if (calls_live())
		return 0;
	return s->own >= 0 ? s->own < cfg.chan_quiet : traffic_mbps(s->iface) < 2;
}

static void plan_look(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	struct cp_params p;
	uint8_t avoid[CP_CH];
	int k, n, j, width = a->width ? a->width : 20;
	const int *c = candidates(a->band, width, &n);

	s->look_at = now;
	cp_params_default(&p);
	p.days = cfg.chan_plan_days;
	p.better = cfg.chan_plan_better;
	for (k = 0; k < s->t.nch; k++) {
		int ch = s->t.ch[k], ok = 0;

		for (j = 0; j < n; j++)
			ok |= c[j] == ch;       /* a primary channel for this width */
		avoid[k] = ch != a->channel && (!ok || avoided(s, ch) || !allowed_here(a->iface, a->band, ch));
	}
	cp_choose(&s->t, a->channel, avoid, (long)now, &p, &s->choice);
	if (s->choice.status == CP_BETTER) {
		s->hold = s->choice.best == s->pick ? s->hold + 1 : 1;
		s->pick = s->choice.best;
	} else {
		s->pick = s->hold = 0;
	}
	if (s->pick)
		AG_INFO("%s: %s (look %d of %d)", a->iface, s->choice.words, s->hold, cfg.chan_plan_hold);
}

/* the planned move: proposed when the looks agree and the home is quiet */
static void plan_propose(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	char target[40], reason[640];
	float loss, rtt, rate;
	int prot;

	s->waits[0] = '\0';
	if (!s->pick || s->hold < cfg.chan_plan_hold || s->tid)
		return;
	snprintf(target, sizeof(target), "%.23s:%d", a->iface, s->pick);
	if (s->last_move && now - s->last_move < cfg.chan_plan_gap) {
		snprintf(s->waits, sizeof(s->waits), "the last planned move was %d days ago (wifi.chan_plan_gap)",
			 (int)((now - s->last_move) / 86400));
		return;
	}
	if (act_channel_recent(target, now - cfg.chan_plan_gap) || (s->asked_at && now - s->asked_at < 86400)) {
		snprintf(s->waits, sizeof(s->waits), "already asked about a move lately");
		return;
	}
	feel_now(a, &loss, &rtt, &rate, &prot);
	if (prot) {
		snprintf(s->waits, sizeof(s->waits), "a protected device (act.protect) is on this access point");
		return;
	}
	if (!quiet_now(s)) {
		snprintf(s->waits, sizeof(s->waits), calls_live() ? "a call or game is on" : "the home is busy now");
		return;
	}
	snprintf(reason, sizeof(reason),
		 "%s Learned over %d days, hour by hour. A trial on channel %d: judged over %d %s of your busy "
		 "hours on what devices feel and the air, kept if clearly better, else back to %d. Devices follow the "
		 "switch without dropping.", s->choice.words, s->choice.days, s->pick,
		 cfg.chan_plan_trial >= 120 ? cfg.chan_plan_trial / 60 : cfg.chan_plan_trial,
		 cfg.chan_plan_trial >= 120 ? "minutes" : "seconds", a->channel);
	AG_INFO("%s: proposing channel %d (from %d): %s", a->iface, s->pick, a->channel, s->choice.words);
	alerts_raise("wifi-plan", "info", target, a->iface, s->choice.gain_pct / 100.0, reason);
	s->asked_at = now;
	s->proposed++;
	plan_event("plan", s, a->channel, s->pick, s->choice.words);
	dm_on_chanplan(s->iface, a->channel, s->pick, "Proposed", s->choice.words, "", "");
}

static void plan_trial(const struct wifi_ap *a, struct chan_state *s, int64_t now)
{
	uint32_t id = 0;
	int active = 0, ours = 0, from = 0, has = act_channel_for(a->iface, "wifi_plan", &id, &active, &ours, &from) == 0;
	char buf[24] = "0", why[300], note[400], bw[120], aw[120];
	struct cp_feel after;
	int64_t expires;
	int keep;

	if (!s->tid) {
		if (has && active && ours && from > 0 && a->channel != from) {
			s->tid = id;
			s->from = from;
			s->to = a->channel;
			s->tstart = now;
			cp_before(&s->t, cp_slot(&s->t, from), &s->before);
			s->s_loss = s->s_rtt = s->s_rate = s->s_air = 0;
			s->n_dev = s->n_rate = s->n_air = s->n_poll = 0;
			s->meas_s = s->trial_last = 0;
			cp_feel_words(&s->before, bw, sizeof(bw));
			AG_INFO("%s: trial on channel %d (from %d): judged over %d s of busy hours (before: %s)", a->iface,
				s->to, s->from, cfg.chan_plan_trial, bw);
			snprintf(note, sizeof(note), "On channel %d now (from %d); judged over the busy hours.", s->to,
				 s->from);
			dm_on_chanplan(s->iface, s->from, s->to, "Applied", note, bw, "");
		}
		return;
	}
	if (!has || id != s->tid || !active) {          /* undone by a person, or its time ran out */
		AG_INFO("%s: the planned move to channel %d ended before it was judged", a->iface, s->to);
		s->tid = 0;
		s->last_move = now;
		return;
	}
	act_field(id, "expires", buf, sizeof(buf));
	expires = strtoll(buf, NULL, 10);
	if (s->meas_s < cfg.chan_plan_trial &&
	    !(expires && now >= expires - 2 * cfg.chan_poll - 60))
		return;
	memset(&after, 0, sizeof(after));
	if (s->n_dev)
		after.loss = (float)(s->s_loss / s->n_dev), after.rtt = (float)(s->s_rtt / s->n_dev);
	if (s->n_rate)
		after.rate = (float)(s->s_rate / s->n_rate);
	if (s->n_air)
		after.air = (float)(s->s_air / s->n_air);
	after.n_dev = s->n_dev;
	after.n_rate = s->n_rate;
	after.n_air = s->n_air;
	keep = cp_verdict(&s->before, &after, cfg.chan_plan_better, why, sizeof(why));
	cp_feel_words(&s->before, bw, sizeof(bw));
	cp_feel_words(&after, aw, sizeof(aw));
	if (keep) {
		snprintf(buf, sizeof(buf), "%d", s->to);
		snprintf(note, sizeof(note), "kept: channel %d, %s", s->to, why);
		act_keep(id, note);
		wifiset_save(a->iface, WS_CHANNEL, buf, "neighbour-aware channels");
		s->kept++;
		snprintf(s->last, sizeof(s->last), "Channel %d -> %d worked over the busy hours (%.200s): kept and saved.",
			 s->from, s->to, why);
	} else {
		snprintf(note, sizeof(note), "tried: channel %d, %s; back to %d", s->to, why, s->from);
		act_undo_because(id, note);
		chan_avoid(a->iface, s->to, cfg.chan_avoid);
		s->tried++;
		snprintf(s->last, sizeof(s->last), "Channel %d -> %d did not help over the busy hours (%.200s): back; "
			 "channel %d is avoided for %d days.", s->from, s->to, why, s->to, cfg.chan_avoid / 86400);
	}
	s->last_at = now;
	AG_INFO("%s: %s", a->iface, s->last);
	plan_event(keep ? "plan-kept" : "plan-tried", s, s->from, s->to, s->last);
	dm_on_chanplan(s->iface, s->from, s->to, keep ? "Kept" : "MovedBack", why, bw, aw);
	s->tid = 0;
	s->last_move = now;
	s->pick = s->hold = 0;
	dev_state_save();
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
		struct surv sv[SURV_MAX];
		double others = -1;
		int n, k, inuse = -1;

		if (!a->band || !a->channel || !(s = state_for(a->iface, 1)) || now - s->last_sample < cfg.chan_poll)
			continue;
		s->last_sample = now;
		n = survey(a->iface, sv, SURV_MAX);
		for (k = 0; k < n; k++)
			if (sv[k].inuse)
				inuse = k;
		if (inuse < 0) {
			s->works = 0;
			s->own = -1;
		} else {
			uint64_t act = sv[inuse].active, busy = sv[inuse].busy, tx = sv[inuse].tx;

			if (s->works && act > s->active && busy >= s->busy) {
				uint64_t da = act - s->active, db = busy - s->busy, dt = tx > s->tx ? tx - s->tx : 0;
				double u = db > dt ? (double)(db - dt) * 100.0 / (double)da : 0;

				if (u > 100)
					u = 100;
				others = u;
				s->own = dt * 100.0 / (double)da;
				s->util = s->util < 0 ? u : 0.7 * s->util + 0.3 * u;
			}
			s->works = 1;
			s->active = act;
			s->busy = busy;
			s->tx = tx;
		}
		if (cfg.chan_plan) {
			plan_setup(a, s);
			plan_learn(a, s, sv, n < 0 ? 0 : n, others, now);
			if (now - s->dump_at >= DUMP_EVERY)
				plan_dump(a, s, now);
			/* a scan of our own: rare, at a quiet moment, in an hour not learned yet; never right after
			   agd starts (a restart is no reason to leave the channel) */
			if (!s->learn_scan)
				s->learn_scan = now;
			if (now - s->learn_scan >= cfg.chan_learn_gap && now - s->last_scan >= cfg.chan_learn_gap &&
			    quiet_now(s) && cp_slot(&s->t, a->channel) >= 0 &&
			    s->t.c[cp_slot(&s->t, a->channel)][local_hour(now)].n_nb < 4) {
				struct bss b[SCAN_MAX];
				int nb;

				s->learn_scan = now;
				nb = scan(a->iface, "scan ap-force", b, SCAN_MAX);
				if (nb >= 0) {
					learn_scan(a, s, b, nb, now);
					s->dump_seen = now;
				}
			}
			plan_trial(a, s, now);
			if (now - s->look_at >= cfg.chan_plan_every)
				plan_look(a, s, now);
			plan_propose(a, s, now);
		}
		if (s->util < 0 || inuse < 0)
			continue;
		if (s->util >= cfg.chan_busy) {
			if (!s->busy_since)
				s->busy_since = now;
		} else {
			s->busy_since = 0;
		}
		if (s->busy_since && now - s->busy_since >= cfg.chan_hold &&
		    (!s->last_scan || now - s->last_scan >= cfg.chan_scan_gap) && !calls_live())
			judge(a, s, now);
	}
}

void chan_blob(struct blob_buf *b, const char *iface)
{
	const struct chan_state *s = state_for(iface, 0);

	if (s && cfg.chan_plan && s->choice.words[0])
		blobmsg_add_string(b, "plan", s->choice.words);
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

static const char *plan_status(const struct chan_state *s)
{
	if (s->tid)
		return "Trial";
	if (s->pick && s->hold >= cfg.chan_plan_hold)
		return "Planned";
	if (s->choice.status == CP_BETTER)
		return "Confirming";
	return s->choice.status == CP_STAY ? "Stay" : "Learning";
}

/* the help-desk brief: what is worth saying of the channels now (a better one learned, a trial, a verdict
   this week), in words; 0 nothing */
int chan_plan_words(char *buf, size_t n)
{
	int i, len = 0;

	buf[0] = '\0';
	for (i = 0; i < wifi_n_aps && len < (int)n; i++) {
		const struct wifi_ap *a = &wifi_aps[i];
		const struct chan_state *s = state_for(a->iface, 0);
		const char *w;

		if (!s || !s->t.nch)
			continue;
		if (s->tid)
			w = "a planned move is on trial";
		else if (s->choice.status == CP_BETTER)
			w = s->choice.words;
		else if (s->last[0] && ag_wall_s() - s->last_at < 7 * 86400)
			w = s->last;
		else
			continue;
		len += snprintf(buf + len, n - (size_t)len, "%s%s GHz: %s", len ? " " : "", a->band == 5 ? "5" : "2.4", w);
	}
	return len > 0;
}

/* ag channels: each access point's channels by hour (table 0: the verdict and counts only) */
void chan_plan_blob(struct blob_buf *b, int table)
{
	void *arr = blobmsg_open_array(b, "radios");
	int i, k, h;

	for (i = 0; i < wifi_n_aps; i++) {
		const struct wifi_ap *a = &wifi_aps[i];
		const struct chan_state *s = state_for(a->iface, 0);
		float w[CP_HOURS];
		char hours[64];
		void *r;

		if (!s || !s->t.nch)
			continue;
		cp_busy_hours(&s->t, w);
		cp_hours_words(w, hours, sizeof(hours));
		r = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "iface", a->iface);
		blobmsg_add_string(b, "band", a->band == 5 ? "5" : "2.4");
		blobmsg_add_u32(b, "channel", (uint32_t)a->channel);
		blobmsg_add_string(b, "status", plan_status(s));
		blobmsg_add_string(b, "verdict", s->choice.words);
		blobmsg_add_u32(b, "best", (uint32_t)s->choice.best);
		blobmsg_add_u32(b, "gain_pct", (uint32_t)(s->choice.gain_pct > 0 ? s->choice.gain_pct : 0));
		blobmsg_add_string(b, "metric", s->choice.metric == CP_AIR ? "air" : s->choice.metric == CP_NB ?
				   "neighbours" : "");
		blobmsg_add_u32(b, "looks", (uint32_t)s->hold);
		blobmsg_add_string(b, "waits", s->waits);
		blobmsg_add_string(b, "busy_hours", hours);
		blobmsg_add_u32(b, "days", (uint32_t)(s->t.first ? (ag_wall_s() - s->t.first) / 86400 : 0));
		blobmsg_add_u32(b, "scans", s->t.scans);
		blobmsg_add_u32(b, "proposed", s->proposed);
		blobmsg_add_u32(b, "kept", s->kept);
		blobmsg_add_u32(b, "tried", s->tried);
		if (s->tid) {
			blobmsg_add_u32(b, "trial_decision", s->tid);
			blobmsg_add_u32(b, "trial_from", (uint32_t)s->from);
			blobmsg_add_u64(b, "trial_since", (uint64_t)s->tstart);
			blobmsg_add_u32(b, "trial_measured_s", (uint32_t)s->meas_s);
		}
		if (s->last[0]) {
			blobmsg_add_string(b, "last", s->last);
			blobmsg_add_u64(b, "last_at", (uint64_t)s->last_at);
		}
		{
			void *av = blobmsg_open_array(b, "avoided");

			for (k = 0; k < AVOID_MAX; k++)
				if (s->avoid[k].ch && s->avoid[k].until > ag_wall_s()) {
					void *x = blobmsg_open_table(b, NULL);

					blobmsg_add_u32(b, "channel", (uint32_t)s->avoid[k].ch);
					blobmsg_add_u64(b, "until", (uint64_t)s->avoid[k].until);
					blobmsg_close_table(b, x);
				}
			blobmsg_close_array(b, av);
		}
		if (table) {
			void *ca = blobmsg_open_array(b, "channels");

			for (k = 0; k < s->t.nch; k++) {
				void *ct = blobmsg_open_table(b, NULL), *ha;

				blobmsg_add_u32(b, "channel", (uint32_t)s->t.ch[k]);
				ha = blobmsg_open_array(b, "air");      /* % by hour, -1 not measured */
				for (h = 0; h < CP_HOURS; h++)
					blobmsg_add_u32(b, NULL, (uint32_t)(int32_t)(s->t.c[k][h].n_air ?
						(int)(s->t.c[k][h].air + 0.5f) : -1));
				blobmsg_close_array(b, ha);
				ha = blobmsg_open_array(b, "neighbours");       /* weight x10 by hour, -1 not scanned */
				for (h = 0; h < CP_HOURS; h++)
					blobmsg_add_u32(b, NULL, (uint32_t)(int32_t)(s->t.c[k][h].n_nb ?
						(int)(s->t.c[k][h].nb * 10 + 0.5f) : -1));
				blobmsg_close_array(b, ha);
				blobmsg_close_table(b, ct);
			}
			blobmsg_close_array(b, ca);
			ca = blobmsg_open_array(b, "use");      /* the home's own share of the air, % by hour */
			for (h = 0; h < CP_HOURS; h++)
				blobmsg_add_u32(b, NULL, (uint32_t)(int32_t)(s->t.n_use[h] ? (int)(s->t.use[h] + 0.5f) : -1));
			blobmsg_close_array(b, ca);
		}
		blobmsg_close_table(b, r);
	}
	blobmsg_close_array(b, arr);
}

/* Device.X_AIGW_.ChannelPlan.Radio.{i}.: one access point's; -1 unknown */
int chan_plan_field(const char *iface, const char *f, char *buf, size_t n)
{
	const struct wifi_ap *a = wifi_ap_find(iface);
	const struct chan_state *s = state_for(iface, 0);
	float w[CP_HOURS];

	if (!a)
		return -1;
	if (!strcmp(f, "Channel")) snprintf(buf, n, "%d", a->channel);
	else if (!s || !s->t.nch) {
		if (!strcmp(f, "Status")) snprintf(buf, n, "Learning");
		else if (!strcmp(f, "Verdict") || !strcmp(f, "BusyHours") || !strcmp(f, "LastResult")) buf[0] = '\0';
		else snprintf(buf, n, "0");
	}
	else if (!strcmp(f, "Status")) snprintf(buf, n, "%s", plan_status(s));
	else if (!strcmp(f, "Verdict")) snprintf(buf, n, "%s", s->choice.words);
	else if (!strcmp(f, "BestChannel")) snprintf(buf, n, "%d", s->choice.best);
	else if (!strcmp(f, "GainPercent")) snprintf(buf, n, "%d", s->choice.gain_pct > 0 ? s->choice.gain_pct : 0);
	else if (!strcmp(f, "BusyHours")) { cp_busy_hours(&s->t, w); cp_hours_words(w, buf, n); }
	else if (!strcmp(f, "LearnedDays")) snprintf(buf, n, "%d", s->t.first ? (int)((ag_wall_s() - s->t.first) / 86400) : 0);
	else if (!strcmp(f, "Scans")) snprintf(buf, n, "%u", s->t.scans);
	else if (!strcmp(f, "Proposed")) snprintf(buf, n, "%u", s->proposed);
	else if (!strcmp(f, "Kept")) snprintf(buf, n, "%u", s->kept);
	else if (!strcmp(f, "MovedBack")) snprintf(buf, n, "%u", s->tried);
	else if (!strcmp(f, "LastResult")) snprintf(buf, n, "%s", s->last);
	else return -1;
	return 0;
}

/* ---------- saved with agd's state (devices.c) ---------- */

void chan_save(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "chanplan");
	int i, k, h;

	for (i = 0; i < WIFI_AP_MAX; i++) {
		const struct chan_state *s = &cs[i];
		void *ap, *a;

		if (!s->iface[0] || !s->t.nch)
			continue;
		ap = blobmsg_open_table(b, s->iface);
		blobmsg_add_u32(b, "band", (uint32_t)s->t.band);
		blobmsg_add_u64(b, "first", (uint64_t)s->t.first);
		blobmsg_add_u32(b, "scans", s->t.scans);
		blobmsg_add_u64(b, "last_move", (uint64_t)s->last_move);
		blobmsg_add_u32(b, "proposed", s->proposed);
		blobmsg_add_u32(b, "kept", s->kept);
		blobmsg_add_u32(b, "tried", s->tried);
		blobmsg_add_string(b, "last", s->last);
		blobmsg_add_u64(b, "last_at", (uint64_t)s->last_at);
		a = blobmsg_open_array(b, "avoid");
		for (k = 0; k < AVOID_MAX; k++)
			if (s->avoid[k].ch && s->avoid[k].until > ag_wall_s()) {
				blobmsg_add_u32(b, NULL, (uint32_t)s->avoid[k].ch);
				blobmsg_add_u64(b, NULL, (uint64_t)s->avoid[k].until);
			}
		blobmsg_close_array(b, a);
		/* one row per channel and hour with anything learned: channel, hour, then value x10 and count each */
		a = blobmsg_open_array(b, "cells");
		for (k = 0; k < s->t.nch; k++)
			for (h = 0; h < CP_HOURS; h++) {
				const struct cp_cell *c = &s->t.c[k][h];
				void *row;

				if (!c->n_air && !c->n_nb && !c->n_dev && !c->n_rate)
					continue;
				row = blobmsg_open_array(b, NULL);
				blobmsg_add_u32(b, NULL, (uint32_t)s->t.ch[k]);
				blobmsg_add_u32(b, NULL, (uint32_t)h);
				blobmsg_add_u32(b, NULL, (uint32_t)(c->air * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, c->n_air);
				blobmsg_add_u32(b, NULL, (uint32_t)(c->nb * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, c->n_nb);
				blobmsg_add_u32(b, NULL, (uint32_t)(c->loss * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, (uint32_t)(c->rtt * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, c->n_dev);
				blobmsg_add_u32(b, NULL, (uint32_t)(c->rate * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, c->n_rate);
				blobmsg_close_array(b, row);
			}
		blobmsg_close_array(b, a);
		a = blobmsg_open_array(b, "use");       /* hour, % x10, count */
		for (h = 0; h < CP_HOURS; h++)
			if (s->t.n_use[h]) {
				blobmsg_add_u32(b, NULL, (uint32_t)h);
				blobmsg_add_u32(b, NULL, (uint32_t)(s->t.use[h] * 10 + 0.5f));
				blobmsg_add_u32(b, NULL, s->t.n_use[h]);
			}
		blobmsg_close_array(b, a);
		blobmsg_close_table(b, ap);
	}
	blobmsg_close_table(b, t);
}

static long long jnum(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long long)blobmsg_get_double(a);
	default: return 0;
	}
}

/* up to max numbers of an array */
static int nums(struct blob_attr *arr, long long *x, int max)
{
	struct blob_attr *v;
	size_t r;
	int k = 0;

	blobmsg_for_each_attr(v, arr, r)
		if (k < max)
			x[k++] = jnum(v);
	return k;
}

void chan_load(struct blob_attr *t)
{
	struct blob_attr *ap, *v, *row;
	size_t r1, r2;

	blobmsg_for_each_attr(ap, t, r1) {
		struct chan_state *s;
		const int *c;
		int n, band = 0;
		long long x[3 * CP_HOURS];

		if (blobmsg_type(ap) != BLOBMSG_TYPE_TABLE || !(s = state_for(blobmsg_name(ap), 1)))
			continue;
		blobmsg_for_each_attr(v, ap, r2)
			if (!strcmp(blobmsg_name(v), "band"))
				band = (int)jnum(v);
		if (band != 2 && band != 5)
			continue;
		c = candidates(band, 20, &n);
		cp_init(&s->t, band, c, n);
		blobmsg_for_each_attr(v, ap, r2) {
			const char *k = blobmsg_name(v);
			size_t r3;
			int m, i;

			if (!strcmp(k, "first")) s->t.first = (long)jnum(v);
			else if (!strcmp(k, "scans")) s->t.scans = (uint32_t)jnum(v);
			else if (!strcmp(k, "last_move")) s->last_move = jnum(v);
			else if (!strcmp(k, "proposed")) s->proposed = (uint32_t)jnum(v);
			else if (!strcmp(k, "kept")) s->kept = (uint32_t)jnum(v);
			else if (!strcmp(k, "tried")) s->tried = (uint32_t)jnum(v);
			else if (!strcmp(k, "last_at")) s->last_at = jnum(v);
			else if (!strcmp(k, "last") && blobmsg_type(v) == BLOBMSG_TYPE_STRING)
				snprintf(s->last, sizeof(s->last), "%s", blobmsg_get_string(v));
			else if (!strcmp(k, "avoid")) {
				m = nums(v, x, 2 * AVOID_MAX);
				for (i = 0; i + 1 < m; i += 2) {
					s->avoid[i / 2].ch = (int)x[i];
					s->avoid[i / 2].until = x[i + 1];
				}
			} else if (!strcmp(k, "use")) {
				m = nums(v, x, 3 * CP_HOURS);
				for (i = 0; i + 2 < m; i += 3)
					if (x[i] >= 0 && x[i] < CP_HOURS) {
						s->t.use[x[i]] = (float)x[i + 1] / 10.0f;
						s->t.n_use[x[i]] = (uint16_t)x[i + 2];
					}
			} else if (!strcmp(k, "cells")) {
				blobmsg_for_each_attr(row, v, r3) {
					struct cp_cell *cl;
					int sl;

					if (blobmsg_type(row) != BLOBMSG_TYPE_ARRAY || nums(row, x, 11) < 11 ||
					    (sl = cp_slot(&s->t, (int)x[0])) < 0 || x[1] < 0 || x[1] >= CP_HOURS)
						continue;
					cl = &s->t.c[sl][x[1]];
					cl->air = (float)x[2] / 10.0f;
					cl->n_air = (uint16_t)x[3];
					cl->nb = (float)x[4] / 10.0f;
					cl->n_nb = (uint16_t)x[5];
					cl->loss = (float)x[6] / 10.0f;
					cl->rtt = (float)x[7] / 10.0f;
					cl->n_dev = (uint16_t)x[8];
					cl->rate = (float)x[9] / 10.0f;
					cl->n_rate = (uint16_t)x[10];
				}
			}
		}
	}
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
