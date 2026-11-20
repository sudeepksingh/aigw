/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW smarter mesh Wi-Fi: the Data Elements model read, and the rules over it (ag/mesh.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag/mesh.h"

void mesh_params_default(struct mesh_params *p)
{
	memset(p, 0, sizeof(*p));
	p->weak_dbm = -75;
	p->good_dbm = -68;
	p->hyst_db = 8;
	p->loss_pct = 10;
	p->hold_s = 60;
	p->cooldown_s = 900;
	p->quiet_s = 300;
	p->max_fails = 2;
	p->fail_pause_s = 3600;
	p->bad_pair_s = 86400;
	p->busy_util = 80;
	p->legacy = 0;
	p->better_pct = 20;
	p->bh_weak_dbm = -72;
	p->bh_min_kbps = 100000;
	p->bh_busy = 80;
}

void mesh_reset(struct mesh_net *n)
{
	memset(n, 0, sizeof(*n));
}

/* ---------- units ---------- */

int mesh_rcpi_dbm(long rcpi)
{
	if (rcpi <= 0 || rcpi > 220)
		return 0;
	return (int)(rcpi / 2 - 110);
}

int mesh_width_of(int c)
{
	switch (c) {
	case 83: case 84: case 116: case 117: case 119: case 120: case 122: case 123: case 126: case 127: case 132:
		return 40;
	case 128: case 130: case 133: case 135:
		return 80;
	case 129: case 134:
		return 160;
	case 137:
		return 320;
	}
	return 20;
}

int mesh_band_of(int ch, int c)
{
	if (c >= 81 && c <= 84)
		return 2;
	if (c >= 115 && c <= 130)
		return 5;
	if (c >= 131 && c <= 137)
		return 6;
	if (ch >= 1 && ch <= 14)
		return 2;
	if (ch >= 32 && ch <= 177)
		return 5;
	return 0;
}

static const int starts40[] = { 36, 44, 52, 60, 100, 108, 116, 124, 132, 140, 149, 157, 165, 173, 0 };
static const int starts80[] = { 36, 52, 100, 116, 132, 149, 165, 0 };
static const int starts160[] = { 36, 100, 149, 0 };

int mesh_center(int band, int ch, int width)
{
	const int *s;
	int span, i;

	if (width <= 20 || ch <= 0)
		return ch;
	if (band == 2)
		return ch <= 7 ? ch + 2 : ch - 2;
	if (band == 6) {
		int idx = (ch - 1) / 4;

		return width == 40 ? 3 + 8 * (idx / 2) : width == 80 ? 7 + 16 * (idx / 4) : 15 + 32 * (idx / 8);
	}
	s = width == 40 ? starts40 : width == 80 ? starts80 : starts160;
	span = width / 5;
	for (i = 0; s[i]; i++)
		if (ch >= s[i] && ch < s[i] + span)
			return s[i] + (span - 4) / 2;
	return ch;
}

int mesh_opclass(int band, int ch, int width)
{
	if (band == 2) {
		if (ch < 1 || ch > 14)
			return 0;
		return width >= 40 ? (ch <= 7 ? 83 : 84) : 81;
	}
	if (band == 6)
		return width >= 160 ? 134 : width >= 80 ? 133 : width >= 40 ? 132 : 131;
	if (band != 5 || ch < 36)
		return 0;
	if (width >= 160)
		return 129;
	if (width >= 80)
		return 128;
	if (width >= 40) {
		int lower = ((mesh_center(5, ch, 40) - 2) == ch);

		if (ch <= 48)
			return lower ? 116 : 117;
		if (ch <= 64)
			return lower ? 119 : 120;
		if (ch <= 144)
			return lower ? 122 : 123;
		return lower ? 126 : 127;
	}
	return ch <= 48 ? 115 : ch <= 64 ? 118 : ch <= 144 ? 121 : ch <= 161 ? 124 : 125;
}

/* half the span of a channel in channel numbers, doubled (integers): 2.4 GHz 20 MHz overlaps 4 either side */
static int half2(int band, int width)
{
	if (band == 2)
		return width >= 40 ? 8 : 5;
	return (width < 20 ? 20 : width) / 5;
}

int mesh_chan_clash(int band, int a, int wa, int b, int wb)
{
	int ca = mesh_center(band, a, wa), cb = mesh_center(band, b, wb);

	if (!a || !b)
		return 0;
	return 2 * abs(ca - cb) < half2(band, wa) + half2(band, wb);
}

static int dfs(int band, int ch)
{
	return band == 5 && ch >= 52 && ch <= 144;
}

/* ---------- 802.11v / k from the (Re)Association Request ---------- */

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* walks the elements from off: 1 when they fill the frame exactly and the first is the SSID */
static int ies_ok(const uint8_t *b, int n, int off)
{
	int i = off;

	if (off + 2 > n || b[off] != 0)
		return 0;
	while (i + 2 <= n) {
		i += 2 + b[i + 1];
		if (i > n)
			return 0;
	}
	return i == n;
}

void mesh_caps(const char *hex, int *btm, int *rrm)
{
	uint8_t b[1024];
	int n = 0, hi = -1, off = -1, i;

	*btm = *rrm = MESH_CAP_UNKNOWN;
	for (; hex && *hex && n < (int)sizeof(b); hex++) {
		int v = hexval((unsigned char)*hex);

		if (v < 0)
			continue;       /* spaces, colons */
		if (hi < 0)
			hi = v;
		else {
			b[n++] = (uint8_t)(hi << 4 | v);
			hi = -1;
		}
	}
	if (ies_ok(b, n, 4))
		off = 4;                /* Association Request: capability, listen interval */
	else if (ies_ok(b, n, 10))
		off = 10;               /* Reassociation Request: ... and the current AP */
	if (off < 0)
		return;
	*btm = *rrm = MESH_CAP_NO;
	for (i = off; i + 2 <= n; i += 2 + b[i + 1]) {
		const uint8_t *e = b + i + 2;
		int len = b[i + 1];

		if (b[i] == 127)        /* Extended Capabilities: bit 19, BSS Transition */
			*btm = len >= 3 && (e[2] & 0x08) ? MESH_CAP_YES : MESH_CAP_NO;
		else if (b[i] == 70)    /* RM Enabled Capabilities: neighbor report or beacon reports */
			*rrm = len >= 1 && (e[0] & 0x72) ? MESH_CAP_YES : MESH_CAP_NO;
	}
}

/* ---------- reading the model ---------- */

static uint32_t inst_of(const char *s)
{
	uint32_t h = 5381;
	const char *p;

	for (p = s; *p && isdigit((unsigned char)*p); p++)
		;
	if (*s && !*p)
		return (uint32_t)strtoul(s, NULL, 10);
	for (p = s; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return h | 0x80000000u;
}

static uint32_t mix(uint32_t h, uint32_t v)
{
	h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
	return h;
}

static void copy(char *d, size_t n, const char *s)
{
	snprintf(d, n, "%s", s ? s : "");
}

static void mac_copy(char *d, const char *s)
{
	int i;

	snprintf(d, 18, "%s", s ? s : "");
	for (i = 0; d[i]; i++)
		d[i] = (char)tolower((unsigned char)d[i]);
	if (!strcmp(d, "00:00:00:00:00:00"))
		d[0] = '\0';
}

static int is_true(const char *v)
{
	return v && (!strcasecmp(v, "true") || !strcmp(v, "1"));
}

static struct mesh_device *dev_slot(struct mesh_net *n, uint32_t inst, int *idx)
{
	int i;

	for (i = 0; i < n->n_dev; i++)
		if (n->dev[i].inst == inst) {
			*idx = i;
			return &n->dev[i];
		}
	if (n->n_dev >= MESH_DEVS)
		return NULL;
	i = n->n_dev++;
	memset(&n->dev[i], 0, sizeof(n->dev[i]));
	n->dev[i].inst = inst;
	n->dev[i].bh_util = -1;
	*idx = i;
	return &n->dev[i];
}

static struct mesh_radio *radio_slot(struct mesh_device *d, uint32_t inst, int *idx)
{
	int i;

	for (i = 0; i < d->n_radio; i++)
		if (d->radio[i].inst == inst) {
			*idx = i;
			return &d->radio[i];
		}
	if (d->n_radio >= MESH_RADIOS)
		return NULL;
	i = d->n_radio++;
	memset(&d->radio[i], 0, sizeof(d->radio[i]));
	d->radio[i].inst = inst;
	d->radio[i].util = -1;
	*idx = i;
	return &d->radio[i];
}

static struct mesh_bss *bss_slot(struct mesh_radio *r, uint32_t inst, int *idx)
{
	int i;

	for (i = 0; i < r->n_bss; i++)
		if (r->bss[i].inst == inst) {
			*idx = i;
			return &r->bss[i];
		}
	if (r->n_bss >= MESH_BSS)
		return NULL;
	i = r->n_bss++;
	memset(&r->bss[i], 0, sizeof(r->bss[i]));
	r->bss[i].inst = inst;
	r->bss[i].fronthaul = 1;
	*idx = i;
	return &r->bss[i];
}

static struct mesh_sta *sta_slot(struct mesh_net *n, uint32_t key, int dev, int radio, int bss)
{
	int i;

	for (i = 0; i < n->n_sta; i++)
		if (n->sta[i].key == key)
			return &n->sta[i];
	if (n->n_sta >= MESH_STAS)
		return NULL;
	i = n->n_sta++;
	memset(&n->sta[i], 0, sizeof(n->sta[i]));
	n->sta[i].key = key;
	n->sta[i].dev = dev;
	n->sta[i].radio = radio;
	n->sta[i].bss = bss;
	n->sta[i].connect_s = -1;
	n->sta[i].steer_s = -1;
	n->sta[i].btm = n->sta[i].rrm = MESH_CAP_UNKNOWN;
	return &n->sta[i];
}

static struct mesh_unassoc *un_slot(struct mesh_net *n, uint32_t key, int dev, int radio)
{
	int i;

	for (i = 0; i < n->n_un; i++)
		if (n->un[i].key == key)
			return &n->un[i];
	if (n->n_un >= MESH_UNASSOC)
		return NULL;
	i = n->n_un++;
	memset(&n->un[i], 0, sizeof(n->un[i]));
	n->un[i].key = key;
	n->un[i].dev = dev;
	n->un[i].radio = radio;
	return &n->un[i];
}

static int sta_param(struct mesh_sta *s, char **t, int nt, const char *v)
{
	long x = strtol(v, NULL, 10);
	const char *p = t[0];

	if (nt == 1) {
		if (!strcmp(p, "MACAddress")) mac_copy(s->mac, v);
		else if (!strcmp(p, "SignalStrength")) s->dbm = mesh_rcpi_dbm(x);
		else if (!strcmp(p, "LastDataDownlinkRate")) s->down_kbps = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(p, "LastDataUplinkRate")) s->up_kbps = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(p, "EstMACDataRateDownlink")) s->est_mbps = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(p, "PacketsSent")) s->pkts = strtoull(v, NULL, 10);
		else if (!strcmp(p, "RetransCount")) s->retrans = strtoull(v, NULL, 10);
		else if (!strcmp(p, "ErrorsSent")) s->errs = strtoull(v, NULL, 10);
		else if (!strcmp(p, "LastConnectTime")) s->connect_s = x;
		else if (!strcmp(p, "ClientCapabilities")) mesh_caps(v, &s->btm, &s->rrm);
		else return 1;
		return 0;
	}
	if (nt == 3 && !strcmp(p, "MultiAPSTA") && !strcmp(t[1], "SteeringSummaryStats")) {
		if (!strcmp(t[2], "LastSteerTime")) s->steer_s = x > 0 ? x : -1;
		else if (!strcmp(t[2], "BTMAttempts")) s->btm_tries = (uint32_t)x;
		else if (!strcmp(t[2], "BTMFailures")) s->btm_fails = (uint32_t)x;
		else return 1;
		return 0;
	}
	if (nt == 3 && !strcmp(p, "MeasurementReport")) {
		uint32_t m = inst_of(t[1]);
		int k = (int)((m & 0x7fffffffu) ? ((m & 0x7fffffffu) - 1) % MESH_MEAS : 0);
		struct mesh_meas *mm = &s->meas[k];

		if (k >= s->n_meas)
			s->n_meas = k + 1;
		if (!strcmp(t[2], "BSSID")) mac_copy(mm->bssid, v);
		else if (!strcmp(t[2], "RCPI")) mm->dbm = mesh_rcpi_dbm(x);
		else return 1;
		return 0;
	}
	return 1;
}

