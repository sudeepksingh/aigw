/* SPDX-License-Identifier: Apache-2.0 */
/* libag: shared helpers for every AIGW component (logging, time, config). */
#ifndef AG_AG_H
#define AG_AG_H

#include <stdint.h>
#include <stddef.h>

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

/* Action modes, weakest first: a policy never acts beyond the global mode. */
enum ag_mode { AG_MODE_OFF, AG_MODE_ADVISE, AG_MODE_ENFORCE };

#define AG_POLICY_MAX 16

/* "When this alert is raised, propose (advise) or take (enforce) this action." */
struct ag_policy {
	char name[32];
	char alert[32];           /* alert kind: dga, new-destination, upload-spike, ... */
	char action[48];          /* quarantine, disconnect, throttle, deprioritize, priority,
	                             or a plug-in's action: "<plugin>.<action>" */
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
	int chan_heal;            /* watch how busy each access point's channel is; suggest a quieter one */
	int chan_busy;            /* % of the air used by others that counts as busy */
	int chan_hold;            /* seconds it must stay that busy */
	int chan_poll;            /* seconds between channel measurements */
	int chan_scan_gap;        /* seconds between two neighbour scans (a scan leaves the channel briefly) */
	char model_dir[256];      /* .agm models */
	char lan_ifaces[256];     /* space-separated; clients on these are "the home" */
	int alerts_enabled;
	double dga_threshold;     /* domain-risk score that raises an alert */
	int dga_min_label;        /* shorter names are not judged */
	int fastpath_enabled;     /* established connections through the kernel flowtable (aigw-nft) */
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

/* ubus socket path: $AIGW_UBUS if set, else NULL (libubus default). */
const char *ag_ubus_path(void);

#endif
