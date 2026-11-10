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

/* Its recent lookups of names nobody in the home ever used (burst.c). Not saved. */
#define BURST_RING 32
struct burst_item {
	int64_t ts;
	uint32_t h;                     /* registrable domain, hashed */
	uint8_t nx;                     /* got "no such name" */
	uint8_t flagged;                /* the name model calls it machine-generated */
};
struct burst {
	struct burst_item it[BURST_RING];
	unsigned head;
	int64_t alerted;                /* last burst alert */
	int64_t hit_until;              /* a never-used name answering before this is named */
	uint8_t hit_named;
	char cand[NAME_LEN];            /* a never-used name that answered right after a failed one */
	int64_t cand_ts;
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
	int wifi_band;                  /* 2 (2.4 GHz) or 5, 0 = unknown */
	double wifi_sig_avg;            /* smoothed signal, dBm */
	uint8_t wifi_btm;               /* takes 802.11v "please move" requests */
	uint8_t wifi_dual;              /* can use 5 GHz (seen there, or says so when joining) */

	/* band steering (steer.c) */
	int64_t steer_cond_since;       /* signal past the line since; 0 = not */
	int64_t steer_last;             /* last move asked */
	int64_t steer_check_at;         /* see whether it moved, then */
	int steer_from_band;
	char steer_method[8];           /* "btm" or "kick" */
	char steer_unban[24];           /* "kick": access point whose deny entry to lift */
	int64_t steer_unban_at;
	uint32_t steer_moves, steer_misses;
	uint8_t steer_fails;            /* misses in a row */
	int64_t steer_pause_until;      /* won't move: leave it be until then */

	/* presence */
	uint8_t present;
	int64_t present_since;

	char zone[16];                 /* "iot", ... ; policies can be limited to a zone */

	/* plug-ins (MQTT telemetry, ...) that reported about this device */
	uint32_t plugin_events;
	char plugin_last[48];           /* last event type */
	int64_t plugin_last_ts;

	struct baseline bl;
	struct burst burst;

	/* traffic meter (meter.c, eBPF): kernel totals per MAC, live rates */
	uint64_t meter_up, meter_down, meter_pkts;
	uint64_t rate_up, rate_down;    /* bits/s over the last second */
	int64_t meter_at;
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
int flow_init(void);
int flow_active(void);
void flow_blob_recent(struct blob_buf *b, unsigned limit);
struct ag_sni;
/* a connection's server name, read from its TLS or QUIC handshake */
void flow_named(const char *client, const char *dst, uint8_t proto, uint16_t sport, uint16_t dport,
		const struct ag_sni *s);
extern uint64_t flows_total, flows_no_dns_total, flows_sni_total;

/* sni.c: TLS / QUIC handshakes copied by the kernel (NFLOG) */
int sni_init(void);
int sni_active(void);
void sni_blob(struct blob_buf *b);

/* alerts.c */
int alerts_init(void);
const char *alerts_model_name(void);
int alerts_model_trees(void);
double alerts_domain_score(const char *qname, char *label, size_t n, float *features);
/* the name's score as judged (1 for a known threat), -1 when not judged */
double alerts_on_dns(const char *client, const char *qname);
void alerts_on_sni(const char *client, const char *dst, const char *name, unsigned dport, int ech);
void alerts_no_dns(const char *client, const char *dst, const char *proto, unsigned dport);
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

/* the trees' own score (no adapter); the subject of an alert by id (0, or -1 if gone) */
double alerts_tree_score(const char *qname, char *label, size_t n, float *features);
int alerts_subject(uint32_t id, char *kind, size_t kn, char *subject, size_t sn);

/* fed.c: federated learning: examples from your answers, local training, the shared layer */
#define FED_W 14                    /* tree logit, 12 features, bias */
struct fed_round {
	uint32_t round;
	double center[12], scale[12], w[FED_W];
	double lr, clip, noise;
	int epochs;
};
void *fed_adapter_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en);
int fed_adapter_check(const void *obj, char *err, size_t n);
void fed_adapter_use(void *obj);
void fed_adapter_free(void *obj);
int fed_adapter_version(const void *obj, char *published, size_t n);
int fed_adapter_size(const void *obj);
double fed_adjust(double p, const float *features);
int fed_label(const char *name, int label, const char *source, char *err, size_t n);
int fed_forget(const char *name);
void fed_on_answer(uint32_t alert_id, int label);
int fed_round_parse(struct blob_attr *tbl, struct fed_round *r, char *err, size_t n);
int fed_train(const struct fed_round *r, double *delta, int *n_used, double *noise, char *err, size_t n);
double fed_epsilon(double rho);
void fed_blob(struct blob_buf *b, int names);
void fed_save(struct blob_buf *b);
void fed_load(struct blob_attr *tbl);

/* fastpath.c: established connections through the kernel flowtable; back to the rules on an action */
int fastpath_release(const char *ip, const char *why);
int fastpath_release_mac(const char *mac, const char *why);
void fastpath_tick(void);
void fastpath_blob(struct blob_buf *b);

/* backend.c: who computes model scores (built in, or an NPU library), checked against the reference */
void backend_select(const struct ag_model *m);
double backend_prob(const struct ag_model *m, const float *x);
const char *backend_name(void);
void backend_blob(struct blob_buf *b);