static int prof_param(int *n_prof, uint32_t *pi, int *pc, int *pch, const char *inst, const char *f, const char *v)
{
	uint32_t in = inst_of(inst);
	int k;

	for (k = 0; k < *n_prof; k++)
		if (pi[k] == in)
			break;
	if (k == *n_prof) {
		if (*n_prof >= 3)
			return 1;
		(*n_prof)++;
		pi[k] = in;
		pc[k] = pch[k] = 0;
	}
	if (!strcmp(f, "Class")) pc[k] = atoi(v);
	else if (!strcmp(f, "Channel")) pch[k] = atoi(v);
	else return 1;
	return 0;
}

static int radio_param(struct mesh_net *n, int di, int ri, struct mesh_radio *r, char **t, int nt, const char *v)
{
	long x = strtol(v, NULL, 10);
	int bi;

	if (nt == 1) {
		if (!strcmp(t[0], "ID")) mac_copy(r->id, v);
		else if (!strcmp(t[0], "Utilization")) r->util = (int)(x * 100 / 255);
		else if (!strcmp(t[0], "Noise")) r->noise_dbm = mesh_rcpi_dbm(x);
		else return 1;
		return 0;
	}
	if (nt == 3 && !strcmp(t[0], "CurrentOperatingClassProfile"))
		return prof_param(&r->n_prof, r->prof_inst, r->prof_class, r->prof_ch, t[1], t[2], v);
	if (nt >= 3 && !strcmp(t[0], "BSS")) {
		struct mesh_bss *b = bss_slot(r, inst_of(t[1]), &bi);

		if (!b)
			return 1;
		if (nt == 3) {
			if (!strcmp(t[2], "BSSID")) mac_copy(b->bssid, v);
			else if (!strcmp(t[2], "SSID")) copy(b->ssid, sizeof(b->ssid), v);
			else if (!strcmp(t[2], "BackhaulUse")) b->backhaul = is_true(v);
			else if (!strcmp(t[2], "FronthaulUse")) b->fronthaul = is_true(v);
			else return 1;
			return 0;
		}
		if (nt >= 5 && !strcmp(t[2], "STA")) {
			uint32_t key = mix(mix(mix(mix(17, n->dev[di].inst), r->inst), b->inst), inst_of(t[3]));
			struct mesh_sta *s = sta_slot(n, key, di, ri, bi);

			return s ? sta_param(s, t + 4, nt - 4, v) : 1;
		}
		return 1;
	}
	if (nt == 3 && !strcmp(t[0], "UnassociatedSTA")) {
		uint32_t key = mix(mix(mix(23, n->dev[di].inst), r->inst), inst_of(t[1]));
		struct mesh_unassoc *u = un_slot(n, key, di, ri);

		if (!u)
			return 1;
		if (!strcmp(t[2], "MACAddress")) mac_copy(u->mac, v);
		else if (!strcmp(t[2], "SignalStrength")) u->dbm = mesh_rcpi_dbm(x);
		else return 1;
		return 0;
	}
	return 1;
}

