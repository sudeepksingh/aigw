/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Alerts: the brain notices something worth a person's attention and says
 * why, in plain words. Policies (act.c) may turn an alert into a proposed
 * action, which waits for approval unless the policy is in enforce mode.
 *
 *   dga         a lookup of a name that looks machine-generated (domain-risk model)
 *   no-dns      a connection to a public address the device never looked up
 *   dns-bypass  a device using its own encrypted DNS, hidden from AIGW
 *
 * Repeats of the same alert (same kind, device and subject within a day)
 * are counted, not raised again.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "agd.h"
#include "ag/agm.h"
#include "ag/domfeat.h"

#define ALERT_MAX    256
#define REPEAT_SECS  86400

struct alert {
	int used;
	uint32_t id;
	int64_t first, last;
	uint32_t count;
	char kind[16];
	char severity[8];
	char device_ip[IP_LEN];
	char device_mac[18];
	char device_name[64];
	char subject[NAME_LEN];
	double score;
	char reason[400];
};

static struct alert alerts[ALERT_MAX];
static unsigned head;
static uint32_t next_id = 1;
static uint64_t raised_total;
static struct ag_model *dga_model;
static const float *bigram;
static int bigram_dim;

/* Public DNS-over-TLS / DNS-over-HTTPS services */
static const char *const resolver_ips[] = {
	"8.8.8.8", "8.8.4.4", "1.1.1.1", "1.0.0.1", "1.1.1.2", "1.0.0.2", "1.1.1.3", "1.0.0.3",
	"9.9.9.9", "149.112.112.112", "208.67.222.222", "208.67.220.220", "94.140.14.14",
	"94.140.15.15", "76.76.2.0", "76.76.10.0", "185.228.168.9", "185.228.169.9",
	"2001:4860:4860::8888", "2001:4860:4860::8844", "2606:4700:4700::1111",
	"2606:4700:4700::1001", "2620:fe::fe", "2620:fe::9", NULL
};
static const char *const resolver_names[] = {
	"dns.google", "dns.google.com", "cloudflare-dns.com", "one.one.one.one",
	"mozilla.cloudflare-dns.com", "chrome.cloudflare-dns.com", "security.cloudflare-dns.com",
	"family.cloudflare-dns.com", "dns.quad9.net", "dns11.quad9.net", "dns.adguard.com",
	"dns.adguard-dns.com", "doh.opendns.com", "dns.nextdns.io", "doh.cleanbrowsing.org", NULL
};

static int in_list(const char *s, const char *const *list)
{
	for (; s && *list; list++)
		if (!strcasecmp(s, *list))
			return 1;
	return 0;
}

int alerts_init(void)
{
	if (!cfg.alerts_enabled) {
		AG_INFO("alerts disabled in config");
		return -1;
	}
	return 0;                           /* the model arrives through models_init() */
}

/* A model may replace the running one only if it reads the features this
 * build extracts and gives sane scores on a few known names. */
int alerts_model_check(const struct ag_model *m, char *err, size_t n)
{
	static const char *const probe[] = { "google.com", "wikipedia.org", "xkqjzvbwptmr.com", NULL };
	float x[AG_DOMFEAT_N];
	char label[NAME_LEN];
	const float *bg;
	int dim, i;

	if (ag_model_schema(m) != ag_fnv1a(AG_DOMFEAT_SCHEMA) || ag_model_nfeatures(m) != AG_DOMFEAT_N) {
		snprintf(err, n, "trained for different features (needs %s, %d features)", AG_DOMFEAT_SCHEMA, AG_DOMFEAT_N);
		return -1;
	}
	bg = ag_model_bigram(m, &dim);
	if (!bg || dim <= 0 || ag_model_ntrees(m) <= 0) {
		snprintf(err, n, "no trees or no bigram table");
		return -1;
	}
	for (i = 0; probe[i]; i++) {
		double p;

		if (ag_domain_label(probe[i], label, sizeof(label)) < 0)
			continue;
		ag_domain_features(probe[i], label, bg, dim, x);
		p = ag_model_prob(m, x);
		if (!(p >= 0 && p <= 1)) {
			snprintf(err, n, "gives no usable score for %s", probe[i]);
			return -1;
		}
	}
	return 0;
}

