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
	int64_t steer_up_at;            /* landed on 5 GHz after a move up (0: none being watched) */
	int64_t steer_up_pause_until;   /* came back down soon after: no move up until then */
	uint8_t steer_bounces;          /* in a row: each doubles the pause */

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
int agd_connected(void);                /* on ubus (events reach anyone) */

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
void keys_blob(struct blob_buf *b, const char *only, int secret);
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
/* the other channels AIGW would use on this access point (allowed here, not avoided), quietest known first */
int chan_options(const struct wifi_ap *a, int *out, int max);
/* neighbour-aware channels (ag/chanplan.h): a channel that didn't help, avoided for secs */
void chan_avoid(const char *iface, int ch, int secs);
void chan_plan_blob(struct blob_buf *b, int table);    /* ag channels; table 0: verdicts and counts only */
int chan_plan_field(const char *iface, const char *f, char *buf, size_t n);     /* ChannelPlan.Radio.{i}.; -1 */
int chan_plan_words(char *buf, size_t n);              /* brief.c: worth saying now, in words; 0 nothing */
void chan_save(struct blob_buf *b);
void chan_load(struct blob_attr *t);
/* dm.c: ChannelPlan! (State: Proposed, Applied, Kept, MovedBack) */
void dm_on_chanplan(const char *iface, int from, int to, const char *state, const char *reason, const char *before,
		    const char *after);
/* calls.c: a call or game is on now, anywhere in the home */
int calls_live(void);

/* presence: arrivals and departures */
void presence_tick(void);
const char *presence_near(const struct device *d);
int presence_count(void);
int act_is_protected(const struct device *d);
int wifi_ban(const char *mac, int on, char *err, size_t errlen);
/* the same, only on the access points of one band (2 or 5) */
int wifi_ban_band(const char *mac, int band, int on, char *err, size_t errlen);
/* act.c, for Self-Healing Wi-Fi */
int act_channel_for(const char *iface, const char *policy, uint32_t *id, int *active, int *ours, int *from);
int act_channel_recent(const char *target, int64_t since);
int act_on_device(const struct device *dv, char *out, size_t n, int *blocked, int *limited, int *on24);
int act_keep(uint32_t id, const char *note);
int act_undo_because(uint32_t id, const char *note);
int act_active(const char *action, const char *target);
/* every access point: send a command; returns how many answered OK (reply of the first in *reply) */
int wifi_all(const char *cmd, char *reply, size_t n);
int wifi_kick(const char *mac);

/* heal.c: Self-Healing Wi-Fi (measure, diagnose, report) */
struct heal_radio {
	char iface[24];
	int status, cause;              /* enum heal_status / heal_cause (ag/heal.h) */
	int measured, loss_pct, rtt_ms;
	int64_t since;                  /* the verdict holds since */
	int64_t cause_since;            /* this cause, whether Degraded or Poor, since */
	int64_t problem_since;          /* not Good since (0: Good or Unknown) */
	char explanation[400];
	char affected[256];             /* MACs, comma-separated */
	int pend_status, pend_cause, pend_n;
};
void heal_init(void);
/* wifi.c: it moved to another access point; its band and signal before */
void heal_roamed(const struct device *d, int old_band, int old_signal);
int heal_hopping(const struct device *d);       /* band changes in the window >= wifi.heal_hops */
const struct heal_radio *heal_for(const char *iface);
void heal_blob(struct blob_buf *b, const char *iface);
void dm_on_heal(const char *iface);
/* act.c: a device was kept on 2.4 GHz (stay24 in effect) */
void heal_on_band_rule(const char *mac, uint32_t decision_id);
/* the healing report of one access point over the last `days` days; -1 none */
struct heal_day;
int heal_report_of(const char *iface, int days, struct heal_day *out);
void heal_device(const struct device *d, int *loss_pct, int *rtt_ms, int *samples);

/* diag.c: Diagnose ("my internet is slow"): measures for a few seconds, then
   calls back with the verdict (ag/diag.h); one at a time, others wait for it */
