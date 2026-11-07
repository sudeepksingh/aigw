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
#define AG_EV_DHCP "ag.dhcp"
#define AG_EV_ALERT "ag.alert"
#define AG_EV_DECISION "ag.decision"

/* Action modes, weakest first: a policy never acts beyond the global mode. */
enum ag_mode { AG_MODE_OFF, AG_MODE_ADVISE, AG_MODE_ENFORCE };

#define AG_POLICY_MAX 16

/* "When this alert is raised, propose (advise) or take (enforce) this action." */
struct ag_policy {
	char name[32];
	char alert[16];           /* alert kind: dga, new-destination, upload-spike, ... */
	char action[16];          /* quarantine, disconnect, throttle, deprioritize, priority */
	int ttl;                  /* seconds; 0 = the global default */
	int rate_kbps;            /* throttle; 0 = the global default */
	int mode;                 /* enum ag_mode, capped by the global mode */
};

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
	char state_file[256];     /* device history; "" = don't save */
	int state_interval;       /* seconds between saves */
	int flow_enabled;         /* conntrack flow sensor */
	int flow_poll;            /* seconds between traffic counter refreshes */
	int flow_warmup;          /* seconds after start before flagging "no DNS" */
	char wifi_ctrl[128];      /* hostapd control socket directory */
	char model_dir[256];      /* .agm models */
	char lan_ifaces[256];     /* space-separated; clients on these are "the home" */
	int alerts_enabled;
	double dga_threshold;     /* domain-risk score that raises an alert */
	int dga_min_label;        /* shorter names are not judged */
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
};

int ag_mode_parse(const char *s, int dflt);
const char *ag_mode_str(int mode);

/* Fills defaults, then overrides from UCI. Never fails hard: a missing
 * config file just leaves the defaults. Honors $AIGW_UCI_CONFDIR. */
void ag_config_load(struct ag_config *c);

/* ubus socket path: $AIGW_UBUS if set, else NULL (libubus default). */
const char *ag_ubus_path(void);

#endif
