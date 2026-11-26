/* SPDX-License-Identifier: Apache-2.0 */
/* AIGW per-app experience: the rules (see ag/appqoe.h). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag/appqoe.h"
#include "ag/why.h"

/* ---------- kinds and groups ---------- */

static const char *const kind_name[AK_N] = {
	"None", "VoiceCall", "VideoCall", "ScreenShare", "VideoStreaming", "CloudGaming", "OnlineGaming",
	"LargeDownload", "Browsing", "SmartHome",
};
static const char *const kind_word[AK_N] = {
	"connection", "voice call", "video call", "screen share", "video stream", "cloud game", "online game",
	"large download", "browsing", "smart-home device",
};
static const char *const group_name[AGR_N] = { "Calls", "Streaming", "Gaming", "Downloads", "Browsing", "SmartHome" };
static const char *const group_word[AGR_N] = { "calls", "streaming", "gaming", "downloads", "browsing", "smart home" };

const char *app_kind_name(int k) { return k > AK_NONE && k < AK_N ? kind_name[k] : "None"; }
const char *app_kind_words(int k) { return k > AK_NONE && k < AK_N ? kind_word[k] : "connection"; }
const char *app_group_name(int g) { return g >= 0 && g < AGR_N ? group_name[g] : ""; }
const char *app_group_words(int g) { return g >= 0 && g < AGR_N ? group_word[g] : ""; }

int app_kind_of(const char *name)
{
	int k;

	for (k = AK_NONE + 1; name && k < AK_N; k++)
		if (!strcasecmp(name, kind_name[k]))
			return k;
	return AK_NONE;
}

int app_group_of(int kind)
{
	switch (kind) {
	case AK_VOICE: case AK_VIDEO: case AK_SCREEN: return AGR_CALLS;
	case AK_STREAM: return AGR_STREAMING;
	case AK_CLOUDGAME: case AK_GAME: return AGR_GAMING;
	case AK_DOWNLOAD: return AGR_DOWNLOADS;
	case AK_BROWSE: return AGR_BROWSING;
	case AK_IOT: return AGR_SMARTHOME;
	}
	return -1;
}

int app_group_by_name(const char *s)
{
	static const struct { const char *w; int g; } alias[] = {
		{ "call", AGR_CALLS }, { "video", AGR_CALLS }, { "voice", AGR_CALLS }, { "stream", AGR_STREAMING },
		{ "tv", AGR_STREAMING }, { "game", AGR_GAMING }, { "games", AGR_GAMING }, { "download", AGR_DOWNLOADS },
		{ "web", AGR_BROWSING }, { "smart-home", AGR_SMARTHOME }, { "iot", AGR_SMARTHOME },
	};
	size_t i;
	int g;

	if (!s || !*s)
		return -1;
	for (g = 0; g < AGR_N; g++)
		if (!strcasecmp(s, group_name[g]) || !strcasecmp(s, group_word[g]))
			return g;
	for (i = 0; i < sizeof(alias) / sizeof(alias[0]); i++)
		if (!strcasecmp(s, alias[i].w))
			return alias[i].g;
	return -1;
}

/* ---------- the rules file ---------- */