struct diag_result;
struct diag_input;
typedef void (*diag_cb)(void *ctx, const struct diag_result *r, const struct diag_input *in);
int diag_start(const char *host, diag_cb cb, void *ctx, char *err, size_t errlen);
void diag_blob(struct blob_buf *b, const struct diag_result *r, const struct diag_input *in);
/* the last verdict, for the data model: 0 none yet */
int64_t diag_last(const struct diag_result **r, const struct diag_input **in, const char **host);
/* dm.c: DiagnosisDone! (mac: the device asked about, "" none) */
void dm_on_diag(const struct diag_result *r, const struct diag_input *in, const char *mac);
void heal_save(struct blob_buf *b);
void heal_load(struct blob_attr *t);
/* the same, quietly, for proactive care: no LastDiagnosis, no DiagnosisDone!; -1 busy */
int diag_check(diag_cb cb, void *ctx);

/* care.c: proactive care (checks by itself, tells the ISP, the experience score) */
struct care_state;
struct care_day;
void care_init(void);
void care_on_heal(int status);
void care_on_acted(const char *what);   /* AIGW changed something on Wi-Fi: "fixed by AIGW" needs it */
int care_check_now(void);
const struct care_state *care_now(void);
int care_report_of(int days, struct care_day *out);
void care_blob(struct blob_buf *b);
void care_save(struct blob_buf *b);
void care_load(struct blob_attr *t);
/* the open problem: a check's, else the nightly speed test's; NULL none */
const struct care_state *care_open(void);
/* early warning (ag/care.h): the open one (active 0: none), learning, in words */
struct care_warning;
const struct care_warning *care_warning_now(void);
int care_warning_learning(void);
void care_warning_words(char *buf, size_t n);
int care_warning_became(void);          /* the warning that just ended: a problem followed */
int care_normal_now(int metric, float *v);      /* the normal at this hour; -1 not known */
int care_learned_days(void);
const char *care_speed_status(void);    /* None, OK, Low, Error */
int care_speed_now(void);               /* the speed test now; -1 no care.speed_url */
int care_speed_url_now(const char *url, char *err, size_t en); /* a person's one test against their server */
struct tr143_out;
void care_speed_seen(const struct tr143_out *o, const char *who); /* the ISP's TR-143 download: the line's speed */
void care_days_blob(struct blob_buf *b, int days);      /* the last days' rows (cortex's fleet report) */
long care_speed_last(double *mbps);     /* when the last speed test ran (wall s), its Mbit/s */
/* dm.c: ProblemDetected! (1) or ProblemCleared! (0); EarlyWarning! (1) or EarlyWarningCleared! (0) */
void dm_on_care(const struct care_state *s, int detected);
void dm_on_warning(int opened);

/* tr143.c: TR-143 diagnostics (ag/tr143.h): one test of each kind at a time, in a child process */
struct tr143_in;
struct tr143_out;
typedef void (*tr143_cb)(void *ctx, int kind, const struct tr143_out *o);
void tr143_init(void);
int tr143_start(int kind, const struct tr143_in *in, const char *who, tr143_cb cb, void *ctx, char *err, size_t en);
int tr143_running(int kind);
const struct tr143_out *tr143_last(int kind, int64_t *at);     /* *at 0: no result (yet, or stale) */
/* a command's arguments (from the kind's defaults); -1 with err when one isn't valid */
int tr143_args(int kind, struct blob_attr *args, struct tr143_in *in, char *err, size_t en);
void tr143_failed(int kind, int status, const char *msg);
/* TR-069's writable inputs and DiagnosticsState: check (apply 0), then apply */
int tr143_set(int kind, const char *name, const char *v, int apply, char *err, size_t en);
/* an input, DiagnosticsState or a result by its TR-181 name; inst > 0: RouteHops / Result row */
int tr143_value(int kind, const char *name, uint32_t inst, char *buf, size_t n);
int tr143_rows(int kind);
void tr143_out_blob(struct blob_buf *b, int kind, const struct tr143_out *o);