/* meter.c: per-device traffic counted in the kernel by an eBPF program (tcx) */
void meter_init(void);
void meter_tick(void);
int meter_active(void);
void meter_blob(struct blob_buf *b);

/* intel.c: shared threat intelligence, a signed list delivered like a model */
void *intel_parse(const uint8_t *data, size_t len, char *name, size_t nn, char *err, size_t en);
int intel_check(const void *obj, char *err, size_t n);
void intel_use(void *obj);
void intel_free(void *obj);
int intel_version(const void *obj, char *published, size_t n);
int intel_size(const void *obj);
int intel_match(const char *qname, char *hit, size_t hn, char *why, size_t wn);

/* models.c: packaged models, signed updates (hot-loaded), rollback; also signed lists */
void models_init(void);
int models_offer(const char *file, const char *sig, const char *origin, int force, int *version,
		 char *err, size_t errlen);
int models_rollback(const char *name, char *err, size_t errlen);
void models_blob(struct blob_buf *b, const char *name);
int models_version(const char *name);
/* 0 when sigfile is a trusted key's usign signature of data; the key's fingerprint in signer */
int models_verify(const uint8_t *data, size_t len, const char *sigfile, char signer[17], char *err, size_t n);

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

/* main.c: a QR code as rows of '1'/'0' under field (for LuCI) */
void agd_qr_rows(struct blob_buf *b, const char *field, const char *text);

/* remote.c: remote access over WireGuard (peers with QR codes) */
void remote_init(void);
void remote_tick(void);
int remote_enable(int on, const char *endpoint, char *err, size_t n);
int remote_add(const char *name, struct blob_buf *reply, char *err, size_t n);
int remote_revoke(const char *name, char *err, size_t n);
int remote_count(int online);
void remote_blob(struct blob_buf *b);

/* baseline.c */
void baseline_domain(struct device *d, const char *qname);
void baseline_traffic(struct device *d, uint64_t up, uint64_t down);
void baseline_tick(void);
int baseline_steady(const struct device *d);
/* the device has a week of history and was never active in this hour of day */
int baseline_quiet_hour(const struct device *d);

/* dm.c: the data model for USP (TR-369) and TR-069 adapters: ubus object ag.dm */
void dm_init(void);
struct ubus_object;
struct ubus_object *dm_ubus_object(void);
void dm_on_alert(uint32_t id);
void dm_on_decision(uint32_t id, const char *state);
void dm_save(struct blob_buf *b);
void dm_load(struct blob_attr *tbl);

/* alerts.c / act.c, for dm.c: ids in ascending order; one field as text (-1 when gone) */
int alerts_ids(uint32_t *out, int max);
int alerts_field(uint32_t id, const char *f, char *buf, size_t n);
int act_ids(uint32_t *out, int max);
int act_field(uint32_t id, const char *f, char *buf, size_t n);
void act_answered_by(const char *who);

/* burst.c: many never-used names that don't exist, from one device */
enum { BURST_IGNORE, BURST_NX, BURST_OK };
void burst_dns(struct device *d, const char *qname, int outcome, int flagged);
void burst_blob(struct blob_buf *b);
void burst_save(struct blob_buf *b);
void burst_load(struct blob_attr *tbl);
void baseline_blob(struct blob_buf *b, const struct device *d, int full);
void baseline_load(struct device *d, struct blob_attr *tbl);

/* wifi.c */
void wifi_init(void);
void wifi_poll(void);
int wifi_station_count(void);

/* an access point (one hostapd interface) as of the last poll */
struct wifi_ap {
	char iface[24];
	char bssid[18];
	char ssid[33];
	int freq, channel, band;        /* band 2 or 5 */
	int width;                      /* 20, 40 or 80 MHz */
	int stations;
};
#define WIFI_AP_MAX 8
extern struct wifi_ap wifi_aps[WIFI_AP_MAX];
extern int wifi_n_aps;
/* one command to one access point; reply length or -1 */
int wifi_cmd(const char *iface, const char *cmd, char *reply, size_t n);
const struct wifi_ap *wifi_ap_find(const char *iface);

/* steer.c: band steering */
void steer_tick(void);
int steer_ready(const struct wifi_ap **a2, const struct wifi_ap **a5);
void steer_blob(struct blob_buf *b);
extern uint64_t steer_total, steer_ok_total;

/* chan.c: channel self-healing */
void chan_tick(void);
void chan_blob(struct blob_buf *b, const char *iface);
int chan_valid(int band, int ch);
int chan_parse(const char *target, const struct wifi_ap **ap, int *ch);
int chan_switch(const char *iface, int ch, char *err, size_t errlen);

/* presence: arrivals and departures */
void presence_tick(void);
const char *presence_near(const struct device *d);
int presence_count(void);
int act_is_protected(const struct device *d);
int wifi_ban(const char *mac, int on, char *err, size_t errlen);
/* every access point: send a command; returns how many answered OK (reply of the first in *reply) */
int wifi_all(const char *cmd, char *reply, size_t n);
int wifi_kick(const char *mac);

#endif