/* swap models between two lookups: nothing holds the old one */
void alerts_model_use(struct ag_model *m)
{
	struct ag_model *old = dga_model;
	int dim = 0;

	dga_model = m;
	bigram = m ? ag_model_bigram(m, &dim) : NULL;
	bigram_dim = dim;
	if (old && old != m)
		ag_model_free(old);
	if (m)
		AG_INFO("domain-risk model in use: %d trees, alert at score >= %.2f", ag_model_ntrees(m),
			cfg.dga_threshold);
}

const char *alerts_model_name(void)
{
	return dga_model ? ag_model_name(dga_model) : "";
}

int alerts_model_trees(void)
{
	return dga_model ? ag_model_ntrees(dga_model) : 0;
}

/* the trees alone; -1 when the name isn't judged (no model, no registrable label) */
double alerts_tree_score(const char *qname, char *label, size_t n, float *features)
{
	float x[AG_DOMFEAT_N];

	if (!dga_model || ag_domain_label(qname, label, n) < 0)
		return -1;
	ag_domain_features(qname, label, bigram, bigram_dim, features ? features : x);
	return ag_model_prob(dga_model, features ? features : x);
}

/* what alerts use: the trees, then the shared layer homes learned together (fed.c) */
double alerts_domain_score(const char *qname, char *label, size_t n, float *features)
{
	float x[AG_DOMFEAT_N], *f = features ? features : x;

	return fed_adjust(alerts_tree_score(qname, label, n, f), f);
}

int alerts_subject(uint32_t id, char *kind, size_t kn, char *subject, size_t sn)
{
	int i;

	for (i = 0; i < ALERT_MAX; i++)
		if (alerts[i].used && alerts[i].id == id) {
			snprintf(kind, kn, "%s", alerts[i].kind);
			snprintf(subject, sn, "%s", alerts[i].subject);
			return 0;
		}
	return -1;
}

static void device_names(const char *ip, char *mac, char *name)
{
	struct device *d = dev_find_ip(ip, 0);

	snprintf(mac, 18, "%s", d ? d->mac : "");
	snprintf(name, 64, "%s", d && d->hostname[0] ? d->hostname : d && d->mac[0] ? d->mac : ip);
}

void alerts_raise(const char *kind, const char *severity, const char *ip, const char *subject,
			double score, const char *reason)
{
	char mac[18], name[64];
	int64_t now = ag_wall_s();
	struct alert *a;
	struct blob_buf b = { 0 };
	int i;

	device_names(ip, mac, name);
	for (i = 0; i < ALERT_MAX; i++) {
		a = &alerts[i];
		if (a->used && !strcmp(a->kind, kind) && !strcmp(a->subject, subject) &&
		    (mac[0] ? !strcasecmp(a->device_mac, mac) : !strcmp(a->device_ip, ip)) &&
		    now - a->last < REPEAT_SECS) {
			a->count++;
			a->last = now;
			return;
		}
	}

	a = &alerts[head];
	head = (head + 1) % ALERT_MAX;
	memset(a, 0, sizeof(*a));
	a->used = 1;
	a->id = next_id++;
	a->first = a->last = now;
	a->count = 1;
	snprintf(a->kind, sizeof(a->kind), "%s", kind);
	snprintf(a->severity, sizeof(a->severity), "%s", severity);
	snprintf(a->device_ip, sizeof(a->device_ip), "%s", ip);
	snprintf(a->device_mac, sizeof(a->device_mac), "%s", mac);
	snprintf(a->device_name, sizeof(a->device_name), "%s", name);
	snprintf(a->subject, sizeof(a->subject), "%s", subject);
	a->score = score;
	snprintf(a->reason, sizeof(a->reason), "%s", reason);
	raised_total++;
	AG_INFO("alert %u [%s] %s", a->id, kind, reason);

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "id", a->id);
	blobmsg_add_string(&b, "kind", kind);
	blobmsg_add_string(&b, "severity", severity);
	blobmsg_add_string(&b, "device", name);
	blobmsg_add_string(&b, "ip", ip);
	blobmsg_add_string(&b, "subject", subject);
	blobmsg_add_string(&b, "reason", reason);
	agd_emit(AG_EV_ALERT, &b);
	blob_buf_free(&b);

	/* a policy may propose (or, in enforce mode, take) an action */
	act_on_alert(a->id, kind, ip, reason);
}