/* manage.c: Wi-Fi settings and firmware, as a management system changes them */
enum { WS_SSID, WS_KEY, WS_MODE, WS_CHANNEL, WS_WIDTH, WS_N };
extern const char *const wifi_modes[];
void wifi_mode(const char *iface, char *buf, size_t n);
const char *wifi_handler(void);
int wifiset_check(const char *iface, int what, const char *v, char *err, size_t en);
void wifiset_stage(const char *iface, int what, const char *v);
void wifiset_flush(const char *who);      /* give what was staged to the handler */
void wifiset_drop(void);                  /* forget what was staged */
/* already in effect: save it in the Wi-Fi config, restart nothing */
void wifiset_save(const char *iface, int what, const char *v, const char *who);
const char *wifiset_pending(const char *iface, int what);
void wifiset_seen(const char *iface, int what, const char *v);

#define FW_MAX 4
struct fw_image {
	uint32_t id;
	char status[24];                /* TR-181: NoImage, Active, Downloading, Available, ... */
	int active, boot;
	char version[64];
	char name[64];
};
struct fw_request {
	const char *url, *user, *password, *size, *alg, *sum;
	int activate, reboot;
};
typedef void (*fw_done_t)(void *ctx, int ok, const char *msg);
const char *fw_handler(void);
int fw_list(struct fw_image *out, int max);      /* reused for FW_CACHE s: for reads */
int fw_list_now(struct fw_image *out, int max);  /* asked now: for decisions */
int fw_find(uint32_t id, struct fw_image *f);
int fw_ids(uint32_t *out, int max);
void fw_version(char *buf, size_t n);
int fw_download(uint32_t id, const struct fw_request *r, const char *who, fw_done_t done, void *ctx, char *err, size_t en);
int fw_activate(uint32_t id, int reboot, const char *who, fw_done_t done, void *ctx, char *err, size_t en);
/* "reboot" or "factory_reset", through the firmware handler, 2 s from now */
int fw_system(const char *action, const char *who, char *err, size_t en);
/* how this gateway goes back to the previous image (the handler's "rollback"):
   "TwoBanks", "KeptCopy", "None" or "Unknown", and in words into text */
const char *fw_rollback(char *text, size_t n);

/* upgrade.c: firmware upgrades that check themselves (ag/upgrade.h) */
#define UPGRADE_WHO "upgrade check"     /* the rollback's own activation: not watched */
struct care_sample;
void upgrade_init(void);                /* after the state and proactive care */
void upgrade_tick(void);
/* manage.c: an image was activated (boots next) */
void upgrade_on_activated(uint32_t to_id, const char *to_ver, uint32_t from_id, const char *from_ver, const char *who);
void upgrade_on_activate_failed(const char *who);       /* ... the handler refused it after all */
/* manage.c: 1 (with err) when this version was rolled back and is held off */
int upgrade_refused(const char *version, char *err, size_t n);
int upgrade_forget(void);               /* lift the hold; 1 when there was one */
const char *upgrade_file(void);         /* the record's path ("" none): carried over a rollback */
/* care.c: a check (the normal at this hour; have[m] 0 not known), a speed test, how often to check now (0: as usual) */
void upgrade_on_check(const struct care_sample *s, const struct care_sample *normal, int online, int problem, long max_gap);
void upgrade_on_speed(float mbps);
int upgrade_quick(void);
void upgrade_blob(struct blob_buf *b, int devices);     /* devices 0: no addresses or names */
int upgrade_field(const char *f, char *buf, size_t n);  /* the data model's Upgrade. values; -1 unknown */
/* dm.c: UpgradeChecked! (Kept, RolledBack, Worse, NotBooted, RollbackConfirmed, RollbackNotConfirmed) */
void dm_on_upgrade(const char *result, const char *from, const char *to, const char *reasons, const char *summary);
/* care.c: did the internet answer on the last check: 1, 0, -1 not checked yet */
int care_online(void);
/* care.c: the normal of a metric (enum care_metric) at a local hour; -1 not known */
int care_normal_hour(int metric, int hour, float *v);