static int bh_param(struct mesh_device *d, char **t, int nt, const char *v)
{
	long x = strtol(v, NULL, 10);

	if (nt == 1) {
		if (!strcmp(t[0], "LinkType"))
			d->bh = !strcasecmp(v, "Wi-Fi") || !strcasecmp(v, "WiFi") ? MESH_BH_WIFI :
				!strcasecmp(v, "Ethernet") ? MESH_BH_ETH : !strcasecmp(v, "None") || !*v ? MESH_BH_NONE :
				MESH_BH_OTHER;
		else if (!strcmp(t[0], "BackhaulMACAddress")) mac_copy(d->bh_mac, v);
		else if (!strcmp(t[0], "BackhaulDeviceID")) mac_copy(d->bh_parent, v);
		else if (!strcmp(t[0], "MACAddress")) mac_copy(d->bh_own, v);
		else return 1;
		return 0;
	}
	if (nt == 2 && !strcmp(t[0], "Stats")) {
		if (!strcmp(t[1], "SignalStrength")) d->bh_dbm = mesh_rcpi_dbm(x);
		else if (!strcmp(t[1], "LastDataDownlinkRate")) d->bh_down_kbps = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(t[1], "LastDataUplinkRate")) d->bh_up_kbps = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(t[1], "LinkUtilization")) d->bh_util = (int)x;
		else return 1;
		return 0;
	}
	if (nt == 3 && !strcmp(t[0], "CurrentOperatingClassProfile")) {
		if (!strcmp(t[2], "Channel")) d->bh_channel = (int)x;
		else if (!strcmp(t[2], "Class")) d->bh_class = (int)x;
		else return 1;
		return 0;
	}
	return 1;
}