void alerts_on_dns(const char *client, const char *qname)
{
	char label[256], reason[320], name[64], mac[18];
	double s;

	if (!cfg.alerts_enabled || is_local(client))
		return;
	{
		char hit[NAME_LEN], why[128];

		/* a name other homes (or you) confirmed as a threat: no model needed */
		if (intel_match(qname, hit, sizeof(hit), why, sizeof(why))) {
			device_names(client, mac, name);
			snprintf(reason, sizeof(reason), "%.63s looked up %.90s, a known threat (%.50s%s%.50s).", name, qname,
				 strcmp(hit, qname) ? hit : "", strcmp(hit, qname) ? ": " : "", why);
			alerts_raise("known-threat", "warning", client, hit, 1.0, reason);
			return;
		}
	}
	s = alerts_domain_score(qname, label, sizeof(label), NULL);
	if (s < cfg.dga_threshold || (int)strlen(label) < cfg.dga_min_label)
		return;
	device_names(client, mac, name);
	snprintf(reason, sizeof(reason),
		 "%s looked up %s, a name that looks machine-generated (score %.2f). "
		 "Malware generates names like this to find its control servers.", name, qname, s);
	alerts_raise("dga", "warning", client, qname, s, reason);
}

void alerts_on_flow(const char *client, const char *dst, const char *domain, const char *proto,
		    unsigned dport, int no_dns)
{
	char reason[400], name[64], mac[18];
	int doh_name = in_list(domain, resolver_names);

	if (!cfg.alerts_enabled)
		return;
	device_names(client, mac, name);
	if (dport == 853 || ((dport == 443 || dport == 8853) && (doh_name || in_list(dst, resolver_ips)))) {
		snprintf(reason, sizeof(reason),
			 "%s uses its own encrypted DNS (%s to %s), so AIGW can't see what it looks up. "
			 "Often a browser or phone \"private DNS\" setting.",
			 name, dport == 853 ? "DNS over TLS" : "DNS over HTTPS", domain && *domain ? domain : dst);
		alerts_raise("dns-bypass", "info", client, domain && *domain ? domain : dst, 0, reason);
		return;
	}
	if (no_dns)
		alerts_no_dns(client, dst, proto, dport);
}

void alerts_no_dns(const char *client, const char *dst, const char *proto, unsigned dport)
{
	char reason[400], name[64], mac[18];

	if (!cfg.alerts_enabled)
		return;
	device_names(client, mac, name);
	snprintf(reason, sizeof(reason),
		 "%s connected to %s (%s/%u) without looking up any name first. "
		 "Apps rarely do this; malware and hard-coded trackers do.", name, dst, proto, dport);
	alerts_raise("no-dns", "info", client, dst, 0, reason);
}

/* A name from a connection's handshake that AIGW never saw looked up (or saw
 * for another address): judged like a lookup. An Encrypted Client Hello's
 * outer name is the provider's, not the site's, so it is not judged. */
void alerts_on_sni(const char *client, const char *dst, const char *sni, unsigned dport, int ech)
{
	char reason[400], name[64], mac[18], label[256], hit[NAME_LEN], why[128];
	double s;

	if (!cfg.alerts_enabled || ech || is_local(client))
		return;
	device_names(client, mac, name);
	if (in_list(sni, resolver_names)) {
		snprintf(reason, sizeof(reason),
			 "%s uses its own encrypted DNS (DNS over HTTPS to %s, named in the connection's handshake), "
			 "so AIGW can't see what it looks up. Often a browser or phone \"private DNS\" setting.", name, sni);
		alerts_raise("dns-bypass", "info", client, sni, 0, reason);
		return;
	}
	if (intel_match(sni, hit, sizeof(hit), why, sizeof(why))) {
		snprintf(reason, sizeof(reason), "%.63s connected to %.90s (%s), a known threat (%.50s%s%.50s), "
			 "without looking it up through AIGW.", name, sni, dst,
			 strcmp(hit, sni) ? hit : "", strcmp(hit, sni) ? ": " : "", why);
		alerts_raise("known-threat", "warning", client, hit, 1.0, reason);
		return;
	}
	s = alerts_domain_score(sni, label, sizeof(label), NULL);
	if (s < cfg.dga_threshold || (int)strlen(label) < cfg.dga_min_label)
		return;
	snprintf(reason, sizeof(reason),
		 "%s connected to %s (port %u), a name that looks machine-generated (score %.2f), without looking "
		 "it up through AIGW. Malware generates names like this to find its control servers.",
		 name, sni, dport, s);
	alerts_raise("dga", "warning", client, sni, s, reason);
}

