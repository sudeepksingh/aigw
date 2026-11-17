/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW calls and games first: real-time traffic noticed, given priority in
 * smart queueing, and the result proved.
 *
 * A video call or an online game needs little: a few hundred kbit/s, but on
 * time. When the TV starts a big download, the line fills and the call's
 * packets wait behind it. Smart queueing (ag/sqm.h) keeps that wait short;
 * calls first goes one step further and lets the call's packets go ahead of
 * the download inside the shaped queue.
 *
 * No deep inspection: a connection is a call or a game by its traits, from
 * the kernel's connection counters (agd/flow.c): UDP, packets both ways at a
 * steady rate, small enough and slow enough for a call or a game, never
 * DNS, QUIC (web) or a VPN. A known port (STUN, Zoom, Meet, FaceTime, Xbox
 * Live, Steam, ...) or the name the device looked up (zoom.us, whatsapp.net,
 * riotgames.com, ...) says which. A name or a port alone is not enough: an
 * idle app keeps its connection open without a call.
 *
 * When it is over, the timeline (ag/why.h) says how it went: the internet's
 * delay, its jitter and loss while the call lasted, and whether the line was
 * full meanwhile. That is the proof for the ISP, in numbers and words
 * without a device's or an app's name.
 *
 * Pure (no I/O): agd (agd/calls.c) reads the counters, applies the priority
 * (scripts/aigw-sqm prio) and keeps the reports. Unit test: tests/test_calls.c.
 */
#ifndef AG_CALLS_H
#define AG_CALLS_H

#include <stddef.h>
#include <stdint.h>

#include "ag/sqm.h"
#include "ag/why.h"

#define CALL_HIST 6                     /* intervals kept for each connection */

/* one look at a connection's counters: what it did since the look before */
struct call_iv {
	uint32_t ms;                    /* how long since */
	uint32_t up_pk, down_pk;        /* packets from the device, to it */
	uint32_t up_b, down_b;          /* bytes */
};

struct call_flow {
	uint8_t proto;                  /* 17 UDP, 6 TCP */
	uint16_t rport;                 /* the far end's port */
	const char *name;               /* the far end's name (DNS or handshake); NULL none */
	struct call_iv iv[CALL_HIST];   /* oldest first */
	int n;
};

enum call_kind { CK_NONE, CK_CALL, CK_GAME, CK_RT };    /* CK_RT: real time, call or game not known */
enum call_by { CB_TRAITS, CB_PORT, CB_NAME };

struct call_params {
	int min_pps;                    /* packets a second, each way, every interval (5) */
	int max_kbps;                   /* at most, each way (10000: a video call with screen sharing) */
	int max_pkt;                    /* the mean packet at most, each way (1300 bytes) */
	int ratio;                      /* the busier way at most this many times the other, in packets (25) */
	int steady;                     /* intervals in a row that look so (2) */
	int spread_pct;                 /* ... the slowest at least (100 - this)% of the fastest (60) */
	int video_kbps;                 /* faster than this (or big packets): a video call (300) */
	/* how it went */
	int smooth_ms;                  /* the internet's delay at most (150 ms) */
	int smooth_jitter;              /* its jitter at most (30 ms) */
	int smooth_loss10;              /* lost at most, in tenths of % (10: 1%) */
};

struct call_class {
	int kind;
	int by;
	const char *app;                /* "Zoom", "Xbox Live"; NULL not known (for the home only) */
};

void call_params_default(struct call_params *p);
/* a call or game (1, with what), else 0 */
int call_classify(const struct call_flow *f, const struct call_params *p, struct call_class *out);
/* what a port or a name says (a hint only); CK_NONE nothing */
int call_hint(uint8_t proto, uint16_t rport, const char *name, int *by, const char **app);
const char *call_kind_name(int kind);   /* "Call", "Game", "Realtime" (the data model) */
const char *call_kind_words(int kind);  /* "call", "game", "call or game" */

/* how it went, from the timeline's samples while it lasted */
struct call_quality {
	int samples;                    /* samples in [from, to) */
	int full;                       /* of them, the line full */
	int judged;                     /* the samples it was judged on: the full ones, else all */
	int ms;                         /* the internet's delay then (median); -1 nothing answered */
	int jitter_ms;                  /* its mean change from one sample to the next; -1 not known */
	int loss10;                     /* echo requests lost, tenths of % */
	uint32_t line_kbps;             /* the download while the line was full (median); 0 */
};

enum call_verdict { CV_UNKNOWN, CV_SMOOTH, CV_ROUGH };

void call_quality_of(const struct why_min *m, int n, long from, long to, const struct sqm_line *l,
		     struct call_quality *q);
int call_verdict(const struct call_quality *q, const struct call_params *p);
const char *call_verdict_name(int v);   /* "Unknown", "Smooth", "Rough" */

struct call_report {
	int kind;
	const char *app;                /* home only; NULL */
	const char *device;             /* home only ("Laptop"); NULL: "a call" */
	const char *top;                /* the device that downloaded most meanwhile, home only; NULL */
	long start, end;
	int verdict;
	int prioritized;                /* it had priority in smart queueing */
	int sqm_on;                     /* smart queueing was on */
	struct call_quality q;
};

/* in words; without device, app and top names it is what the ISP may see */
void call_words(const struct call_report *r, char *buf, size_t n);
/* "45 s", "12 min", "1 h 05 min" */
void call_duration(long s, char *buf, size_t n);

#endif
