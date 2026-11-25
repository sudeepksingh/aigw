/* SPDX-License-Identifier: Apache-2.0 */
/* libag: shared helpers for every AIGW component (logging, time, config). */
#ifndef AG_AG_H
#define AG_AG_H

#include <stdint.h>
#include <stddef.h>

#include "ag/features.h"        /* the features this build has (generated: cmake/features.cmake) */

#ifndef AG_VERSION
#define AG_VERSION "0.0.0"
#endif

/* ubus event names published by AIGW sensors */
#define AG_EV_DNS  "ag.dns"
#define AG_EV_FLOW "ag.flow"
#define AG_EV_WIFI "ag.wifi"
#define AG_EV_PRESENCE "ag.presence"  /* a device arrived home or left */
#define AG_EV_DHCP "ag.dhcp"
#define AG_EV_ALERT "ag.alert"
#define AG_EV_DECISION "ag.decision"
#define AG_EV_PLUGIN "ag.plugin"   /* plug-in lifecycle; plug-ins' own events use their types (iot.*) */
#define AG_EV_MODEL "ag.model"     /* a model update loaded, refused or rolled back */
#define AG_EV_CARE "ag.care"       /* proactive care: a problem found, reported, over */
#define AG_EV_UPGRADE "ag.upgrade" /* a firmware upgrade: watched, kept, worse, rolled back */
#define AG_EV_CALL "ag.call"       /* a call or game noticed, and over (with how it went) */
#define AG_EV_TICKET "ag.ticket"   /* proactive care, before the call: a ticket raised, fixed, over */
#define AG_EV_MESH "ag.mesh"       /* mesh Wi-Fi: the controller found or lost, a device moved, a backhaul, a trial */
/* the platform's mesh handler (in /usr/libexec/aigw), and the one before 0.47,
 * which a config may still name: read as the platform's */
#define MESH_HANDLER "mesh-dataelements"
#define MESH_HANDLER_OLD "mesh-prplmesh"
#define AG_EV_DM "ag.dm.event"     /* a data model event (TR-181 path ending in '!'), for USP / TR-069 */

/* Action modes, weakest first: a policy never acts beyond the global mode. */
enum ag_mode { AG_MODE_OFF, AG_MODE_ADVISE, AG_MODE_ENFORCE };

#define AG_POLICY_MAX 16

/* "When this alert is raised, propose (advise) or take (enforce) this action." */
struct ag_policy {
	char name[32];
	char alert[32];           /* alert kind: dga, new-destination, upload-spike, ... */
	char action[48];          /* quarantine, disconnect, throttle, deprioritize, priority,
	                             channel, stay24, or a plug-in's action: "<plugin>.<action>" */
	char zone[16];            /* only devices in this zone ("" = any) */
	int ttl;                  /* seconds; 0 = the global default */
	int rate_kbps;            /* throttle; 0 = the global default */
	int mode;                 /* enum ag_mode, capped by the global mode */
};

/* Plug-in names: 1-31 of [a-z0-9-], starting with a letter. */
int ag_plugin_name_ok(const char *name);
/* Built-in or plug-in ("name.action") action a policy may use. */
int ag_policy_action_ok(const char *action);

#ifndef AG_DATADIR
#define AG_DATADIR "/usr/share/aigw"
#endif

enum ag_log_level {
	AG_LOG_ERR,
	AG_LOG_WARN,
	AG_LOG_INFO,
	AG_LOG_DEBUG,
};

void ag_log_init(const char *ident, int level);
int ag_log_level_parse(const char *s);
void ag_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define AG_ERR(...)   ag_log(AG_LOG_ERR, __VA_ARGS__)
#define AG_WARN(...)  ag_log(AG_LOG_WARN, __VA_ARGS__)
#define AG_INFO(...)  ag_log(AG_LOG_INFO, __VA_ARGS__)
#define AG_DEBUG(...) ag_log(AG_LOG_DEBUG, __VA_ARGS__)

