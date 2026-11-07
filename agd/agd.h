/* SPDX-License-Identifier: Apache-2.0 */
/* Internal interfaces between agd's modules. */
#ifndef AGD_H
#define AGD_H

#include <netinet/in.h>
#include <stdint.h>

#include <libubox/blobmsg.h>

#include "ag/ag.h"

#define DEV_MAX     256
#define DEV_DOM_MAX 32
#define DEV_DST_MAX 16
#define NAME_LEN    128
#define IP_LEN      INET6_ADDRSTRLEN

struct top_item {
	char name[NAME_LEN];
	uint64_t n;
};

#define BLOOM_BYTES 1024        /* 8192 bits: ~1000 sites at ~2% false "known" */

/* What is normal for one device. Learned continuously; judged once steady. */
struct baseline {
	uint8_t bloom[BLOOM_BYTES];     /* registrable domains it has used */
	uint32_t known;                 /* distinct sites added */
	int64_t since;                  /* learning started */
	int64_t bucket;                 /* start of the current activity bucket */
	uint32_t buckets;               /* finished buckets observed */
	uint32_t new_now;               /* new sites in the current bucket */
	uint64_t up_now, down_now;      /* bytes in the current bucket */
	double novelty;                 /* EWMA of new sites per bucket */
	double up_mean, up_var;         /* EWMA of upload per bucket */
	uint16_t active[24];            /* buckets with activity, by hour of day */
	uint8_t spike_alerted, unusual_alerted;
};

struct device {
	int used;
	char ip[IP_LEN];
	char mac[18];
	char hostname[64];
	char vendor_class[64];
	char dhcp_options[96];          /* DHCP option 55 list, a device fingerprint */
	char type[24];                  /* rule-based guess until the device model lands */
	int64_t first_seen, last_seen;

	/* DNS */
	uint32_t queries;
	struct top_item doms[DEV_DOM_MAX];      /* by lookups */

	/* flows */
	uint32_t flows, flows_no_dns;
	uint64_t bytes_up, bytes_down;
	struct top_item dsts[DEV_DST_MAX];      /* by bytes */

	/* Wi-Fi */
	int wifi_connected;
	int wifi_signal;                /* dBm */
	uint32_t wifi_rate_kbps;        /* AP -> device */
	int64_t wifi_since;
	char wifi_iface[24];
	unsigned wifi_seen;             /* poll sequence */
	char wifi_key[32];              /* name of its own Wi-Fi key, if it joined with one */

	char zone[16];                  /* "iot", ... ; policies can be limited to a zone */

	/* plug-ins (MQTT telemetry, ...) that reported about this device */
	uint32_t plugin_events;
	char plugin_last[48];           /* last event type */
	int64_t plugin_last_ts;

	struct baseline bl;
};

/* main.c */
extern struct ag_config cfg;
extern int64_t started;
void agd_emit(const char *type, struct blob_buf *b);
/* also to plug-ins subscribed to it, except `from` (the plug-in that sent it) */
void agd_emit_from(const char *type, struct blob_buf *b, const void *from);

/* devices.c */
extern struct device devs[DEV_MAX];
struct device *dev_find_ip(const char *ip, int create);
struct device *dev_find_mac(const char *mac, int create);
struct device *dev_learn(const char *ip, const char *mac, const char *hostname);
int dev_count(void);
void dev_guess_type(struct device *d);
void dev_blob(struct blob_buf *b, const struct device *d, int full);
void top_add(struct top_item *t, int n, const char *name, uint64_t inc);
void top_sorted(const struct top_item *t, int n, struct top_item *out);
void local_addrs_refresh(void);
int is_local(const char *ip);
int is_lan_client(const char *ip);
int is_private(const char *ip);
void dev_poll_leases(void);
void dev_poll_arp(void);
int dev_state_save(void);
int dev_state_load(void);

/* endpoint.c: which name each IP was looked up as (the DNS <-> flow join) */
void ep_add(const char *ip, const char *domain, const char *client, uint32_t ttl);
const char *ep_lookup(const char *ip);
const char *ep_lookup_near(const char *ip);
int ep_count(void);
void ep_save(struct blob_buf *b);
int ep_load(struct blob_attr *arr);

/* flow.c */
extern uint64_t flows_total, flows_no_dns_total;
int flow_init(void);
int flow_active(void);
void flow_blob_recent(struct blob_buf *b, unsigned limit);

