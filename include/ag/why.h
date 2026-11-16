/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW "why was it slow?": the home's timeline, explained afterwards.
 *
 * A customer calls about last night: "the internet was slow at 8". Diagnose
 * answers for now; by the time someone asks, 8 o'clock is gone. agd
 * (agd/why.c) keeps a sample of the home every minute and a log of what
 * happened (devices joining, leaving and changing band, Wi-Fi health, channel
 * changes, AIGW's own decisions, care's problems, speed tests, upgrades,
 * restarts). This turns a stretch of that timeline into a verdict, with
 * Diagnose's codes (ag/diag.h) so a help desk counts them the same, and a
 * short story of what happened, in order.
 *
 * Pure (no I/O): unit test tests/test_why.c.
 */
#ifndef AG_WHY_H
#define AG_WHY_H

#include <stddef.h>
#include <stdint.h>

#define WHY_RADIOS   4
#define WHY_TOP      2
#define WHY_NONE     0xffff          /* no delay measured */
#define WHY_FINDINGS 16
#define WHY_STORY    16
#define WHY_TEXT     160
#define WHY_MAX_SAMPLES 4320            /* the most samples one stretch is judged on (3 days of minutes) */

/* one access point in one sample (Self-Healing Wi-Fi's verdict) */
struct why_radio {
	uint8_t used;
	uint8_t status, cause;          /* enum heal_status / heal_cause (ag/heal.h) */
	uint8_t loss;                   /* % of its probes lost */
	uint16_t rtt;                   /* ms, WHY_NONE */
	uint8_t channel, stations;
};

/* one sample of the home (a minute: why.interval) */
struct why_min {
	uint32_t t;                     /* its start, wall s; 0: empty */
	uint8_t route;                  /* a default route */
	uint8_t gw_sent, gw_lost;       /* echo requests to the next hop */
	uint8_t in_sent, in_lost;       /* ... to the internet (the best target) */
	uint8_t pad;
	uint16_t gw_ms, in_ms;          /* median of those answered; WHY_NONE */
	uint16_t dns_q, dns_fail;       /* lookups answered by the gateway's DNS; of them failed (SERVFAIL) */
	uint16_t dns_ms;                /* their mean time; WHY_NONE */
	uint16_t pad2;
	uint32_t down_kbps, up_kbps;    /* the WAN, over the sample */
	struct why_radio r[WHY_RADIOS];
	struct {
		uint8_t mac[6];
		uint16_t pad;
		uint32_t kbps;          /* downloaded */
	} top[WHY_TOP];                 /* the devices that downloaded most (kbps 0: none) */
};

/* what happened */
enum why_kind {
	WE_START,                       /* agd started (text: after how long off) */
	WE_JOIN, WE_LEAVE, WE_ROAM,     /* a device on Wi-Fi; flag: band now (2 or 5) */
	WE_STEER,                       /* AIGW moved a device to another band */
	WE_WEAK, WE_SIGNAL_OK,          /* a device's signal turned weak, fine again; flag: dBm (negated) */
	WE_HEALTH,                      /* an access point's Wi-Fi health; flag: status */
	WE_CHANNEL,                     /* an access point's channel changed */
	WE_DECISION,                    /* an AIGW decision took effect or ended; flag WF_* */
	WE_CARE,                        /* proactive care: a problem found, reported, over; an early warning */
	WE_SPEED,                       /* the nightly speed test */
	WE_UPGRADE,                     /* a firmware upgrade */
	WE_N
};

#define WF_BLOCKS 1                     /* WE_DECISION: keeps a device off (quarantine, disconnect) */
#define WF_LIMITS 2                     /* ... slows it (throttle, deprioritize) */
#define WF_WIFI   4                     /* ... changes Wi-Fi (a channel, a device kept on 2.4 GHz) */
#define WF_ENDED  8                     /* ... ended (reverted, expired) */

struct why_ev {
	uint32_t t;
	uint8_t kind;
	uint8_t mac[6];                 /* the device it is about; zero: none */
	uint8_t flag;
	char text[WHY_TEXT];            /* in words, with names ("TV moved to 2.4 GHz (-71 dBm)") */
};

/* the normal of the line (for the stretch's hours), from proactive care or the timeline itself */
enum { WN_GW, WN_INET, WN_LOSS, WN_DNS, WN_N };

struct why_in {
	long from, to;                  /* the stretch asked about (wall s) */
	int step;                       /* s between samples */
	const struct why_min *m;        /* its samples, oldest first */
	int n;
	const struct why_ev *e;         /* events from a little before it to its end, oldest first */
	int ne;
	long recorded_since;            /* the oldest sample kept; 0: none */
	float norm[WN_N];               /* ms, ms, %, ms */
	int have_norm[WN_N];
	float down_mbps;                /* the line's download speed (sold, else tested); 0 not known */
	float peak_down_mbps, peak_up_mbps;     /* the most the timeline has seen (when the speed isn't known) */
	const char *radio[WHY_RADIOS];  /* the access points' names ("5 GHz (wlan1)"); NULL unused */
	const uint8_t *host;            /* the device asked about (MAC); NULL: the whole home */
	const char *host_name;
	/* a device's name ("TV"), else its MAC; ctx is passed back */
	const char *(*name)(const uint8_t mac[6], void *ctx);
	void *ctx;
};

struct why_finding {
	const char *code;               /* Diagnose's (ag/diag.h), and LINE_FULL, GATEWAY_OFF, ... */
	const char *area;               /* Internet, DNS, WiFi, Device, AIGW, All */
	int sev;                        /* enum diag_sev */
	int samples;                    /* how long it lasted, in samples */
	long since, until;              /* its longest stretch */
	char text[400];
};

struct why_line {
	long t;
	char text[WHY_TEXT + 64];
};

struct why_result {
	int n;
	struct why_finding f[WHY_FINDINGS];     /* f[0] is the verdict */
	int recorded, expected;         /* samples kept of those the stretch should have */
	int nstory;
	struct why_line story[WHY_STORY];       /* in order of time */
};

void why_judge(const struct why_in *in, struct why_result *out);
/* the line's normal from samples outside [from, to) (medians); have[] 0: too few */
void why_baseline(const struct why_min *m, int n, long from, long to, float norm[WN_N], int have[WN_N]);
/* what to do (Diagnose's advice, and the timeline's own codes) */
const char *why_advice(const char *code);
/* "20:02" (local time); with the day ("Mon 20:02") when it isn't today (as of now) */
void why_clock(long t, long now, char *buf, size_t n);
const char *why_kind_name(int kind);

#endif