int mesh_set(struct mesh_net *n, const char *path, const char *value)
{
	char buf[320], *t[24], *p, *save = NULL;
	const char *v = value ? value : "";
	int nt = 0, di, ri, rc = 1;
	struct mesh_device *d;
	struct mesh_radio *r;

	if (!strncmp(path, "Device.WiFi.DataElements.", 25))
		path += 25;
	if (strncmp(path, "Network.", 8))
		return -1;
	snprintf(buf, sizeof(buf), "%s", path + 8);
	for (p = strtok_r(buf, ".", &save); p && nt < 24; p = strtok_r(NULL, ".", &save))
		t[nt++] = p;
	if (nt == 1) {
		if (!strcmp(t[0], "ID")) { copy(n->id, sizeof(n->id), v); rc = 0; }
		else if (!strcmp(t[0], "ControllerID")) { mac_copy(n->controller, v); rc = 0; }
	} else if (nt >= 3 && !strcmp(t[0], "Device") && (d = dev_slot(n, inst_of(t[1]), &di))) {
		if (nt == 3) {
			rc = 0;
			if (!strcmp(t[2], "ID")) mac_copy(d->id, v);
			else if (!strcmp(t[2], "Manufacturer")) copy(d->maker, sizeof(d->maker), v);
			else if (!strcmp(t[2], "ManufacturerModel")) copy(d->model, sizeof(d->model), v);
			else rc = 1;
		} else if (nt >= 5 && !strcmp(t[2], "MultiAPDevice") && !strcmp(t[3], "Backhaul")) {
			rc = bh_param(d, t + 4, nt - 4, v);
		} else if (nt >= 5 && !strcmp(t[2], "Radio") && (r = radio_slot(d, inst_of(t[3]), &ri))) {
			rc = radio_param(n, di, ri, r, t + 4, nt - 4, v);
		}
	}
	if (rc)
		n->ignored++;
	return rc;
}

/* ---------- after the read ---------- */

static int radio_band(const struct mesh_radio *r)
{
	int k, b;

	for (k = 0; k < r->n_prof; k++)
		if ((b = mesh_band_of(r->prof_ch[k], r->prof_class[k])))
			return b;
	return 0;
}

void mesh_finish(struct mesh_net *n)
{
	int i, j, k, gw = -1;

	for (i = 0; i < n->n_dev; i++) {
		struct mesh_device *d = &n->dev[i];

		for (j = 0; j < d->n_radio; j++) {
			struct mesh_radio *r = &d->radio[j];
			int wide = 0;

			r->band = radio_band(r);
			r->channel = 0;
			r->opclass = 0;
			r->width = 20;
			for (k = 0; k < r->n_prof; k++) {
				int w = mesh_width_of(r->prof_class[k]);

				if (w <= 20 && r->prof_ch[k] && !r->channel) {
					r->channel = r->prof_ch[k];     /* the primary channel */
					if (!r->opclass)
						r->opclass = r->prof_class[k];
				}
				if (w >= r->width && r->prof_class[k]) {
					r->width = w;
					r->opclass = r->prof_class[k];
					wide = r->prof_ch[k];
				}
			}
			if (!r->channel && wide) {
				/* only the wide class: its channel is the block's centre; the primary is the block's first */
				r->channel = r->width >= 160 ? wide - 14 : r->width >= 80 ? wide - 6 : r->width >= 40 ? wide - 2 : wide;
				if (r->band == 2)
					r->channel = wide;
			}
		}
		if (d->id[0] && n->controller[0] && !strcasecmp(d->id, n->controller))
			gw = i;
	}
	if (gw < 0)     /* the agent with no parent and no wireless backhaul */
		for (i = 0; i < n->n_dev && gw < 0; i++)
			if (!n->dev[i].bh_parent[0] && n->dev[i].bh != MESH_BH_WIFI)
				gw = i;
	if (gw < 0 && n->n_dev)
		gw = 0;
	for (i = 0; i < n->n_dev; i++) {
		struct mesh_device *d = &n->dev[i];

		d->gateway = i == gw;
		d->bh_band = 0;
		if (d->bh != MESH_BH_WIFI)
			continue;
		if (d->bh_channel)
			d->bh_band = mesh_band_of(d->bh_channel, d->bh_class);
		if (!d->bh_band && d->bh_mac[0]) {
			int pd, pr, pb;

			if (mesh_find_bss(n, d->bh_mac, &pd, &pr, &pb) == 0)
				d->bh_band = n->dev[pd].radio[pr].band;
		}
		if (!d->bh_parent[0] && d->bh_mac[0]) {
			int pd, pr, pb;

			if (mesh_find_bss(n, d->bh_mac, &pd, &pr, &pb) == 0)
				snprintf(d->bh_parent, sizeof(d->bh_parent), "%s", n->dev[pd].id);
		}
	}
	/* stations without a MAC (a partial read) are no use */
	for (i = 0; i < n->n_sta; ) {
		if (!n->sta[i].mac[0]) {
			n->sta[i] = n->sta[--n->n_sta];
			continue;
		}
		i++;
	}
}

int mesh_find_sta(const struct mesh_net *n, const char *mac)
{
	int i;

	for (i = 0; mac && i < n->n_sta; i++)
		if (!strcasecmp(n->sta[i].mac, mac))
			return i;
	return -1;
}

int mesh_find_dev(const struct mesh_net *n, const char *id)
{
	int i;

	for (i = 0; id && *id && i < n->n_dev; i++)
		if (!strcasecmp(n->dev[i].id, id))
			return i;
	return -1;
}

int mesh_find_bss(const struct mesh_net *n, const char *bssid, int *dev, int *radio, int *bss)
{
	int i, j, k;

	for (i = 0; bssid && *bssid && i < n->n_dev; i++)
		for (j = 0; j < n->dev[i].n_radio; j++)
			for (k = 0; k < n->dev[i].radio[j].n_bss; k++)
				if (!strcasecmp(n->dev[i].radio[j].bss[k].bssid, bssid)) {
					*dev = i;
					*radio = j;
					*bss = k;
					return 0;
				}
	return -1;
}