uint64_t ag_mono_us(void);     /* monotonic clock, microseconds */
int64_t ag_wall_s(void);       /* wall clock, seconds since epoch */
int ag_is_openwrt(void);
/* the demo image's simulated home (docs/try-it.md): a label only */
#define AG_DEMO_MARK "/var/run/aigw/demo"
int ag_demo(void);

/* Settings from the UCI package "aigw" (/etc/config/aigw on every host). */
struct ag_config {
	char log_level[16];
	char host[16];            /* "openwrt" or "linux" */
	char leases[256];         /* DHCP leases file */
	char dns_listen[64];
	int dns_port;
	char dns_upstream[128];   /* "auto", "ip" or "ip#port" */
	int dns_cache;            /* answers agdns keeps; 0 = no cache */
	int dns_prefetch;         /* refresh names in use before they expire */
	int dns_stale;            /* seconds an expired answer may still be served when upstream fails; 0 = never */
	int dns_max_ttl;          /* longest time an answer is kept fresh */
	char state_file[256];     /* device history; "" = don't save */
	int state_interval;       /* seconds between saves */
	int flow_enabled;         /* conntrack flow sensor */
	int flow_poll;            /* seconds between traffic counter refreshes */
	int flow_warmup;          /* seconds after start before flagging "no DNS" */
	int flow_sni;             /* name connections from TLS / QUIC handshakes (NFLOG) */
	int flow_sni_group;       /* NFLOG group the kernel copies those packets to */
	char wifi_ctrl[128];      /* hostapd control socket directory */
	int steer;                /* band steering between a 2.4 and a 5 GHz access point with one name */
	int steer_up;             /* dBm: a 2.4 GHz device this strong moves to 5 GHz */
	int steer_down;           /* dBm: a 5 GHz device this weak moves to 2.4 GHz */
	int steer_hold;           /* seconds a signal must stay past the line first */
	int steer_gap;            /* seconds between two moves of one device */
	int steer_kick;           /* devices without 802.11v may be moved by refusing them on one band */
	int steer_rate;           /* most moves a minute in the whole home */
	int presence_away;        /* seconds unseen before a device has left */
	int heal;                 /* Self-Healing Wi-Fi: measure what devices feel, name what is wrong */
	int heal_loss;            /* % of lost probes that is a problem */
	int heal_delay;           /* ms of delay that is a problem (and 3x a device's usual) */
	int heal_weak;            /* dBm at or below which a signal is weak */
	int heal_hops;            /* band changes in 10 minutes that count as hopping */
	int heal_act;             /* act on it: channel trials, keeping a device on 2.4 GHz (via policies) */
	int heal_hold;            /* seconds of congestion before a channel trial */
	int heal_trial;           /* seconds a trial runs before it is judged */
	int heal_retry;           /* seconds between two trials on one access point */
	int heal_weak_margin;     /* dB above heal_weak before a weak signal counts as fine again */
	char diag_targets[128];   /* Diagnose: internet addresses it sends echo requests to (the best counts) */
	char diag_name[128];      /* Diagnose: the name it asks the gateway's DNS for; "" = DNS not checked */
	int care;                 /* proactive care: the gateway checks itself and tells the ISP */
	int care_interval;        /* s between checks while all is fine */
	int care_recheck;         /* s to the next check after one found something (to confirm, then to see it clear) */
	int care_wifi_grace;      /* s Self-Healing Wi-Fi has before a Wi-Fi problem is reported to the ISP */
	int care_warn;            /* early warnings: the home's normal learned, a drift told before it is a fault */
	int care_learn_days;      /* days of learning before the first warning */
	char care_speed_url[256]; /* the nightly speed test's download (TR-143, HTTP); "" none */
	int care_speed_hour;      /* local hour it runs in (a random minute of it) */
	int care_speed_seconds;   /* how long it downloads, at most */
	int care_speed_mbps;      /* the line's speed (what was sold); 0 not known */
	int care_speed_min_pct;   /* below this share of it, on two tests in a row: LINE_SLOW_SPEED, reported */
	int chan_heal;            /* watch how busy each access point's channel is; suggest a quieter one */
	int chan_busy;            /* % of the air used by others that counts as busy */
	int chan_hold;            /* seconds it must stay that busy */
	int chan_poll;            /* seconds between channel measurements */
	int chan_scan_gap;        /* seconds between two neighbour scans (a scan leaves the channel briefly) */
	int chan_plan;            /* neighbour-aware channels: learn each channel by hour, move when clearly better */
	int chan_plan_days;       /* days of history before a move */
	int chan_plan_better;     /* % less load over the busy hours that is clearly better */
	int chan_plan_hold;       /* looks in a row that must agree */
	int chan_plan_every;      /* seconds between two looks */
	int chan_plan_gap;        /* seconds between two planned moves on one access point */
	int chan_plan_trial;      /* seconds measured in busy hours before a move is judged */
	int chan_avoid;           /* seconds a channel that didn't help is avoided */
	int chan_learn_gap;       /* seconds between two learning scans (only when the home is quiet) */
	int chan_quiet;           /* % of the air the home itself uses below which it is quiet now */
	char model_dir[256];      /* .agm models */
	char lan_ifaces[256];     /* space-separated; clients on these are "the home" */
	int alerts_enabled;
	double dga_threshold;     /* domain-risk score that raises an alert */
	int dga_min_label;        /* shorter names are not judged */
	int burst_enabled;        /* lookup bursts: many never-used names that don't exist, from one device */
	int burst_window;         /* seconds the burst is counted over */
	int burst_min;            /* at least this many distinct never-used names got NXDOMAIN */
	double burst_share;       /* ... and they are at least this share of its never-used names then */
	/* management by an ISP (USP / TR-069) through the data model */
	int mgmt_enabled;         /* start the protocol clients (their init scripts read this) */
	int mgmt_actions;         /* may a management system quarantine, block, change the mode */
	int mgmt_alert_details;   /* may it see alert subjects and reasons (names looked up) */
	char mgmt_serial[64];     /* DeviceInfo.SerialNumber; empty: from the machine id or the LAN MAC */
	char mgmt_oui[8];         /* DeviceInfo.ManufacturerOUI; empty: the LAN MAC's */
	int mgmt_wifi;            /* may a management system change Wi-Fi settings (name, key, channel) */
	int mgmt_firmware;        /* may it download and activate firmware */
	int mgmt_reset;           /* may it reboot the gateway or reset it to factory settings */
	char wifi_handler[128];   /* program that saves and applies Wi-Fi settings; empty: the platform's */
	char fw_handler[128];     /* program that downloads, checks and activates firmware; empty: the platform's */
	/* firmware upgrades that check themselves (agd/upgrade.c) */
	int upgrade_check;        /* compare the home before and after an upgrade */
	int upgrade_rollback;     /* worse: boot the previous image again, by itself */
	int upgrade_watch;        /* s the new image is watched */
	int upgrade_reconnect;    /* s devices have to come back */
	int upgrade_hold_days;    /* a version rolled back isn't installed again for this long */
	int upgrade_report_delay; /* s: a verdict found at start is told this much later (protocol clients starting) */
	char upgrade_file[256];   /* the record (kept over the upgrade); empty: beside state_file */
	/* "why was it slow?": the home's timeline (agd/why.c) */
	int why;                  /* keep a sample of the home every why_interval, and what happened */
	int why_hours;            /* how far back it goes */
	int why_interval;         /* s between samples (60; less only for tests) */
	int why_probe;            /* each sample sends 3 echo requests to the next router and the internet */
	char why_file[256];       /* where it is kept over a restart of agd; "" not kept */
	char why_wan[32];         /* the WAN interface sampled; "" the default route's */
	/* self-tuning smart queueing (agd/sqm.c) */
	int sqm;                  /* bufferbloat found: propose (advise) or try (enforce) SQM, kept when it helps */
	char sqm_handler[128];    /* the program that shapes the line; "" aigw-sqm */
	int sqm_detect;           /* samples of the last hour full and slow before it is bloat */
	int sqm_trial;            /* full samples after it is on before it is judged */
	int sqm_hold;             /* s after a trial (or a person saying no) before it is proposed again */
	/* calls and games first (agd/calls.c) */
	int calls;                /* notice calls and games, report how they went */
	int calls_prio;           /* ... and let them go first inside AIGW's smart queueing (only while it is on) */
	int calls_share;          /* the ISP may see the reports: kinds and numbers, never device or app names */
	int calls_poll;           /* s between looks at the connections' counters */
	int calls_min_s;          /* a call or game shorter than this isn't reported */
	/* signals for the ISP (agd/offers.c) */
	int offers;               /* a faster plan, mesh Wi-Fi, smart queueing, a customer at risk, a line to repair */
	int offers_share;         /* the ISP may see them (data model, cortex): counts and words, never names */
	/* the help-desk assistant (agd/brief.c) */
	int brief;                /* SupportBrief(), agctl brief, and a brief in each report to cortex */
	int brief_names;          /* the ISP's brief names devices by their names; 0: by kind ("a laptop") */
	/* area outages (agd/area.c): cortex says many homes of this uplink are hit; agd holds its own fixes */
	int area;                 /* take the notice (from the bridge) and hold fixes meanwhile */
	char area_uplink_id[64];  /* the operator's id of this home's uplink (an OLT, a BNG, an area); "" none */
	/* proactive care, before the call (agd/proactive.c) */
	int pro;                  /* find faults a customer would notice; fix them or tell the ISP */
	int pro_fix;              /* AIGW may fix them itself (as act.mode allows: enforce applies, advise proposes) */
	int pro_interval;         /* s between looks */
	int pro_threshold;        /* points worth acting on (ag/proactive.h) */
	int pro_min_s;            /* a fault must have lasted this long */
	int pro_clear_s;          /* gone this long: over (each flap doubles it) */
	int pro_cooldown_s;       /* back within this: the same ticket, no new word to the ISP */
	int pro_max_per_day;      /* new tickets to the ISP in 24 hours */
	/* smarter mesh Wi-Fi over a standard EasyMesh controller (agd/mesh.c) */
	int mesh;                 /* read the controller's Data Elements, decide, act through the controller */
	char unbuilt[320];        /* sections of features this build doesn't have, ignored: "mesh (AIGW_FEATURE_MESH), ..." */
	char mesh_handler[128];   /* the controller's handler; "" the platform's (MESH_HANDLER) */
	char mesh_source[256];    /* read this Data Elements snapshot (JSON or path=value) instead; "" the handler */
	int mesh_poll;            /* s between reads */
	int mesh_steer;           /* move struggling devices to a better access point (as act.mode allows) */
	int mesh_channel;         /* plan channels across the gateway and extenders (as act.mode allows) */
	int mesh_trial;           /* s a move or channel change is measured before it is judged */
	int mesh_hold;            /* s a device must struggle before it is moved */
	int mesh_legacy;          /* also steer devices without 802.11v (the controller's blocklist steering) */
	int mesh_weak_dbm;        /* a device at or below this is struggling */
	int mesh_hyst_db;         /* a target must hear it this much better */
	int mesh_cooldown;        /* s after a move by AIGW before the same device again */
	int mesh_quiet;           /* s after the controller, or the device itself, moved it */
	char mesh_names[512];     /* "<AL MAC>=<name>;..." the home's names for its extenders */
	/* coverage placement advice (agd/coverage.c) */
	int cov;                  /* learn each Wi-Fi device's coverage, its weak spots, and what fixes them */
	int cov_interval;         /* s between looks (60) */
	int cov_learn;            /* looks at a device before it is judged (60) */
	int cov_share;            /* cortex gets the verdict and counts (never names) */
	/* line health and area fault prediction (agd/line.c) */
	int line;                 /* learn the line's trends, day by day */
	int line_interval;        /* s between looks at the line's own numbers (900) */
	int line_share;           /* cortex gets the line health vector (the line's physics only) */
	char line_handler[128];   /* the WAN's numbers; "" the platform's (line-stats) */
	char mgmt_prefix[32];     /* vendor prefix of AIGW's own names (X_<OUI or domain>_); empty: X_AIGW_ */
	char mgmt_manufacturer[64]; /* DeviceInfo.Manufacturer, ModelName, ProductClass (an OEM's); empty: AIGW OS */
	char mgmt_model[64];
	char mgmt_product[64];
	int fastpath_enabled;    /* established connections through the kernel flowtable (aigw-nft) */
	int meter_enabled;        /* count each device's traffic in the kernel (eBPF on the LAN interfaces) */
	int fed_enabled;         /* take part in federated learning rounds (names never leave) */
	double fed_noise;         /* at least this noise multiplier on what leaves, whatever cortex asks */
	int fed_min_examples;     /* answered examples needed before taking part in a round */
	double fed_budget;        /* stop taking part before epsilon passes this; 0 = no limit */
	/* per-device behaviour baselines */
	int bucket_seconds;       /* length of one activity bucket (an hour) */
	int learn_minutes;        /* watch a device this long before judging it */
	int min_known;            /* ... and until it has used this many sites */
	double novelty_max;       /* new sites/day below which a device counts as stable */
	double spike_factor;      /* upload alert: this many std-devs above usual */
	double spike_min_mb;      /* ... and at least this much in one bucket */
	int unusual_min_days;     /* days of history before "unusual hour" alerts */
	/* acting: decisions, guardrails, actuators */
	int act_mode;             /* enum ag_mode */
	int act_ttl;              /* default life of an action, seconds */
	int act_max_ttl;          /* no action lives longer */
	int act_max_auto;         /* automatic actions per hour (enforce mode) */
	int act_rate_kbps;        /* default throttle rate */
	char act_protect[512];    /* space-separated MACs/IPs never acted on */
	char dns_policy[256];     /* block list file agd writes and agdns reads */
	int n_policies;
	struct ag_policy policies[AG_POLICY_MAX];
	/* plug-ins */
	int plugins_enabled;
	char plugin_dir[256];     /* from packages: trusted */
	char plugin_user_dir[256];/* added with agctl: privileged capabilities need your OK */
	char plugin_user[32];     /* plug-ins run as this user ("" = as agd) */
	char plugin_state[256];   /* each plug-in gets <plugin_state>/<name>, writable by it */
	/* model updates (signed, hot-loaded) */
	char models_dir[256];     /* installed updates; the packaged ones stay in model_dir */
	char trusted_keys[256];   /* usign public keys whose signatures are accepted */
	int models_require_sig;   /* 0: also accept unsigned updates (development only) */
	char model_backend[32];   /* who computes scores: auto | interp | flat | a loaded back-end's name */
	char backend_dir[256];    /* back-end libraries (an NPU's), root's only */
	/* remote access (WireGuard) */
	int remote_enabled;
	char remote_endpoint[128];/* host or address phones use to reach this home */
	int remote_port;          /* UDP */
	char remote_subnet[32];   /* tunnel network, e.g. 10.77.0.0/24; the gateway is .1 */
	char remote_iface[16];
	int remote_full;          /* 1: all of a phone's traffic goes home (AIGW protects it everywhere) */
	char remote_dir[256];     /* keys and peers */
	/* per-device Wi-Fi keys */
	char wifi_keys[256];      /* hostapd wpa_psk_file that agd manages */
	int key_ttl;              /* an unused join key expires after this many seconds */
};

int ag_mode_parse(const char *s, int dflt);
const char *ag_mode_str(int mode);

/* Fills defaults, then overrides from UCI. Never fails hard: a missing
 * config file just leaves the defaults. Honors $AIGW_UCI_CONFDIR. */
void ag_config_load(struct ag_config *c);
/* writes one option of /etc/config/aigw (section must exist) and commits; 0 or -1 */
int ag_config_set(const char *section, const char *option, const char *value);
/* makes the named section (its type its name) when it isn't there yet; 0 or -1 */
int ag_config_ensure(const char *section);

/* ubus socket path: $AIGW_UBUS if set, else NULL (libubus default). */
const char *ag_ubus_path(void);

#endif