static const char default_rules[] =
	/* the rules only: config/apps.rules is the same with its comments (what each condition means) */
	"kind ScreenShare proto=udp port=3478-3481,5349,8801-8810,19302-19309,16384-16402,9000 up=300-20000 ratio=250- conf=70\n"
	"kind VideoCall proto=udp port=3478-3481,5349,8801-8810,19302-19309,16384-16402,9000 down=200-20000 pps=30- conf=75\n"
	"kind VoiceCall proto=udp port=3478-3481,5349,8801-8810,19302-19309,16384-16402,9000 down=6-250 up=6-250 pps=8- conf=75\n"
	"kind OnlineGaming proto=udp port=3074,3659,27000-27050,7000-7500,19132-19133 pps=10- down=2-3000 conf=75\n"
	"kind CloudGaming proto=udp down=4000-120000 ratio=0-8 pps=300- conf=55\n"
	"kind ScreenShare proto=udp up=400-20000 down=0-1500 ratio=300- pps=30- steady=80 conf=50\n"
	"kind VideoCall proto=udp down=250-20000 up=40-20000 pps=60- ratio=5-1000 pkt=300- steady=70 conf=55\n"
	"kind VoiceCall proto=udp down=8-300 up=8-300 pps=16-260 pkt=40-450 ratio=25-400 steady=60 conf=50\n"
	"kind OnlineGaming proto=udp down=2-1500 up=2-800 pps=16-300 pkt=30-700 ratio=10-1000 steady=70 named=1 conf=50\n"
	"kind LargeDownload proto=tcp,quic down=6000- ratio=0-6 pkt=1000- duty=100- steady=35 looks=4 conf=50\n"
	"kind VideoStreaming proto=tcp,quic down=400-120000 ratio=0-8 pkt=700- looks=3 conf=50\n"
	"kind SmartHome iot=1 down=0-64 up=0-64 life=60- conf=60\n"
	"browse 4\n"
	"family Zoom VideoCall,VoiceCall,ScreenShare zoom.us zoom.com zoomgov.com\n"
	"family Teams VideoCall,VoiceCall,ScreenShare teams.microsoft.com teams.live.com skype.com lync.com\n"
	"family Google_Meet VideoCall,VoiceCall,ScreenShare meet.google.com stun.l.google.com\n"
	"family WhatsApp VoiceCall,VideoCall whatsapp.net whatsapp.com\n"
	"family FaceTime VideoCall,VoiceCall facetime.apple.com\n"
	"family Webex VideoCall,VoiceCall,ScreenShare webex.com\n"
	"family Discord VoiceCall,VideoCall,ScreenShare discord.media discord.gg\n"
	"family Signal VoiceCall,VideoCall signal.org\n"
	"family YouTube VideoStreaming googlevideo.com youtube.com\n"
	"family Netflix VideoStreaming nflxvideo.net netflix.com nflxso.net\n"
	"family Prime_Video VideoStreaming aiv-cdn.net primevideo.com\n"
	"family Disney+ VideoStreaming dssott.com disneyplus.com bamgrid.com\n"
	"family Hotstar VideoStreaming hotstar.com hotstarext.com\n"
	"family JioCinema VideoStreaming jiocinema.com\n"
	"family Twitch VideoStreaming ttvnw.net twitch.tv\n"
	"family GeForce_NOW CloudGaming nvidiagrid.net geforcenow.com\n"
	"family Xbox_Cloud CloudGaming gssv-play-prod.xboxlive.com xcloud.xboxlive.com\n"
	"family Xbox_Live OnlineGaming,LargeDownload xboxlive.com\n"
	"family PlayStation OnlineGaming,LargeDownload playstation.net\n"
	"family Steam OnlineGaming,LargeDownload steamserver.net valve.net steamcontent.com\n"
	"family Riot_Games OnlineGaming riotgames.com\n"
	"family Epic_Games OnlineGaming,LargeDownload epicgames.com epicgames.dev\n"
	"family Roblox OnlineGaming roblox.com rbxcdn.com\n"
	"family Battle.net OnlineGaming,LargeDownload battle.net blizzard.com\n"
	"family Windows_Update LargeDownload windowsupdate.com delivery.mp.microsoft.com\n"
	"family Apple_Updates LargeDownload swcdn.apple.com updates.cdn-apple.com\n"
	"family Tuya SmartHome tuyaus.com tuyaeu.com tuyacn.com tuyain.com\n"
	"family Ring SmartHome ring.com\n"
	"family Philips_Hue SmartHome meethue.com\n"
	"family Nest SmartHome nest.com\n";

const char *app_rules_default(void)
{
	return default_rules;
}

/* condition bits */
#define RC_DOWN  1
#define RC_UP    2
#define RC_PPS   4
#define RC_PKT   8
#define RC_RATIO 16
#define RC_DUTY  32
#define RC_LIFE  64

static int num(const char *s, uint32_t *v)
{
	char *e;
	unsigned long x;

	if (!s || !isdigit((unsigned char)*s))
		return -1;
	x = strtoul(s, &e, 10);
	if (*e || x > 0xfffffffful)
		return -1;
	*v = (uint32_t)x;
	return 0;
}

/* "lo-hi", "lo-", "-hi" or "n" */
static int range(const char *s, struct app_range *r)
{
	char lo[16], *d = strchr(s, '-');

	r->lo = 0;
	r->hi = 0xffffffffu;
	if (!d)
		return num(s, &r->lo) < 0 ? -1 : (r->hi = r->lo, 0);
	if ((size_t)(d - s) >= sizeof(lo))
		return -1;
	memcpy(lo, s, (size_t)(d - s));
	lo[d - s] = '\0';
	if (*lo && num(lo, &r->lo) < 0)
		return -1;
	if (d[1] && num(d + 1, &r->hi) < 0)
		return -1;
	return r->lo <= r->hi ? 0 : -1;
}