void mesh_dev_words(const struct mesh_net *n, int dev, char *buf, size_t sz)
{
	const struct mesh_device *d;

	if (dev < 0 || dev >= n->n_dev) {
		snprintf(buf, sz, "an access point");
		return;
	}
	d = &n->dev[dev];
	if (d->gateway)
		snprintf(buf, sz, "the gateway");
	else if (d->name[0])
		snprintf(buf, sz, "the extender '%s'", d->name);
	else
		snprintf(buf, sz, "the extender %s", d->id[0] ? d->id : "(no id)");
}

static const char *band_words(int b)
{
	return b == 2 ? "2.4 GHz" : b == 5 ? "5 GHz" : b == 6 ? "6 GHz" : "Wi-Fi";
}

int mesh_extenders(const struct mesh_net *n)
{
	int i, k = 0;

	for (i = 0; i < n->n_dev; i++)
		k += !n->dev[i].gateway;
	return k;
}

int mesh_stas_on(const struct mesh_net *n, int dev)
{
	int i, k = 0;

	for (i = 0; i < n->n_sta; i++)
		k += n->sta[i].dev == dev;
	return k;
}

/* ---------- steering ---------- */

static const char *bssid_of(const struct mesh_net *n, int d, int r, int b)
{
	if (d < 0 || d >= n->n_dev || r < 0 || r >= n->dev[d].n_radio || b < 0 || b >= n->dev[d].radio[r].n_bss)
		return "";
	return n->dev[d].radio[r].bss[b].bssid;
}

void mesh_hist_update(struct mesh_hist *h, const struct mesh_net *n, int sta, long now)
{
	const struct mesh_sta *s = &n->sta[sta];
	const char *at = bssid_of(n, s->dev, s->radio, s->bss);

	if (h->last_bssid[0] && *at && strcasecmp(h->last_bssid, at))
		h->moved_at = now;
	if (*at)
		snprintf(h->last_bssid, sizeof(h->last_bssid), "%s", at);
	if (h->pkts && s->pkts >= h->pkts + 50 && s->retrans >= h->retrans) {
		uint64_t dp = s->pkts - h->pkts, dr = s->retrans - h->retrans;

		h->retry_pct = (int)(dr * 100 / dp);
		if (h->retry_pct > 100)
			h->retry_pct = 100;
	} else if (!h->pkts || s->pkts < h->pkts) {
		h->retry_pct = -1;      /* first read, or the counters started over */
	}
	h->pkts = s->pkts;
	h->retrans = s->retrans;
	h->seen = now;
}

/* what candidate (d, r) hears of the station: 0 and dBm in *dbm, how in *how; -1 not known */
static int heard(const struct mesh_net *n, const struct mesh_sta *s, int d, int r, int b, int *dbm, const char **how)
{
	const struct mesh_radio *rad = &n->dev[d].radio[r];
	const struct mesh_radio *cur = &n->dev[s->dev].radio[s->radio];
	int i;

	for (i = 0; i < s->n_meas; i++)
		if (s->meas[i].bssid[0] && s->meas[i].dbm && !strcasecmp(s->meas[i].bssid, rad->bss[b].bssid)) {
			*dbm = s->meas[i].dbm;
			*how = "its own 802.11k report";
			return 0;
		}
	for (i = 0; i < n->n_un; i++)
		if (n->un[i].dbm && n->un[i].dev == d && n->un[i].radio == r && !strcasecmp(n->un[i].mac, s->mac)) {
			*dbm = n->un[i].dbm;
			*how = "measured by that access point";
			return 0;
		}
	/* the same box, the other band: the same spot, so the signal differs by the band's reach */
	if (d == s->dev && s->dbm && rad->band && cur->band && rad->band != cur->band) {
		int shift = rad->band > cur->band ? -7 : 7;

		if (rad->band == 6 || cur->band == 6)
			shift = rad->band > cur->band ? -10 : 10;
		if (cur->band == 6 && rad->band == 2)
			shift = 15;
		*dbm = s->dbm + shift;
		*how = "estimated: the same access point, the other band";
		return 0;
	}
	return -1;
}

static int bh_weak(const struct mesh_device *d, const struct mesh_params *p)
{
	return !d->gateway && d->bh == MESH_BH_WIFI &&
	       ((d->bh_dbm && d->bh_dbm <= p->bh_weak_dbm) || (d->bh_down_kbps && d->bh_down_kbps < p->bh_min_kbps));
}

