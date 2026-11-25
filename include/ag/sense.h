/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Wi-Fi sensing (v0.52.0): someone home or not, movement per zone, and
 * "activity seen as usual today" (a care check-in for someone who lives
 * alone). Opt-in: off unless the home turns it on (agd/sense.c checks).
 *
 * It works from what the access point already gets: the signal of each frame
 * an associated device sends (hostapd's per-station signal, read every poll),
 * and a vendor's channel state information (CSI) only through a handler when
 * the driver offers one. No monitor mode, no other adapter.
 *
 * People change the signal between a device and the access point: a body in
 * the path absorbs and reflects it. A device that stays put (a TV, a smart
 * plug, a speaker) has a steady signal when nobody moves near it, and a
 * spread-out one when someone does. Lessons from RoomSense (a research CNN on
 * the same signals) shape the small model here:
 *
 *   links         one per device and band (mixing what is not the same link
 *                 blurs it); a look counts only when the device sent frames
 *                 since the last one (devices send in bursts with gaps of
 *                 3-7 s: an old reading says nothing)
 *   features      per window (window looks): the median and the spread around
 *                 it (mean absolute deviation from the median: robust to one
 *                 stray frame), and how far the median moved since the last
 *                 window (a change point)
 *   motion        the spread well above the link's own quiet spread (learned
 *                 from its quiet windows only), or a change point
 *   zones         a sticky two-state HMM (still / moving) over the links of
 *                 each zone: one noisy window doesn't flip it
 *   presence      a sticky two-state HMM (nobody / someone home) over
 *                 movement, personal devices (phones, watches) present, and
 *                 how often there is movement at this hour on usual days, so
 *                 a quiet night is not read as an empty home
 *   check-in      each hour's usual movement, learned over days; a stretch
 *                 with no movement in which there would usually be
 *                 checkin_min minutes of it is unusual inactivity (not while
 *                 the household is away: its personal devices all gone)
 *
 * Not validated on real, labelled activity: the accuracy AIGW states is on a
 * simulated home with whole days held out (tests/test_sense.c,
 * docs/wifi-sensing.md).
 *
 * Pure (no I/O): unit test tests/test_sense.c.
 */
#ifndef AG_SENSE_H
#define AG_SENSE_H

#include <stddef.h>
#include <stdint.h>

#define SN_LINKS  24            /* device links watched */
#define SN_ZONES  8
#define SN_WIN    16            /* looks per window at most */
#define SN_NAME   16

struct sn_params {
	int window;             /* looks in a window (6: 30 s at agd's 5 s look) */
	int look_s;             /* s between looks (5) */
	int min_fresh;          /* looks with new frames a link needs in a window (3) */
	float k_spread;         /* moving: the spread this many times the quiet one (2.5) */
	float min_db;           /* ... and at least this many dB above it (1.5) */
	int cp_db;              /* or the median moved this many dB (4) */
	int learn;              /* quiet windows before a link judges (20) */
	float stay_zone;        /* a zone stays as it is from window to window (0.85) */
	float stay_home;        /* presence stays (0.995) */
	int night_from, night_to;       /* local hours when stillness means asleep, not away (23, 7) */
	int learn_days;         /* days before the check-in judges (7) */
	int checkin_min;        /* unusual inactivity: this many minutes of usual movement missed (45) */
};

struct sn_link {
	char mac[18];
	uint8_t band;           /* 2 or 5; "" mac: free */
	uint8_t zone;
	uint8_t personal;       /* a phone or watch: someone's own */
	uint8_t nbuf, looks;    /* fresh looks in the window; looks at all */
	int8_t buf[SN_WIN];
	int8_t last_med;        /* the last window's median, 0 none */
	uint8_t motion;         /* the last window: 1 moving */
	uint8_t present;        /* seen in this window */
	uint8_t csi;            /* a vendor handler's motion score this window (0-100), 255 none */
	uint16_t quiet_n;
	float quiet;            /* its quiet spread, dB */
	uint32_t rx;            /* frames received, as of the last look */
	long last;
};

struct sn_zone {
	char name[SN_NAME];
	float p;                /* P(moving) */
	uint8_t moving;
	long since;             /* moving or still since */
	long last_motion;
	uint32_t motion_windows;
};

struct sn_state {
	struct sn_link l[SN_LINKS];
	struct sn_zone z[SN_ZONES];
	int nz;
	float p_home;
	uint8_t home;           /* 1 someone home */
	uint8_t frozen;         /* learn nothing more (the benchmark's held-out days) */
	uint8_t inactive;       /* unusual inactivity now */
	uint8_t personal_known; /* some link is someone's own device */
	uint8_t run;            /* windows in a row with movement */
	long home_since;
	long last_motion;
	long inactive_since;
	float expected;         /* windows of movement usual since the last one */
	uint16_t usual[2][24];  /* windows with movement per hour on usual weekdays, weekend days (x10) */
	uint16_t seen[24];      /* ... today */
	uint16_t judged[24];    /* windows with something to judge, per hour today */
	long day;               /* local day number (days since 1970-01-01, a Thursday) */
	uint16_t days;          /* days learned */
	uint16_t days_of[2];    /* ... weekdays, weekend days */
	uint32_t windows;
	char why[64];           /* the presence's last reason */
};

/* what a window changed */
enum { SN_EV_PRESENCE = 1, SN_EV_INACTIVE = 2, SN_EV_ACTIVE_AGAIN = 4 };

void sn_params_default(struct sn_params *p);
void sn_init(struct sn_state *s);
/* a zone by name (created); -1 when full */
int sn_zone(struct sn_state *s, const char *name);
/* a look at one device: its signal (dBm) and frames received so far; zone by index */
void sn_look(struct sn_state *s, const char *mac, int band, int zone, int personal, int dbm, uint32_t rx, long t);
/* a vendor handler's CSI motion score (0-100) for a device in this window */
void sn_csi(struct sn_state *s, const char *mac, int score);
/* the window ends: judge it; local hour and day; returns SN_EV_* bits */
int sn_window(struct sn_state *s, long t, int hour, long day, const struct sn_params *p);

/* "AsUsual", "Less", "None", "Inactive", "Learning" */
const char *sn_activity(const struct sn_state *s, int hour, const struct sn_params *p);
/* minutes of movement today by now, and usually by now (-1 not known) */
void sn_activity_minutes(const struct sn_state *s, int hour, const struct sn_params *p, int *today, int *usual);
/* in words (no names): "Movement seen for 34 min today by 11:00, usually about 41 by now: as usual." */
void sn_activity_words(const struct sn_state *s, int hour, const struct sn_params *p, char *buf, size_t n);

#endif