/* alerts.c */
int alerts_init(void);
const char *alerts_model_name(void);
int alerts_model_trees(void);
double alerts_domain_score(const char *qname, char *label, size_t n, float *features);
void alerts_on_dns(const char *client, const char *qname);
void alerts_on_flow(const char *client, const char *dst, const char *domain, const char *proto,
		    unsigned dport, int no_dns);
uint64_t alerts_total(void);
void alerts_blob(struct blob_buf *b, unsigned limit, const char *name);
void alerts_save(struct blob_buf *b);
void alerts_load(struct blob_attr *arr);

void alerts_raise(const char *kind, const char *severity, const char *ip, const char *subject,
		  double score, const char *reason);
/* the domain-risk model: is this one usable here; start using it (takes ownership) */
struct ag_model;
int alerts_model_check(const struct ag_model *m, char *err, size_t n);
void alerts_model_use(struct ag_model *m);

/* models.c: packaged models, signed updates (hot-loaded), rollback */
void models_init(void);
int models_offer(const char *file, const char *sig, const char *origin, int force, int *version,
		 char *err, size_t errlen);
int models_rollback(const char *name, char *err, size_t errlen);
void models_blob(struct blob_buf *b, const char *name);
int models_version(const char *name);

/* act.c: decisions, guardrails, actuators */
struct act_opts {
	int rate_kbps;              /* throttle */
	const char *device;         /* block: just this device (IP, MAC or name); NULL = all */
	const char *origin;         /* who asks; NULL = "you" */
};

void act_init(void);
void act_tick(void);
void act_on_alert(uint32_t alert_id, const char *kind, const char *ip, const char *reason);
int act_request(const char *action, const char *what, int ttl, const struct act_opts *o,
		const char *reason, uint32_t *id, char *err, size_t errlen);
int act_approve(uint32_t id, int ttl, char *err, size_t errlen);
int act_reject(uint32_t id, char *err, size_t errlen);
int act_revert(uint32_t id, char *err, size_t errlen);
int act_count(int active);
uint64_t act_applied_total(void);
void act_blob(struct blob_buf *b, unsigned limit, const char *name);
void act_save(struct blob_buf *b);
void act_load(struct blob_attr *arr);
/* plug-ins: a proposal waiting for a person; an actuator plug-in's result; re-send after a restart */
int act_propose(const char *action, const char *what, int ttl, const char *origin, const char *reason,
		uint32_t *id, char *err, size_t errlen);
void act_plugin_result(uint32_t id, int ok, const char *error);
void act_plugin_started(const char *plugin);
void act_plugin_gone(const char *plugin);
struct device *act_find_device(const char *q);

/* plugin.c: plug-in host (discovery, checks, wiring, supervision) */
void plugin_init(void);
void plugin_tick(void);
void plugin_stop_all(void);
void plugin_dispatch(const char *type, struct blob_attr *msg, const void *from);
int plugin_action(const char *action, int *device_subject, char *err, size_t errlen);
int plugin_act(const char *action, uint32_t id, const char *target, const struct device *dv, int ttl,
	       const char *reason, int undo, char *err, size_t errlen);
const struct ag_policy *plugin_policy_for(const char *kind, const char *zone, char *origin, size_t n, int *cap_mode);
int plugin_count(int running);
void plugin_blob(struct blob_buf *b, const char *name, const char *only);
int plugin_set(const char *name, const char *what, int on, char *err, size_t errlen);
int plugin_config(const char *name, const char *key, const char *value, char *err, size_t errlen);
void plugin_rescan(void);

/* keys.c: per-device Wi-Fi keys (hostapd wpa_psk_file) and QR onboarding */
void keys_init(void);
void keys_tick(void);
void keys_seen(struct device *d, const char *keyid);
int keys_add(const char *name, const char *zone, int ttl, struct blob_buf *reply, char *err, size_t errlen);
int keys_revoke(const char *name, char *err, size_t errlen);
void keys_blob(struct blob_buf *b, const char *only);
int keys_count(void);

/* baseline.c */
void baseline_domain(struct device *d, const char *qname);
void baseline_traffic(struct device *d, uint64_t up, uint64_t down);
void baseline_tick(void);
int baseline_steady(const struct device *d);
void baseline_blob(struct blob_buf *b, const struct device *d, int full);
void baseline_load(struct device *d, struct blob_attr *tbl);

/* wifi.c */
void wifi_init(void);
void wifi_poll(void);
int wifi_station_count(void);
int wifi_ban(const char *mac, int on, char *err, size_t errlen);
/* every access point: send a command; returns how many answered OK (reply of the first in *reply) */
int wifi_all(const char *cmd, char *reply, size_t n);
int wifi_kick(const char *mac);

#endif
