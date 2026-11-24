/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW smarter mesh Wi-Fi (v0.44.0): the intelligence over a standard
 * Wi-Fi Alliance EasyMesh controller with a Data Elements northbound.
 *
 * AIGW is not another 1905.1 stack. The controller runs the mesh (onboarding,
 * backhaul, its own steering); AIGW reads it through the standard northbound,
 * the Data Elements model (TR-181 Device.WiFi.DataElements.), judges it with
 * what the gateway already learns, and acts through the controller's own
 * requests:
 *
 *   read     each parameter of Device.WiFi.DataElements.Network. by its path
 *            (mesh_set): agents (Device.{i}), their radios, BSSs, associated
 *            stations with link metrics, the backhaul link of each agent, and
 *            unassociated-station and 802.11k measurements when present
 *
 *   steer    a struggling station (weak signal, or losing traffic) to a BSS it
 *            hears clearly better (measured, not guessed), with hysteresis; never
 *            right after the controller or the device itself moved it, never in
 *            a call, never a device whose last steers failed (paused, doubling),
 *            legacy (no 802.11v) devices only when allowed (the controller then
 *            uses its blocklist steering, which drops the device for a moment)
 *
 *   judge    a trial: the station's link before and after the move; kept when
 *            it is clearly better, else moved back and that pair avoided a day
 *
 *   channels a plan across all radios: no co-channel between the gateway and
 *            an extender where avoidable. A radio that carries an extender's
 *            wireless backhaul is tied to its parent's channel and isn't moved.
 *
 *   backhaul each extender's link to the gateway: wired or wireless, how
 *            strong and fast; weak, slow or on 2.4 GHz (5/6 GHz possible), and
 *            "hurts more than helps": a weak backhaul with devices behind it
 *
 * Signal strengths in the Data Elements model are RCPI (802.11 Table 9-176):
 * dBm = RCPI / 2 - 110; 0 and 221-255 are "not known".
 *
 * Pure (no I/O): agd (agd/mesh.c) reads the controller, keeps the history and
 * acts through it. Unit test: tests/test_mesh.c.
 */
#ifndef AG_MESH_H
#define AG_MESH_H

#include <stddef.h>
#include <stdint.h>

#define MESH_DEVS     8         /* agents: the gateway and its extenders */
#define MESH_RADIOS   3         /* per agent */
#define MESH_BSS      4         /* per radio */
#define MESH_STAS     64
#define MESH_UNASSOC  32
#define MESH_MEAS     4         /* 802.11k beacon reports kept per station */
#define MESH_NAME     32

enum mesh_bh { MESH_BH_NONE, MESH_BH_WIFI, MESH_BH_ETH, MESH_BH_OTHER };
enum mesh_cap { MESH_CAP_UNKNOWN = -1, MESH_CAP_NO = 0, MESH_CAP_YES = 1 };

struct mesh_bss {
	uint32_t inst;
	char bssid[18];
	char ssid[33];
	int backhaul;           /* BackhaulUse: carries an extender's backhaul */
	int fronthaul;          /* FronthaulUse (1 unless the controller says it isn't) */
};

struct mesh_radio {
	uint32_t inst;
	char id[18];            /* RUID */
	int band;               /* 2, 5 or 6; 0 not known yet */
	int channel, opclass;   /* CurrentOperatingClassProfile */
	int util;               /* % of air in use (Utilization 0-255 scaled), -1 not known */
	int noise_dbm;          /* 0 not known */
	int width;              /* MHz, from the widest operating class (mesh_finish) */
	int n_prof;             /* CurrentOperatingClassProfile.{i}: class and channel as read */
	uint32_t prof_inst[3];
	int prof_class[3], prof_ch[3];
	int n_bss;
	struct mesh_bss bss[MESH_BSS];
};

struct mesh_device {
	uint32_t inst;
	char id[18];            /* the agent's AL MAC (Device.{i}.ID) */
	char name[MESH_NAME];   /* from the home's own names (mesh.name), else "" */
	char maker[MESH_NAME], model[MESH_NAME];
	int gateway;            /* the controller's own agent (mesh_finish) */
	/* MultiAPDevice.Backhaul. */
	int bh;                 /* enum mesh_bh */
	char bh_mac[18];        /* BackhaulMACAddress: the parent's interface it links to */
	char bh_parent[18];     /* BackhaulDeviceID: the parent agent */
	char bh_own[18];        /* MACAddress: its own backhaul interface (a bSTA) */
	int bh_dbm;             /* Stats.SignalStrength, 0 not known */
	uint32_t bh_down_kbps, bh_up_kbps;      /* Stats.LastDataDownlinkRate / Uplink */
	int bh_util;            /* Stats.LinkUtilization %, -1 not known */
	int bh_channel;         /* Backhaul CurrentOperatingClassProfile channel, 0 not known */
	int bh_class;           /* ... its operating class, 0 not known */
	int bh_band;            /* its band (from the channel, or the radio it is on) */
	int n_radio;
	struct mesh_radio radio[MESH_RADIOS];
};