uint64_t alerts_total(void)
{
	return raised_total;
}

static void alert_blob(struct blob_buf *b, const struct alert *a)
{
	blobmsg_add_u32(b, "id", a->id);
	blobmsg_add_string(b, "kind", a->kind);
	blobmsg_add_string(b, "severity", a->severity);
	blobmsg_add_u64(b, "first", (uint64_t)a->first);
	blobmsg_add_u64(b, "last", (uint64_t)a->last);
	blobmsg_add_u32(b, "count", a->count);
	blobmsg_add_string(b, "device", a->device_name);
	blobmsg_add_string(b, "ip", a->device_ip);
	blobmsg_add_string(b, "mac", a->device_mac);
	blobmsg_add_string(b, "subject", a->subject);
	blobmsg_add_double(b, "score", a->score);
	blobmsg_add_string(b, "reason", a->reason);
}

void alerts_blob(struct blob_buf *b, unsigned limit, const char *name)
{
	void *arr, *t;
	unsigned i, n = 0;

	arr = blobmsg_open_array(b, name);
	for (i = 0; i < ALERT_MAX && n < limit; i++) {
		const struct alert *a = &alerts[(head + ALERT_MAX - 1 - i) % ALERT_MAX];  /* newest first */

		if (!a->used)
			continue;
		t = blobmsg_open_table(b, NULL);
		alert_blob(b, a);
		blobmsg_close_table(b, t);
		n++;
	}
	blobmsg_close_array(b, arr);
}

void alerts_save(struct blob_buf *b)
{
	alerts_blob(b, ALERT_MAX, "alerts");
}

void alerts_load(struct blob_attr *arr)
{
	struct blob_attr *e, *cur;
	size_t rem, r2;
	int n = 0;

	/* saved newest first: load in reverse so the ring keeps its order */
	struct blob_attr *items[ALERT_MAX];
	int k = 0;

	blobmsg_for_each_attr(e, arr, rem)
		if (k < ALERT_MAX)
			items[k++] = e;
	while (k-- > 0) {
		struct alert *a = &alerts[head];

		memset(a, 0, sizeof(*a));
		blobmsg_for_each_attr(cur, items[k], r2) {
			const char *key = blobmsg_name(cur);
			int t = blobmsg_type(cur);
			const char *s = t == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : "";
			uint64_t v = t == BLOBMSG_TYPE_INT64 ? blobmsg_get_u64(cur) :
				     t == BLOBMSG_TYPE_INT32 ? blobmsg_get_u32(cur) : 0;

			if (!strcmp(key, "id")) a->id = (uint32_t)v;
			else if (!strcmp(key, "kind")) snprintf(a->kind, sizeof(a->kind), "%s", s);
			else if (!strcmp(key, "severity")) snprintf(a->severity, sizeof(a->severity), "%s", s);
			else if (!strcmp(key, "first")) a->first = (int64_t)v;
			else if (!strcmp(key, "last")) a->last = (int64_t)v;
			else if (!strcmp(key, "count")) a->count = (uint32_t)v;
			else if (!strcmp(key, "device")) snprintf(a->device_name, sizeof(a->device_name), "%s", s);
			else if (!strcmp(key, "ip")) snprintf(a->device_ip, sizeof(a->device_ip), "%s", s);
			else if (!strcmp(key, "mac")) snprintf(a->device_mac, sizeof(a->device_mac), "%s", s);
			else if (!strcmp(key, "subject")) snprintf(a->subject, sizeof(a->subject), "%s", s);
			else if (!strcmp(key, "score") && t == BLOBMSG_TYPE_DOUBLE) a->score = blobmsg_get_double(cur);
			else if (!strcmp(key, "reason")) snprintf(a->reason, sizeof(a->reason), "%s", s);
		}
		if (!a->id || !a->kind[0])
			continue;
		a->used = 1;
		if (a->id >= next_id)
			next_id = a->id + 1;
		head = (head + 1) % ALERT_MAX;
		n++;
	}
	AG_INFO("restored %d alerts", n);
}