int mesh_steer_pick(const struct mesh_net *n, int sta, struct mesh_hist *h, const struct mesh_extra *x,
		    const struct mesh_params *p, long now, struct mesh_steer *out, char *skip, size_t sn)
{
	const struct mesh_sta *s = &n->sta[sta];
	const struct mesh_radio *cur;
	const char *from, *how = NULL, *best_how = NULL;
	int weak, lossy, loss = x ? x->loss_pct : -1, best = -1000, bd = -1, br = -1, bb = -1, best_dbm = 0;
	int d, r, b, btm_ok;
	char cw[64], tw[64], ext[48] = "";

	memset(out, 0, sizeof(*out));
	if (skip && sn)
		skip[0] = '\0';
#define SKIP(...) do { if (skip && sn) snprintf(skip, sn, __VA_ARGS__); return 0; } while (0)
	if (s->dev < 0 || s->dev >= n->n_dev || s->radio < 0 || s->radio >= n->dev[s->dev].n_radio)
		SKIP("not on a known access point");
	cur = &n->dev[s->dev].radio[s->radio];
	from = bssid_of(n, s->dev, s->radio, s->bss);
	weak = s->dbm && s->dbm <= p->weak_dbm;
	lossy = (loss >= p->loss_pct) || (h->retry_pct >= 0 && h->retry_pct >= 2 * p->loss_pct);
	if (!weak && !lossy) {
		h->struggling_since = 0;
		SKIP("fine");
	}
	if (!h->struggling_since)
		h->struggling_since = now;
	if (x && x->protected_dev)
		SKIP("protected (act.protect): AIGW never moves it");
	if (now - h->struggling_since < p->hold_s)
		SKIP("struggling for %lds: waiting to see it last", now - h->struggling_since);
	if (x && x->in_call)
		SKIP("in a call or game: moving it now would break it up");
	if (h->pause_until > now)
		SKIP("its last steers didn't land: left alone for %ld min", (h->pause_until - now + 59) / 60);
	/* the controller's last steer, unless it was AIGW's own request (the controller carried it out then) */
	if (s->steer_s >= 0 && s->steer_s < p->quiet_s && !(h->last_steer && now - s->steer_s <= h->last_steer + 5))
		SKIP("the mesh controller steered it %lds ago: not fighting it", s->steer_s);
	if (h->last_steer && now - h->last_steer < p->cooldown_s)
		SKIP("AIGW moved it %ld min ago", (now - h->last_steer) / 60);
	if ((h->moved_at && now - h->moved_at < p->quiet_s) || (s->connect_s >= 0 && s->connect_s < p->quiet_s))
		SKIP("it just joined or moved by itself");
	btm_ok = s->btm == MESH_CAP_YES || (s->btm == MESH_CAP_UNKNOWN && s->btm_tries > s->btm_fails);
	if (!btm_ok && !p->legacy)
		SKIP("it %s 802.11v: moving it would drop it for a moment (mesh.legacy)",
		     s->btm == MESH_CAP_NO ? "has no" : "may not have");
	for (d = 0; d < n->n_dev; d++)
		for (r = 0; r < n->dev[d].n_radio; r++) {
			const struct mesh_radio *rad = &n->dev[d].radio[r];

			if (rad->util >= p->busy_util)
				continue;
			for (b = 0; b < rad->n_bss; b++) {
				const struct mesh_bss *c = &rad->bss[b];
				int dbm, score;

				if (!c->bssid[0] || !c->fronthaul || !strcasecmp(c->bssid, from))
					continue;
				if (cur->bss[s->bss].ssid[0] && c->ssid[0] && strcmp(cur->bss[s->bss].ssid, c->ssid))
					continue;       /* another network (a guest one) */
				if (h->bad_until > now && !strcasecmp(h->bad_from, from) && !strcasecmp(h->bad_to, c->bssid))
					continue;       /* tried, didn't help */
				if (heard(n, s, d, r, b, &dbm, &how) < 0 || dbm < p->good_dbm)
					continue;
				if (s->dbm && dbm < s->dbm + p->hyst_db)
					continue;
				score = dbm;
				if (bh_weak(&n->dev[d], p))
					score -= 6;     /* behind a weak backhaul: the link alone isn't the whole story */
				if (rad->band >= 5)
					score += 3;     /* the faster band, when it is heard well */
				if (score > best) {
					best = score;
					bd = d;
					br = r;
					bb = b;
					best_dbm = dbm;
					best_how = how;
				}
			}
		}
	if (bd < 0)
		SKIP("no access point hears it clearly better");
	out->sta = sta;
	out->to_dev = bd;
	out->to_radio = br;
	out->to_bss = bb;
	snprintf(out->from, sizeof(out->from), "%s", from);
	snprintf(out->to, sizeof(out->to), "%s", n->dev[bd].radio[br].bss[bb].bssid);
	out->cur_dbm = s->dbm;
	out->exp_dbm = best_dbm;
	out->method = btm_ok ? MESH_STEER_BTM : MESH_STEER_MANDATE;
	mesh_dev_words(n, s->dev, cw, sizeof(cw));
	mesh_dev_words(n, bd, tw, sizeof(tw));
	if (loss >= 0 && loss >= p->loss_pct)
		snprintf(ext, sizeof(ext), ", %d%% lost", loss);
	else if (h->retry_pct >= 0 && h->retry_pct >= 2 * p->loss_pct)
		snprintf(ext, sizeof(ext), ", %d%% sent again", h->retry_pct);
	snprintf(out->why, sizeof(out->why),
		 "struggling on %s %s (%s%d dBm%s) for %ld min; %s %s hears it at %d dBm (%s)%s.",
		 cw, band_words(cur->band), s->dbm ? "" : "signal not known, ", s->dbm, ext,
		 (now - h->struggling_since + 59) / 60, tw, band_words(n->dev[bd].radio[br].band), best_dbm, best_how,
		 out->method == MESH_STEER_BTM ? "" : "; it has no 802.11v, so the controller drops it for a moment");
	return 1;
#undef SKIP
}

void mesh_sample_of(const struct mesh_net *n, int sta, const struct mesh_hist *h, int loss_pct, struct mesh_sample *s)
{
	memset(s, 0, sizeof(*s));
	s->retry_pct = -1;
	s->loss_pct = loss_pct;
	if (sta < 0 || sta >= n->n_sta)
		return;
	s->dbm = n->sta[sta].dbm;
	s->kbps = n->sta[sta].down_kbps ? n->sta[sta].down_kbps : n->sta[sta].est_mbps * 1000;
	s->retry_pct = h ? h->retry_pct : -1;
}

