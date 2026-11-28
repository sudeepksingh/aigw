/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW per-app experience: how each video call, stream and game went, in the
 * app's own terms, and why.
 *
 * Customers don't call about packet loss. They call because "my video call
 * froze" or "Netflix keeps buffering". The gateway sees every connection's
 * counters (agd/flow.c) and the name the device looked up or sent in its
 * handshake, so it can tell a video call from a stream from a game, estimate
 * how it went for the person using it, and say why, from what the timeline
 * (ag/why.h), Self-Healing Wi-Fi and coverage already know.
 *
 *   recognised   a connection's kind from its traits over its last looks:
 *                UDP, TCP or QUIC, its rate each way, packets a second, mean
 *                packet size, the up/down ratio, how steady its rate is, how
 *                much of the time it is busy. A table of rules does it, first
 *                match wins, and a table of server-name families (zoom.us is
 *                Zoom, nflxvideo.net is Netflix) makes it surer. Both come
 *                from a text file an ISP or OEM can change without new code
 *                (apps.rules, the built-in text below is the default).
 *   estimated    each minute of a session, in the app's terms: a call's
 *                mean opinion score (ITU-T G.107 E-model) from the delay,
 *                jitter and loss, and the seconds it froze from the
 *                connection's own gaps; a stream's likely rebuffering or lower
 *                resolution from what came in against its bitrate; a game's
 *                lag spikes. Each with a confidence.
 *   explained    a bad moment's cause, from the timeline's verdict for that
 *                device then (Wi-Fi congestion, a weak signal, the line full
 *                and by whom, the ISP) and, when nothing in the home or on the
 *                line was wrong, the far end. What AIGW did next and how the
 *                same kind of session went since.
 *
 * Privacy: only a kind and a family name ("Zoom") are kept, never a server
 * name, an address, a URL or any content (the gateway never sees URLs).
 *
 * Pure (no I/O): agd (agd/appqoe.c) reads the counters, keeps the sessions and
 * asks the timeline. Unit test: tests/test_appqoe.c.
 */
#ifndef AG_APPQOE_H
#define AG_APPQOE_H

#include <stddef.h>
#include <stdint.h>

#define APP_HIST    6                   /* looks kept for each connection */
#define APP_RULES   32
#define APP_FAMS    64
#define APP_SUFFIX  192
#define APP_FAM_KINDS 4

enum app_kind {
	AK_NONE, AK_VOICE, AK_VIDEO, AK_SCREEN, AK_STREAM, AK_CLOUDGAME, AK_GAME, AK_DOWNLOAD, AK_BROWSE, AK_IOT, AK_N
};
/* what a score is kept for */
enum app_group { AGR_CALLS, AGR_STREAMING, AGR_GAMING, AGR_DOWNLOADS, AGR_BROWSING, AGR_SMARTHOME, AGR_N };

const char *app_kind_name(int k);       /* "VideoCall" (the data model, the rules file) */
const char *app_kind_words(int k);      /* "video call" */
int app_kind_of(const char *name);      /* AK_NONE when not a kind */
int app_group_of(int kind);
const char *app_group_name(int g);      /* "Calls" */
const char *app_group_words(int g);     /* "calls" */
int app_group_by_name(const char *s);   /* "calls", "Calls", "call", "streaming", ...; -1 */

/* ---------- the rules ---------- */

struct app_range { uint32_t lo, hi; };  /* hi UINT32_MAX: no upper bound */

#define AP_UDP  1
#define AP_TCP  2
#define AP_QUIC 4                       /* UDP 443 */

struct app_rule {
	uint8_t kind, conf, looks;      /* looks: the last ones judged (3) */
	uint8_t proto;                  /* AP_* bits, 0 any */
	uint8_t named, iot;             /* only when a family named it; only a smart-home device */
	uint8_t steady;                 /* the packet rate's spread at most this % (0: any) */
	uint8_t nports;
	uint16_t cond;                  /* which ranges are set (bits, core/libag/appqoe.c) */
	struct app_range down, up, pps, pkt, ratio, duty, life;
	uint16_t port_lo[6], port_hi[6];
};

struct app_family {
	char name[24];                  /* "Netflix" ("Prime_Video" in the file is "Prime Video") */
	uint8_t kinds[APP_FAM_KINDS];   /* in the file's order; AK_NONE after the last */
};

struct app_rules {
	int nrule, nfam, nsuf;
	int browse_new;                 /* new web connections a minute that are browsing (3) */
	struct app_rule rule[APP_RULES];
	struct app_family fam[APP_FAMS];
	struct { char s[40]; uint8_t fam; } suf[APP_SUFFIX];
	char err[160];                  /* the first line it refused, why */
};

/* the built-in rules (the text of config/apps.rules) */
const char *app_rules_default(void);
/* the text's rules (lines it can't read are skipped, the first one is in err); the count of rules, -1 none */
int app_rules_parse(struct app_rules *r, const char *text);
/* the family a server name belongs to (longest suffix, on a label boundary; "x.zoom.us (nearby)" too); -1 none */
int app_family_of(const struct app_rules *r, const char *name);
/* the family is one of the kind's (a Netflix name on a call's connection is not the call's app) */
int app_family_has(const struct app_rules *r, int fam, int kind);

/* one look at a connection: what it did since the look before */
struct app_iv {
	uint32_t ms;
	uint32_t up_pk, down_pk;        /* packets from the device, to it */
	uint32_t up_b, down_b;
};

struct app_flow {
	uint8_t proto;                  /* 17 UDP, 6 TCP */
	uint16_t port;                  /* the far end's */
	uint32_t age_s;                 /* since the connection began */
	int fam;                        /* app_family_of its name; -1 none (or names not used) */
	int iot;                        /* the device is a smart-home one (its guessed type, or the iot zone) */
	int n;
	struct app_iv iv[APP_HIST];     /* oldest first */
};

