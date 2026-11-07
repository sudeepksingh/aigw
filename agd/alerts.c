/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Alerts (phase 2, advise mode): the brain notices something worth a
 * person's attention and says why, in plain words. Nothing is blocked yet;
 * acting on alerts comes with the policy engine in phase 3.
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
	char path[512], err[128];
	int dim;

	if (!cfg.alerts_enabled) {
		AG_INFO("alerts disabled in config");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/domain-risk.agm", cfg.model_dir);
	dga_model = ag_model_load(path, err, sizeof(err));
	if (!dga_model) {
		AG_WARN("domain-risk model not loaded (%s: %s); other alerts still work", path, err);
		return -1;
	}
	if (ag_model_schema(dga_model) != ag_fnv1a(AG_DOMFEAT_SCHEMA) ||
	    ag_model_nfeatures(dga_model) != AG_DOMFEAT_N) {
		AG_WARN("%s was trained for different features; refusing it", path);
		ag_model_free(dga_model);
		dga_model = NULL;
		return -1;
	}
	bigram = ag_model_bigram(dga_model, &dim);
	bigram_dim = dim;
	AG_INFO("domain-risk model loaded: %d trees, alert at score >= %.2f",
		ag_model_ntrees(dga_model), cfg.dga_threshold);
	return 0;
}

const char *alerts_model_name(void)
{
	return dga_model ? ag_model_name(dga_model) : "";
}

int alerts_model_trees(void)
{
	return dga_model ? ag_model_ntrees(dga_model) : 0;
}

/* -1 when the name isn't judged (no model, no registrable label, too short) */
double alerts_domain_score(const char *qname, char *label, size_t n, float *features)
{
	float x[AG_DOMFEAT_N];

	if (!dga_model || ag_domain_label(qname, label, n) < 0)
		return -1;
	ag_domain_features(qname, label, bigram, bigram_dim, features ? features : x);
	return ag_model_prob(dga_model, features ? features : x);
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
}

void alerts_on_dns(const char *client, const char *qname)
{
	char label[256], reason[320], name[64], mac[18];
	double s;

	if (!cfg.alerts_enabled || is_local(client))
		return;
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
	if (no_dns) {
		snprintf(reason, sizeof(reason),
			 "%s connected to %s (%s/%u) without looking up any name first. "
			 "Apps rarely do this; malware and hard-coded trackers do.", name, dst, proto, dport);
		alerts_raise("no-dns", "info", client, dst, 0, reason);
	}
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