struct mesh_meas {
	char bssid[18];
	int dbm;
};

struct mesh_sta {
	char mac[18];
	uint32_t key;           /* the path's instances, hashed: Device, Radio, BSS, STA */
	int dev, radio, bss;    /* where it is (indices) */
	int dbm;                /* uplink signal, 0 not known */
	uint32_t down_kbps, up_kbps;    /* LastDataDownlinkRate / Uplink */
	uint32_t est_mbps;      /* EstMACDataRateDownlink */
	uint64_t pkts, retrans, errs;   /* PacketsSent, RetransCount, ErrorsSent */
	long connect_s;         /* LastConnectTime: seconds on this BSS, -1 not known */
	long steer_s;           /* SteeringSummaryStats.LastSteerTime: s since the controller last steered it, -1 */
	uint32_t btm_tries, btm_fails;
	int btm, rrm;           /* enum mesh_cap: 802.11v BSS transition, 802.11k (ClientCapabilities) */
	int n_meas;
	struct mesh_meas meas[MESH_MEAS];
};

struct mesh_unassoc {
	uint32_t key;
	char mac[18];
	int dev, radio;         /* the radio that heard it */
	int dbm;
};

struct mesh_net {
	char id[40];            /* Network.ID */
	char controller[18];    /* Network.ControllerID */
	int n_dev;
	struct mesh_device dev[MESH_DEVS];
	int n_sta;
	struct mesh_sta sta[MESH_STAS];
	int n_un;
	struct mesh_unassoc un[MESH_UNASSOC];
	int ignored;            /* parameters not used (or beyond the limits) */
};

void mesh_reset(struct mesh_net *n);
/*
 * One parameter by its path, with or without "Device.WiFi.DataElements."
 * ("Network.Device.2.Radio.1.BSS.1.STA.3.SignalStrength", "160"). Instance
 * numbers may also be aliases ("cpe-Device-1"). 0 used, 1 ignored, -1 not a
 * Data Elements path.
 */
int mesh_set(struct mesh_net *n, const char *path, const char *value);
/* after the last mesh_set: which agent is the gateway, bands, backhaul bands */
void mesh_finish(struct mesh_net *n);

int mesh_rcpi_dbm(long rcpi);                   /* 0: not known */
int mesh_band_of(int channel, int opclass);     /* 2, 5, 6; 0 not known */
int mesh_width_of(int opclass);                 /* MHz (20 when not known) */
/* the opclass for a channel at a width (20, 40, 80) on a band; 0 none */
int mesh_opclass(int band, int channel, int width);
/* the centre channel of a primary channel's 40/80/160 MHz block (the channel itself at 20 MHz) */
int mesh_center(int band, int channel, int width);
/* 802.11v / 802.11k support from a (Re)Association Request frame body in hex */
void mesh_caps(const char *hex, int *btm, int *rrm);

int mesh_find_sta(const struct mesh_net *n, const char *mac);
int mesh_find_dev(const struct mesh_net *n, const char *id);
/* the BSS with this BSSID: 0 and its indices, -1 none */
int mesh_find_bss(const struct mesh_net *n, const char *bssid, int *dev, int *radio, int *bss);
/* the agent's name for people: its own name, "the gateway", else "the extender <id>" */
void mesh_dev_words(const struct mesh_net *n, int dev, char *buf, size_t sz);
int mesh_extenders(const struct mesh_net *n);
int mesh_stas_on(const struct mesh_net *n, int dev);

/* ---------- the rules ---------- */

struct mesh_params {
	int weak_dbm;           /* a station at or below this is struggling (-75) */
	int good_dbm;           /* a target must be heard at least this well (-68) */
	int hyst_db;            /* ... and this much better than where it is (8) */
	int loss_pct;           /* or it loses this much traffic (10): retries, or the gateway's probes */
	long hold_s;            /* struggling this long first (60) */
	long cooldown_s;        /* after a steer of ours, leave it this long (900) */
	long quiet_s;           /* the controller or the device moved it within this: leave it (300) */
	int max_fails;          /* steers that didn't land in a row before a pause (2) */
	long fail_pause_s;      /* the pause (3600), doubled by each further failure */
	long bad_pair_s;        /* a move that didn't help: that pair avoided this long (86400) */
	int busy_util;          /* a target radio busier than this % is not a target (80) */
	int legacy;             /* steer devices without 802.11v (the controller's blocklist steering) (0) */
	int better_pct;         /* a trial: the rate this much higher is better (20) */
	int bh_weak_dbm;        /* a wireless backhaul at or below this is weak (-72) */
	uint32_t bh_min_kbps;   /* ... or slower than this (100000) */
	int bh_busy;            /* ... or its link busier than this % (80) */
};

void mesh_params_default(struct mesh_params *p);