int mesh_trial_judge(const struct mesh_sample *a, const struct mesh_sample *z, const struct mesh_params *p, char *why,
		     size_t n)
{
	int better = 0, worse = 0;
	char g[200] = "", w[200] = "";
	size_t gl = 0, wl = 0;

#define GOOD(...) do { better++; gl += (size_t)snprintf(g + gl, gl < sizeof(g) ? sizeof(g) - gl : 0, __VA_ARGS__); } while (0)
#define BAD(...) do { worse++; wl += (size_t)snprintf(w + wl, wl < sizeof(w) ? sizeof(w) - wl : 0, __VA_ARGS__); } while (0)
	if (a->dbm && z->dbm) {
		if (z->dbm >= a->dbm + p->hyst_db / 2)
			GOOD("%ssignal %d -> %d dBm", gl ? ", " : "", a->dbm, z->dbm);
		else if (z->dbm <= a->dbm - p->hyst_db / 2)
			BAD("%ssignal %d -> %d dBm", wl ? ", " : "", a->dbm, z->dbm);
	}
	if (a->kbps && z->kbps) {
		if ((uint64_t)z->kbps * 100 >= (uint64_t)a->kbps * (uint64_t)(100 + p->better_pct))
			GOOD("%srate %u -> %u Mbit/s", gl ? ", " : "", a->kbps / 1000, z->kbps / 1000);
		else if ((uint64_t)z->kbps * (uint64_t)(100 + p->better_pct) <= (uint64_t)a->kbps * 100)
			BAD("%srate %u -> %u Mbit/s", wl ? ", " : "", a->kbps / 1000, z->kbps / 1000);
	}
	if (a->loss_pct >= 0 && z->loss_pct >= 0) {
		if (z->loss_pct + 5 <= a->loss_pct)
			GOOD("%slost %d%% -> %d%%", gl ? ", " : "", a->loss_pct, z->loss_pct);
		else if (z->loss_pct >= a->loss_pct + 3)
			BAD("%slost %d%% -> %d%%", wl ? ", " : "", a->loss_pct, z->loss_pct);
	}
	if (a->retry_pct >= 0 && z->retry_pct >= 0) {
		if (z->retry_pct + 10 <= a->retry_pct)
			GOOD("%ssent again %d%% -> %d%%", gl ? ", " : "", a->retry_pct, z->retry_pct);
		else if (z->retry_pct >= a->retry_pct + 10)
			BAD("%ssent again %d%% -> %d%%", wl ? ", " : "", a->retry_pct, z->retry_pct);
	}
#undef GOOD
#undef BAD
	if (better && !worse) {
		snprintf(why, n, "clearly better: %s", g);
		return MESH_KEEP;
	}
	if (worse)
		snprintf(why, n, "worse: %s%s%s", w, better ? "; better: " : "", better ? g : "");
	else
		snprintf(why, n, "not clearly better (%s)", !a->dbm && !a->kbps && a->loss_pct < 0 ? "nothing measured before" :
			 "the same within the margins");
	return MESH_UNDO;
}

int mesh_steer_landed(const struct mesh_net *n, const char *mac, const char *to)
{
	int i = mesh_find_sta(n, mac);

	return i >= 0 && !strcasecmp(bssid_of(n, n->sta[i].dev, n->sta[i].radio, n->sta[i].bss), to);
}

void mesh_steer_failed(struct mesh_hist *h, const struct mesh_params *p, long now, int landed, const char *from,
		       const char *to)
{
	if (!landed) {
		h->misses++;
		h->fails++;
		h->last_steer = now;            /* the cooldown runs from the miss */
		if (h->fails >= p->max_fails) {
			long pause = p->fail_pause_s;
			int k;

			for (k = p->max_fails; k < h->fails && pause < 7 * 86400; k++)
				pause *= 2;
			if (pause > 7 * 86400)
				pause = 7 * 86400;
			h->pause_until = now + pause;
		}
		return;
	}
	h->undone++;
	h->fails = 0;
	snprintf(h->bad_from, sizeof(h->bad_from), "%s", from ? from : "");
	snprintf(h->bad_to, sizeof(h->bad_to), "%s", to ? to : "");
	h->bad_until = now + p->bad_pair_s;
}

/* ---------- the channel plan ---------- */

/* the radio carries a wireless backhaul (the extender's own link, or a child's link to it) */
static int tied(const struct mesh_net *n, int d, int r)
{
	const struct mesh_device *dv = &n->dev[d];
	const struct mesh_radio *rad = &dv->radio[r];
	int i, k;

	if (dv->bh == MESH_BH_WIFI && dv->bh_band && dv->bh_band == rad->band) {
		/* its backhaul station is on this band: tied unless another radio of the band is on the backhaul's channel */
		int other = 0;

		for (k = 0; k < dv->n_radio; k++)
			if (k != r && dv->radio[k].band == rad->band && dv->bh_channel &&
			    mesh_chan_clash(rad->band, dv->radio[k].channel, dv->radio[k].width, dv->bh_channel,
					    mesh_width_of(dv->bh_class)))
				other = 1;
		if (!other)
			return 1;
	}
	for (k = 0; k < rad->n_bss; k++)
		for (i = 0; i < n->n_dev; i++)
			if (i != d && n->dev[i].bh == MESH_BH_WIFI && rad->bss[k].bssid[0] &&
			    !strcasecmp(n->dev[i].bh_mac, rad->bss[k].bssid))
				return 1;       /* a child links to this BSS */
	return 0;
}

static int clashes(const struct mesh_net *n, int d, int r, int band, int ch, int width)
{
	int i, j, k = 0;

	for (i = 0; i < n->n_dev; i++)
		for (j = 0; j < n->dev[i].n_radio; j++) {
			const struct mesh_radio *o = &n->dev[i].radio[j];

			if ((i == d && j == r) || o->band != band || !o->channel)
				continue;
			k += mesh_chan_clash(band, ch, width, o->channel, o->width);
		}
	return k;
}

