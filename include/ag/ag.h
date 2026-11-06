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
};

/* Fills defaults, then overrides from UCI. Never fails hard: a missing
 * config file just leaves the defaults. Honors $AIGW_UCI_CONFDIR. */
void ag_config_load(struct ag_config *c);

/* ubus socket path: $AIGW_UBUS if set, else NULL (libubus default). */
const char *ag_ubus_path(void);

#endif