/* why.c: "why was it slow?": the home's timeline (ag/why.h) */
struct why_result;
struct why_in;
void why_init(void);                    /* after proactive care */
void why_save(void);
void why_on_emit(const char *type, struct blob_attr *msg);     /* every event agd sends */
void why_on_dns(int rcode, uint32_t latency_us);
/* a stretch explained (host: a name, MAC or IP; "" the home); -1 with err */
int why_ask(long from, long to, const char *host, struct why_result *r, struct why_in *in, char *err, size_t en);
void why_blob(struct blob_buf *b, long from, long to, const char *host, int raw);
int why_field(const char *f, char *buf, size_t n);      /* Device.X_AIGW_.Timeline.; -1 unknown */

/* offers.c: signals for the ISP (ag/offer.h), counted from the timeline's samples */
struct why_min;
void offers_init(void);                 /* after the state */
void offers_tick(void);
void offers_on_sample(const struct why_min *m, long secs, float peak_down_mbps, float peak_up_mbps);
void offers_on_weak(const char *mac, long secs);
void offers_on_wifi(const char *mac, int joined);
void offers_blob(struct blob_buf *b, int names);
void offers_care_blob(struct blob_buf *b);      /* for cortex: active ones, no names; none unless offers.share */
int offers_field(const char *f, char *buf, size_t n);  /* Device.X_AIGW_.Offers.; -1 unknown */
void offers_save(struct blob_buf *b);
void offers_load(struct blob_attr *t);
/* dm.c: OfferFound! */
void dm_on_offer(const char *code, const char *kind, const char *product, const char *confidence, const char *evidence);
/* why.c, for smart queueing: the WAN sampled, the samples [from, to) oldest first, the line now */
struct sqm_line;
const char *why_wan(void);
int why_samples(long from, long to, struct why_min *out, int max);
void why_line_now(struct sqm_line *l);

/* sqm.c: self-tuning smart queueing (ag/sqm.h) */
void sqm_init(void);
void sqm_on_sample(void);               /* every sample of the timeline */
int sqm_target_ok(const char *target, char *words, size_t wn); /* "eth0:45000:8500": 0, in words */
int sqm_apply(const char *target, char *err, size_t en);
void sqm_remove(const char *target);
int sqm_on(void);                       /* in effect (a trial, kept, or a person's) */
int sqm_request(int on, uint32_t *id, char *err, size_t en);   /* agctl sqm --on / --off */
void sqm_blob(struct blob_buf *b);
int sqm_field(const char *f, char *buf, size_t n);     /* Device.X_AIGW_.SQM.; -1 unknown */
void sqm_save(struct blob_buf *b);
void sqm_load(struct blob_attr *t);
int sqm_shaping(char *ifc, size_t n);   /* the WAN AIGW's smart queueing shapes now; -1 none */
int sqm_prio(const char *ifc, const char *const *eps, int n, char *out, size_t on);

/* calls.c: calls and games first (ag/calls.h) */
void calls_init(void);
void calls_counters(uint32_t id, const char *client, const char *dst, const char *domain, uint8_t proto,
		    uint16_t dport, uint64_t up_b, uint64_t down_b, uint64_t up_pk, uint64_t down_pk);
void calls_flow_end(uint32_t id);
void calls_dump_done(void);             /* after each look at all connections' counters */
void calls_sqm_changed(void);           /* smart queueing applied or removed: priority to be set again */
void calls_blob(struct blob_buf *b);
int calls_field(const char *f, char *buf, size_t n);    /* Device.X_AIGW_.CallsFirst.; -1 unknown */
void calls_save(struct blob_buf *b);
void calls_load(struct blob_attr *t);
/* why.c: a call or game on the timeline (mac: the device) */
void why_note_call(const char *mac, int verdict, const char *text);
void why_hide_calls(int on);            /* dm.c: WhySlow() from the ISP with calls.share 0 */
/* dm.c: CallReport! */
void dm_on_call(const char *kind, const char *verdict, long duration, long full_s, uint32_t line_kbps, int ms,
		int jitter, int loss10, int prioritized, const char *summary);

/* sqm.c: smart queueing proposed on the line as it is now (waits for a yes) */
int sqm_propose(const char *origin, const char *reason, uint32_t *id, char *err, size_t en);