static int kinds_list(char *s, uint8_t *out, int max)
{
	char *sv = NULL, *t;
	int n = 0;

	for (t = strtok_r(s, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
		int k = app_kind_of(t);

		if (k == AK_NONE || n >= max)
			return -1;
		out[n++] = (uint8_t)k;
	}
	return n;
}

static int rule_line(struct app_rules *r, char **tok, int nt, char *err, size_t en)
{
	struct app_rule *u;
	int i;

	if (r->nrule >= APP_RULES) {
		snprintf(err, en, "more than %d rules", APP_RULES);
		return -1;
	}
	u = &r->rule[r->nrule];
	memset(u, 0, sizeof(*u));
	if (nt < 2 || (u->kind = (uint8_t)app_kind_of(tok[1])) == AK_NONE) {
		snprintf(err, en, "not a kind: %s", nt < 2 ? "(none)" : tok[1]);
		return -1;
	}
	u->conf = 50;
	u->looks = 3;
	for (i = 2; i < nt; i++) {
		char *k = tok[i], *v = strchr(k, '=');
		struct app_range *rg = NULL;
		uint16_t bit = 0;
		uint32_t x;

		if (!v) {
			snprintf(err, en, "%s: key=value", k);
			return -1;
		}
		*v++ = '\0';
		if (!strcmp(k, "down")) rg = &u->down, bit = RC_DOWN;
		else if (!strcmp(k, "up")) rg = &u->up, bit = RC_UP;
		else if (!strcmp(k, "pps")) rg = &u->pps, bit = RC_PPS;
		else if (!strcmp(k, "pkt")) rg = &u->pkt, bit = RC_PKT;
		else if (!strcmp(k, "ratio")) rg = &u->ratio, bit = RC_RATIO;
		else if (!strcmp(k, "duty")) rg = &u->duty, bit = RC_DUTY;
		else if (!strcmp(k, "life")) rg = &u->life, bit = RC_LIFE;
		if (rg) {
			if (range(v, rg) < 0) {
				snprintf(err, en, "%s=%s: a range (lo-hi, lo- or -hi)", k, v);
				return -1;
			}
			u->cond |= bit;
		} else if (!strcmp(k, "proto")) {
			char *sv = NULL, *t;

			for (t = strtok_r(v, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
				if (!strcmp(t, "udp")) u->proto |= AP_UDP;
				else if (!strcmp(t, "tcp")) u->proto |= AP_TCP;
				else if (!strcmp(t, "quic")) u->proto |= AP_QUIC;
				else if (strcmp(t, "any")) {
					snprintf(err, en, "proto=%s: udp, tcp, quic or any", t);
					return -1;
				}
		} else if (!strcmp(k, "port")) {
			char *sv = NULL, *t;

			for (t = strtok_r(v, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
				struct app_range pr;

				if (u->nports >= sizeof(u->port_lo) / sizeof(u->port_lo[0]) || range(t, &pr) < 0 ||
				    pr.hi > 65535) {
					snprintf(err, en, "port=%s: up to %zu ports or ranges", t,
						 sizeof(u->port_lo) / sizeof(u->port_lo[0]));
					return -1;
				}
				u->port_lo[u->nports] = (uint16_t)pr.lo;
				u->port_hi[u->nports++] = (uint16_t)pr.hi;
			}
		} else if (num(v, &x) < 0) {
			snprintf(err, en, "%s=%s: a number", k, v);
			return -1;
		} else if (!strcmp(k, "steady") && x >= 1 && x <= 100) u->steady = (uint8_t)x;
		else if (!strcmp(k, "looks") && x >= 1 && x <= APP_HIST) u->looks = (uint8_t)x;
		else if (!strcmp(k, "conf") && x >= 1 && x <= 99) u->conf = (uint8_t)x;
		else if (!strcmp(k, "named") && x <= 1) u->named = (uint8_t)x;
		else if (!strcmp(k, "iot") && x <= 1) u->iot = (uint8_t)x;
		else {
			snprintf(err, en, "%s=%s: not known (or out of range)", k, v);
			return -1;
		}
	}
	r->nrule++;
	return 0;
}

static int family_line(struct app_rules *r, char **tok, int nt, char *err, size_t en)
{
	struct app_family *f;
	char *p;
	int i, n;

	if (nt < 4) {
		snprintf(err, en, "family <Name> <Kind>[,<Kind>] <suffix>...");
		return -1;
	}
	if (r->nfam >= APP_FAMS || r->nsuf + nt - 3 > APP_SUFFIX) {
		snprintf(err, en, "more than %d families or %d names", APP_FAMS, APP_SUFFIX);
		return -1;
	}
	f = &r->fam[r->nfam];
	memset(f, 0, sizeof(*f));
	if (strlen(tok[1]) >= sizeof(f->name)) {
		snprintf(err, en, "%s: a name of up to %zu characters", tok[1], sizeof(f->name) - 1);
		return -1;
	}
	snprintf(f->name, sizeof(f->name), "%s", tok[1]);
	for (p = f->name; *p; p++)
		if (*p == '_')
			*p = ' ';
	if ((n = kinds_list(tok[2], f->kinds, APP_FAM_KINDS)) < 1) {
		snprintf(err, en, "%s: up to %d kinds, comma-separated", tok[2], APP_FAM_KINDS);
		return -1;
	}
	for (i = 3; i < nt; i++) {
		char *s = tok[i];

		while (*s == '.')
			s++;
		if (!*s || strlen(s) >= sizeof(r->suf[0].s)) {
			snprintf(err, en, "%s: a server name suffix of up to %zu characters", tok[i], sizeof(r->suf[0].s) - 1);
			return -1;
		}
		for (p = s; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		snprintf(r->suf[r->nsuf].s, sizeof(r->suf[0].s), "%s", s);
		r->suf[r->nsuf++].fam = (uint8_t)r->nfam;
	}
	r->nfam++;
	return 0;
}

int app_rules_parse(struct app_rules *r, const char *text)
{
	const char *p = text ? text : "";
	int line = 0, bad = 0;

	memset(r, 0, sizeof(*r));
	r->browse_new = 4;
	while (*p) {
		char buf[512], *tok[48], *sv = NULL, *t, *h, err[120] = "";
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		int nt = 0, rc = 0;

		line++;
		if (l >= sizeof(buf)) {
			if (!bad++)
				snprintf(r->err, sizeof(r->err), "line %d: longer than %zu characters", line, sizeof(buf) - 1);
			p += l + (e ? 1 : 0);
			continue;
		}
		memcpy(buf, p, l);
		buf[l] = '\0';
		p += l + (e ? 1 : 0);
		if ((h = strchr(buf, '#')))
			*h = '\0';
		for (t = strtok_r(buf, " \t\r", &sv); t && nt < 48; t = strtok_r(NULL, " \t\r", &sv))
			tok[nt++] = t;
		if (!nt)
			continue;
		if (!strcmp(tok[0], "kind"))
			rc = rule_line(r, tok, nt, err, sizeof(err));
		else if (!strcmp(tok[0], "family"))
			rc = family_line(r, tok, nt, err, sizeof(err));
		else if (!strcmp(tok[0], "browse") && nt == 2) {
			uint32_t x;

			if (num(tok[1], &x) < 0 || x < 1 || x > 1000) {
				snprintf(err, sizeof(err), "browse <1-1000>");
				rc = -1;
			} else
				r->browse_new = (int)x;
		} else {
			snprintf(err, sizeof(err), "%s: kind, family or browse", tok[0]);
			rc = -1;
		}
		if (rc < 0 && !bad++)
			snprintf(r->err, sizeof(r->err), "line %d: %s", line, err);
	}
	return r->nrule ? r->nrule : -1;
}

/* name ends with suffix (s long), on a label boundary */
static int ends(const char *name, size_t l, const char *suffix, size_t s)
{
	return l >= s && !strncasecmp(name + l - s, suffix, s) && (l == s || name[l - s - 1] == '.');
}

int app_family_of(const struct app_rules *r, const char *name)
{
	size_t l, best = 0;
	int i, fam = -1;

	if (!r || !name || !*name)
		return -1;
	/* "x.zoom.us (nearby)": the name the home looked up for the same /24 */
	l = strcspn(name, " ");
	while (l && name[l - 1] == '.')
		l--;
	for (i = 0; i < r->nsuf; i++) {
		size_t s = strlen(r->suf[i].s);

		if (s > best && ends(name, l, r->suf[i].s, s)) {
			best = s;
			fam = r->suf[i].fam;
		}
	}
	return fam;
}

/* ---------- the traits ---------- */

void app_features(const struct app_flow *f, int looks, struct app_feat *o)
{
	uint64_t ms = 0, ub = 0, db = 0, up = 0, dp = 0;
	uint32_t hi = 0, lo = 0xffffffffu;
	int i, from, act = 0, n = 0;

	memset(o, 0, sizeof(*o));
	if (looks > f->n)
		looks = f->n;
	from = f->n - looks;
	for (i = from; i < f->n; i++) {
		const struct app_iv *v = &f->iv[i];
		uint32_t pps;

		if (!v->ms)
			continue;
		n++;
		ms += v->ms;
		ub += v->up_b;
		db += v->down_b;
		up += v->up_pk;
		dp += v->down_pk;
		/* busy: at least 1 kB a second either way */
		if ((uint64_t)(v->up_b + (uint64_t)v->down_b) * 1000 >= (uint64_t)v->ms * 1000)
			act++;
		pps = (uint32_t)(((uint64_t)v->up_pk + v->down_pk) * 1000 / v->ms);
		if (pps > hi)
			hi = pps;
		if (pps < lo)
			lo = pps;
	}
	o->looks = n;
	if (!n || !ms)
		return;
	o->down_kbps = (uint32_t)(db * 8 / ms);
	o->up_kbps = (uint32_t)(ub * 8 / ms);
	o->pps = (uint32_t)((up + dp) * 1000 / ms);
	if (db >= ub)
		o->pkt = dp ? (uint32_t)(db / dp) : 0;
	else
		o->pkt = up ? (uint32_t)(ub / up) : 0;
	o->ratio = db ? (uint32_t)(ub * 100 / db > 100000 ? 100000 : ub * 100 / db) : (ub ? 100000 : 0);
	o->duty = (uint32_t)(act * 100 / n);
	o->spread = hi ? (hi - lo) * 100 / hi : 0;
}

static int in(const struct app_range *r, uint32_t v)
{
	return v >= r->lo && v <= r->hi;
}

static int proto_bit(const struct app_flow *f)
{
	return f->proto == 17 ? (f->port == 443 ? AP_QUIC : AP_UDP) : f->proto == 6 ? AP_TCP : 0;
}

static int match(const struct app_rule *u, const struct app_flow *f, const struct app_feat *x)
{
	int i, ok;

	if (f->n < u->looks || x->looks < u->looks)
		return 0;
	if (u->proto && !(u->proto & proto_bit(f)))
		return 0;
	if ((u->named && f->fam < 0) || (u->iot && !f->iot))
		return 0;
	if (u->nports) {
		for (i = 0, ok = 0; i < u->nports && !ok; i++)
			ok = f->port >= u->port_lo[i] && f->port <= u->port_hi[i];
		if (!ok)
			return 0;
	}
	if (((u->cond & RC_DOWN) && !in(&u->down, x->down_kbps)) || ((u->cond & RC_UP) && !in(&u->up, x->up_kbps)) ||
	    ((u->cond & RC_PPS) && !in(&u->pps, x->pps)) || ((u->cond & RC_PKT) && !in(&u->pkt, x->pkt)) ||
	    ((u->cond & RC_RATIO) && !in(&u->ratio, x->ratio)) || ((u->cond & RC_DUTY) && !in(&u->duty, x->duty)) ||
	    ((u->cond & RC_LIFE) && !in(&u->life, f->age_s)))
		return 0;
	return !u->steady || x->spread <= u->steady;
}

static int fam_has(const struct app_family *fa, int kind)
{
	int i;

	for (i = 0; i < APP_FAM_KINDS && fa->kinds[i]; i++)
		if (fa->kinds[i] == kind)
			return 1;
	return 0;
}

int app_family_has(const struct app_rules *r, int fam, int kind)
{
	return fam >= 0 && fam < r->nfam && fam_has(&r->fam[fam], kind);
}

/* kinds a name alone may give (not real time: a call app's name on a web connection is not a call) */
static int passive(int kind)
{
	return kind == AK_STREAM || kind == AK_DOWNLOAD || kind == AK_IOT || kind == AK_BROWSE;
}

int app_classify(const struct app_rules *r, const struct app_flow *f, int *conf)
{
	struct app_feat feat[APP_HIST + 1];
	unsigned char have[APP_HIST + 1] = { 0 };
	const struct app_family *fa = f->fam >= 0 && f->fam < r->nfam ? &r->fam[f->fam] : NULL;
	int i, pass;

	*conf = 0;
	if (!f->n)
		return AK_NONE;
	/* with a family: its kinds first (surer), then any (less sure: the name says otherwise) */
	for (pass = fa ? 0 : 1; pass < 2; pass++)
		for (i = 0; i < r->nrule; i++) {
			const struct app_rule *u = &r->rule[i];
			int L = u->looks;

			if (pass == 0 && !fam_has(fa, u->kind))
				continue;
			if (!have[L]) {
				app_features(f, L, &feat[L]);
				have[L] = 1;
			}
			if (!match(u, f, &feat[L]))
				continue;
			if (pass == 0)
				*conf = u->conf + 25 > 98 ? 98 : u->conf + 25;
			else
				*conf = fa ? (u->conf > 20 ? u->conf - 10 : 10) : u->conf;
			return u->kind;
		}
	/* the name alone: a stream, a download, a smart-home device whose rule doesn't match now (idle while its
	   buffer is full, say); over TCP or QUIC, once it has a few looks (plain UDP is real time or nothing) */
	if (fa && proto_bit(f) != AP_UDP && f->n >= 3)
		for (i = 0; i < APP_FAM_KINDS && fa->kinds[i]; i++)
			if (passive(fa->kinds[i])) {
				*conf = 60;
				return fa->kinds[i];
			}
	return AK_NONE;
}

/* ---------- how it went ---------- */

static const char *const what_name[AW_N] = {
	"None", "Freeze", "Stutter", "Rebuffer", "LowerResolution", "Lag", "Slow", "Offline",
};

const char *app_what_name(int w) { return w >= 0 && w < AW_N ? what_name[w] : "None"; }
const char *app_state_name(int s) { return s == AQ_GOOD ? "Good" : s == AQ_FAIR ? "Fair" : "Bad"; }

int app_emodel(int one_way_ms, int loss10, int *rr)
{
	/* ITU-T G.107: R = 93.2 - Id - Ie,eff (no echo, no quantisation); Ie 0 (a modern codec), Bpl 20 */
	double d = one_way_ms < 0 ? 0 : one_way_ms, ppl = loss10 < 0 ? 0 : loss10 / 10.0, id, ie, R, mos;

	id = 0.024 * d + (d > 177.3 ? 0.11 * (d - 177.3) : 0);
	ie = 95.0 * ppl / (ppl + 20.0);
	R = 93.2 - id - ie;
	if (R < 0)
		R = 0;
	if (R > 100)
		R = 100;
	mos = R <= 0 ? 1.0 : 1 + 0.035 * R + 7e-6 * R * (R - 60) * (100 - R);
	if (mos > 4.5)
		mos = 4.5;
	if (mos < 1)
		mos = 1;
	if (rr)
		*rr = (int)(R + 0.5);
	return (int)(mos * 10 + 0.5);
}

static int cap(int v, int hi)
{
	return v > hi ? hi : v;
}

/* the whole path: the internet, then the device's Wi-Fi hop */
static int path_rtt(const struct app_win *w)
{
	if (w->rtt_ms < 0 && w->wifi_rtt_ms < 0)
		return -1;
	return (w->rtt_ms > 0 ? w->rtt_ms : 0) + (w->wifi_rtt_ms > 0 ? w->wifi_rtt_ms : 0);
}

static int path_loss10(const struct app_win *w)
{
	if (w->loss10 < 0 && w->wifi_loss_pct < 0)
		return -1;
	return cap((w->loss10 > 0 ? w->loss10 : 0) + (w->wifi_loss_pct > 0 ? w->wifi_loss_pct * 10 : 0), 1000);
}

static void call_q(const struct app_win *w, struct app_q *q)
{
	int rtt = path_rtt(w), loss = path_loss10(w), jit = w->jitter_ms > 0 ? w->jitter_ms : 0;
	uint32_t gap = w->gap_ms;

	q->conf = 35 + (w->named ? 10 : 0) + (w->class_conf >= 70 ? 5 : 0);
	if (rtt >= 0 || loss >= 0) {
		/* mouth to ear: half the round trip, a jitter buffer of twice the jitter, 20 ms of codec */
		int one_way = (rtt > 0 ? rtt / 2 : 0) + 2 * jit + 20;
		int k = w->kind == AK_VOICE ? loss : loss * 2;  /* video shows loss more than voice (a lower Bpl) */

		q->mos10 = app_emodel(one_way, k < 0 ? 0 : cap(k, 1000), &q->r);
		q->conf += 25;
	}
	if (w->ref_pps)
		q->conf += 20;
	/* a voice call goes quiet by itself while nobody talks (no packets in silence): its gaps count only when the
	   network shows trouble then too. Video doesn't stop by itself */
	if (w->kind == AK_VOICE && !(loss >= 10 || jit >= 30 || w->wifi_loss_pct >= 3))
		gap = 0;
	if (gap >= 2000) {
		q->state = AQ_BAD;
		q->what = AW_FREEZE;
		q->seconds = (int)((gap + 500) / 1000);
		if (q->conf < 60)
			q->conf = 60;
	} else if (q->mos10 && q->mos10 < 36) {
		q->state = AQ_BAD;
		q->what = AW_STUTTER;
		q->seconds = (int)(w->ms / 1000);
	} else if ((q->mos10 && q->mos10 < 40) || gap >= 500)
		q->state = AQ_FAIR;
}

static void cloud_q(const struct app_win *w, struct app_q *q)
{
	int rtt = path_rtt(w), loss = path_loss10(w);

	q->conf = 35 + (w->named ? 15 : 0) + (w->ref_kbps ? 15 : 0) + (rtt >= 0 ? 10 : 0);
	if (w->gap_ms >= 1000) {
		q->state = AQ_BAD;
		q->what = AW_FREEZE;
		q->seconds = (int)((w->gap_ms + 500) / 1000);
	} else if (w->ref_kbps && w->active >= w->looks && (uint64_t)w->down_kbps * 100 < (uint64_t)w->ref_kbps * 60) {
		q->state = AQ_BAD;
		q->what = AW_LOWRES;
		q->seconds = (int)(w->ms / 1000);
	} else if (rtt > 100 || loss >= 10) {
		q->state = AQ_BAD;
		q->what = AW_LAG;
	} else if (rtt > 60 || loss >= 5)
		q->state = AQ_FAIR;
}

static void game_q(const struct app_win *w, struct app_q *q)
{
	int rtt = path_rtt(w), loss = path_loss10(w);
	int usual = w->rtt_usual_ms > 0 ? w->rtt_usual_ms : 0;
	int spike = rtt >= 0 && rtt >= (usual + 60 > 100 ? usual + 60 : 100);

	q->conf = 35 + (w->named ? 15 : 0) + (rtt >= 0 ? 20 : 0) + (w->ref_pps ? 10 : 0);
	if (spike || loss >= 20 || w->jitter_ms >= 40 || w->gap_ms >= 500) {
		q->state = AQ_BAD;
		q->what = AW_LAG;
		q->seconds = (int)((w->gap_ms + 500) / 1000);
	} else if (rtt > 60 || loss >= 5)
		q->state = AQ_FAIR;
}

#define STREAM_BUFFER_S 10              /* a player's buffer when the trouble starts (they keep 10-60 s) */

static void stream_q(struct app_win *w, struct app_q *q)
{
	double T = w->ms / 1000.0, share;
	int busy = w->looks && w->active * 100 >= w->looks * 90;

	q->conf = 30 + (w->named ? 20 : 0) + (w->ref_kbps ? 20 : 0);
	if (!w->ref_kbps || !w->looks)
		return;                 /* its bitrate isn't known yet */
	share = (double)w->down_kbps / w->ref_kbps;
	if (!busy) {
		/* it paused fetching: its buffer was full */
		w->drain_s = 0;
		w->counted_s = 0;
		return;
	}
	if (share < 1)
		w->drain_s += (float)(T * (1 - share));
	else {
		w->drain_s -= (float)(T * (share - 1));
		if (w->drain_s < 0)
			w->drain_s = 0;
	}
	if (w->drain_s - STREAM_BUFFER_S > w->counted_s) {
		q->state = AQ_BAD;
		q->what = AW_REBUFFER;
		q->seconds = (int)(w->drain_s - STREAM_BUFFER_S - w->counted_s + 0.5f);
		if (q->seconds < 1)
			q->seconds = 1;
		w->counted_s += q->seconds;
		q->conf += 10;
	} else if (share < 0.6) {
		q->state = AQ_BAD;
		q->what = AW_LOWRES;
		q->seconds = (int)T;
	} else if (share < 0.8)
		q->state = AQ_FAIR;
}

static void download_q(const struct app_win *w, struct app_q *q)
{
	q->conf = 30 + (w->named ? 15 : 0) + (w->ref_kbps ? 15 : 0);
	if (w->ref_kbps && w->looks && w->active >= w->looks && (uint64_t)w->down_kbps * 100 < (uint64_t)w->ref_kbps * 30) {
		q->state = AQ_BAD;
		q->what = AW_SLOW;
		q->seconds = (int)(w->ms / 1000);
	}
}

static void browse_q(const struct app_win *w, struct app_q *q)
{
	int rtt = path_rtt(w), usual = w->rtt_usual_ms > 0 ? w->rtt_usual_ms : 0, loss = path_loss10(w);

	q->conf = 30 + (rtt >= 0 ? 20 : 0) + (w->dns_ms >= 0 ? 10 : 0);
	if ((rtt >= 0 && rtt >= (usual * 2 + 20 > 150 ? usual * 2 + 20 : 150)) || w->dns_fail_pct >= 10 ||
	    w->dns_ms >= 300 || loss >= 50) {
		q->state = AQ_BAD;
		q->what = AW_SLOW;
		q->seconds = (int)(w->ms / 1000);
	} else if (rtt >= 100 || w->dns_ms >= 150)
		q->state = AQ_FAIR;
}

void app_quality(struct app_win *w, struct app_q *q)
{
	memset(q, 0, sizeof(*q));
	q->state = AQ_GOOD;
	switch (w->kind) {
	case AK_VOICE: case AK_VIDEO: case AK_SCREEN: call_q(w, q); break;
	case AK_CLOUDGAME: cloud_q(w, q); break;
	case AK_GAME: game_q(w, q); break;
	case AK_STREAM: stream_q(w, q); break;
	case AK_DOWNLOAD: download_q(w, q); break;
	case AK_BROWSE: browse_q(w, q); break;
	case AK_IOT:
		q->conf = 45;
		if (w->offline_s >= 30) {
			q->state = AQ_BAD;
			q->what = AW_OFFLINE;
			q->seconds = w->offline_s;
		}
		break;
	}
	if (q->conf > 95)
		q->conf = 95;
}

/* ---------- why ---------- */

static const char *const cause_name[AC_N] = {
	"Unknown", "FarEnd", "WiFiCongestion", "WeakSignal", "WiFiDrops", "WiFi", "LineFull", "Line", "ISP", "DNS",
	"AIGWLimit", "Device", "GatewayOff",
};

const char *app_cause_name(int c) { return c >= 0 && c < AC_N ? cause_name[c] : "Unknown"; }

int app_cause_home(int c)
{
	return c == AC_WIFI_BUSY || c == AC_WIFI_WEAK || c == AC_WIFI_DROPS || c == AC_WIFI || c == AC_LINE_FULL ||
	       c == AC_AIGW;
}

int app_cause_of(const struct app_evidence *e)
{
	static const struct { const char *code; uint8_t cause; } codes[] = {
		{ "WIFI_CONGESTION", AC_WIFI_BUSY }, { "WIFI_COVERAGE", AC_WIFI_WEAK }, { "WIFI_HOPPING", AC_WIFI_DROPS },
		{ "WIFI_DROPS", AC_WIFI_DROPS }, { "WIFI_RADIO", AC_WIFI }, { "WIFI_DEVICE", AC_WIFI },
		{ "WIFI_PROBLEM", AC_WIFI }, { "MESH_BACKHAUL", AC_WIFI }, { "LINE_FULL", AC_LINE_FULL },
		{ "LINE_LOSS", AC_LINE }, { "LINE_SLOW", AC_LINE }, { "LINE_DOWN", AC_LINE }, { "NO_ROUTE", AC_LINE },
		{ "LINE_SLOW_SPEED", AC_LINE }, { "ISP_LOSS", AC_ISP }, { "ISP_SLOW", AC_ISP }, { "ISP_DOWN", AC_ISP },
		{ "AREA_OUTAGE", AC_ISP }, { "DNS_FAILING", AC_DNS }, { "DNS_SLOW", AC_DNS }, { "DEVICE_LIMITED", AC_AIGW },
		{ "DEVICE_BLOCKED", AC_AIGW }, { "DEVICE_OFFLINE", AC_DEVICE }, { "GATEWAY_OFF", AC_GATEWAY },
	};
	size_t i;
	int usual;

	for (i = 0; e->code && i < sizeof(codes) / sizeof(codes[0]); i++)
		if (!strcmp(e->code, codes[i].code))
			return codes[i].cause;
	/* the timeline found nothing for the stretch as a whole: the moment's own evidence */
	if (e->wifi_loss_pct >= 10)
		return AC_WIFI;
	if (e->signal_dbm && e->signal_dbm <= -75)
		return AC_WIFI_WEAK;
	if (e->line_full)
		return AC_LINE_FULL;
	usual = e->rtt_usual_ms > 0 ? e->rtt_usual_ms : 0;
	if ((e->rtt_ms >= 0 && e->rtt_ms >= (usual * 2 > 150 ? usual * 2 : 150)) || e->loss10 >= 20)
		return AC_ISP;
	return e->recorded ? AC_FAR_END : AC_UNKNOWN;
}

void app_cause_words(int c, const struct app_evidence *e, char *buf, size_t n)
{
	const char *band = e->band == 2 ? "2.4 GHz " : e->band == 5 ? "5 GHz " : "";
	char sig[24] = "";

	if (e->signal_dbm)
		snprintf(sig, sizeof(sig), " (%d dBm)", e->signal_dbm);
	switch (c) {
	case AC_WIFI_BUSY:
		snprintf(buf, n, "the %sWi-Fi channel was congested", band);
		break;
	case AC_WIFI_WEAK:
		snprintf(buf, n, "its Wi-Fi signal was weak%s", sig);
		break;
	case AC_WIFI_DROPS:
		snprintf(buf, n, "it kept dropping off Wi-Fi or switching bands");
		break;
	case AC_WIFI:
		if (e->wifi_loss_pct > 0)
			snprintf(buf, n, "its %sWi-Fi was losing packets (%d%%)", band, e->wifi_loss_pct);
		else
			snprintf(buf, n, "the %sWi-Fi was poor", band);
		break;
	case AC_LINE_FULL:
		if (e->top && *e->top)
			snprintf(buf, n, "the line was full (%s downloading, %.1f Mbit/s)", e->top, e->top_kbps / 1000.0);
		else
			snprintf(buf, n, "the line was full");
		break;
	case AC_LINE:
		snprintf(buf, n, "the line to the ISP was losing packets or slow");
		break;
	case AC_ISP:
		if (e->rtt_ms > 0)
			snprintf(buf, n, "the ISP's network was slow or losing packets (the internet answered in %d ms)", e->rtt_ms);
		else
			snprintf(buf, n, "the ISP's network was slow or losing packets");
		break;
	case AC_DNS:
		snprintf(buf, n, "names were slow to look up, or failed");
		break;
	case AC_AIGW:
		snprintf(buf, n, "AIGW was limiting the device (a decision in effect)");
		break;
	case AC_DEVICE:
		snprintf(buf, n, "the device was offline");
		break;
	case AC_GATEWAY:
		snprintf(buf, n, "the gateway was off");
		break;
	case AC_FAR_END:
		snprintf(buf, n, "nothing in the home or on the line was wrong then, so likely the far end (the app's "
			 "servers, or the other side)");
		break;
	default:
		snprintf(buf, n, "AIGW has no record of the line then");
	}
}

void app_what_words(int kind, int what, int seconds, char *buf, size_t n)
{
	char s[32] = "";

	if (seconds > 0) {
		if (seconds < 90)
			snprintf(s, sizeof(s), " for about %d s", seconds);
		else
			snprintf(s, sizeof(s), " for about %d min", (seconds + 30) / 60);
	}
	switch (what) {
	case AW_FREEZE: snprintf(buf, n, "froze%s", s); break;
	case AW_STUTTER: snprintf(buf, n, "was choppy%s", s); break;
	case AW_REBUFFER: snprintf(buf, n, "likely stopped to buffer%s", s); break;
	case AW_LOWRES: snprintf(buf, n, "likely dropped to a lower picture quality%s", s); break;
	case AW_LAG: snprintf(buf, n, kind == AK_CLOUDGAME ? "lagged%s" : "had lag spikes%s", s); break;
	case AW_SLOW: snprintf(buf, n, kind == AK_BROWSE ? "was slow (pages likely loaded slowly)%s" : "slowed down%s", s); break;
	case AW_OFFLINE: snprintf(buf, n, "lost its connection%s", s); break;
	default: snprintf(buf, n, "was fine");
	}
}

void app_moment_text(const struct app_moment_words *m, char *buf, size_t n)
{
	char who[160], when[24], what[96], cause[200], fix[220] = "", since[120] = "";
	int g = app_group_of(m->kind);
	size_t l;

	if (m->device && *m->device)
		snprintf(who, sizeof(who), "The %s on %s%s%s%s", app_kind_words(m->kind), m->device, m->app ? " (" : "",
			 m->app ? m->app : "", m->app ? ")" : "");
	else
		snprintf(who, sizeof(who), "A %s%s%s%s", app_kind_words(m->kind), m->app ? " (" : "", m->app ? m->app : "",
			 m->app ? ")" : "");
	if (m->kind == AK_BROWSE)
		snprintf(who, sizeof(who), "Browsing on %s", m->device && *m->device ? m->device : "a device");
	why_clock(m->t, m->now, when, sizeof(when));
	app_what_words(m->kind, m->what, m->seconds, what, sizeof(what));
	app_cause_words(m->cause, m->ev, cause, sizeof(cause));
	if (m->fix && *m->fix) {
		char fw[24];

		why_clock(m->fix_t, m->now, fw, sizeof(fw));
		snprintf(fix, sizeof(fix), "; then at %s %s", fw, m->fix);
	}
	if (m->since_ok + m->since_bad > 0) {
		const char *join = m->fix && *m->fix ? ", and" : ";";

		if (m->since_bad)
			snprintf(since, sizeof(since), "%s %d of the %d %s since had trouble too", m->fix && *m->fix ? ", but" : ";",
				 m->since_bad, m->since_ok + m->since_bad, app_group_words(g));
		else if (m->since_ok == 1)
			snprintf(since, sizeof(since), "%s the %s since was fine", join, g == AGR_CALLS ? "call" :
				 app_kind_words(m->kind));
		else
			snprintf(since, sizeof(since), "%s the %d %s since were fine", join, m->since_ok,
				 g == AGR_CALLS ? "calls" : g == AGR_GAMING ? "games" : g == AGR_STREAMING ? "streams" :
				 app_group_words(g));
	}
	l = (size_t)snprintf(buf, n, "%s at %s %s: %s%s%s.", who, when, what, cause, fix, since);
	(void)l;
}

int app_score(uint32_t minutes, uint32_t bad, uint32_t min_minutes)
{
	if (!minutes || minutes < min_minutes)
		return -1;
	if (bad > minutes)
		bad = minutes;
	return (int)((uint64_t)(minutes - bad) * 100 / minutes);
}