/* what its last `looks` show */
struct app_feat {
	uint32_t down_kbps, up_kbps;    /* means */
	uint32_t pps;                   /* packets a second, both ways */
	uint32_t pkt;                   /* the mean packet of the busier way, bytes */
	uint32_t ratio;                 /* up bytes per 100 down bytes */
	uint32_t duty;                  /* % of the looks with traffic down (or up) */
	uint32_t spread;                /* (busiest - quietest) * 100 / busiest, in packets a second */
	int looks;
};
void app_features(const struct app_flow *f, int looks, struct app_feat *o);
/* its kind (AK_NONE: none of the rules), with a confidence (%) */
int app_classify(const struct app_rules *r, const struct app_flow *f, int *conf);

/* ---------- how it went ---------- */

enum app_state { AQ_GOOD, AQ_FAIR, AQ_BAD };
enum app_what { AW_NONE, AW_FREEZE, AW_STUTTER, AW_REBUFFER, AW_LOWRES, AW_LAG, AW_SLOW, AW_OFFLINE, AW_N };

/* one window of a session (a minute: why.interval) */
struct app_win {
	int kind;
	uint32_t ms;                    /* its length */
	int looks, active;              /* looks in it; with traffic */
	uint32_t down_kbps, up_kbps;    /* means over it */
	uint32_t down_pps;
	uint32_t ref_kbps, ref_pps;     /* the session's own usual, from its good windows; 0 not known yet */
	uint32_t gap_ms;                /* stalled: looks far below its usual between usual ones (agd/appqoe.c) */
	int offline_s;                  /* smart home: away that long before it came back; 0 */
	int named;                      /* a family named it */
	int class_conf;                 /* how sure the kind is (%) */
	/* the network meanwhile; -1 not known */
	int rtt_ms;                     /* the internet's round trip (the timeline's sample) */
	int rtt_usual_ms;               /* ... its normal at this hour */
	int jitter_ms;                  /* its change from the sample before */
	int loss10;                     /* lost, tenths of % */
	int wifi_rtt_ms, wifi_loss_pct; /* the device's own Wi-Fi hop (Self-Healing Wi-Fi's probes) */
	int dns_ms, dns_fail_pct;
	/* streaming: seconds of video played beyond what came in, carried from window to window (in and out) */
	float drain_s;
	int counted_s;                  /* ... of which already counted as rebuffering (in and out) */
};

struct app_q {
	int state;                      /* AQ_* */
	int what;                       /* AW_* */
	int seconds;                    /* about how long (0: not known, lag spikes) */
	int mos10;                      /* calls: mean opinion score x10 (10-45); 0 */
	int r;                          /* calls: the E-model's R (0-100) */
	int conf;                       /* % */
};

/* ITU-T G.107 (simplified): a call's R and mean opinion score (x10) from one-way delay and loss */
int app_emodel(int one_way_ms, int loss10, int *r);
void app_quality(struct app_win *w, struct app_q *q);
const char *app_what_name(int w);       /* Freeze, Stutter, Rebuffer, LowerResolution, Lag, Slow, Offline */
const char *app_state_name(int s);      /* Good, Fair, Bad */

/* ---------- why ---------- */

enum app_cause {
	AC_UNKNOWN, AC_FAR_END, AC_WIFI_BUSY, AC_WIFI_WEAK, AC_WIFI_DROPS, AC_WIFI, AC_LINE_FULL, AC_LINE, AC_ISP,
	AC_DNS, AC_AIGW, AC_DEVICE, AC_GATEWAY, AC_N
};

struct app_evidence {
	const char *code;               /* the timeline's verdict for the device around then (ag/why.h); NULL */
	int recorded;                   /* the timeline has samples of then */
	int band;                       /* the device's Wi-Fi band then (2, 5); 0 wired or not known */
	int signal_dbm;                 /* its signal; 0 not known */
	int wifi_loss_pct;              /* its Wi-Fi hop lost; -1 not known */
	int line_full;                  /* the line was full meanwhile */
	const char *top;                /* ... by whom ("TV"); NULL */
	uint32_t top_kbps;
	int rtt_ms, rtt_usual_ms, loss10;       /* the internet meanwhile; -1 */
};
int app_cause_of(const struct app_evidence *e);
const char *app_cause_name(int c);      /* WiFiCongestion, WeakSignal, ..., FarEnd (the data model) */
/* "the 2.4 GHz channel was congested" */
void app_cause_words(int c, const struct app_evidence *e, char *buf, size_t n);
/* the home can do something about it (Wi-Fi, the line full, AIGW's own limit) */
int app_cause_home(int c);

/* a bad moment in words */
struct app_moment_words {
	int kind;
	const char *device;             /* "laptop"; NULL: "a video call" (for the ISP) */
	const char *app;                /* "Zoom"; NULL */
	long t, now;                    /* when (wall s); now for the clock's day */
	int what, seconds, conf;
	int cause;
	const struct app_evidence *ev;
	const char *fix;                /* what happened next, in words ("2.4 GHz (wlan0) moved from channel 11 to 6"); NULL */
	long fix_t;
	int since_ok, since_bad;        /* sessions of its group since (the fix, else the moment): fine, with trouble */
};
void app_moment_text(const struct app_moment_words *m, char *buf, size_t n);
/* "froze for about 8 s", "likely rebuffered for about 30 s", "had lag spikes" */
void app_what_words(int kind, int what, int seconds, char *buf, size_t n);

/* the experience score of a group: % of its minutes that were fine; -1 too few minutes to say */
int app_score(uint32_t minutes, uint32_t bad, uint32_t min_minutes);

#endif