/* what agd remembers of a station (kept over restarts) */
struct mesh_hist {
	char mac[18];
	long seen;              /* last read */
	long struggling_since;  /* 0: fine */
	long last_steer;        /* ours */
	long pause_until;       /* steers failed: leave it until then */
	int fails;              /* in a row */
	uint32_t steers, kept, undone, misses;
	char bad_from[18], bad_to[18];  /* a move that didn't help */
	long bad_until;
	char last_bssid[18];    /* where it was at the last read */
	long moved_at;          /* it changed BSS (by itself or the controller) */
	uint64_t pkts, retrans; /* counters at the last read */
	int retry_pct;          /* retransmissions per packet sent since the last read, -1 not known */
};

/* what agd knows of a station beyond the controller */
struct mesh_extra {
	int loss_pct;           /* the gateway's own probes (Self-Healing Wi-Fi), -1 not measured */
	int in_call;            /* a call or game is on (calls first) */
	int protected_dev;      /* act.protect */
};

enum mesh_method { MESH_STEER_BTM, MESH_STEER_MANDATE };

struct mesh_steer {
	int sta;
	int to_dev, to_radio, to_bss;
	char from[18], to[18];  /* BSSIDs */
	int cur_dbm, exp_dbm;   /* where it is, what the target hears of it */
	int method;             /* enum mesh_method */
	char why[320];          /* in words */
};

/* update the history from a fresh read (moves, retry rate); call once per read */
void mesh_hist_update(struct mesh_hist *h, const struct mesh_net *n, int sta, long now);
/*
 * Should this station move? 1 with the move in out; 0 not (skip says why: "fine",
 * "in a call", ...). The history's struggling_since is kept up to date.
 */
int mesh_steer_pick(const struct mesh_net *n, int sta, struct mesh_hist *h, const struct mesh_extra *x,
		    const struct mesh_params *p, long now, struct mesh_steer *out, char *skip, size_t sn);

/* a station's link, for a trial */
struct mesh_sample {
	int dbm;                /* 0 not known */
	uint32_t kbps;          /* downlink rate, 0 not known */
	int retry_pct;          /* -1 not known */
	int loss_pct;           /* -1 not known */
};
void mesh_sample_of(const struct mesh_net *n, int sta, const struct mesh_hist *h, int loss_pct, struct mesh_sample *s);

enum mesh_verdict { MESH_KEEP, MESH_UNDO };
/* a trial: kept when the link is clearly better and nothing got worse; why in words */
int mesh_trial_judge(const struct mesh_sample *before, const struct mesh_sample *after, const struct mesh_params *p,
		     char *why, size_t n);
/* after a steer: 1 when it landed (the station is on the target), 0 not (yet) */
int mesh_steer_landed(const struct mesh_net *n, const char *mac, const char *to_bssid);
/* a steer that didn't land, or a trial undone: the history learns (pauses, the bad pair) */
void mesh_steer_failed(struct mesh_hist *h, const struct mesh_params *p, long now, int landed, const char *from,
		       const char *to);

struct mesh_chan {
	int dev, radio;
	int from, to;           /* channels */
	int opclass;            /* for the target */
	char why[240];
};
/*
 * The channel plan: radios of the same band on the gateway and its extenders
 * that share a channel (2.4 GHz: overlap; 5 GHz: the same 20/40/80 MHz block)
 * where it is avoidable. The extender's radio moves (never the gateway's), to
 * the channel that clashes with no other radio of the home, outside DFS. A
 * radio carrying a wireless backhaul stays with its parent. How many changes.
 */
int mesh_chan_plan(const struct mesh_net *n, struct mesh_chan *out, int max);
/* do two radios clash (same band, overlapping channels at their widths) */
int mesh_chan_clash(int band, int ch_a, int width_a, int ch_b, int width_b);

enum mesh_bh_kind {
	MBH_WEAK,               /* a wireless backhaul with a weak signal */
	MBH_SLOW,               /* ... slow */
	MBH_BUSY,               /* ... nearly full */
	MBH_24,                 /* ... on 2.4 GHz where 5 or 6 GHz would do */
	MBH_TRY6,               /* ... on 5 GHz where both ends have 6 GHz and the signal allows it */
	MBH_N
};

struct mesh_bh_finding {
	int dev;
	int kind;               /* enum mesh_bh_kind */
	int stations;           /* devices behind it */
	int hurts;              /* weak or slow with devices behind it: the extender hurts more than it helps */
	char words[240];
	char advice[240];
};
/* the backhaul of each extender; how many findings (the worst kind of each extender first) */
int mesh_backhaul(const struct mesh_net *n, const struct mesh_params *p, struct mesh_bh_finding *out, int max);
const char *mesh_bh_kind_name(int kind);        /* Weak, Slow, Busy, On24, Try6 */
const char *mesh_bh_name(int bh);               /* None, Wi-Fi, Ethernet, Other */

#endif