/* brief.c: the help-desk assistant (ag/brief.h): asks the home if wanted, then answers once */
struct brief;
struct brief_ask {
	long from, to;                  /* the period (wall s) */
	char host[64];                  /* a device (name, MAC or IP); "" the home */
	int check;                      /* a fresh quiet Diagnose first */
	int speed;                      /* ... and the speed test (care.speed_url) */
	int propose;                    /* a remote fix becomes a proposed decision (never applied) */
	int names;                      /* devices by name; 0 by kind (the ISP, unless brief.names) */
	int hide_calls;                 /* calls.share 0 and the ISP asks */
	char who[48];                   /* who asks: "you", "usp", "acs", ... */
};
struct brief_meta {
	long from, to;
	int checked, speed_test, names;
	long data_age;                  /* s since the newest fact used; -1 not known */
	uint32_t decision;              /* the fix's decision (proposed now, or waiting already) */
	int proposed;                   /* ... proposed by this brief */
	char propose_error[160];
};
typedef void (*brief_cb)(void *ctx, const struct brief *b, const struct brief_meta *m);
int brief_start(const struct brief_ask *a, brief_cb cb, void *ctx, char *err, size_t en);
void brief_blob(struct blob_buf *b, const struct brief *r, const struct brief_meta *m);
void brief_report_blob(struct blob_buf *b);     /* the bridge: the last hour, no fresh check, the ISP's names rule */
void brief_save(struct blob_buf *b);
void brief_load(struct blob_attr *t);
void dm_serial(char *buf, size_t n);    /* dm.c: Device.DeviceInfo.SerialNumber */
/* main.c / dm.c: a period in words ("tonight 20:00-22:00") or Start/End */
int brief_parse_period(const char *words, long *from, long *to, char *err, size_t en);

/* area.c: an area outage cortex told of (many homes of this uplink at once): fixes held meanwhile */
enum { AH_SQM, AH_SQM_JUDGE, AH_STEER, AH_CHANNEL, AH_STAY24, AH_N };
struct area_now {
	uint32_t incident;              /* 0: none */
	const char *kind, *confidence, *uplink;
	long since, until;              /* until: the notice lapses unless renewed */
	int homes, of;
	unsigned held;                  /* bits of AH_*: what was held so far */
};
int area_notice(struct blob_attr *a, const char *who, char *err, size_t en);  /* {incident, kind, ...}; 0 */
int area_hold(int what);                /* 1: hold it (an area outage is on), 0: go ahead */
int area_holds_action(const char *action);      /* sqm, channel, stay24: area_hold; others never */
int area_get(struct area_now *o);       /* 1 while one is on */
void area_held_words(char *buf, size_t n);
void area_wan(char *gw, size_t gn, char *peer, size_t pn);      /* the WAN's next hop, else its peer (BNG) */
void area_status_blob(struct blob_buf *b);      /* uplink_id, wan_gateway, bng: for cortex */
int area_set_uplink(const char *v, char *err, size_t en);
int area_field(const char *f, char *buf, size_t n);    /* Device.X_AIGW_.AreaOutage.; -1 unknown */
void area_blob(struct blob_buf *b);
/* dm.c: AreaOutage! (started 1) or AreaOutageCleared! (0) */
void dm_on_area(int started, uint32_t incident, const char *kind, long since, long duration, int homes, int of,
		const char *confidence, const char *uplink, const char *held, const char *why);
/* brief.c, for proactive care: a brief from what is known (no fresh check, the ISP's names rule);
   a text with the home's names taken out for the ISP */
int brief_quick(long from, long to, const char *host, struct brief *out);
void brief_scrub_isp(char *text, size_t n);