int mesh_chan_plan(const struct mesh_net *n, struct mesh_chan *out, int max)
{
	static const int c24[] = { 1, 6, 11, 0 };
	static const int c5_80[] = { 36, 149, 0 };
	static const int c5_40[] = { 36, 44, 149, 157, 0 };
	static const int c5_20[] = { 36, 40, 44, 48, 149, 153, 157, 161, 0 };
	int d, r, k = 0;

	for (d = 0; d < n->n_dev && k < max; d++) {
		if (n->dev[d].gateway)
			continue;               /* the gateway keeps its channels; extenders move around it */
		for (r = 0; r < n->dev[d].n_radio && k < max; r++) {
			const struct mesh_radio *rad = &n->dev[d].radio[r];
			const int *cand;
			int i, pick = 0;
			char me[64], who[64] = "";

			if ((rad->band != 2 && rad->band != 5) || !rad->channel || tied(n, d, r) ||
			    !clashes(n, d, r, rad->band, rad->channel, rad->width))
				continue;
			cand = rad->band == 2 ? c24 : rad->width >= 80 ? c5_80 : rad->width >= 40 ? c5_40 : c5_20;
			for (i = 0; cand[i] && !pick; i++)
				if (!dfs(rad->band, cand[i]) && !mesh_chan_clash(rad->band, cand[i], rad->width, rad->channel, rad->width) &&
				    !clashes(n, d, r, rad->band, cand[i], rad->width))
					pick = cand[i];
			if (!pick)
				continue;               /* unavoidable: every channel is taken by the home's own radios */
			/* who it shares with, for the words */
			for (i = 0; i < n->n_dev && !who[0]; i++) {
				int j;

				for (j = 0; j < n->dev[i].n_radio; j++)
					if (!(i == d && j == r) && n->dev[i].radio[j].band == rad->band &&
					    mesh_chan_clash(rad->band, rad->channel, rad->width, n->dev[i].radio[j].channel,
							    n->dev[i].radio[j].width)) {
						mesh_dev_words(n, i, who, sizeof(who));
						break;
					}
			}
			mesh_dev_words(n, d, me, sizeof(me));
			out[k].dev = d;
			out[k].radio = r;
			out[k].from = rad->channel;
			out[k].to = pick;
			out[k].opclass = mesh_opclass(rad->band, pick, rad->width);
			snprintf(out[k].why, sizeof(out[k].why),
				 "%s %s is on channel %d, the same air as %s: they take turns instead of both sending. "
				 "Channel %d clashes with no other radio of the home.", me, band_words(rad->band), rad->channel,
				 who[0] ? who : "another access point", pick);
			k++;
		}
	}
	return k;
}

/* ---------- backhaul ---------- */

const char *mesh_bh_kind_name(int k)
{
	static const char *const nm[] = { "Weak", "Slow", "Busy", "On24", "Try6" };

	return k >= 0 && k < MBH_N ? nm[k] : "None";
}

const char *mesh_bh_name(int bh)
{
	return bh == MESH_BH_WIFI ? "Wi-Fi" : bh == MESH_BH_ETH ? "Ethernet" : bh == MESH_BH_OTHER ? "Other" : "None";
}

static int has_band(const struct mesh_device *d, int band)
{
	int i;

	for (i = 0; i < d->n_radio; i++)
		if (d->radio[i].band == band)
			return 1;
	return 0;
}

int mesh_backhaul(const struct mesh_net *n, const struct mesh_params *p, struct mesh_bh_finding *out, int max)
{
	int d, k = 0;

	for (d = 0; d < n->n_dev && k < max; d++) {
		const struct mesh_device *dv = &n->dev[d];
		int pi = mesh_find_dev(n, dv->bh_parent), kind = -1, st;
		const struct mesh_device *pa;
		char me[64], par[64], rate[40] = "", sig[24] = "";
		struct mesh_bh_finding *f;

		if (dv->gateway || dv->bh != MESH_BH_WIFI)
			continue;
		pa = pi >= 0 ? &n->dev[pi] : NULL;
		if (dv->bh_dbm && dv->bh_dbm <= p->bh_weak_dbm)
			kind = MBH_WEAK;
		else if (dv->bh_down_kbps && dv->bh_down_kbps < p->bh_min_kbps)
			kind = MBH_SLOW;
		else if (dv->bh_util >= p->bh_busy)
			kind = MBH_BUSY;
		else if (dv->bh_band == 2 && (has_band(dv, 5) || has_band(dv, 6)) &&
			 (!pa || has_band(pa, 5) || has_band(pa, 6)))
			kind = MBH_24;
		else if (dv->bh_band == 5 && has_band(dv, 6) && pa && has_band(pa, 6) && dv->bh_dbm && dv->bh_dbm >= -65)
			kind = MBH_TRY6;
		if (kind < 0)
			continue;
		st = mesh_stas_on(n, d);
		f = &out[k++];
		memset(f, 0, sizeof(*f));
		f->dev = d;
		f->kind = kind;
		f->stations = st;
		f->hurts = (kind == MBH_WEAK || kind == MBH_SLOW) && st > 0;
		mesh_dev_words(n, d, me, sizeof(me));
		if (pi >= 0)
			mesh_dev_words(n, pi, par, sizeof(par));
		else
			snprintf(par, sizeof(par), "the gateway");
		if (dv->bh_dbm)
			snprintf(sig, sizeof(sig), "%d dBm", dv->bh_dbm);
		if (dv->bh_down_kbps)
			snprintf(rate, sizeof(rate), "%s%u Mbit/s", sig[0] ? ", " : "", dv->bh_down_kbps / 1000);
		snprintf(f->words, sizeof(f->words), "%s has a %s wireless backhaul to %s (%s%s%s%s); %d device%s behind it.",
			 me, kind == MBH_WEAK ? "weak" : kind == MBH_SLOW ? "slow" : kind == MBH_BUSY ? "nearly full" :
			 kind == MBH_24 ? "2.4 GHz" : "5 GHz", par, sig, rate, dv->bh_band ? (sig[0] || rate[0] ? ", " : "") : "",
			 dv->bh_band ? band_words(dv->bh_band) : "", st, st == 1 ? " is" : "s are");
		snprintf(f->advice, sizeof(f->advice), "%s",
			 kind == MBH_WEAK ? "Move it a room closer to the gateway (about halfway is best), or connect it with a "
					    "cable (Ethernet, powerline or MoCA)." :
			 kind == MBH_SLOW ? "Its link to the gateway is slow: move it closer, or connect it with a cable." :
			 kind == MBH_BUSY ? "Its link to the gateway is nearly full: a cable, or a faster wireless band for "
					    "the backhaul, would carry more." :
			 kind == MBH_24 ? "It links over 2.4 GHz: move it closer so it can use 5 GHz, or connect it with a cable." :
			 "Both ends have 6 GHz and the signal is good: a 6 GHz backhaul would likely be faster "
			 "(the controller's backhaul preference).");
	}
	return k;
}