/* proactive.c: proactive care, before the call (ag/proactive.h) */
void proactive_init(void);              /* after the state, proactive care and the timeline */
void proactive_on_acted(const char *action, const char *target, uint32_t id);  /* act.c: a fix took effect */
void proactive_on_called(void);         /* brief.c: the help desk was asked */
void proactive_on_wifi(struct blob_attr *msg);         /* main.c: an ag.wifi event (joined, left) */
int proactive_run(void);                /* a look now; -1 off */
void proactive_blob(struct blob_buf *b, int names);   /* names 0: the ISP's view (no device targets) */
void proactive_report_blob(struct blob_buf *b);        /* the bridge: counts and tickets, no names */
int proactive_ids(uint32_t *out, int max);              /* the data model's Ticket.{i}. */
int proactive_field(uint32_t id, const char *f, char *buf, size_t n);  /* -1 unknown */
int proactive_count(const char *f, char *buf, size_t n);              /* Device.X_AIGW_.Proactive.; -1 unknown */
void proactive_save(struct blob_buf *b);
void proactive_load(struct blob_attr *t);
/* dm.c: ProactiveTicket! */
void dm_on_ticket(uint32_t id);
int sqm_auto(const char *origin, const char *reason, uint32_t *id, int *applied, char *err, size_t en);
/* act.c: a fix by proactive care, as act.mode allows (enforce applies, advise proposes) */
int act_auto(const char *action, const char *what, int ttl, const char *origin, const char *reason, uint32_t *id,
	     int *applied, char *err, size_t errlen);

/* calls.c, for mesh steering: a call or game is on for this device now */
int calls_live_on(const char *mac);

/* mesh.c: smarter mesh Wi-Fi over a standard EasyMesh controller (ag/mesh.h) */
void mesh_init(void);                   /* after the state and act */
int mesh_run(void);                     /* read the controller now; -1 off */
/* act.c: actions meshsteer ("<station MAC>@<target BSSID>") and meshchan ("<radio RUID>:<channel>"): a request
   checked now (target, scope: where it is now; device: in words); 0, or -1 with err */
int mesh_guard(const char *action, const char *what, char *target, size_t tn, char *scope, size_t sn, char *device,
	       size_t dn, char *err, size_t en);
int mesh_target_ok(const char *action, const char *target);    /* the target's form only */
/* act.c: carry a decision out through the controller (undo 0), or take it back (undo 1) */
int mesh_apply(uint32_t id, const char *action, const char *target, const char *scope, int undo, char *err, size_t en);
void mesh_blob(struct blob_buf *b, int names);
void mesh_report_blob(struct blob_buf *b);      /* the bridge: counts and findings, no names */
int mesh_field(const char *f, char *buf, size_t n);    /* Device.X_AIGW_.Mesh.; -1 unknown */
void mesh_save(struct blob_buf *b);
void mesh_load(struct blob_attr *t);
/* brief.c, proactive.c: what is wrong in the mesh now (an extender's weak backhaul) */
struct mesh_issue {
	char code[24];                  /* MESH_BACKHAUL */
	char target[18];                /* the extender (its AL MAC) */
	char kind[8];                   /* Weak, Slow, Busy, On24, Try6 */
	char text[400];                 /* in words, with the advice */
	long since;
	int stations;                   /* devices behind it */
	int hurts;                      /* weak or slow with devices behind it */
};
int mesh_issues(struct mesh_issue *out, int max);
/* dm.c: MeshDecision! */
void dm_on_mesh(uint32_t id, const char *kind, const char *state, const char *station, const char *from,
		const char *to, const char *reason, const char *before, const char *after);

/* act.c: the live decision of an action (state 0 proposed, 1 active, 2 kept); -1 none */
int act_find_action(const char *action, uint32_t *id, int *state, char *target, size_t tn, int64_t *applied,
		    char *origin, size_t on);
int act_recent(const char *action, int64_t since);     /* one made since, in any state */

/* care.c: proactive care's days (for the signals); how many */
struct care_day;
int care_days_get(struct care_day *out, int max);

/* handler.c: platform handlers (Wi-Fi settings, firmware), replaceable by an OEM */
typedef void (*handler_done_t)(void *ctx, int code, const char *msg);
const char *handler_path(const char *configured, const char *name);
/* in the background: done(ctx, exit code, its last line) on the loop; 0 when started */
int handler_start(const char *prog, char *const argv[], char *const env[], handler_done_t done, void *ctx);
/* and wait (for quick questions): exit code, output in out, -1 on timeout */
int handler_run(const char *prog, char *const argv[], char *out, size_t n, int timeout_ms);

#endif
